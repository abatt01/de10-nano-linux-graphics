#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CSR_PHYS      0xFF240000u
#define BUF_PHYS      0x3F800000u
#define BUF_MAX_BYTES (8u << 20)
#define FPGA_CLK_HZ   50e6
#define ARRAY_SUM_ID  0x53554D31u
#define DT_NODE_NAME  "gpio@100040000"
#define PAGE_BYTES    4096u

enum {
    REG_PIO_DATA, REG_PIO_DIR, REG_PIO_IRQMASK, REG_PIO_EDGECAP,
    REG_CTRL, REG_STATUS, REG_ADDR, REG_COUNT,
    REG_SUM_LO, REG_SUM_HI, REG_CYCLES, REG_ID
};

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;

    ssize_t result = write(fd, text, strlen(text));
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result < 0 ? -1 : 0;
}

static int read_text(const char *path, char *text, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    ssize_t result = read(fd, text, size - 1);
    close(fd);
    if (result < 0)
        return -1;

    text[result] = '\0';
    return 0;
}

/* Linux assigns GPIO numbers dynamically. Find this controller by its label. */
static int find_gpio_number(void)
{
    DIR *dir = opendir("/sys/class/gpio");
    struct dirent *entry;
    char path[320], text[256];
    int base = -1;

    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "gpiochip", 8) != 0)
            continue;

        snprintf(path, sizeof(path), "/sys/class/gpio/%s/label", entry->d_name);
        if (read_text(path, text, sizeof(text)) == 0 &&
            strstr(text, DT_NODE_NAME) != NULL) {
            snprintf(path, sizeof(path), "/sys/class/gpio/%s/base", entry->d_name);
            if (read_text(path, text, sizeof(text)) == 0)
                base = atoi(text);
            break;
        }
    }

    closedir(dir);
    return base;
}

static void finish_memory_writes(void)
{
#if defined(__arm__)
    __asm__ __volatile__("dsb" ::: "memory");
#else
    __sync_synchronize();
#endif
}

int main(int argc, char **argv)
{
    unsigned long requested = 1000000;
    if (argc > 1) {
        char *end;
        errno = 0;
        requested = strtoul(argv[1], &end, 0);
        if (errno || end == argv[1] || *end != '\0')
            requested = 0;
    }
    if (requested < 1 || requested > BUF_MAX_BYTES / 4) {
        fprintf(stderr, "Number of elements must be 1 through %u\n",
                BUF_MAX_BYTES / 4);
        return 1;
    }
    uint32_t n = (uint32_t)requested;

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) {
        perror("open /dev/mem");
        return 1;
    }

    size_t bytes = ((size_t)n * 4 + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
    volatile uint32_t *csr = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, mem, CSR_PHYS);
    volatile uint32_t *buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, mem, BUF_PHYS);
    if (csr == MAP_FAILED || buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    close(mem);

    uint32_t id = csr[REG_ID];
    if (id != ARRAY_SUM_ID) {
        fprintf(stderr, "Wrong FPGA ID: got 0x%08x, expected 0x%08x\n",
                id, ARRAY_SUM_ID);
        return 1;
    }

    uint32_t *copy = malloc((size_t)n * sizeof(*copy));
    if (!copy) {
        perror("malloc");
        return 1;
    }

    uint32_t random = 2463534242u;
    for (uint32_t i = 0; i < n; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        copy[i] = random;
    }

    uint64_t cpu_sum = 0, shared_sum = 0;
    double start = now_ms();
    for (uint32_t i = 0; i < n; ++i)
        cpu_sum += copy[i];
    double cached_ms = now_ms() - start;

    for (uint32_t i = 0; i < n; ++i)
        buf[i] = copy[i];

    start = now_ms();
    for (uint32_t i = 0; i < n; ++i)
        shared_sum += buf[i];
    double shared_ms = now_ms() - start;

    int gpio = find_gpio_number();
    if (gpio < 0) {
        fprintf(stderr, "GPIO controller %s was not found\n", DT_NODE_NAME);
        return 1;
    }

    char number[16], path[128], initial;
    snprintf(number, sizeof(number), "%d", gpio);
    int exported = write_text("/sys/class/gpio/export", number) == 0;
    if (!exported && errno != EBUSY) {
        perror("export GPIO");
        return 1;
    }

    int result = 1, value_fd = -1;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    if (write_text(path, "in") < 0) {
        perror("set GPIO direction");
        goto cleanup;
    }

    csr[REG_PIO_EDGECAP] = 1;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/edge", gpio);
    if (write_text(path, "rising") < 0) {
        perror("set GPIO edge");
        goto cleanup;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    value_fd = open(path, O_RDONLY);
    if (value_fd < 0 || read(value_fd, &initial, 1) != 1) {
        fprintf(stderr, "Could not read the GPIO value\n");
        goto cleanup;
    }

    csr[REG_ADDR] = BUF_PHYS;
    csr[REG_COUNT] = n;
    finish_memory_writes();

    start = now_ms();
    csr[REG_CTRL] = 1;
    struct pollfd event = { .fd = value_fd, .events = POLLPRI | POLLERR };
    int ready = poll(&event, 1, 5000);
    double wall_ms = now_ms() - start;

    if (ready < 0) {
        perror("poll");
        goto cleanup;
    }
    if (ready == 0 || !(event.revents & POLLPRI)) {
        fprintf(stderr, "No FPGA interrupt within 5 s (STATUS=0x%x)\n",
                csr[REG_STATUS]);
        goto cleanup;
    }

    uint64_t fpga_sum = ((uint64_t)csr[REG_SUM_HI] << 32) | csr[REG_SUM_LO];
    uint32_t cycles = csr[REG_CYCLES];
    double fpga_ms = cycles / FPGA_CLK_HZ * 1e3;
    int match = fpga_sum == cpu_sum && cpu_sum == shared_sum;

    puts("Interrupt received from the FPGA.");
    printf("Elements                    : %u\n", n);
    printf("FPGA sum                    : %llu\n", (unsigned long long)fpga_sum);
    printf("CPU sum                     : %llu\n", (unsigned long long)cpu_sum);
    printf("Result                      : %s\n", match ? "MATCH" : "MISMATCH");
    printf("FPGA busy time              : %u cycles = %.3f ms\n", cycles, fpga_ms);
    printf("Start -> interrupt (wall)   : %.3f ms\n", wall_ms);
    printf("CPU sum, cached copy        : %.3f ms\n", cached_ms);
    printf("CPU sum, same buffer as FPGA: %.3f ms\n", shared_ms);
    result = match ? 0 : 2;

cleanup:
    if (value_fd >= 0)
        close(value_fd);
    if (exported)
        write_text("/sys/class/gpio/unexport", number);
    free(copy);
    munmap((void *)buf, bytes);
    munmap((void *)csr, PAGE_BYTES);
    return result;
}

#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
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

#include "reference.h"

#define CSR_PHYS 0xff240000u
#define INPUT_PHYS 0x3f800000u
#define OUTPUT_PHYS 0x3fc00000u
#define FIR_ID 0x46495231u
#define DT_NODE_NAME "gpio@100040000"

enum {
    REG_PIO_DATA, REG_PIO_DIR, REG_PIO_IRQMASK, REG_PIO_EDGECAP,
    REG_CTRL, REG_STATUS, REG_INPUT, REG_COUNT, REG_OUTPUT,
    REG_UNUSED_9, REG_UNUSED_10, REG_ID
};

static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
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
    return result == (ssize_t)strlen(text) ? 0 : -1;
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
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static int save_pcm(const char *path, const int16_t *samples, size_t count)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return 1;
    }
    int ok = fwrite(samples, sizeof(*samples), count, file) == count;
    if (fclose(file) != 0)
        ok = 0;
    if (!ok)
        fprintf(stderr, "Could not write PCM to %s\n", path);
    return !ok;
}

int main(int argc, char **argv)
{
    static int16_t input[FIR_SAMPLES], cpu[FIR_SAMPLES], fpga[FIR_SAMPLES];
    const size_t bytes = sizeof(input);
    const int reference_only = argc == 4 && strcmp(argv[1], "--reference") == 0;
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "Usage: %s input.s16le [arm.s16le fpga.s16le]\n"
                        "       %s --reference input.s16le output.s16le\n",
                argv[0], argv[0]);
        return 1;
    }
    FILE *clip = fopen(argv[reference_only ? 2 : 1], "rb");
    if (!clip) {
        perror("open clip");
        return 1;
    }
    int clip_ok = fread(input, 1, bytes, clip) == bytes &&
                  fgetc(clip) == EOF && !ferror(clip);
    fclose(clip);
    if (!clip_ok) {
        fprintf(stderr, "Expected exactly %zu bytes of signed little-endian PCM\n", bytes);
        return 1;
    }

    if (reference_only) {
        double start = now_ms();
        fir_reference(input, cpu, FIR_SAMPLES);
        double elapsed = now_ms() - start;
        if (save_pcm(argv[3], cpu, FIR_SAMPLES))
            return 1;
        printf("CPU reference              %.3f ms (%d samples)\n",
               elapsed, FIR_SAMPLES);
        return 0;
    }

    int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) {
        perror("open /dev/mem");
        return 1;
    }
    volatile uint32_t *csr = mmap(NULL, 0x40, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, mem, CSR_PHYS);
    volatile uint16_t *staging_input = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, mem, INPUT_PHYS);
    const volatile uint16_t *staging_output = mmap(NULL, bytes, PROT_READ,
                                                  MAP_SHARED, mem, OUTPUT_PHYS);
    close(mem);
    if (csr == MAP_FAILED || staging_input == MAP_FAILED || staging_output == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    uint32_t id = csr[REG_ID];
    if (id != FIR_ID || (csr[REG_STATUS] & 1)) {
        fprintf(stderr, "Unexpected device state: ID=0x%08x STATUS=0x%08x\n",
                id, csr[REG_STATUS]);
        return 1;
    }
    int gpio = find_gpio_number();
    if (gpio < 0) {
        fprintf(stderr, "GPIO controller %s was not found\n", DT_NODE_NAME);
        return 1;
    }

    char number[16], path[128], initial;
    snprintf(number, sizeof(number), "%d", gpio);
    if (write_text("/sys/class/gpio/export", number) != 0) {
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
        fprintf(stderr, "Could not read GPIO value\n");
        goto cleanup;
    }
    if ((csr[REG_PIO_IRQMASK] & 1) == 0) {
        fprintf(stderr, "GPIO driver did not arm the device IRQ\n");
        goto cleanup;
    }

    double start = now_ms();
    fir_reference(input, cpu, FIR_SAMPLES);
    double cpu_ms = now_ms() - start;

    start = now_ms();
    for (size_t i = 0; i < FIR_SAMPLES; ++i)
        staging_input[i] = (uint16_t)input[i];
    finish_memory_writes();
    csr[REG_INPUT] = INPUT_PHYS;
    csr[REG_COUNT] = FIR_SAMPLES;
    csr[REG_OUTPUT] = OUTPUT_PHYS;
    finish_memory_writes();
    if (csr[REG_INPUT] != INPUT_PHYS || csr[REG_COUNT] != FIR_SAMPLES ||
        csr[REG_OUTPUT] != OUTPUT_PHYS) {
        fprintf(stderr, "FIR configuration readback failed\n");
        goto cleanup;
    }
    csr[REG_CTRL] = 1;
    struct pollfd event = { .fd = value_fd, .events = POLLPRI | POLLERR };
    int ready = poll(&event, 1, 5000);
    finish_memory_writes();
    uint32_t status = csr[REG_STATUS];
    if (ready != 1 || !(event.revents & POLLPRI) || status != 2) {
        fprintf(stderr, "FPGA completion failed: poll=%d revents=0x%x STATUS=0x%08x ID=0x%08x\n",
                ready, (unsigned)event.revents, status, csr[REG_ID]);
        goto cleanup;
    }
    for (size_t i = 0; i < FIR_SAMPLES; ++i)
        fpga[i] = (int16_t)staging_output[i];
    finish_memory_writes();
    double fpga_ms = now_ms() - start;

    for (size_t i = 0; i < FIR_SAMPLES; ++i) {
        if (cpu[i] != fpga[i]) {
            fprintf(stderr, "MISMATCH at sample %zu: ARM=%d FPGA=%d\n", i, cpu[i], fpga[i]);
            result = 2;
            goto cleanup;
        }
    }
    puts("MATCH (16384 samples, one on-board timing pair)");
    printf("ARM CPU                    %.3f ms\n", cpu_ms);
    printf("FPGA end-to-end            %.3f ms\n", fpga_ms);
    printf("CPU minus FPGA             %.3f ms\n", cpu_ms - fpga_ms);
    printf("CPU / FPGA                 %.3fx\n", cpu_ms / fpga_ms);
    result = 0;
    if (argc == 4) {
        int arm_error = save_pcm(argv[2], cpu, FIR_SAMPLES);
        int fpga_error = save_pcm(argv[3], fpga, FIR_SAMPLES);
        result = arm_error || fpga_error;
    }

cleanup:
    if (value_fd >= 0)
        close(value_fd);
    if (write_text("/sys/class/gpio/unexport", number) < 0) {
        perror("unexport GPIO");
        result = 1;
    }
    munmap((void *)csr, 0x40);
    munmap((void *)staging_input, bytes);
    munmap((void *)staging_output, bytes);
    return result;
}

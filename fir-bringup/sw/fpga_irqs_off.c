#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define GICD_BASE 0xFFFED000UL
#define ISENABLER 0x100
#define ICENABLER 0x180
#define FIRST_IRQ 72
#define LAST_IRQ  135

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    volatile uint32_t *gicd = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, GICD_BASE);
    if (gicd == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    int masked = 0;
    for (int reg = FIRST_IRQ / 32; reg <= LAST_IRQ / 32; ++reg) {
        uint32_t mask = 0;

        for (int bit = 0; bit < 32; ++bit) {
            int irq = reg * 32 + bit;
            if (irq >= FIRST_IRQ && irq <= LAST_IRQ)
                mask |= 1u << bit;
        }

        uint32_t enabled = gicd[(ISENABLER / 4) + reg] & mask;
        for (int bit = 0; bit < 32; ++bit) {
            if (enabled & (1u << bit)) {
                int irq = reg * 32 + bit;
                int fpga_irq = irq - FIRST_IRQ;
                printf("masking FPGA interrupt: GIC %d (f2h_irq%d bit %d)\n",
                       irq, fpga_irq / 32, fpga_irq % 32);
                ++masked;
            }
        }
        if (enabled)
            gicd[(ICENABLER / 4) + reg] = enabled;
    }

    printf("%d FPGA interrupt(s) masked.\n", masked);
    munmap((void *)gicd, 0x1000);
    close(fd);
    return 0;
}

/*
 * DE10-Nano Linux graphics character device.
 *
 * User programs submit one ASCII command per write to /dev/video. Drawing
 * targets the back buffer; sync presents it at vertical blank. The complete
 * driver enables every lab command, while double_buffer selects the memory
 * layout needed by a particular demonstration.
 *
 * Hardware: course DE10-Nano Computer FPGA system, RGB332 pixel memory,
 * 320x240 stored pixels, and an independent 80x60 character overlay.
 */
#define LAB_PART 6
/* DE10-Nano Computer video driver. LAB_PART is set in each video.c. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include <linux/ctype.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#define ioremap_nocache ioremap
#endif

/* Physical FPGA addresses are mapped before use; they are never dereferenced
 * directly. Each pixel frame reserves 512 columns x 256 rows, or 128 KiB.
 * Visible dimensions are smaller than the addressable row and frame strides. */
#define CTRL_BASE 0xff203020
#define PIX_BASE  0xc8000000
#define PIX_SPAN  0x40000
#define FRAME_SPAN 0x20000
#define CHAR_BASE 0xc9000000
#define CHAR_SPAN 0x2000
#define MAX_COMMAND 256
static void __iomem *ctrl, *pixels, *chars, *charctrl;
static unsigned int width, height, textwidth, textheight;
static u32 saved_front, saved_back;
static bool configured;
static bool double_buffer = (LAB_PART >= 4);
module_param(double_buffer, bool, 0444);
MODULE_PARM_DESC(double_buffer, "Use two FPGA pixel buffers (default from part 4)");
static dev_t video_number;
static struct cdev video_cdev;
static struct class *video_class;
static DEFINE_MUTEX(video_lock);

/* Swap at vertical blank; never allow a stalled controller to hang Linux. */
/* ctrl offsets: +0 Buffer, +4 Backbuffer, +8 Resolution, +12 Status.
 * Buffer writes request a swap rather than assigning a front-buffer address.
 * S=1 means a request is pending; S=0 means the swap has completed. */
static int sync_screen(void)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(200);
    wmb();
    writel(1, ctrl);
    while (readl(ctrl + 12) & 1) {
        if (time_after_eq(jiffies, deadline))
            return -ETIMEDOUT;
        usleep_range(100, 300);
    }
    return 0;
}

/* Every draw operation resolves the current back buffer after any swap. */
static void __iomem *draw_buffer(void)
{
    u32 base = readl(ctrl + 4);
    if (base == PIX_BASE || base == PIX_BASE + FRAME_SPAN)
        return pixels + (base - PIX_BASE);
    return NULL;
}

/* Commands use RGB565, matching the handout; hardware stores RGB332. */
/* Keep the high color bits: RGB565 R[4:2], G[5:3], B[4:3] become
 * RGB332 R[2:0], G[2:0], B[1:0]. All command colors remain RGB565. */
static u8 rgb332(unsigned int color)
{
    return ((color >> 8) & 0xe0) | ((color >> 6) & 0x1c)
           | ((color >> 3) & 0x03);
}
/* Byte writes match the Nano framebuffer. The y shift is a 512-byte stride,
 * not the 320-pixel visible width. This final bounds check guards every caller. */
static void plot(void __iomem *buf, int x, int y, unsigned int color)
{
    if (x >= 0 && y >= 0 && x < width && y < height)
        writeb(rgb332(color), buf + (y << 9) + x);
}
/* Clear only visible pixels; padding at the end of each row is not displayed. */
static void clear_screen(void __iomem *buf)
{
    unsigned int y;
    for (y = 0; y < height; ++y)
        memset_io(buf + (y << 9), 0, width);
}
/* Reject invalid command endpoints before an entire primitive is drawn. */
static bool valid_point(int x, int y)
{
    return x >= 0 && y >= 0 && x < width && y < height;
}

/* Integer Bresenham: transpose steep lines and order the major axis. */
static void draw_line(void __iomem *buf, int x0, int y0,
                      int x1, int y1, unsigned int color)
{
    int steep = abs(y1 - y0) > abs(x1 - x0);
    int dx, dy, error, y, step, x;
    if (steep) { swap(x0, y0); swap(x1, y1); }
    if (x0 > x1) { swap(x0, x1); swap(y0, y1); }
    dx = x1 - x0;
    dy = abs(y1 - y0);
    error = -(dx / 2);
    y = y0;
    step = y0 < y1 ? 1 : -1;
    for (x = x0; x <= x1; ++x) {
        if (steep) plot(buf, y, x, color);
        else plot(buf, x, y, color);
        error += dy;
        if (error >= 0) { y += step; error -= dx; }
    }
}
/* Corners are inclusive. Normalize their order so either drag direction works. */
static void draw_box(void __iomem *buf, int x0, int y0,
                     int x1, int y1, unsigned int color)
{
    int x, y;
    if (x0 > x1) swap(x0, x1);
    if (y0 > y1) swap(y0, y1);
    for (y = y0; y <= y1; ++y)
        for (x = x0; x <= x1; ++x)
            plot(buf, x, y, color);
}
/* Character cells store ASCII bytes with a 128-byte row stride. Spaces remove
 * glyphs without changing the pixel graphics underneath the character layer. */
static void erase_text(void)
{
    unsigned int y;
    for (y = 0; y < textheight; ++y)
        memset_io(chars + (y << 7), ' ', textwidth);
}
/* sscanf conversion counts alone do not reject trailing junk; validate the
 * remaining command characters separately after the last parsed argument. */
static bool at_end(const char *s)
{
    while (*s && isspace(*s)) ++s;
    return !*s;
}
/* Base 16 accepts both F800 and 0xF800. Values above RGB565 are rejected. */
static int parse_color(char *token, unsigned int *color)
{
    int ret = kstrtouint(token, 16, color);
    if (ret) return ret;
    return *color <= 0xffff ? 0 : -ERANGE;
}
/* Called with video_lock held. Resolve the drawing buffer for each command
 * because a completed sync exchanges the controller's front/back addresses.
 * LAB_PART gates capabilities, and the shared full build selects all six. */
static int execute(char *cmd)
{
    void __iomem *buf = draw_buffer();
    char colorstr[17];
    unsigned int color;
    int x0, y0, x1, y1, n = 0, ret, x;
    char *s;
    if (!buf) return -EIO;
    if (!strcmp(cmd, "clear")) { clear_screen(buf); return 0; }
    if (LAB_PART >= 3 && !strcmp(cmd, "sync")) return sync_screen();
    if (LAB_PART >= 6 && !strcmp(cmd, "erase")) { erase_text(); return 0; }
    if (sscanf(cmd, "pixel %d,%d %16s %n", &x0, &y0, colorstr, &n) == 3
        && n && at_end(cmd + n)) {
        if (!valid_point(x0, y0)) return -ERANGE;
        ret = parse_color(colorstr, &color);
        if (ret) return ret;
        plot(buf, x0, y0, color);
        return 0;
    }
    n = 0;
    if (LAB_PART >= 2 &&
        sscanf(cmd, "line %d,%d %d,%d %16s %n",
               &x0, &y0, &x1, &y1, colorstr, &n) == 5
        && n && at_end(cmd + n)) {
        if (!valid_point(x0, y0) || !valid_point(x1, y1)) return -ERANGE;
        ret = parse_color(colorstr, &color);
        if (ret) return ret;
        draw_line(buf, x0, y0, x1, y1, color);
        return 0;
    }
    n = 0;
    if (LAB_PART >= 4 &&
        sscanf(cmd, "box %d,%d %d,%d %16s %n",
               &x0, &y0, &x1, &y1, colorstr, &n) == 5
        && n && at_end(cmd + n)) {
        if (!valid_point(x0, y0) || !valid_point(x1, y1)) return -ERANGE;
        ret = parse_color(colorstr, &color);
        if (ret) return ret;
        draw_box(buf, x0, y0, x1, y1, color);
        return 0;
    }
    n = 0;
    if (LAB_PART >= 6 &&
        sscanf(cmd, "text %d,%d %n", &x0, &y0, &n) == 2 && n) {
        if (x0 < 0 || y0 < 0 || x0 >= textwidth || y0 >= textheight)
            return -ERANGE;
        s = cmd + n;
        for (x = x0; *s && x < textwidth; ++s, ++x)
            writeb(*s, chars + (y0 << 7) + x);
        return 0;
    }
    return -EINVAL;
}
static int device_open(struct inode *inode, struct file *file) { return 0; }
static int device_release(struct inode *inode, struct file *file) { return 0; }
/* A normal per-open read offset supplies EOF after the resolution string,
 * allowing cat /dev/video to finish rather than printing forever. */
static ssize_t device_read(struct file *file, char __user *out,
                           size_t count, loff_t *offset)
{
    char text[32];
    int len = scnprintf(text, sizeof(text), "%03u %03u\n", width, height);
    return simple_read_from_buffer(out, count, offset, text, len);
}
/* The protocol is record-oriented: one complete command per write. Copy
 * userspace data before parsing, reject embedded records, and serialize MMIO
 * operations. Lock scope covers a complete primitive or synchronization. */
static ssize_t device_write(struct file *file, const char __user *in,
                            size_t count, loff_t *offset)
{
    char cmd[MAX_COMMAND], *start;
    int ret;
    if (!count) return 0;
    if (count >= sizeof(cmd)) return -E2BIG;
    if (copy_from_user(cmd, in, count)) return -EFAULT;
    /* Accept the optional final NUL used by some handout examples. */
    if (memchr(cmd, 0, count) &&
        memchr(cmd, 0, count) != cmd + count - 1) return -EINVAL;
    cmd[count] = 0;
    start = strim(cmd);
    if (strchr(start, '\n') || strchr(start, '\r')) return -EINVAL;
    if (mutex_lock_interruptible(&video_lock)) return -ERESTARTSYS;
    ret = execute(start);
    mutex_unlock(&video_lock);
    return ret ? ret : count;
}
static const struct file_operations video_fops = {
    .owner = THIS_MODULE, .open = device_open, .release = device_release,
    .read = device_read, .write = device_write, .llseek = no_llseek
};

static void release_hardware(void)
{
    if (configured) {
        /* Put the controller back as it was before this module loaded. */
        writel(saved_front, ctrl + 4);
        if (sync_screen()) pr_warn("video: restoring front buffer timed out\n");
        writel(saved_back, ctrl + 4);
    }
    if (charctrl) iounmap(charctrl);
    if (chars) iounmap(chars);
    if (pixels) iounmap(pixels);
    if (ctrl) iounmap(ctrl);
}
/* Initialize hardware before publishing the device node. Each failure label
 * releases only resources already acquired, in reverse initialization order.
 * Register checks assume the course FPGA image and bridges are already active. */
static int __init start_video(void)
{
    struct device *dev;
    u32 res;
    int ret;
    ctrl = ioremap_nocache(CTRL_BASE, 16);
    pixels = ioremap_nocache(PIX_BASE, PIX_SPAN);
    if (!ctrl || !pixels) { ret = -ENOMEM; goto fail_hw; }
    res = readl(ctrl + 8);
    width = res & 0xffff;
    height = res >> 16;
    if (width != 320 || height != 240) {
        pr_err("video: expected DE10-Nano Computer 320x240; got %ux%u\n",
               width, height);
        ret = -ENODEV; goto fail_hw;
    }
    /* BS occupies status bits 11:8; DE10-Nano pixels must be 8-bit. */
    if (((readl(ctrl + 12) >> 8) & 15) != 7) {
        pr_err("video: FPGA image does not report 8-bit pixels\n");
        ret = -ENODEV; goto fail_hw;
    }
    if (readl(ctrl + 12) & 1) { ret = -EBUSY; goto fail_hw; }
    saved_front = readl(ctrl);
    saved_back = readl(ctrl + 4);
    if (LAB_PART >= 6) {
        chars = ioremap_nocache(CHAR_BASE, CHAR_SPAN);
        charctrl = ioremap_nocache(0xff203030, 16);
        if (!chars || !charctrl) { ret = -ENOMEM; goto fail_hw; }
        res = readl(charctrl + 8);
        textwidth = res & 0xffff; textheight = res >> 16;
        if (textwidth != 80 || textheight != 60) {
            pr_err("video: expected character resolution 80x60\n");
            ret = -ENODEV; goto fail_hw;
        }
    }
    clear_screen(pixels);
    clear_screen(pixels + FRAME_SPAN);
    configured = true;
    writel(PIX_BASE, ctrl + 4);
    ret = sync_screen();
    if (ret) goto fail_hw;
    writel(PIX_BASE + (double_buffer ? FRAME_SPAN : 0), ctrl + 4);
    if (chars) erase_text();
    ret = alloc_chrdev_region(&video_number, 0, 1, "video");
    if (ret) goto fail_hw;
    cdev_init(&video_cdev, &video_fops);
    ret = cdev_add(&video_cdev, video_number, 1);
    if (ret) goto fail_number;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    video_class = class_create("midterm_video");
#else
    video_class = class_create(THIS_MODULE, "midterm_video");
#endif
    if (IS_ERR(video_class)) { ret = PTR_ERR(video_class); goto fail_cdev; }
    dev = device_create(video_class, NULL, video_number, NULL, "video");
    if (IS_ERR(dev)) { ret = PTR_ERR(dev); goto fail_class; }
    pr_info("video: part %d, %ux%u RGB332, double_buffer=%d\n",
            LAB_PART, width, height, double_buffer);
    return 0;
fail_class: class_destroy(video_class);
fail_cdev: cdev_del(&video_cdev);
fail_number: unregister_chrdev_region(video_number, 1);
fail_hw: release_hardware(); return ret;
}
/* Open files pin THIS_MODULE, so normal removal occurs after applications
 * close their descriptors. Restore controller pointers before unmapping I/O. */
static void __exit stop_video(void)
{
    device_destroy(video_class, video_number);
    class_destroy(video_class);
    cdev_del(&video_cdev);
    unregister_chrdev_region(video_number, 1);
    release_hardware();
}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Midterm graphics project");
MODULE_DESCRIPTION("DE10-Nano Computer graphics character device");
module_init(start_video);
module_exit(stop_video);

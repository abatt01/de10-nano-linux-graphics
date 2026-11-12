/*
 * Optional DE10-Nano KEY/SW character devices.
 * Reads return ASCII hexadecimal: KEY is a consumed edge-event mask and SW
 * is the current switch-state mask. Use existing course drivers instead when
 * they already own these PIO registers; do not run competing KEY readers.
 */
/* Optional input devices for Parts V and VI. Do not load over existing drivers. */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#define ioremap_nocache ioremap
#endif
static void __iomem *keys, *switches;
static DEFINE_MUTEX(input_lock);
/* A captured edge survives between animation polls. Read it once per open
 * and acknowledge only the bits actually returned, using write-one-to-clear.
 * The FPGA PIO must be configured for the intended button-event polarity. */
static ssize_t key_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
    char text[8];
    u32 value;
    ssize_t ret;
    if (*off || !len) return 0;
    if (len < 2) return -EINVAL;
    if (mutex_lock_interruptible(&input_lock)) return -ERESTARTSYS;
    value = readl(keys + 12) & 3;
    scnprintf(text, sizeof(text), "%X\n", value);
    ret = simple_read_from_buffer(buf, len, off, text, 2);
    if (ret > 0) writel(value, keys + 12); /* acknowledge only captured events */
    mutex_unlock(&input_lock);
    return ret;
}
/* Switches are state inputs, so reads do not consume or acknowledge events. */
static ssize_t sw_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
    char text[8];
    int n = scnprintf(text, sizeof(text), "%X\n", readl(switches) & 15);
    return simple_read_from_buffer(buf, len, off, text, n);
}
static const struct file_operations key_fops = {
    .owner = THIS_MODULE, .read = key_read, .llseek = no_llseek
};
static const struct file_operations sw_fops = {
    .owner = THIS_MODULE, .read = sw_read, .llseek = no_llseek
};
static struct miscdevice key_dev = {
    .minor = MISC_DYNAMIC_MINOR, .name = "KEY", .fops = &key_fops
};
static struct miscdevice sw_dev = {
    .minor = MISC_DYNAMIC_MINOR, .name = "SW", .fops = &sw_fops
};
/* miscdevice supplies dynamic minor numbers and the /dev/KEY and /dev/SW
 * names. Initialization unwinds both registrations and mappings on failure. */
static int __init start_inputs(void)
{
    int ret;
    keys = ioremap_nocache(0xff200050, 16);
    switches = ioremap_nocache(0xff200040, 4);
    if (!keys || !switches) { ret = -ENOMEM; goto fail_map; }
    ret = misc_register(&key_dev);
    if (ret) goto fail_map;
    ret = misc_register(&sw_dev);
    if (ret) { misc_deregister(&key_dev); goto fail_map; }
    writel(3, keys + 12);
    pr_info("key_sw: KEY returns edge events; SW returns switch state, in hex\n");
    return 0;
fail_map:
    if (keys) iounmap(keys);
    if (switches) iounmap(switches);
    return ret;
}
/* Remove device nodes before releasing the underlying register mappings. */
static void __exit stop_inputs(void)
{
    misc_deregister(&sw_dev);
    misc_deregister(&key_dev);
    iounmap(switches);
    iounmap(keys);
}
module_init(start_inputs);
module_exit(stop_inputs);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DE10-Nano KEY edge events and SW states");

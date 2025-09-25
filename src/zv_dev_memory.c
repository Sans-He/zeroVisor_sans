#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <asm/io.h>

#include <../include/zv_dev_memory.h>
#include <../include/zv_log.h>

#define DEVICE_NAME "zv_shmem"
#define BUF_SIZE (PAGE_SIZE * 4)

static struct cdev cdev;
static dev_t devt;
char *zv_inst_buf;
static struct class *shmem_class;

static char *zv_shmem_devnode(struct device *dev, umode_t *mode);
static int zv_shmem_mmap(struct file *filp, struct vm_area_struct *vma);
static ssize_t zv_shmem_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);

static char *zv_shmem_devnode(struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666;
    return NULL;
}

static int zv_shmem_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long pfn;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > BUF_SIZE)
        return -EINVAL;

    pfn = virt_to_phys(zv_inst_buf) >> PAGE_SHIFT;

    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot))
        return -EAGAIN;

    return 0;
}

static ssize_t zv_shmem_write(struct file *filp, const char __user *buf,
                           size_t count, loff_t *f_pos)
{
    if (count > BUF_SIZE)
        count = BUF_SIZE;
    if (copy_from_user(zv_inst_buf, buf, count))
        return -EFAULT;
    return count;
}

static const struct file_operations shmem_fops = {
    .owner = THIS_MODULE,
    .mmap = zv_shmem_mmap,
    .write = zv_shmem_write,
};

int zv_shmem_init(void)
{
    int ret;

    //alloc dev_region
    ret = alloc_chrdev_region(&devt, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        zv_log_write(LOG_NORMAL, "Shmem","shmem: alloc_chrdev_region failed\n");
        return ret;
    }

    //init cdev
    cdev_init(&cdev, &shmem_fops);
    ret = cdev_add(&cdev, devt, 1);
    if (ret) {
        unregister_chrdev_region(devt, 1);
        zv_log_write(LOG_NORMAL, "Shmem", "shmem: cdev_add failed\n");
        return ret;
    }

    // allocate shared memory
    zv_inst_buf = kmalloc(BUF_SIZE, GFP_KERNEL);
    if (!zv_inst_buf) {
        cdev_del(&cdev);
        unregister_chrdev_region(devt, 1);
        zv_log_write(LOG_NORMAL, "Shmem", "shmem: kmalloc failed\n");
        return -ENOMEM;
    }
    memset(zv_inst_buf, 0, BUF_SIZE);

    // create class
    shmem_class = class_create(THIS_MODULE, "zv_class");
    if (IS_ERR(shmem_class)) {
        kfree(zv_inst_buf);
        cdev_del(&cdev);
        unregister_chrdev_region(devt, 1);
        return PTR_ERR(shmem_class);
    }

    shmem_class->devnode = zv_shmem_devnode;
    
    // create /dev/zv_shmem
    if (IS_ERR(device_create(shmem_class, NULL, devt, NULL, DEVICE_NAME))) {
        class_destroy(shmem_class);
        kfree(zv_inst_buf);
        cdev_del(&cdev);
        unregister_chrdev_region(devt, 1);
        zv_log_write(LOG_NORMAL, "Shmem", "shmem: device_create failed\n");
        return -ENOMEM;
    }
    zv_log_write(LOG_NORMAL, "Shmem", "Shmem: init, major %d, /dev/%s created\n", MAJOR(devt), DEVICE_NAME);
    return 0;
}

void  zv_shmem_exit(void)
{
    device_destroy(shmem_class, devt);
    class_destroy(shmem_class);
    kfree(zv_inst_buf);
    cdev_del(&cdev);
    unregister_chrdev_region(devt, 1);
    zv_log_write(LOG_NORMAL, "Shmem", "Shary memory module exit");
}


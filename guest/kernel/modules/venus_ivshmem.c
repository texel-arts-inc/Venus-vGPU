// SPDX-License-Identifier: GPL-2.0
#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define IV_POSITION 8
#define IV_DOORBELL 12
#define IV_MEMORY_BAR 2
#define VENUS_IVSHMEM_PEER _IOR('V', 0, __u32)
#define VENUS_IVSHMEM_SLICE _IOW('V', 1, struct rv_ivshmem_slice)
#define VENUS_IVSHMEM_SIZE _IOR('V', 2, __u64)

struct rv_ivshmem_slice {
	__u64 offset;
	__u64 length;
};

struct venus_device {
	struct miscdevice misc;
	void __iomem *registers;
	resource_size_t physical;
	resource_size_t length;
	wait_queue_head_t wait;
	atomic64_t kicks;
	atomic_t opened;
};

struct venus_slice {
	struct file *owner;
	resource_size_t physical;
	size_t length;
};

static int venus_open(struct inode *inode, struct file *file)
{
	struct venus_device *v = container_of(file->private_data, struct venus_device, misc);

	return atomic_cmpxchg(&v->opened, 0, 1) ? -EBUSY : 0;
}

static int venus_release(struct inode *inode, struct file *file)
{
	struct venus_device *v = container_of(file->private_data, struct venus_device, misc);

	atomic_set(&v->opened, 0);
	return 0;
}

static irqreturn_t venus_irq(int irq, void *data)
{
	struct venus_device *v = data;

	atomic64_inc(&v->kicks);
	wake_up_interruptible(&v->wait);
	return IRQ_HANDLED;
}

static ssize_t venus_read(struct file *file, char __user *out, size_t size, loff_t *offset)
{
	struct venus_device *v = container_of(file->private_data, struct venus_device, misc);
	u64 kicks;
	int err;

	if (size != sizeof(kicks))
		return -EINVAL;
	if (!atomic64_read(&v->kicks) && file->f_flags & O_NONBLOCK)
		return -EAGAIN;
	err = wait_event_interruptible(v->wait, atomic64_read(&v->kicks));
	if (err)
		return err;
	kicks = atomic64_xchg(&v->kicks, 0);
	if (copy_to_user(out, &kicks, sizeof(kicks)))
		return -EFAULT;
	return sizeof(kicks);
}

static ssize_t venus_write(struct file *file, const char __user *in, size_t size, loff_t *offset)
{
	struct venus_device *v = container_of(file->private_data, struct venus_device, misc);
	u32 peer;

	if (size != sizeof(peer))
		return -EINVAL;
	if (copy_from_user(&peer, in, sizeof(peer)))
		return -EFAULT;
	if (peer >= U16_MAX || peer == readl(v->registers + IV_POSITION))
		return -EINVAL;
	writel(peer << 16, v->registers + IV_DOORBELL);
	return sizeof(peer);
}

static __poll_t venus_poll(struct file *file, poll_table *table)
{
	struct venus_device *v = container_of(file->private_data, struct venus_device, misc);

	poll_wait(file, &v->wait, table);
	return atomic64_read(&v->kicks) ? EPOLLIN | EPOLLRDNORM : 0;
}

static int venus_map_range(struct vm_area_struct *vma,
			   resource_size_t physical, size_t length)
{
	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != length ||
	    !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	vm_flags_clear(vma, VM_MAYEXEC);
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTDUMP | VM_DONTEXPAND);
	/* BAR2 is shared RAM supplied by this QEMU device, with no MMIO registers. */
	return remap_pfn_range(vma, vma->vm_start, PHYS_PFN(physical),
			       length, vma->vm_page_prot);
}

static int venus_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct venus_device *v = container_of(file->private_data, struct venus_device, misc);

	return venus_map_range(vma, v->physical, v->length);
}

static int venus_slice_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct venus_slice *slice = file->private_data;

	return venus_map_range(vma, slice->physical, slice->length);
}

static int venus_slice_release(struct inode *inode, struct file *file)
{
	struct venus_slice *slice = file->private_data;

	fput(slice->owner);
	kfree(slice);
	return 0;
}

static const struct file_operations venus_slice_ops = {
	.owner = THIS_MODULE,
	.release = venus_slice_release,
	.mmap = venus_slice_mmap,
};

static int venus_slice_fd(struct venus_device *v, struct file *owner,
			  unsigned long arg)
{
	struct rv_ivshmem_slice range;
	struct venus_slice *slice;
	int fd;

	if (copy_from_user(&range, (void __user *)arg, sizeof(range)))
		return -EFAULT;
	if (!range.length || !PAGE_ALIGNED(range.offset) ||
	    !PAGE_ALIGNED(range.length) || range.offset >= v->length ||
	    range.length > v->length - range.offset)
		return -EINVAL;

	slice = kmalloc(sizeof(*slice), GFP_KERNEL);
	if (!slice)
		return -ENOMEM;
	/* Keep the exclusive device open until the last exported mapping closes. */
	slice->owner = get_file(owner);
	slice->physical = v->physical + range.offset;
	slice->length = range.length;
	fd = anon_inode_getfd("venus-ivshmem-slice", &venus_slice_ops, slice,
			     O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fput(slice->owner);
		kfree(slice);
	}
	return fd;
}

static long venus_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct venus_device *v = container_of(file->private_data, struct venus_device, misc);
	u32 peer;

	if (command == VENUS_IVSHMEM_SLICE)
		return venus_slice_fd(v, file, arg);
	if (command == VENUS_IVSHMEM_SIZE)
		return put_user((__u64)v->length, (__u64 __user *)arg);
	if (command != VENUS_IVSHMEM_PEER)
		return -ENOTTY;
	peer = readl(v->registers + IV_POSITION);
	return put_user(peer, (u32 __user *)arg);
}

static const struct file_operations venus_ops = {
	.owner = THIS_MODULE,
	.open = venus_open,
	.release = venus_release,
	.read = venus_read,
	.write = venus_write,
	.poll = venus_poll,
	.unlocked_ioctl = venus_ioctl,
	.mmap = venus_mmap,
};

static int venus_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct venus_device *v;
	resource_size_t length = pci_resource_len(pdev, IV_MEMORY_BAR);
	int err;

	if ((length != SZ_4M && length != SZ_32M) ||
	    !(pci_resource_flags(pdev, IV_MEMORY_BAR) & IORESOURCE_PREFETCH))
		return -EINVAL;
	err = pcim_enable_device(pdev);
	if (err)
		return err;
	v = devm_kzalloc(&pdev->dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->registers = pcim_iomap(pdev, 0, 0);
	if (!v->registers)
		return -ENOMEM;
	v->physical = pci_resource_start(pdev, IV_MEMORY_BAR);
	v->length = length;
	init_waitqueue_head(&v->wait);
	atomic64_set(&v->kicks, 0);
	atomic_set(&v->opened, 0);

	pci_set_master(pdev);
	err = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
	if (err < 0)
		return err;
	err = request_irq(pci_irq_vector(pdev, 0), venus_irq, 0, "venus-ivshmem", v);
	if (err)
		goto free_vectors;
	v->misc.minor = MISC_DYNAMIC_MINOR;
	v->misc.name = "venus-ivshmem";
	v->misc.mode = 0600;
	v->misc.fops = &venus_ops;
	v->misc.parent = &pdev->dev;
	err = misc_register(&v->misc);
	if (err)
		goto free_irq;
	pci_set_drvdata(pdev, v);
	dev_info(&pdev->dev, "Venus peer %u with MSI-X vector 0\n",
		 readl(v->registers + IV_POSITION));
	return 0;

free_irq:
	free_irq(pci_irq_vector(pdev, 0), v);
free_vectors:
	pci_free_irq_vectors(pdev);
	return err;
}

static void venus_remove(struct pci_dev *pdev)
{
	struct venus_device *v = pci_get_drvdata(pdev);

	misc_deregister(&v->misc);
	free_irq(pci_irq_vector(pdev, 0), v);
	pci_free_irq_vectors(pdev);
}

static const struct pci_device_id venus_ids[] = {
	{ PCI_DEVICE(0x1af4, 0x1110) },
	{ }
};
MODULE_DEVICE_TABLE(pci, venus_ids);

static struct pci_driver venus_driver = {
	.name = "venus-ivshmem",
	.id_table = venus_ids,
	.probe = venus_probe,
	.remove = venus_remove,
	.driver.suppress_bind_attrs = true,
};
module_pci_driver(venus_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Venus IVSHMEM aperture and MSI-X doorbell for the QEMU lab");

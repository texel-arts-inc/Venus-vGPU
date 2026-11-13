// SPDX-License-Identifier: GPL-2.0
/*
 * Export bounded leases from the QEMU file-backed NUMA RAM tail. This region
 * is claimed as ordinary refcounted pages before GPU initialization. Leases
 * suballocate aligned ranges within it, outside the identity prefix. No PFN
 * is accepted from userspace.
 */
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/highmem.h>
#include <linux/fdtable.h>
#include <linux/overflow.h>
#include <linux/ioctl.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "rv_guest_pool_uapi.h"

#define RV_POOL_MAX_BYTES (1536UL * 1024 * 1024)
#define RV_POOL_CLAIM_BYTES (4UL * 1024 * 1024)
#define RV_POOL_FILE_HEADER (16UL * 1024)


struct rv_pool_dev {
	struct kref refs;
	struct mutex lock;
	struct mutex alloc_lock;
	bool dying;
	struct device *dev;
	struct miscdevice misc;
	phys_addr_t ram_start; /* first allocatable physical byte */
	resource_size_t ram_size;
	u64 file_offset;       /* corresponding backing-file offset */
	u64 file_size;
	unsigned long first_pfn;
	unsigned int units, chunks;
	unsigned long *allocated;
	unsigned long *claimed;
	u8 pool_id[RV_POOL_ID_BYTES];
};
struct rv_pool_lease {
	struct rv_pool_dev *pool;
	struct page **pages;
	unsigned long first_unit;
	u64 bytes, offset;
};

static void rv_pool_release(struct kref *ref)
{
	struct rv_pool_dev *p = container_of(ref, struct rv_pool_dev, refs);
	unsigned long chunk;
	const unsigned long pages = RV_POOL_CLAIM_BYTES >> PAGE_SHIFT;

	if (p->claimed)
		for_each_set_bit(chunk, p->claimed, p->chunks)
			free_contig_range(p->first_pfn + chunk * pages, pages);
	bitmap_free(p->claimed);
	bitmap_free(p->allocated);
	put_device(p->dev);
	kfree(p);
}
static void rv_lease_free(struct rv_pool_lease *l)
{
	struct rv_pool_dev *p = l->pool;

	mutex_lock(&p->alloc_lock);
	bitmap_clear(p->allocated, l->first_unit, l->bytes / RV_POOL_ALIGN);
	mutex_unlock(&p->alloc_lock);
	kvfree(l->pages);
	kref_put(&p->refs, rv_pool_release);
	kfree(l);
}
static int rv_lease_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct rv_pool_lease *l = f->private_data;
	unsigned long n = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	int ret = -EINVAL;
	if (!l || vma->vm_pgoff || !(vma->vm_flags & VM_SHARED) ||
	    (vma->vm_flags & VM_EXEC) || vma->vm_end - vma->vm_start != l->bytes)
		return -EINVAL;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vm_flags_clear(vma, VM_MAYEXEC);
	ret = vm_insert_pages(vma, vma->vm_start, l->pages, &n);
	return ret;
}
static int rv_lease_release(struct inode *ino, struct file *f)
{
	rv_lease_free(f->private_data);
	return 0;
}
static const struct file_operations rv_lease_fops = {
	.owner = THIS_MODULE, .release = rv_lease_release, .mmap = rv_lease_mmap,
};
static int rv_pool_open(struct inode *ino, struct file *f)
{
	struct miscdevice *m = f->private_data;
	struct rv_pool_dev *p = container_of(m, struct rv_pool_dev, misc);
	mutex_lock(&p->lock);
	if (p->dying) { mutex_unlock(&p->lock); return -ENODEV; }
	kref_get(&p->refs);
	mutex_unlock(&p->lock);
	f->private_data = p;
	return 0;
}
static int rv_alloc_pages(struct rv_pool_dev *p, u64 bytes,
			  struct rv_pool_lease *l)
{
	const unsigned long pages_per_unit = RV_POOL_ALIGN >> PAGE_SHIFT;
	const unsigned long npages = bytes >> PAGE_SHIFT;
	const unsigned int units = bytes / RV_POOL_ALIGN;
	unsigned long first, start = 0, i;

	l->pages = kvmalloc_array(npages, sizeof(*l->pages), GFP_KERNEL);
	if (!l->pages)
		return -ENOMEM;

	mutex_lock(&p->alloc_lock);
	for (;;) {
		first = bitmap_find_next_zero_area(p->allocated, p->units,
						  start, units, 0);
		if (first > p->units - units)
			break;
		for (i = 0; i < npages; i++) {
			struct page *page = pfn_to_page(p->first_pfn + first * pages_per_unit + i);

			/* A driver pin can outlive the last userspace mapping. */
			if (page_count(page) != 1)
				break;
			l->pages[i] = page;
		}
		if (i == npages) {
			bitmap_set(p->allocated, first, units);
			mutex_unlock(&p->alloc_lock);

			for (i = 0; i < npages; i++)
				clear_highpage(l->pages[i]);
			l->first_unit = first;
			l->bytes = bytes;
			l->offset = p->file_offset + first * RV_POOL_ALIGN;
			return 0;
		}
		start = first + i / pages_per_unit + 1;
	}
	mutex_unlock(&p->alloc_lock);
	kvfree(l->pages);
	l->pages = NULL;
	return -ENOSPC;
}
static long rv_pool_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct rv_pool_dev *p = f->private_data;
	struct rv_pool_query q = { .version = RV_POOL_ABI_VERSION,
		.alignment = RV_POOL_ALIGN, .pool_size = p->file_size };
	struct rv_pool_alloc a;
	struct rv_pool_lease *l;
	int fd, ret;
	if (cmd == RV_POOL_IOC_QUERY) {
		memcpy(q.pool_id, p->pool_id, sizeof(q.pool_id));
		return copy_to_user((void __user *)arg, &q, sizeof(q)) ? -EFAULT : 0;
	}
	if (cmd != RV_POOL_IOC_ALLOC) return -ENOTTY;
	if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;
	if (!a.bytes || a.bytes > p->ram_size || a.bytes % RV_POOL_ALIGN ||
	    !PAGE_ALIGNED(a.bytes)) return -EINVAL;
	mutex_lock(&p->lock);
	if (p->dying) { mutex_unlock(&p->lock); return -ENODEV; }
	l = kzalloc(sizeof(*l), GFP_KERNEL);
	if (!l) { mutex_unlock(&p->lock); return -ENOMEM; }
	l->pool = p;
	kref_get(&p->refs); /* lease lifetime extends through anon fd and VMAs */
	ret = rv_alloc_pages(p, a.bytes, l);
	if (ret) { kref_put(&p->refs, rv_pool_release); kfree(l); mutex_unlock(&p->lock); return ret; }
	if (l->offset < RV_POOL_FILE_HEADER || l->offset > p->file_size ||
	    l->bytes > p->file_size - l->offset) {
		rv_lease_free(l); mutex_unlock(&p->lock); return -ERANGE;
	}
	a.map_fd = -1; a.reserved = 0; a.offset = l->offset; a.length = l->bytes;
	/* Reserve the descriptor but install it only after userspace copy succeeds. */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { rv_lease_free(l); mutex_unlock(&p->lock); return fd; }
	{
		struct file *lease_file = anon_inode_getfile("rv-guest-pool-lease", &rv_lease_fops, l, O_RDWR);
		if (IS_ERR(lease_file)) {
			put_unused_fd(fd); rv_lease_free(l); mutex_unlock(&p->lock);
			return PTR_ERR(lease_file);
		}
		a.map_fd = fd;
		if (copy_to_user((void __user *)arg, &a, sizeof(a))) {
			put_unused_fd(fd); fput(lease_file); mutex_unlock(&p->lock); return -EFAULT;
		}
		fd_install(fd, lease_file);
	}
	mutex_unlock(&p->lock);
	return 0;
}
static int rv_pool_release_file(struct inode *ino, struct file *f)
{
	struct rv_pool_dev *p = f->private_data;
	kref_put(&p->refs, rv_pool_release);
	return 0;
}
static const struct file_operations rv_pool_fops = {
	.owner = THIS_MODULE, .open = rv_pool_open, .release = rv_pool_release_file,
	.unlocked_ioctl = rv_pool_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};
static int rv_pool_probe(struct platform_device *pdev)
{
	const unsigned long pages = RV_POOL_CLAIM_BYTES >> PAGE_SHIFT;
	const unsigned int units = RV_POOL_CLAIM_BYTES / RV_POOL_ALIGN;
	struct rv_pool_dev *p;
	struct resource r;
	unsigned int chunk, available = 0;
	u32 nid;
	u64 file_offset;
	int ret;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) return -ENOMEM;
	ret = of_address_to_resource(pdev->dev.of_node, 0, &r);
	if (ret) goto fail;
	ret = of_property_read_u64(pdev->dev.of_node, "reverse-venus,file-offset", &file_offset);
	if (ret) goto fail;
	ret = of_property_read_u32(pdev->dev.of_node, "numa-node-id", &nid);
	if (ret) goto fail;
	if (nid >= MAX_NUMNODES || !node_online(nid) || !IS_ALIGNED(r.start, RV_POOL_CLAIM_BYTES) ||
	    !IS_ALIGNED(resource_size(&r), RV_POOL_CLAIM_BYTES) || !resource_size(&r) ||
	    resource_size(&r) > RV_POOL_MAX_BYTES || file_offset < RV_POOL_FILE_HEADER ||
	    !IS_ALIGNED(file_offset, RV_POOL_ALIGN) ||
	    resource_size(&r) > U64_MAX - file_offset) { ret = -EINVAL; goto fail; }
	ret = of_property_read_u8_array(pdev->dev.of_node, "reverse-venus,pool-id",
					p->pool_id, sizeof(p->pool_id));
	if (ret) goto fail;
	p->ram_start = r.start; p->ram_size = resource_size(&r);
	p->file_offset = file_offset; p->file_size = file_offset + p->ram_size;
	p->dev = &pdev->dev;
	get_device(p->dev);
	kref_init(&p->refs);
	mutex_init(&p->lock);
	mutex_init(&p->alloc_lock);
	p->units = p->ram_size / RV_POOL_ALIGN;
	p->chunks = p->ram_size / RV_POOL_CLAIM_BYTES;
	p->first_pfn = PHYS_PFN(p->ram_start);
	p->allocated = bitmap_zalloc(p->units, GFP_KERNEL);
	p->claimed = bitmap_zalloc(p->chunks, GFP_KERNEL);
	if (!p->allocated || !p->claimed) {
		ret = -ENOMEM;
		goto fail_release;
	}
	if (!pfn_valid(p->first_pfn) || !pfn_valid(PHYS_PFN(r.end)) ||
	    page_to_nid(pfn_to_page(p->first_pfn)) != nid ||
	    page_to_nid(pfn_to_page(PHYS_PFN(r.end))) != nid) {
		ret = -EINVAL;
		goto fail_release;
	}
	/* Leave chunks already occupied by unmovable boot allocations unavailable. */
	bitmap_fill(p->allocated, p->units);
	for (chunk = 0; chunk < p->chunks; chunk++) {
		unsigned long first = p->first_pfn + chunk * pages;

		ret = alloc_contig_range(first, first + pages, 0, GFP_KERNEL);
		if (ret)
			continue;
		__set_bit(chunk, p->claimed);
		bitmap_clear(p->allocated, chunk * units, units);
		available++;
	}
	if (!available) {
		ret = -ENOMEM;
		goto fail_release;
	}
	p->misc.minor = MISC_DYNAMIC_MINOR; p->misc.name = "rv-guest-pool";
	p->misc.fops = &rv_pool_fops; p->misc.parent = &pdev->dev;
	ret = misc_register(&p->misc);
	if (ret)
		goto fail_release;
	platform_set_drvdata(pdev, p);
	dev_info(&pdev->dev, "claimed %lu MiB for shared Vulkan allocations\n",
		 available * (RV_POOL_CLAIM_BYTES >> 20));
	return 0;
fail_release:
	kref_put(&p->refs, rv_pool_release);
	return dev_err_probe(&pdev->dev, ret, "could not claim guest pool\n");
fail:
	kfree(p);
	return dev_err_probe(&pdev->dev, ret, "invalid guest pool DT properties\n");
}
static void rv_pool_remove(struct platform_device *pdev)
{
	struct rv_pool_dev *p = platform_get_drvdata(pdev);
	misc_deregister(&p->misc);
	mutex_lock(&p->lock); p->dying = true; mutex_unlock(&p->lock);
	kref_put(&p->refs, rv_pool_release);
}
static const struct of_device_id rv_pool_match[] = {
	{ .compatible = "reverse-venus,guest-page-pool" }, {}
};
MODULE_DEVICE_TABLE(of, rv_pool_match);
static struct platform_driver rv_pool_driver = {
	.probe = rv_pool_probe, .remove = rv_pool_remove,
	.driver = { .name = "rv-guest-pool", .of_match_table = rv_pool_match,
		   .suppress_bind_attrs = true },
};
module_platform_driver(rv_pool_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Leased exporter for QEMU file-backed NUMA RAM");

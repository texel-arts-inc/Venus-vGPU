/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef RV_GUEST_POOL_UAPI_H
#define RV_GUEST_POOL_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>
#define RV_POOL_IOC_MAGIC 'R'
#define RV_POOL_ABI_VERSION 1
#define RV_POOL_ALIGN (16U * 1024U)
#define RV_POOL_ID_BYTES 16
struct rv_pool_query {
    __u32 version;          /* output: RV_POOL_ABI_VERSION */
    __u32 alignment;        /* output: RV_POOL_ALIGN */
    __u64 pool_size;        /* output: whole backing-file size including header */
    __u8 pool_id[RV_POOL_ID_BYTES]; /* output: configured identity UUID bytes */
};
struct rv_pool_alloc {
    __u64 bytes;            /* input: positive multiple of alignment */
    __s32 map_fd;           /* output: mmap lease fd, close when done */
    __u32 reserved;
    __u64 offset;           /* output: absolute backing-file offset */
    __u64 length;           /* output: exact allocation length */
};
#define RV_POOL_IOC_QUERY _IOR(RV_POOL_IOC_MAGIC, 0x00, struct rv_pool_query)
#define RV_POOL_IOC_ALLOC _IOWR(RV_POOL_IOC_MAGIC, 0x01, struct rv_pool_alloc)
#endif

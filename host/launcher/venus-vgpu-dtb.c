#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libfdt.h"

static int pool_memory(void *tree, uint64_t *start, uint64_t *size)
{
    int node;

    fdt_for_each_subnode(node, tree, 0) {
        int len;
        const char *name = fdt_get_name(tree, node, NULL);
        const fdt32_t *numa = fdt_getprop(tree, node, "numa-node-id", &len);

        if (!name || strncmp(name, "memory@", 7) || !numa || len != 4 ||
            fdt32_to_cpu(*numa) != 1) {
            continue;
        }
        const fdt64_t *range = fdt_getprop(tree, node, "reg", &len);
        if (!range || len != 16) {
            return -1;
        }
        *start = fdt64_ld(range);
        *size = fdt64_ld(range + 1);
        return 0;
    }
    return -1;
}

/* The first 4 MiB remains reserved, including the pool identity. */
int venus_vgpu_add_pool(void *tree, size_t capacity, const uint8_t identity[16])
{
    const uint64_t header = UINT64_C(4) << 20;
    uint64_t start;
    uint64_t size;
    char name[48];

    if (capacity > INT_MAX || fdt_check_full(tree, capacity) ||
        fdt_open_into(tree, tree, (int)capacity) ||
        pool_memory(tree, &start, &size) ||
        size != (UINT64_C(1536) << 20) || start % header) {
        return -1;
    }
    int reserved = fdt_path_offset(tree, "/reserved-memory");
    if (reserved == -FDT_ERR_NOTFOUND) {
        reserved = fdt_add_subnode(tree, 0, "reserved-memory");
        if (reserved < 0 || fdt_setprop_u32(tree, reserved, "#address-cells", 2) ||
            fdt_setprop_u32(tree, reserved, "#size-cells", 2) ||
            fdt_setprop(tree, reserved, "ranges", NULL, 0)) {
            return -1;
        }
    }
    snprintf(name, sizeof(name), "venus-identity@%llx", (unsigned long long)start);
    int node = fdt_add_subnode(tree, reserved, name);
    fdt64_t range[2] = { cpu_to_fdt64(start), cpu_to_fdt64(header) };

    if (node < 0 || fdt_setprop(tree, node, "reg", range, sizeof(range)) ||
        fdt_setprop(tree, node, "no-map", NULL, 0)) {
        return -1;
    }
    snprintf(name, sizeof(name), "venus-gpu-pool@%llx",
             (unsigned long long)(start + header));
    node = fdt_add_subnode(tree, 0, name);
    range[0] = cpu_to_fdt64(start + header);
    range[1] = cpu_to_fdt64(size - header);
    if (node < 0 ||
        fdt_setprop_string(tree, node, "compatible", "reverse-venus,guest-page-pool") ||
        fdt_setprop(tree, node, "reg", range, sizeof(range)) ||
        fdt_setprop_u32(tree, node, "numa-node-id", 1) ||
        fdt_setprop_u64(tree, node, "reverse-venus,file-offset", header) ||
        fdt_setprop(tree, node, "reverse-venus,pool-id", identity, 16)) {
        return -1;
    }
    return fdt_pack(tree);
}

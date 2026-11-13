#ifndef RV_CONTROL_H
#define RV_CONTROL_H
#include <stddef.h>
#include <stdint.h>

/* Provisional guest-local control schema. It is not the published wire ABI. */
#define RV_CTL_MAX_LEASES 4096u
#define RV_CTL_MAX_QUEUES 64u
#define RV_CTL_MAX_SYNCS 1024u
#define RV_CTL_MAX_WAIT_CONDITIONS 8u
#define RV_CTL_CAP_WORDS 40u /* sizeof(struct virgl_renderer_capset_venus) / 4 */
enum rv_ctl_status { RV_CTL_OK=0, RV_CTL_INVALID=1, RV_CTL_STALE_SESSION=2,
 RV_CTL_EXISTS=3, RV_CTL_NOT_FOUND=4, RV_CTL_BACKEND_ERROR=5,
 RV_CTL_DEVICE_LOST=6, RV_CTL_TIMEOUT=7, RV_CTL_OUT_OF_SPACE=8 };
enum rv_lease_class { RV_LEASE_REPLY=1, RV_LEASE_COMMAND=2, RV_LEASE_APPLICATION=3 };
enum rv_ctl_opcode { RV_CTL_QUERY_CAPS=1, RV_CTL_REGISTER_LEASE=2,
 RV_CTL_RELEASE_LEASE=3, RV_CTL_SUBMIT_STREAM=4, RV_CTL_GET_SESSION=5,
 RV_CTL_CREATE_SYNC=6, RV_CTL_DESTROY_SYNC=7, RV_CTL_READ_SYNC=8,
 RV_CTL_WRITE_SYNC=9, RV_CTL_QUERY_VRAM=10, RV_CTL_ALLOC_VRAM=11,
 RV_CTL_RELEASE_VRAM=12 };
struct rv_lease_desc { uint64_t generation, resource_id, offset, length; uint32_t resource_class, reserved; };
struct rv_submit_desc { uint64_t generation, lease_id, offset, length; };
struct rv_sync_desc { uint64_t generation; uint32_t sync_id, reserved; uint64_t initial_value; };
struct rv_ctl_request { uint32_t opcode, reserved; union { struct rv_lease_desc lease; struct rv_submit_desc submit; struct rv_sync_desc sync; } u; };
/* Fixed-width wire reply. `caps` is the complete upstream Venus capset when
 * opcode is RV_CTL_QUERY_CAPS.  resource_id/resource_size identify the
 * imported renderer resource for REGISTER_LEASE. */
struct rv_ctl_reply {
 uint32_t status, cap_count;
 uint64_t ticket, resource_id, resource_size, generation;
 uint32_t caps[RV_CTL_CAP_WORDS];
};
struct rv_fence_condition { uint32_t queue; uint64_t value; };
struct rv_stream_sync { uint32_t sync_id, reserved; uint64_t value; };
struct rv_stream_desc {
 uint64_t generation, lease_id, offset, length;
 uint32_t ring_idx, sync_count;
 uint64_t reserved;
 struct rv_stream_sync syncs[RV_CTL_MAX_WAIT_CONDITIONS];
};
struct rv_gpu_wait { uint32_t wait_any, count; uint32_t timeout_ms, reserved; struct rv_fence_condition conditions[RV_CTL_MAX_WAIT_CONDITIONS]; };
_Static_assert(sizeof(struct rv_stream_desc) == 176, "stream ABI");
struct rv_control;
struct rv_backend {
 void *userdata;
 int (*query_caps)(void *, uint32_t caps[RV_CTL_CAP_WORDS], uint32_t *count);
 int (*lease_import)(void *, const struct rv_lease_desc *);
 int (*lease_release)(void *, const struct rv_lease_desc *);
 /* Enqueue only. The renderer worker later calls rv_control_complete_cpu. */
 int (*submit_stream)(void *, uint64_t ticket, uint64_t lease_id, uint64_t offset, uint64_t length);
};
/* alignment is max(host_page_size, guest_page_size), a power of two. */
struct rv_control *rv_control_create(uint64_t session_generation, uint64_t alignment, const struct rv_backend *);
void rv_control_destroy(struct rv_control *);
enum rv_ctl_status rv_control_dispatch(struct rv_control *, const struct rv_ctl_request *, struct rv_ctl_reply *);
enum rv_ctl_status rv_control_complete_cpu(struct rv_control *, uint64_t ticket, enum rv_ctl_status result);
enum rv_ctl_status rv_control_wait_cpu(struct rv_control *, uint64_t ticket, uint32_t timeout_ms);
/* Renderer reports completed GPU timeline counters; values must be monotonic. */
enum rv_ctl_status rv_control_signal_gpu(struct rv_control *, uint32_t queue, uint64_t value);
enum rv_ctl_status rv_control_wait_gpu(struct rv_control *, const struct rv_fence_condition *, size_t count, int wait_all, uint32_t timeout_ms);
void rv_control_renderer_died(struct rv_control *);
uint64_t rv_control_generation(const struct rv_control *);
#endif

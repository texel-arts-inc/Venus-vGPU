#define _POSIX_C_SOURCE 200809L
#include "rv_control.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICKET_WINDOW 64u
#define RV_VRAM_ID_FIRST UINT32_C(0x40000000)
#define RV_VRAM_ID_END   UINT32_C(0x80000000)
struct lease_entry { int used; struct rv_lease_desc d; };
struct ticket_entry { uint64_t ticket; int done; enum rv_ctl_status result; };
struct sync_entry { uint32_t id; int used; uint64_t value; };
struct rv_control { uint64_t generation, alignment, submitted, cpu_done; int dead; struct rv_backend backend; struct lease_entry leases[RV_CTL_MAX_LEASES]; struct ticket_entry tickets[TICKET_WINDOW]; struct sync_entry gpu[1024]; pthread_mutex_t mutex; pthread_cond_t changed; };
static int add_ok(uint64_t a,uint64_t b){return b<=UINT64_MAX-a;}
static struct lease_entry *find_lease(struct rv_control*c,uint64_t id){for(size_t i=0;i<RV_CTL_MAX_LEASES;i++)if(c->leases[i].used&&c->leases[i].d.resource_id==id)return &c->leases[i];return NULL;}
static struct sync_entry *find_sync(struct rv_control*c,uint32_t id){for(size_t i=0;i<1024;i++)if(c->gpu[i].used&&c->gpu[i].id==id)return &c->gpu[i];return NULL;}
static void deadline_after(struct timespec*t,uint32_t ms){clock_gettime(CLOCK_REALTIME,t);t->tv_sec+=ms/1000;t->tv_nsec+=(long)(ms%1000)*1000000L;if(t->tv_nsec>=1000000000L){t->tv_sec++;t->tv_nsec-=1000000000L;}}
struct rv_control *rv_control_create(uint64_t generation,uint64_t alignment,const struct rv_backend*b){if(!generation||!alignment||(alignment&(alignment-1))||!b)return NULL;struct rv_control*c=calloc(1,sizeof(*c));if(!c)return NULL;c->generation=generation;c->alignment=alignment;c->backend=*b;pthread_mutex_init(&c->mutex,NULL);pthread_cond_init(&c->changed,NULL);return c;}
void rv_control_destroy(struct rv_control*c){if(!c)return;pthread_cond_destroy(&c->changed);pthread_mutex_destroy(&c->mutex);free(c);}
uint64_t rv_control_generation(const struct rv_control*c){return c?c->generation:0;}
void rv_control_renderer_died(struct rv_control*c){if(!c)return;pthread_mutex_lock(&c->mutex);c->dead=1;pthread_cond_broadcast(&c->changed);pthread_mutex_unlock(&c->mutex);}
enum rv_ctl_status rv_control_dispatch(struct rv_control *c, const struct rv_ctl_request *q,
                                       struct rv_ctl_reply *r)
{
    if (!c || !q || !r) return RV_CTL_INVALID;
    memset(r, 0, sizeof(*r));
    pthread_mutex_lock(&c->mutex);
    if (c->dead) { r->status = RV_CTL_DEVICE_LOST; goto out; }
    switch (q->opcode) {
    case RV_CTL_GET_SESSION:
        r->generation = c->generation;
        break;
    case RV_CTL_QUERY_CAPS: {
        if (!c->backend.query_caps) { r->status = RV_CTL_BACKEND_ERROR; break; }
        pthread_mutex_unlock(&c->mutex);
        int error = c->backend.query_caps(c->backend.userdata, r->caps, &r->cap_count);
        pthread_mutex_lock(&c->mutex);
        if (c->dead) r->status = RV_CTL_DEVICE_LOST;
        else if (error || r->cap_count != RV_CTL_CAP_WORDS) r->status = RV_CTL_BACKEND_ERROR;
        break;
    }
    case RV_CTL_REGISTER_LEASE: {
        const struct rv_lease_desc *d = &q->u.lease;
        if (d->generation != c->generation) { r->status = RV_CTL_STALE_SESSION; break; }
        if (!d->resource_id || d->resource_id == 1 || d->resource_id == 2 ||
            d->resource_id > UINT32_MAX || !d->length || !add_ok(d->offset, d->length) ||
            (d->resource_id >= RV_VRAM_ID_FIRST && d->resource_id < RV_VRAM_ID_END) ||
            (d->offset & (c->alignment - 1)) || (d->length & (c->alignment - 1)) ||
            d->resource_class < RV_LEASE_REPLY || d->resource_class > RV_LEASE_APPLICATION) {
            r->status = RV_CTL_INVALID; break;
        }
        if (find_lease(c, d->resource_id)) { r->status = RV_CTL_EXISTS; break; }
        struct lease_entry *slot = NULL;
        for (size_t i = 0; i < RV_CTL_MAX_LEASES; i++) {
            if (!c->leases[i].used) { if (!slot) slot = &c->leases[i]; continue; }
            uint64_t x = c->leases[i].d.offset, y = x + c->leases[i].d.length;
            if (d->offset < y && x < d->offset + d->length) { r->status = RV_CTL_INVALID; goto out; }
        }
        if (!slot || !c->backend.lease_import) { r->status = slot ? RV_CTL_BACKEND_ERROR : RV_CTL_OUT_OF_SPACE; break; }
        pthread_mutex_unlock(&c->mutex);
        int error = c->backend.lease_import(c->backend.userdata, d);
        pthread_mutex_lock(&c->mutex);
        if (c->dead) r->status = RV_CTL_DEVICE_LOST;
        else if (error) r->status = RV_CTL_BACKEND_ERROR;
        else { slot->d = *d; slot->used = 1; r->resource_id = d->resource_id; r->resource_size = d->length; }
        break;
    }
    case RV_CTL_RELEASE_LEASE: {
        const struct rv_lease_desc *d = &q->u.lease;
        if (d->generation != c->generation) { r->status = RV_CTL_STALE_SESSION; break; }
        struct lease_entry *e = find_lease(c, d->resource_id);
        if (!e) { r->status = RV_CTL_NOT_FOUND; break; }
        if (e->d.offset != d->offset || e->d.length != d->length || e->d.resource_class != d->resource_class) { r->status = RV_CTL_INVALID; break; }
        if (!c->backend.lease_release) { r->status = RV_CTL_BACKEND_ERROR; break; }
        pthread_mutex_unlock(&c->mutex);
        int error = c->backend.lease_release(c->backend.userdata, &e->d);
        pthread_mutex_lock(&c->mutex);
        if (c->dead) r->status = RV_CTL_DEVICE_LOST;
        else if (error) r->status = RV_CTL_BACKEND_ERROR;
        else memset(e, 0, sizeof(*e));
        break;
    }
    case RV_CTL_SUBMIT_STREAM: {
        const struct rv_submit_desc *d = &q->u.submit;
        if (d->generation != c->generation) { r->status = RV_CTL_STALE_SESSION; break; }
        struct lease_entry *e = find_lease(c, d->lease_id);
        if (!e) { r->status = RV_CTL_NOT_FOUND; break; }
        if (e->d.resource_class != RV_LEASE_COMMAND || !d->length || !add_ok(d->offset, d->length) || d->offset > e->d.length || d->length > e->d.length - d->offset) { r->status = RV_CTL_INVALID; break; }
        if (c->submitted - c->cpu_done >= TICKET_WINDOW) { r->status = RV_CTL_OUT_OF_SPACE; break; }
        uint64_t ticket = c->submitted + 1;
        if (!c->backend.submit_stream) { r->status = RV_CTL_BACKEND_ERROR; break; }
        pthread_mutex_unlock(&c->mutex);
        int error = c->backend.submit_stream(c->backend.userdata, ticket, d->lease_id, d->offset, d->length);
        pthread_mutex_lock(&c->mutex);
        if (c->dead) r->status = RV_CTL_DEVICE_LOST;
        else if (error) r->status = RV_CTL_BACKEND_ERROR;
        else { c->submitted = ticket; struct ticket_entry *t = &c->tickets[ticket % TICKET_WINDOW]; *t = (struct ticket_entry){ .ticket = ticket }; r->ticket = ticket; }
        break;
    }
    case RV_CTL_CREATE_SYNC:
    case RV_CTL_DESTROY_SYNC:
    case RV_CTL_READ_SYNC:
    case RV_CTL_WRITE_SYNC: {
        const struct rv_sync_desc *d = &q->u.sync;
        if (d->generation != c->generation || d->reserved) { r->status = RV_CTL_STALE_SESSION; break; }
        struct sync_entry *e = find_sync(c, d->sync_id);
        if (q->opcode == RV_CTL_CREATE_SYNC) {
            if (e) { r->status = RV_CTL_EXISTS; break; }
            for (size_t i = 0; i < 1024; i++) if (!c->gpu[i].used) { e = &c->gpu[i]; break; }
            if (!e) { r->status = RV_CTL_OUT_OF_SPACE; break; }
            *e = (struct sync_entry){ .id = d->sync_id, .used = 1, .value = d->initial_value };
        } else if (!e) { r->status = RV_CTL_NOT_FOUND; break; }
        else if (q->opcode == RV_CTL_DESTROY_SYNC) memset(e, 0, sizeof(*e));
        else if (q->opcode == RV_CTL_READ_SYNC) r->ticket = e->value;
        else if (d->initial_value < e->value) r->status = RV_CTL_INVALID;
        else { e->value = d->initial_value; r->ticket = e->value; pthread_cond_broadcast(&c->changed); }
        break;
    }
    default: r->status = RV_CTL_INVALID; break;
    }
out:
    pthread_mutex_unlock(&c->mutex);
    return r->status;
}
enum rv_ctl_status rv_control_complete_cpu(struct rv_control*c,uint64_t ticket,enum rv_ctl_status result){if(!c||!ticket||result<RV_CTL_OK||result>RV_CTL_OUT_OF_SPACE)return RV_CTL_INVALID;pthread_mutex_lock(&c->mutex);enum rv_ctl_status s=RV_CTL_OK;if(c->dead)s=RV_CTL_DEVICE_LOST;else if(ticket>c->submitted||ticket<=c->cpu_done)s=RV_CTL_INVALID;else{struct ticket_entry*t=&c->tickets[ticket%TICKET_WINDOW];if(t->ticket!=ticket||t->done)s=RV_CTL_INVALID;else{t->done=1;t->result=result;while(c->cpu_done<c->submitted){struct ticket_entry*n=&c->tickets[(c->cpu_done+1)%TICKET_WINDOW];if(n->ticket!=c->cpu_done+1||!n->done)break;c->cpu_done++;}pthread_cond_broadcast(&c->changed);}}pthread_mutex_unlock(&c->mutex);return s;}
enum rv_ctl_status rv_control_wait_cpu(struct rv_control*c,uint64_t ticket,uint32_t ms){if(!c||!ticket)return RV_CTL_INVALID;struct timespec until;deadline_after(&until,ms);pthread_mutex_lock(&c->mutex);enum rv_ctl_status s=RV_CTL_OK;while(c->cpu_done<ticket&&!c->dead){if(ticket>c->submitted){s=RV_CTL_INVALID;goto out;}int e=pthread_cond_timedwait(&c->changed,&c->mutex,&until);if(e){s=RV_CTL_TIMEOUT;goto out;}}if(c->dead){s=RV_CTL_DEVICE_LOST;goto out;}struct ticket_entry*t=&c->tickets[ticket%TICKET_WINDOW];if(t->ticket!=ticket)s=RV_CTL_NOT_FOUND;else if(t->result!=RV_CTL_OK)s=t->result;out:pthread_mutex_unlock(&c->mutex);return s;}
enum rv_ctl_status rv_control_signal_gpu(struct rv_control*c,uint32_t id,uint64_t value){if(!c)return RV_CTL_INVALID;pthread_mutex_lock(&c->mutex);enum rv_ctl_status s=RV_CTL_OK;if(c->dead)s=RV_CTL_DEVICE_LOST;else{struct sync_entry*e=find_sync(c,id);if(!e){for(size_t i=0;i<1024;i++)if(!c->gpu[i].used){e=&c->gpu[i];*e=(struct sync_entry){.id=id,.used=1};break;}}if(!e)s=RV_CTL_OUT_OF_SPACE;else if(value<e->value)s=RV_CTL_INVALID;else{e->value=value;pthread_cond_broadcast(&c->changed);}}pthread_mutex_unlock(&c->mutex);return s;}
enum rv_ctl_status rv_control_wait_gpu(struct rv_control*c,const struct rv_fence_condition*f,size_t n,int all,uint32_t ms){if(!c||!f||!n||n>RV_CTL_MAX_WAIT_CONDITIONS)return RV_CTL_INVALID;for(size_t i=0;i<n;i++)for(size_t j=0;j<i;j++)if(f[i].queue==f[j].queue)return RV_CTL_INVALID;struct timespec until;deadline_after(&until,ms);pthread_mutex_lock(&c->mutex);enum rv_ctl_status s=RV_CTL_OK;for(;;){if(c->dead){s=RV_CTL_DEVICE_LOST;break;}size_t met=0;for(size_t i=0;i<n;i++){struct sync_entry*e=find_sync(c,f[i].queue);if((e?e->value:0)>=f[i].value)met++;}if((all&&met==n)||(!all&&met)){s=RV_CTL_OK;break;}int e=pthread_cond_timedwait(&c->changed,&c->mutex,&until);if(e){s=RV_CTL_TIMEOUT;break;}}pthread_mutex_unlock(&c->mutex);return s;}

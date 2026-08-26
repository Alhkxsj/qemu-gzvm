#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "system/accel-ops.h"
#include "system/cpus.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"
#include "qapi/error.h"

bool gzvm_allowed;

/*
 * Whether to offer VIRTIO_RING_F_EVENT_IDX to guests.  Off under gzvm; set
 * GZVM_EVENT_IDX=on to offer it anyway, which reproduces the hang below.
 *
 * Both of virtio's split-ring handshakes have two forms, and which pair gets
 * negotiated is decided by this one feature bit:
 *
 *   guest -> host kick    EVENT_IDX: vring_need_event(used->avail_event, ...),
 *                                    an index *we* wrote into guest memory
 *                         flags:     !(used->flags & VRING_USED_F_NO_NOTIFY)
 *
 *   host -> guest notify  EVENT_IDX: vring_need_event(avail->used_event, ...),
 *                                    an index the *guest* wrote
 *                         flags:     !(avail->flags & VRING_AVAIL_F_NO_INTERRUPT)
 *
 * The flag forms carry their own recovery.  Whoever re-enables re-examines the
 * ring afterwards -- virtqueue_enable_cb() in the guest is literally
 * enable_cb_prepare() followed by !virtqueue_poll(), and the device side does the
 * same when it calls virtio_queue_set_notification(vq, 1) and then re-checks
 * virtio_queue_empty().  Read a stale flag and you lose a notification, then the
 * re-poll finds the work anyway.
 *
 * The index forms have no such step.  Each side trusts that it is reading an
 * index the peer wrote recently enough, with nothing to fall back on, and both
 * indices live in memory the hypervisor maps rather than we do.  Lose a kick and
 * QEMU sits in virtqueue_pop() with an empty avail ring waiting for a doorbell
 * that will never ring; lose a notification and completed requests sit in the
 * used ring while a level-triggered INTx line stays where it is.  Neither
 * recovers, and with no ITS in this tree every virtio-pci device is on INTx.
 *
 * The evidence for taking the bit away rather than working around it from the
 * host side: a temporary patch that forced a notification on every single push --
 * a strict superset of the notifications the flag form produces -- booted -smp 8
 * only occasionally, while event_idx=off on the command line booted every time.
 * No host-side fix can do better, because the kick direction is the guest reading
 * a stale index and deciding not to ring the doorbell; there is no point in QEMU
 * where that decision can be observed, let alone corrected.
 *
 * Nor is this the guest's barriers being too weak.  Offering
 * VIRTIO_F_ORDER_PLATFORM sets vq->weak_barriers = false in the guest, which on
 * arm64 turns virtio_mb() from dmb(ish) into dsb(sy) and virtio_rmb()/wmb() from
 * dmb(ishld)/dmb(ishst) into dmb(oshld)/dmb(oshst) -- Inner Shareable becomes
 * Outer Shareable or full system.  With that bit and EVENT_IDX both on, -smp 8
 * still hung.  So the guest cannot reach us with any barrier it has, which points
 * at GZ's stage-2 shareability or cacheability being wrong outright rather than
 * merely under-ordered.  That is not reachable from here, and not from the host
 * driver either: drivers/virt/geniezone only hands GZ an address range and never
 * touches guest RAM attributes.  gz.img is a blob.  Withdrawing the bit is the
 * fix.
 *
 * More vCPUs mean more concurrent ring traffic and more chances to lose one,
 * which is why -smp 2 was reliable, 3 and 4 marginal, and 5 and up never
 * finished booting.
 */
bool gzvm_event_idx_allowed(void)
{
    static int allowed = -1;

    if (allowed < 0) {
        const char *env = getenv("GZVM_EVENT_IDX");

        allowed = env && (!strcmp(env, "on") || !strcmp(env, "1"));
        if (allowed) {
            warn_report("gzvm: offering VIRTIO_RING_F_EVENT_IDX; expect hangs "
                        "at -smp 4 and above");
        }
    }

    return allowed != 0;
}

static int gzvm_init(MachineState *ms)
{
    GZVMState *s = GZVM_STATE(current_accel());

    gzvm_ioctl_set_state(s);

    return gzvm_create_vm();
}

static void gzvm_accel_instance_finalize(Object *obj)
{
    GZVMState *s = GZVM_STATE(obj);
    if (s->fd >= 0) {
        close(s->fd);
    }
    if (s->vmfd >= 0) {
        close(s->vmfd);
    }
    g_free(s->slots);
    g_free(s->sorted_ids);
}

static void gzvm_accel_instance_init(Object *obj)
{
    GZVMState *s = GZVM_STATE(obj);
    s->fd = -1;
    s->vmfd = -1;
    s->slots = NULL;
    s->sorted_ids = NULL;
}

static void gzvm_setup_post(MachineState *ms, AccelState *accel)
{
    int r = gzvm_start_vm();
    if (r < 0) {
        warn_report("gzvm: VM start failed");
    }
}

static void gzvm_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "GZVM";
    ac->init_machine = gzvm_init;
    ac->allowed = &gzvm_allowed;
    ac->setup_post = gzvm_setup_post;
}

static const TypeInfo gzvm_accel_type = {
    .name = TYPE_GZVM_ACCEL,
    .parent = TYPE_ACCEL,
    .instance_init = gzvm_accel_instance_init,
    .instance_finalize = gzvm_accel_instance_finalize,
    .class_init = gzvm_accel_class_init,
    .instance_size = sizeof(GZVMState),
};

static void gzvm_type_init(void)
{
    type_register_static(&gzvm_accel_type);
}

type_init(gzvm_type_init);

static void gzvm_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/GZVM",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, gzvm_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void gzvm_kick_vcpu_thread(CPUState *cpu)
{
    cpus_kick_thread(cpu);
}

static bool gzvm_vcpu_thread_is_idle(CPUState *cpu)
{
    return false;
}

static void gzvm_accel_ops_class_init(ObjectClass *oc, void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);
    ops->create_vcpu_thread = gzvm_start_vcpu_thread;
    ops->kick_vcpu_thread = gzvm_kick_vcpu_thread;
    ops->cpu_thread_is_idle = gzvm_vcpu_thread_is_idle;
    ops->synchronize_post_reset = gzvm_cpu_synchronize_post_reset;
    ops->synchronize_post_init = gzvm_cpu_synchronize_post_init;
}

static const TypeInfo gzvm_accel_ops_type = {
    .name = ACCEL_OPS_NAME("gzvm"),
    .parent = TYPE_ACCEL_OPS,
    .class_init = gzvm_accel_ops_class_init,
    .abstract = true,
};

static void gzvm_accel_ops_register_types(void)
{
    type_register_static(&gzvm_accel_ops_type);
}

type_init(gzvm_accel_ops_register_types);

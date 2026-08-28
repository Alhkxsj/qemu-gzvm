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
 * The index forms have those same re-poll steps -- virtqueue_enable_cb() works
 * under EVENT_IDX too, and virtblk_done() loops on it -- so one lost index update
 * is not by itself terminal.  What they lack is a conservative default.  A stale
 * flag read errs towards notifying; a stale index read is a comparison against a
 * number that may be arbitrarily far behind, and vring_need_event() answers "no"
 * to most of those.  That leaves a window in which neither side has any reason to
 * look, and the pair settles into a state each of them considers steady.  Both
 * indices live in memory the hypervisor maps rather than we do, and with no ITS
 * in this tree every virtio-pci device is on INTx.
 *
 * A four-cell matrix pinned down the direction.  All four ran on one binary with
 * INTx already carried by an irqfd, so that nothing de-duplicates between
 * virtio_notify() and the hypervisor, three or four times each at -smp 8 with
 * EVENT_IDX on:
 *
 *   nothing else            hangs
 *   GZVM_VQ_REPOLL_MS=1     boots
 *   GZVM_NOTIFY_FORCE=on    hangs
 *   both                    boots, no better than the re-poll alone
 *
 * Synthesising the guest's doorbell repairs it and notifying unconditionally does
 * not, so the lost event is the guest -> host kick: the guest reads a stale
 * used->avail_event, vring_need_event() answers no, and we wait in front of an
 * avail ring we believe is empty.  Forced notification on top of the re-poll buys
 * nothing, which rules the host -> guest half out as a contributor.
 *
 * Two caveats.  The re-poll here is the two-tick form described on
 * gzvm_vq_repoll_ms(); it kicks an order of magnitude less often than the
 * one-tick form it replaced and is no less stable, so the kicks it does issue are
 * doing real work and "1 ms merely perturbs timing" no longer explains the
 * result.  But those cells are "almost always" rather than always, and one blind
 * spot is known: if avail->idx itself is stale to us then virtio_queue_empty()
 * returns true, the probe never fires, and a loss of that shape is invisible to
 * this instrument.
 *
 * None of which makes EVENT_IDX shippable.  For the guest to kick it must read
 * avail_event == new - 1, and virtio_queue_split_set_notification() already
 * writes exactly the value that produces that -- vring_avail_idx(vq), followed by
 * an smp_mb().  No other value forces a kick out of a stale read; 0xffff, for
 * one, actively suppresses it.  So the levers are withdrawing the feature or
 * running a permanent 1 ms timer per virtio device, and the timer is not worth it
 * for a feature whose whole purpose is to do less work.  (An early
 * forced-notification test that booted -smp 8 "only occasionally" is not evidence
 * either way: it ran while the level/edge defect was still present, so the extra
 * notifications ran into that bug instead.)
 *
 * Nor is this the guest's barriers being too weak.  Offering
 * VIRTIO_F_ORDER_PLATFORM sets vq->weak_barriers = false in the guest, which on
 * arm64 turns virtio_mb() from dmb(ish) into dsb(sy) and virtio_rmb()/wmb() from
 * dmb(ishld)/dmb(ishst) into dmb(oshld)/dmb(oshst) -- Inner Shareable becomes
 * Outer Shareable or full system.  With that bit and EVENT_IDX both on, -smp 8
 * still hung.  Which is what you would expect either way: a barrier orders writes
 * a CPU has already made, it does not make them propagate sooner, so no barrier
 * the guest can execute changes when our write to avail_event becomes visible to
 * it.
 *
 * Nor is it plain non-coherency, and this is the part worth handing to MediaTek.
 * virtqueue_kick_prepare_split() reads either vring_avail_event() or used->flags
 * after the same virtio_mb() -- same used ring, same page, same memory type, one
 * function, one barrier -- and the flag form is reliable: with EVENT_IDX off,
 * -smp 8 boots every time.  If our stores to that page were simply not arriving,
 * the flag form would break too.  What differs between them is not visibility but
 * how each one fails under lag.  A stale flag read only suppresses if it catches
 * the brief window where NO_NOTIFY is set, which is rare and self-correcting.  A
 * stale index read suppresses whenever the value it sees is more than one behind,
 * and avail_event advances monotonically under load, so "more than one behind" is
 * the common case rather than a rare one.  Same lag, wildly different
 * consequences.
 *
 * So the ask is narrower than "host writes to guest RAM are invisible": our stores
 * become visible to the guest with a delay long enough that a continuously
 * advancing counter is routinely read stale, while a rarely toggled flag in the
 * same page survives.  Not reachable from here, and not from the host driver
 * either: drivers/virt/geniezone only hands GZ an address range and never touches
 * guest RAM attributes.  gz.img is a blob.  Withdrawing the bit is the fix.
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

/*
 * Virtqueue re-poll interval in milliseconds, 0 to disable (the default).
 *
 * This is a diagnostic, not a fix.  It exists to split the EVENT_IDX failure by
 * direction, which the boot outcome alone cannot do:
 *
 *   guest -> host.  The guest decides whether to ring the doorbell from
 *   used->avail_event, which we write.  If it reads a stale value it suppresses
 *   the kick, and we sit in the main loop in front of an avail ring we think is
 *   empty.  A periodic re-poll synthesises the missing kick, so setting this
 *   makes the boot succeed.
 *
 *   host -> guest.  We decide whether to interrupt from avail->used_event, which
 *   the guest writes.  A re-poll on our side cannot help, because the completions
 *   are already in the used ring and it is the guest that is not looking.  The
 *   boot still hangs.
 *
 * Do not read the hit counter as a count of lost kicks.  An earlier version of
 * this comment drew a direction verdict from it and had to withdraw it: a run at
 * 1 ms logged eight virtio-blk hits while still inside UEFI, and EDK2 does not
 * negotiate EVENT_IDX at all -- VirtioBlkDxe masks the feature set down to
 * BLK_SIZE | TOPOLOGY | RO | FLUSH | VERSION_1 | IOMMU_PLATFORM -- so those hits
 * cannot have been suppression, only the timer landing between the guest's write
 * to avail->idx and our notify handler running.  The same counts, in the same
 * per-device distribution, showed up in boots that succeeded.
 *
 * Hence the two-tick rule below.  A hit now requires the queue to be non-empty on
 * two consecutive ticks with last_avail_idx unmoved in between, which means we
 * consumed nothing at all while work was sitting there.  Sub-millisecond races
 * cannot survive that, so a hit is either a real lost kick or a host-side stall of
 * a full tick.
 *
 * With that rule in place the split came out guest -> host; see
 * gzvm_event_idx_allowed() for the matrix.  Note that the interval matters much
 * less than it looks: 1 and 5 ms improve the odds, 10, 50, 100 and 500 are roughly
 * level with each other, and none of them eliminates the hang.  A single
 * repairable lost kick should make a long interval slow rather than fatal, so
 * either something upstream in the guest times out while we dawdle, or there is a
 * second loss this probe cannot see -- if avail->idx is itself stale to us,
 * virtio_queue_empty() returns true and we never fire.
 *
 * Use with GZVM_EVENT_IDX=on; with EVENT_IDX withdrawn there is nothing to
 * measure, because the flag forms already re-poll.
 */
int gzvm_vq_repoll_ms(void)
{
    static int ms = -1;

    if (ms < 0) {
        const char *env = getenv("GZVM_VQ_REPOLL_MS");

        ms = env ? atoi(env) : 0;
        if (ms < 0) {
            ms = 0;
        }
    }

    return ms;
}

/*
 * Ignore the guest's used_event when deciding whether to interrupt.  Off by
 * default; set GZVM_NOTIFY_FORCE=on.
 *
 * This is the other half of the direction split, and the half no re-poll can
 * reach.  EVENT_IDX makes both sides consult a counter the other side publishes:
 * the guest reads used->avail_event before ringing the doorbell, and we read
 * avail->used_event in virtio_split_should_notify() before raising an interrupt.
 * gzvm_vq_repoll_ms() can synthesise a doorbell, so it covers the first.  Nothing
 * covers the second: by the time we decide not to interrupt, the completions are
 * already in the used ring and it is the guest that is not looking, so there is
 * no later event to correct the decision.
 *
 * Setting this keeps EVENT_IDX advertised -- the guest's own kick suppression is
 * left completely alone -- and only stops us acting on used_event, so we notify
 * unconditionally.  That isolates the host -> guest direction.
 *
 * The answer was negative, and that is why this stays in the tree.  At -smp 8
 * with EVENT_IDX on, this alone still hangs, while GZVM_VQ_REPOLL_MS=1 alone
 * boots and the two together are no better than the re-poll alone.  So we are not
 * the side losing events; keep this knob as the negative control that makes the
 * guest -> host verdict in gzvm_event_idx_allowed() falsifiable by someone else.
 *
 * Never a candidate fix, whichever way it had come out.  It throws away every
 * interrupt EVENT_IDX was meant to elide, which is most of the feature's value,
 * and it would be a strange thing to ship next to simply withdrawing the feature.
 * Withdrawing it stays the default; see gzvm_event_idx_allowed().
 */
bool gzvm_notify_force(void)
{
    static int force = -1;

    if (force < 0) {
        const char *env = getenv("GZVM_NOTIFY_FORCE");

        force = env && (!strcmp(env, "on") || !strcmp(env, "1"));
        if (force) {
            warn_report("gzvm: ignoring used_event; every completion will "
                        "interrupt the guest");
        }
    }

    return force != 0;
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

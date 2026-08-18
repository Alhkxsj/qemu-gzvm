#include "qemu/osdep.h"
#include <signal.h>
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"

static void gzvm_ipi_signal(int sig)
{
    if (current_cpu) {
        qatomic_set(&GZVCPU(current_cpu)->run->immediate_exit, 1);
    }
}

void gzvm_cpu_kick_self(void)
{
    qatomic_set(&GZVCPU(current_cpu)->run->immediate_exit, 1);
}

void gzvm_init_cpu_signals(void)
{
    struct sigaction sigact;
    sigset_t set;

    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = gzvm_ipi_signal;
    sigact.sa_flags = SA_RESTART;
    sigaction(SIG_IPI, &sigact, NULL);

    pthread_sigmask(SIG_BLOCK, NULL, &set);
    sigdelset(&set, SIG_IPI);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
}

void gzvm_eat_signals(CPUState *cpu)
{
    struct timespec ts = { 0, 0 };
    sigset_t waitset, chkset;
    siginfo_t siginfo;

    qatomic_set(&GZVCPU(cpu)->run->immediate_exit, 0);

    sigemptyset(&waitset);
    sigaddset(&waitset, SIG_IPI);
    do {
        if (sigtimedwait(&waitset, &siginfo, &ts) < 0 &&
            errno != EAGAIN && errno != EINTR)
            break;
    } while (sigpending(&chkset) == 0 && sigismember(&chkset, SIG_IPI));
}

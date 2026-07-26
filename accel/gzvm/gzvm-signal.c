#include "qemu/osdep.h"
#include <sys/mman.h>
#include "qemu/error-report.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"

static uintptr_t gzvm_signal_page_size;

#define GZVM_SIGNAL_MAX_REGIONS 64
typedef struct {
    uintptr_t start;
    uintptr_t end;
} GZVMSignalHvaRange;

static GZVMSignalHvaRange gzvm_signal_hva_ranges[GZVM_SIGNAL_MAX_REGIONS];
static int gzvm_signal_nr_hva_ranges;

void gzvm_signal_update_regions(GZVMState *s)
{
    gzvm_signal_nr_hva_ranges = 0;
    for (int i = 0; i < (int)s->nr_active_slots &&
                gzvm_signal_nr_hva_ranges < GZVM_SIGNAL_MAX_REGIONS; i++) {
        gzvm_slot *slot = &s->slots[s->sorted_ids[i]];
        if (slot->mem) {
            int idx = gzvm_signal_nr_hva_ranges++;
            gzvm_signal_hva_ranges[idx].start = (uintptr_t)slot->mem;
            gzvm_signal_hva_ranges[idx].end = (uintptr_t)slot->mem +
                                              slot->size;
        }
    }
}

static void gzvm_sigsegv_handler(int sig, siginfo_t *si, void *ctx)
{
    if (sig == SIGBUS && si->si_addr) {
        uintptr_t page_mask = ~(gzvm_signal_page_size - 1);
        uintptr_t page_addr = (uintptr_t)si->si_addr & page_mask;
        int map_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        void *ret;
        bool in_gzvm = false;

        for (int i = 0; i < gzvm_signal_nr_hva_ranges; i++) {
            if ((uintptr_t)si->si_addr >= gzvm_signal_hva_ranges[i].start &&
                (uintptr_t)si->si_addr < gzvm_signal_hva_ranges[i].end) {
                in_gzvm = true;
                break;
            }
        }

        if (in_gzvm) {
#ifdef MAP_FIXED_NOREPLACE
            map_flags |= MAP_FIXED_NOREPLACE;
#else
            map_flags |= MAP_FIXED;
#endif
            ret = mmap((void *)page_addr, gzvm_signal_page_size,
                       PROT_READ | PROT_WRITE, map_flags, -1, 0);
            if (ret != MAP_FAILED) {
                return;
            }
#ifdef MAP_FIXED_NOREPLACE
            if (errno == EEXIST) {
                return;
            }
#endif
        }
    }

    {
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        sigaction(sig, &dfl, NULL);
    }
    raise(sig);
}

void gzvm_install_sigsegv_handler(void)
{
    sigset_t set;

    gzvm_signal_page_size = qemu_real_host_page_size();

#ifndef MAP_FIXED_NOREPLACE
    warn_report("gzvm: MAP_FIXED_NOREPLACE not available (kernel < 4.17), "
                "falling back to MAP_FIXED");
#endif

    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    sigaddset(&set, SIGSEGV);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

void gzvm_init_vcpu_sigsegv(void)
{
    struct sigaction sa;
    sigset_t set;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = gzvm_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    sigaddset(&set, SIGSEGV);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}


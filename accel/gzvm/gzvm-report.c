#include "qemu/osdep.h"
#include "system/gzvm_report.h"

static const char *gz_color_for(const char *msg)
{
    uint64_t ok, skipped, failed;

    if (sscanf(msg,
               "%*s collapse: %" SCNu64 " OK, %" SCNu64
               " skipped, %" SCNu64 " failed",
               &ok, &skipped, &failed) == 3) {
        return failed == 0 && ok > 0 ? gz_green : gz_yellow;
    }
    if (strstr(msg, "FAILED") || strstr(msg, "failed") || strstr(msg, "ERROR") ||
        strstr(msg, "Error") || strstr(msg, "CRASHED") ||
        strstr(msg, "WDT BITE") || strstr(msg, "WILL SIGBUS") ||
        strstr(msg, "invalid") || strstr(msg, "timeout") ||
        strstr(msg, "Timed out") || strstr(msg, "Dependency") ||
        strstr(msg, "dependency") || strstr(msg, "Assertion") ||
        strstr(msg, "assert") || strstr(msg, "not supported") ||
        strstr(msg, "unsupported") || strstr(msg, "ENOTTY") ||
        strstr(msg, "started before") || strstr(msg, "cannot be started again") ||
        strstr(msg, "frozen") || strstr(msg, "concurrency") ||
        strstr(msg, "CONCUR") || strstr(msg, "WARNING") ||
        strstr(msg, "Warning") || strstr(msg, "warn")) {
        return gz_red;
    }
    if (strstr(msg, " OK") || strstr(msg, "OK") || strstr(msg, "opened") ||
        strstr(msg, "created") || strstr(msg, "loaded") ||
        strstr(msg, "placed") || strstr(msg, "registered") ||
        strstr(msg, "armed") || strstr(msg, "done") || strstr(msg, "started") ||
        strstr(msg, "enabled") || strstr(msg, "kept mapped")) {
        return gz_green;
    }
    return gz_yellow;
}

static const char *gz_file_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

void gz_report_at(const char *file, const char *func, int line,
                  const char *fmt, ...)
{
    va_list ap;
    char msg[1024];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, " %s\xe2\x80\xa2%s %s %s:%s:%d\n", gz_color_for(msg),
            gz_normal, msg, gz_file_name(file), func, line);
}

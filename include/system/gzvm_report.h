#ifndef GZVM_REPORT_H
#define GZVM_REPORT_H

#define gz_red "\033[0;1;31m"
#define gz_yellow "\033[1;33m"
#define gz_green "\033[0;32m"
#define gz_normal "\033[0m"

void gz_report_at(const char *file, const char *func, int line,
                  const char *fmt, ...);
#define gz_report(...) gz_report_at(__FILE__, __func__, __LINE__, __VA_ARGS__)
#define gz_report_once(...) ({ \
    static bool print_once_; \
    !print_once_ && (print_once_ = true, gz_report(__VA_ARGS__), true); \
})

#endif

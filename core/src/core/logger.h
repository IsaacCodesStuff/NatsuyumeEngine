#pragma once

#ifdef __ANDROID__
#  include <android/log.h>
#  ifndef PMLOG
#    define PMLOG(...) do { ((void)__android_log_print(ANDROID_LOG_DEBUG, "NatsuyumePM", __VA_ARGS__)); } while (0)
#  endif
#else
#  include <cstdio>
#  include <cstdarg>

#  if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#  endif
inline void pmlog_desktop(const char *fmt, ...)
{
    std::va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[NatsuyumePM] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

#  ifndef PMLOG
#    define PMLOG(...) do { pmlog_desktop(__VA_ARGS__); } while (0)
#  endif
#endif

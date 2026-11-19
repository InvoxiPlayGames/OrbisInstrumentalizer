#pragma once

// opens /data/GoldHEN/OrbisInstrumentalizer.log, replacing the last session's log
void OILogOpen(void);

// logs a line (include the trailing newline) to the kernel log and the log file
void OILog(const char *format, ...) __attribute__((format(printf, 1, 2)));

// logs only the first `max` times this line is reached, so hot paths can't flood the log
#define OILogLimited(max, format, ...) do { \
        static int oi_log_count_ = 0; \
        if (oi_log_count_ < (max)) { \
            oi_log_count_++; \
            OILog(format, ##__VA_ARGS__); \
        } \
    } while (0)

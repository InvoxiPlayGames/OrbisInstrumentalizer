/*
    oi_log.c - OrbisInstrumentalizer
    Logging to the kernel log and to a file that can be read over FTP, since the kernel
    log only gets plugin output when GoldHEN's TTY redirect is enabled.
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <GoldHEN/Common.h>
#include <orbis/libkernel.h>

#include "oi_log.h"

#define OI_LOG_PATH "/data/GoldHEN/OrbisInstrumentalizer.log"

// FreeBSD open flags
#define OI_O_WRONLY 0x0001
#define OI_O_APPEND 0x0008
#define OI_O_CREAT  0x0200
#define OI_O_TRUNC  0x0400

static int log_fd = -1;
static uint64_t log_start = 0;

void OILogOpen(void) {
    if (log_fd >= 0)
        return;
    log_start = sceKernelGetProcessTime();
    log_fd = sceKernelOpen(OI_LOG_PATH, OI_O_WRONLY | OI_O_CREAT | OI_O_TRUNC | OI_O_APPEND, 0666);
}

void OILog(const char *format, ...) {
    char line[512];
    uint64_t ms = (sceKernelGetProcessTime() - log_start) / 1000;
    int prefix = snprintf(line, sizeof(line), "[OrbisInstrumentalizer %6lu.%03lu] ",
        (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    if (prefix < 0 || prefix >= (int)sizeof(line))
        return;
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
    va_end(args);
    if (length < 0)
        return;
    length += prefix;
    if (length >= (int)sizeof(line))
        length = sizeof(line) - 1;

    klog("%s", line);
    if (log_fd >= 0)
        sceKernelWrite(log_fd, line, length);
}

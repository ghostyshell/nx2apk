// Android counterparts of the Apple-only host units (haptics_apple.m, memstat.m, probe.c)
#include "../host/hle.h"

// probe.c (DM_PROBE breakpoints and DM_WATCH watchpoints) uses Mach thread state; not available here
void probe_install(void) {}

// libSystem's per-thread {instructions, cycles} counters (hle_nvn.c submit statistics): read as zero
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return 0; }

/* sysctl(KERN_PROC) compatibility: only the process start time is used. */
#pragma once
#include <sys/types.h>
#include <sys/time.h>

#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PID 1
#define KERN_PROC_ALL 0

struct kinfo_proc {
    struct { struct timeval p_starttime; } kp_proc;
};

int sysctl(int *name, unsigned int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);

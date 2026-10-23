/* Getsectiondata for ELF: the host registers its HLE tables in the sections
 * named swhle / swforce (see tools/patch_android_host.py). */
#pragma once
#include <stdint.h>

struct mach_header;
extern const struct mach_header dm_fake_mh_execute_header;
#define _mh_execute_header dm_fake_mh_execute_header

unsigned char *getsectiondata(const struct mach_header *mh, const char *segname,
                              const char *sectname, unsigned long *size);

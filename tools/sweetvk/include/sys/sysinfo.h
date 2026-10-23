/*
 * tools/sweetvk: iOS has no <sys/sysinfo.h> and no sysinfo(2); this shadow
 * header - found first because the cross file puts include/ on the search path
 * - gives Mesa's msm back ends the little of it they use. compat.c provides
 * sysinfo() itself, backed by hw.memsize.
 *
 * Only the two fields Mesa reads are here: turnip's tu_get_system_heap_size()
 * does `info.totalram * info.mem_unit` (gallium's freedreno_screen.c uses
 * totalram on its own), so filing totalram in bytes with mem_unit 1 keeps both
 * of them right.
 *
 * extern "C": tu_device.cc (C++) calls sysinfo(), compat.c defines it in C.
 */
#ifndef SWEET_SYS_SYSINFO_H
#define SWEET_SYS_SYSINFO_H

#if defined(__APPLE__)

struct sysinfo {
	unsigned long totalram; /* total usable RAM, in mem_unit units */
	unsigned int mem_unit;  /* size of one unit, in bytes */
};

#ifdef __cplusplus
extern "C" {
#endif

int sysinfo(struct sysinfo *info);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */

#endif /* SWEET_SYS_SYSINFO_H */

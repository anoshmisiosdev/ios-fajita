/*
 * tools/sweetvk: the msm DRM emulation (sweet_drm.c) and its own log.
 * Ported from joranv1/ios-sailfish's tools/sweetgl/sweet_drm.h.
 */
#ifndef SWEET_DRM_H
#define SWEET_DRM_H

#include <stdint.h>

/* A "DRM fd" for fd_device_new (dup of /dev/null), or -1 without SweetGPU. */
int sweet_drm_open(void);

/* The kernel's mapping of the bo behind an msm mmap offset (MAP_FAILED if none). */
void *sweet_drm_bo_map(int fd, uint64_t offset);

/* A GEM handle for an IOSurface's memory: 0 or -errno. */
int sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, uint32_t *handle, uint64_t *size);

/* printf-style one line to stderr ("sweetvk: " first), for the phone's console. */
void sweet_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The chip id the kernel reports (0 without SweetGPU); for the test's banner. */
uint32_t sweet_drm_chip_id(void);

#endif

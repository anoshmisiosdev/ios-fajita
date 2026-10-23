/*
 * The subset of libdrm's xf86drm.h that Mesa's freedreno/Turnip use. There is
 * no DRM on iOS: sweet_drm.c implements these calls in user space on top of
 * the SweetGPU IOKit user client (xnu iokit/Kernel/arm/SweetGPU.cpp),
 * answering the msm DRM ioctls itself.
 *
 * This header is passed to the Mesa build through the cross file's c_args
 * (-I tools/sweetvk/include), so every <xf86drm.h> include in Mesa resolves
 * here instead of against a libdrm that does not exist for iOS. Ported from
 * joranv1/ios-sailfish's tools/sweetgl/include/xf86drm.h (Turnip needs the
 * drmSyncobj* calls its Vulkan runtime uses as well; see sweet_drm.c).
 *
 * extern "C": Mesa's command-submit backend, tu_knl_drm_msm.cc, is C++ and
 * includes this through its C++-compiled headers. Without the block its calls
 * would be mangled, and sweet_drm.o's plain C definitions would not match -
 * ld says so ("declaration possibly missing 'extern \"C\"'") and the ICD would
 * fail to load on the phone with a missing mangled symbol.
 */
#ifndef SWEET_XF86DRM_H
#define SWEET_XF86DRM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/ioccom.h>
#include "drm-uapi/drm.h"

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

#ifdef __cplusplus
extern "C" {
#endif

int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmOpenWithType(const char *name, const char *busid, int type);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);

#ifdef __cplusplus
}
#endif

#endif

/*
 * tools/sweetvk: declarations for the calls Mesa's Vulkan runtime makes that
 * iOS does not declare. compat.c defines them; ios-cross.txt.in forces this
 * header into every Mesa translation unit (-include in c_args), which is the
 * only way the callers can see them - they are calls the macOS SDK's headers
 * simply do not have.
 *
 * Two groups, neither of them on a path this driver runs:
 *
 *  - pthread_condattr_setclock: Darwin's condition variables only use the
 *    real-time clock (pthread_cond_timedwait takes an absolute gettimeofday()
 *    time), so there is no way to ask for CLOCK_MONOTONIC. Mesa's WSI event
 *    queues (X11, display) and its X11 present-wait code ask for it, but this
 *    ICD is built without any window system, so those callers are not
 *    compiled; wsi_common.c, which every Vulkan driver links, is.
 *
 *  - the build_id trio: src/util/build_id.h declares them only when
 *    HAVE_DL_ITERATE_PHDR is set, i.e. on Linux: they read the ELF
 *    .note.gnu.build-id of the loaded driver. Mach-O has no such note, so
 *    compat.c stands in with the image's Mach-O UUID, which is what ir3's
 *    shader disk cache keys on.
 *
 * Because this header is -include'd, it must stay as harmless to meson's
 * configuration probes as it can: it includes only the three headers whose
 * types the prototypes need, and adds nothing but these four declarations.
 * <time.h> is enough to upset meson's *unprefixed* has_function('clock_gettime')
 * probe, whose template deliberately declares the function itself; that probe
 * is answered in the cross file's [properties] instead (see ios-cross.txt.in).
 */
#ifndef SWEET_COMPAT_H
#define SWEET_COMPAT_H

#if defined(__APPLE__)

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);

struct build_id_note;

const struct build_id_note *build_id_find_nhdr_for_addr(const void *addr);
unsigned build_id_length(const struct build_id_note *note);
const uint8_t *build_id_data(const struct build_id_note *note);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */

#endif /* SWEET_COMPAT_H */

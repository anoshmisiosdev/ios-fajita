/*
 * tools/sweetvk: the calls Mesa's Vulkan runtime makes that iOS has not got.
 * Three of them, none on a path this driver runs:
 *
 *  - pthread_condattr_setclock: Darwin's condition variables only ever use the
 *    real-time clock (pthread_cond_timedwait takes an absolute gettimeofday()
 *    time), so there is no way to ask for CLOCK_MONOTONIC. Mesa's WSI event
 *    queues (X11, display) and its X11 present-wait code ask for it, but this
 *    ICD is built without any window system, so those callers are not built -
 *    the symbol is only reached from wsi_common.c, which every Vulkan driver
 *    links. Answering EINVAL (the POSIX answer for a clock id the
 *    implementation cannot use) keeps that code honest if it ever runs.
 *
 *  - the build_id trio: src/util/build_id.c keys Mesa's shader disk cache on
 *    the ELF note .note.gnu.build-id of the binary the driver was loaded from,
 *    and is only compiled when the platform has dl_iterate_phdr. Darwin has
 *    neither, so the note stands in as the image's Mach-O UUID, hashed to the
 *    20 bytes Mesa expects: the UUID changes on every link of the dylib, which
 *    is exactly what a cache key is for.
 *
 * Compiled into out/libsweetvk.a next to sweet_drm.o and force-loaded into
 * Mesa's ICD link (tools/sweetvk/build.sh).
 */
#include "sweet_compat.h"

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/sysinfo.h>

int
pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
	(void)attr;
	(void)clock_id;
	return EINVAL;
}

/* Mesa's freedreno and turnip ask the kernel how much RAM the machine has
 * (Linux's sysinfo(2)) and split it into a heap for the GPU. iOS has neither
 * sysinfo nor <sys/sysinfo.h>, so include/sys/sysinfo.h carries the two fields
 * they read and this answers from hw.memsize - the same source
 * util/os_misc.c's os_get_total_physical_memory() uses on Apple. */
int
sysinfo(struct sysinfo *info)
{
	int mib[2] = { CTL_HW, HW_MEMSIZE };
	uint64_t memsize = 0;
	size_t len = sizeof(memsize);

	memset(info, 0, sizeof(*info));
	info->mem_unit = 1;

	if (sysctl(mib, 2, &memsize, &len, NULL, 0) != 0 || memsize == 0) {
		/* Mesa calls sysinfo() without looking at the result, so failing
		 * here would only turn into a zero-sized heap. Guess instead:
		 * the smallest phone this driver could be running on. */
		memsize = 4ull * 1024 * 1024 * 1024;
	}
	info->totalram = (unsigned long) memsize;
	return 0;
}

/* Mesa's shader disk cache wants an id that changes when the driver binary
 * does. The ELF build-id note is that on Linux; the Mach-O UUID is that here. */
struct build_id_note {
	uint8_t digest[20]; /* sha1, what Mesa's callers assume */
};

static bool
uuid_digest(const void *base, uint8_t out[20])
{
	const struct mach_header *header = base;
	const uint8_t *p;
	unsigned i;

	if (header->magic == MH_MAGIC_64)
		p = (const uint8_t *)base + sizeof(struct mach_header_64);
	else if (header->magic == MH_MAGIC)
		p = (const uint8_t *)base + sizeof(struct mach_header);
	else
		return false;

	for (i = 0; i < header->ncmds; i++) {
		const struct load_command *lc = (const struct load_command *)p;

		if (lc->cmdsize == 0)
			break;
		if (lc->cmd == LC_UUID) {
			const struct uuid_command *uc = (const struct uuid_command *)lc;

			CC_SHA1(uc->uuid, sizeof(uc->uuid), out);
			return true;
		}
		p += lc->cmdsize;
	}

	return false;
}

const struct build_id_note *
build_id_find_nhdr_for_addr(const void *addr)
{
	static struct build_id_note note;
	static bool done;
	Dl_info info;

	if (done)
		return &note;
	done = true;

	if (dladdr(addr, &info) != 0 && info.dli_fbase != NULL &&
	    uuid_digest(info.dli_fbase, note.digest))
		return &note;

	/* Nothing to key on: hand out an id private to this process rather than a
	 * constant, so a stale cache entry can never be mistaken for this build's. */
	arc4random_buf(note.digest, sizeof(note.digest));
	return &note;
}

unsigned
build_id_length(const struct build_id_note *note)
{
	return sizeof(note->digest);
}

const uint8_t *
build_id_data(const struct build_id_note *note)
{
	return note->digest;
}

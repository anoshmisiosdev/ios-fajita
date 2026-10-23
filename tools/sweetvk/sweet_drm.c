/*
 * libdrm + msm DRM emulation for Mesa Turnip on iOS (there is no DRM here).
 *
 * Turnip's msm backend (src/freedreno/drm/msm, tu_knl_drm_msm.cc) reaches
 * the kernel only through the libdrm calls declared in include/xf86drm.h and
 * one mmap (src/freedreno/drm/freedreno_bo.c, hooked by mesa.patch to
 * sweet_drm_bo_map). This file answers them in user space on top of the
 * SweetGPU IOKit user client (xnu iokit/Kernel/arm/SweetGPU.cpp, selectors
 * documented there), which is how a phone-side process reaches the A630:
 *
 *   GEM_NEW / GEM_CLOSE      sel 10 / 11: wired buffer, kernel-chosen GPU VA,
 *                            the kernel's CPU mapping
 *   GEM_INFO GET_OFFSET      handle << 32; sweet_drm_bo_map() turns it back
 *                            into the kernel's mapping
 *   GEM_INFO GET_IOVA        the kernel GPU VA (Turnip 23.1 softpins: its
 *                            has_set_iova is hardcoded false, so IOVAs are
 *                            always the kernel's)
 *   GEM_SUBMIT               sel 13: CMD_BUF cmds become IBs at bo VA +
 *                            offset; the kernel executes them in order and
 *                            flushes its cache over every listed bo
 *   WAIT_FENCE, CPU_PREP     the kernel's fence page (sel 14), then sel 16,
 *                            which drops the CPU's cached lines so the CPU
 *                            sees what the GPU wrote
 *   GET_PARAM                sel 15 (chip id, GMEM) and constants
 *   syncobjs                 emulated here: a submit that signals an
 *                            out-syncobj remembers its seqno, in-syncobjs are
 *                            waited on before the submit
 *
 * Ported from joranv1/ios-sailfish's tools/sweetgl/sweet_drm.c (the same msm
 * emulation for gallium freedreno) and extended for Turnip: A630 GPU id, the
 * client VA window, MSM_INFO_SET_IOVA refused, syncobjs, and the UAPI 1.7
 * submit shape. Everything the emulation cannot honour is listed in
 * tools/sweetvk/README.md.
 *
 * One device per process; the "fd" is /dev/null (Mesa dup()s and closes it).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"
#include "sweet_drm.h"

/* SweetGPU user-client selectors (xnu iokit/Kernel/arm/SweetGPU.cpp). */
enum {
	SEL_BO_NEW    = 10,     /* in: size              out: handle, VA, CPU address */
	SEL_BO_FREE   = 11,     /* in: handle */
	SEL_BO_IMPORT = 12,     /* in: IOSurface id      out: handle, VA, size */
	SEL_SUBMIT    = 13,     /* struct sweet_gpu_submit in   out: fence seqno */
	SEL_WAIT      = 14,     /* in: seqno, timeout ms */
	SEL_INFO      = 15,     /* out: chip id, GMEM size, GMEM base, max Hz, 0 */
	SEL_CPU_PREP  = 16,     /* in: handle */
};

/* The kernel's SWEET_GPU_MAX_IBS / MAX_BOS, and the client VA window
 * (0086 bsd/dev/sweet_gpu.c: GVA_BASE + GVA_PAGES, 8 GiB). Turnip 23.1 does
 * not ask for the window (has_set_iova is hardcoded false), but an emulated
 * kernel answers what the real one would. */
#define SWEET_MAX_IBS   256u
#define SWEET_MAX_BOS   16384u
#define SWEET_MAX_BO_BYTES (256ull << 20)
#define SWEET_GVA_BASE  0x200000000ull
#define SWEET_GVA_SIZE  (8ull << 30)

/* The GPU the kernel patches bring up (0086 bsd/dev/sweet_gpu_a6xx.c). The
 * kernel's get_info has the chip id but no gpu id. */
#define SWEET_GPU_ID    630
#define SWEET_CHIP_ID   0x06030001u

/* How long an in-syncobj that no submit has signalled yet is waited on: the
 * real kernel blocks until one does (dma_fence), we give up instead. */
#define SWEET_SUBMIT_WAIT_MS 10000

struct sbo {
	uint64_t iova, map, size;
	uint32_t seq;           /* last submit listing it */
	int used;
};

/* An emulated syncobj. Binary only: it is signalled by drmSyncobjSignal (or
 * by the kernel on creation) or when the submit that was told to signal it
 * completes. */
struct ssobj {
	uint32_t seq;           /* the submit that signals it, 0: none */
	int signalled;
	int used;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t conn;
static int dev_fd = -1;
static struct sbo *bos;
static uint32_t nbos;
static struct ssobj *sobjs;
static uint32_t nsobjs;
static uint32_t chip_id, gmem_size;
static uint64_t gmem_base, max_freq;
static uint32_t queues;

void
sweet_drm_log(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "sweetvk: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

/* Lock held. The GPU comes up a while after boot: callers may retry. */
static void
connect_dev(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (svc == 0 || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		conn = 0;
		return;
	}
	IOObjectRelease(svc);
	uint64_t out[5];
	uint32_t n = 5;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_INFO, NULL, 0, out, &n);
	if (kr != KERN_SUCCESS || n != 5) {
		sweet_drm_log("SweetGPU is up but did not answer selector 15 (%x)", (unsigned)kr);
		IOServiceClose(conn);
		conn = 0;
		return;
	}
	chip_id = (uint32_t)out[0];
	gmem_size = (uint32_t)out[1];
	gmem_base = out[2];
	max_freq = out[3];
	dev_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	sweet_drm_log("A630 up: chip id %08x, GMEM %u KiB at %llx, max %llu Hz", chip_id,
	    gmem_size / 1024, gmem_base, max_freq);
}

int
sweet_drm_open(void)
{
	pthread_mutex_lock(&lock);
	if (!conn) {
		connect_dev();
	}
	int fd = conn && dev_fd >= 0 ? dup(dev_fd) : -1;
	pthread_mutex_unlock(&lock);
	return fd;
}

uint32_t
sweet_drm_chip_id(void)
{
	return chip_id;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	return name && strcmp(name, "msm") == 0 ? sweet_drm_open() : -1;
}

/* Lock held: the bo for a handle, or NULL. */
static struct sbo *
sbo(uint32_t handle)
{
	return handle >= 1 && handle <= nbos && bos[handle - 1].size ? &bos[handle - 1] : NULL;
}

/* Lock held. */
static int
sbo_add(uint32_t handle, uint64_t iova, uint64_t map, uint64_t size)
{
	if (handle == 0) {
		return -EINVAL;
	}
	if (handle > nbos) {
		uint32_t n = nbos ? nbos : 256;
		while (n < handle) {
			n *= 2;
		}
		struct sbo *b = realloc(bos, n * sizeof(*b));
		if (b == NULL) {
			return -ENOMEM;
		}
		memset(b + nbos, 0, (n - nbos) * sizeof(*b));
		bos = b;
		nbos = n;
	}
	bos[handle - 1] = (struct sbo){ .iova = iova, .map = map, .size = size };
	return 0;
}

/* Lock held: the syncobj for a handle, or NULL. */
static struct ssobj *
ssobj(uint32_t handle)
{
	return handle >= 1 && handle <= nsobjs && sobjs[handle - 1].used ? &sobjs[handle - 1] : NULL;
}

/* Lock held: allocate the lowest free syncobj handle. */
static int
ssobj_add(uint32_t *handle)
{
	for (uint32_t i = 0; i < nsobjs; i++) {
		if (!sobjs[i].used) {
			sobjs[i] = (struct ssobj){ .used = 1 };
			*handle = i + 1;
			return 0;
		}
	}
	uint32_t n = nsobjs ? nsobjs * 2 : 32;
	struct ssobj *s = realloc(sobjs, n * sizeof(*s));
	if (s == NULL) {
		return -ENOMEM;
	}
	memset(s + nsobjs, 0, (n - nsobjs) * sizeof(*s));
	sobjs = s;
	*handle = nsobjs + 1;
	nsobjs = n;
	sobjs[*handle - 1] = (struct ssobj){ .used = 1 };
	return 0;
}

/* Poll the kernel's fence page: has the submit with this seqno completed? */
static int
fence_passed(uint32_t seq)
{
	uint64_t in[2] = {seq, 0};
	return IOConnectCallScalarMethod(conn, SEL_WAIT, in, 2, NULL, NULL) == KERN_SUCCESS;
}

/* Wait until seq completed, at most ms milliseconds (UINT64_MAX: forever).
 * Returns 0, -ETIMEDOUT, or -EIO once the kernel says the GPU is gone. */
static int
fence_wait(uint32_t seq, uint64_t ms)
{
	if (fence_passed(seq)) {
		return 0;
	}
	for (;;) {
		uint64_t chunk = ms > 5000 ? 5000 : ms;
		uint64_t in[2] = {seq, chunk};
		kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_WAIT, in, 2, NULL, NULL);
		if (kr == KERN_SUCCESS || fence_passed(seq)) {
			return 0;
		}
		if (kr != kIOReturnTimeout) {
			sweet_drm_log("wait for fence %u failed: %x", seq, (unsigned)kr);
			return -EIO;
		}
		if (ms != UINT64_MAX) {
			ms -= chunk;
			if (ms == 0) {
				return -ETIMEDOUT;
			}
		}
	}
}

/* msm timeouts are absolute CLOCK_MONOTONIC times; -> milliseconds from now.
 * A negative time is libdrm's "forever" (it arrives as ~0ull). */
static uint64_t
timeout_ms(const struct drm_msm_timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ns = (t->tv_sec - now.tv_sec) * 1000000000ll + (t->tv_nsec - now.tv_nsec);
	if (t->tv_sec > now.tv_sec + 3600) {
		return UINT64_MAX;
	}
	return ns <= 0 ? 0 : (uint64_t)(ns / 1000000) + 1;
}

/* drmSyncobjWait's timeout is an absolute CLOCK_MONOTONIC time in ns (or
 * negative / INT64_MAX for "forever"). */
static uint64_t
syncobj_timeout_ms(int64_t nsec)
{
	if (nsec < 0) {
		return UINT64_MAX;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ns = nsec - (now.tv_sec * 1000000000ll + now.tv_nsec);
	if (ns <= 0) {
		return 0;
	}
	return (uint64_t)(ns / 1000000) + 1;
}

/* Lock held. Is the syncobj signalled? A submit's signal is only final once
 * that submit completed. */
static int
ssobj_signalled_locked(struct ssobj *s)
{
	if (s->signalled) {
		return 1;
	}
	if (s->seq != 0 && fence_passed(s->seq)) {
		s->signalled = 1;
		return 1;
	}
	return 0;
}

static int
ssobj_signalled(uint32_t handle, int *pending)
{
	pthread_mutex_lock(&lock);
	struct ssobj *s = ssobj(handle);
	int signalled = s ? ssobj_signalled_locked(s) : 0;
	if (pending != NULL) {
		*pending = s && s->seq != 0;
	}
	pthread_mutex_unlock(&lock);
	return signalled;
}

/*
 * Wait for a syncobj to be signalled, at most ms from now (UINT64_MAX:
 * forever). wait_for_submit lets a syncobj no submit has signalled yet be
 * waited on (the kernel blocks until one does; we give up after
 * SWEET_SUBMIT_WAIT_MS). Returns 0, -ETIME, -EINVAL or -EIO.
 */
static int
ssobj_wait(uint32_t handle, uint64_t ms, int wait_for_submit)
{
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		int pending = 0;
		if (ssobj_signalled(handle, &pending)) {
			return 0;
		}
		pthread_mutex_lock(&lock);
		uint32_t seq = ssobj(handle) ? ssobj(handle)->seq : 0;
		int known = ssobj(handle) != NULL;
		pthread_mutex_unlock(&lock);
		if (!known) {
			return -EINVAL;
		}
		if (!pending) {
			if (!wait_for_submit) {
				return -EINVAL;         /* no submit will signal it */
			}
			/* the kernel sleeps on a dma_fence that does not exist
			 * yet; poll for the submit that will create it */
			if (ms == UINT64_MAX) {
				ms = SWEET_SUBMIT_WAIT_MS;
			}
			usleep(200);
		} else {
			int e = fence_wait(seq, ms > 1000 ? 1000 : ms);
			if (e == 0) {
				continue;               /* re-check: now signalled */
			}
			if (e != -ETIMEDOUT) {
				return e;
			}
		}
		if (ms != UINT64_MAX) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			int64_t waited = (now.tv_sec - start.tv_sec) * 1000ll +
			    (now.tv_nsec - start.tv_nsec) / 1000000;
			if (waited >= 0 && (uint64_t)waited >= ms) {
				return -ETIME;
			}
		}
	}
}

static int
get_param(struct drm_msm_param *p)
{
	switch (p->param) {
	case MSM_PARAM_GPU_ID:
		p->value = SWEET_GPU_ID;
		return 0;
	case MSM_PARAM_CHIP_ID:
		/* the kernel reads this off the GPU (a630 rev 2 here) */
		p->value = chip_id ? chip_id : SWEET_CHIP_ID;
		return 0;
	case MSM_PARAM_GMEM_SIZE:
		p->value = gmem_size;
		return 0;
	case MSM_PARAM_GMEM_BASE:
		p->value = gmem_base;
		return 0;
	case MSM_PARAM_MAX_FREQ:
		p->value = max_freq;
		return 0;
	case MSM_PARAM_TIMESTAMP: {
		/* RBBM_ALWAYSON counts XO (19.2 MHz); mach_absolute_time is
		 * ns on Apple silicon. Unproven on the phone (see NOTES.md),
		 * but Turnip only uses it for timestamps. */
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		uint64_t ns = mach_absolute_time() * tb.numer / tb.denom;
		p->value = ns * 192 / 10000;
		return 0;
	}
	case MSM_PARAM_PRIORITIES:
		p->value = 1;
		return 0;
	case MSM_PARAM_FAULTS:
		/* sweet_gpu.h has no fault count: a GPU fault reaches user
		 * space as a failed or timed-out submit, not as a count. */
		p->value = 0;
		return 0;
	case MSM_PARAM_SUSPENDS:
		p->value = 0;
		return 0;
	case MSM_PARAM_VA_START:
		p->value = SWEET_GVA_BASE;
		return 0;
	case MSM_PARAM_VA_SIZE:
		p->value = SWEET_GVA_SIZE;
		return 0;
	default:
		return -EINVAL;
	}
}

static int
gem_new(struct drm_msm_gem_new *r)
{
	if (r->size == 0 || r->size > SWEET_MAX_BO_BYTES) {
		return -EINVAL;
	}
	/* The kernel's bos are CPU-cached and uncached for the GPU, so the
	 * cached flags are what they are. Cached-coherent (CPU and GPU sharing
	 * cache lines, UAPI 1.8+) is not something sweet_gpu.h can give: say
	 * so rather than hand out memory that would need cache flushes the
	 * kernel does not do. */
	if (r->flags & MSM_BO_CACHED_COHERENT) {
		sweet_drm_log("MSM_BO_CACHED_COHERENT (%x) is not supported", r->flags);
		return -EINVAL;
	}
	uint64_t in = r->size, out[3];
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_BO_NEW, &in, 1, out, &n);
	if (kr != KERN_SUCCESS) {
		sweet_drm_log("bo new of %llu bytes failed: %x", r->size, (unsigned)kr);
		return -ENOMEM;
	}
	pthread_mutex_lock(&lock);
	int ret = sbo_add((uint32_t)out[0], out[1], out[2], (r->size + 16383) & ~16383ull);
	pthread_mutex_unlock(&lock);
	if (ret) {
		IOConnectCallScalarMethod(conn, SEL_BO_FREE, out, 1, NULL, NULL);
		return ret;
	}
	r->handle = (uint32_t)out[0];
	return 0;
}

static int
gem_close(uint32_t handle)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(handle);
	if (b) {
		memset(b, 0, sizeof(*b));
	}
	pthread_mutex_unlock(&lock);
	uint64_t in = handle;
	return b && IOConnectCallScalarMethod(conn, SEL_BO_FREE, &in, 1, NULL, NULL) == KERN_SUCCESS ? 0 : -EINVAL;
}

static int
gem_info(struct drm_msm_gem_info *r)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(r->handle);
	int ret = b ? 0 : -ENOENT;
	if (b) {
		switch (r->info) {
		case MSM_INFO_GET_OFFSET:
			r->value = (uint64_t)r->handle << 32;
			break;
		case MSM_INFO_GET_IOVA:
			r->value = b->iova;
			break;
		case MSM_INFO_SET_NAME:
			break;
		case MSM_INFO_SET_IOVA:
			/* The kernel picks the VA (sweet_gpu_vm_map has no "map
			 * at this VA"); Turnip 23.1 never asks (has_set_iova is
			 * hardcoded false). Refuse instead of lying. */
			sweet_drm_log("MSM_INFO_SET_IOVA (handle %u, %llx) is not supported",
			    r->handle, (unsigned long long)r->value);
			ret = -EINVAL;
			break;
		default:
			ret = -EINVAL;
		}
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
cpu_prep(struct drm_msm_gem_cpu_prep *r)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(r->handle);
	uint32_t seq = b ? b->seq : 0;
	int used = b && b->used;
	pthread_mutex_unlock(&lock);
	if (!b) {
		return -ENOENT;
	}
	if (used && !fence_passed(seq)) {
		if (r->op & MSM_PREP_NOSYNC) {
			return -EBUSY;
		}
		int e = fence_wait(seq, timeout_ms(&r->timeout));
		if (e) {
			return e;
		}
	}
	/* the bo is CPU-cached: drop the lines the GPU may have written over */
	uint64_t in = r->handle;
	return !used || IOConnectCallScalarMethod(conn, SEL_CPU_PREP, &in, 1, NULL, NULL) == KERN_SUCCESS ? 0 : -EIO;
}

static int
gem_submit(struct drm_msm_gem_submit *r)
{
	/* Turnip 23.1 fences through syncobjs; a fence fd would need a kernel
	 * sync_file, which sweet_gpu.h has not got. */
	if (r->flags & (MSM_SUBMIT_FENCE_FD_IN | MSM_SUBMIT_FENCE_FD_OUT | MSM_SUBMIT_FENCE_SN_IN)) {
		sweet_drm_log("submit flags %x want fence fds, not supported", r->flags);
		return -EINVAL;
	}
	if (r->flags & ~(MSM_SUBMIT_FLAGS | MSM_PIPE_ID_MASK)) {
		sweet_drm_log("submit flags %x not understood", r->flags);
		return -EINVAL;
	}
	if (r->syncobj_stride != 0 && r->syncobj_stride < sizeof(struct drm_msm_gem_submit_syncobj)) {
		return -EINVAL;
	}
	const struct drm_msm_gem_submit_syncobj *sin = (const void *)(uintptr_t)r->in_syncobjs;
	const struct drm_msm_gem_submit_syncobj *sout = (const void *)(uintptr_t)r->out_syncobjs;
	if ((r->nr_in_syncobjs && !(r->flags & MSM_SUBMIT_SYNCOBJ_IN)) ||
	    (r->nr_out_syncobjs && !(r->flags & MSM_SUBMIT_SYNCOBJ_OUT))) {
		return -EINVAL;
	}

	/* The kernel waits on the in-syncobjs before it runs the submit. */
	for (uint32_t i = 0; i < r->nr_in_syncobjs; i++) {
		if (sin[i].flags & ~MSM_SUBMIT_SYNCOBJ_RESET) {
			return -EINVAL;
		}
		if (sin[i].point != 0) {
			/* binary syncobjs have no point; a timeline point from a
			 * caller other than Mesa's own would need one */
			sweet_drm_log("in-syncobj %u point %llu: treating it as signalled at any point",
			    sin[i].handle, (unsigned long long)sin[i].point);
		}
		int e = ssobj_wait(sin[i].handle, UINT64_MAX, 1);
		if (e) {
			sweet_drm_log("wait on in-syncobj %u failed: %d", sin[i].handle, -e);
			return e;
		}
		if (sin[i].flags & MSM_SUBMIT_SYNCOBJ_RESET) {
			pthread_mutex_lock(&lock);
			struct ssobj *s = ssobj(sin[i].handle);
			if (s) {
				s->seq = 0;
				s->signalled = 0;
			}
			pthread_mutex_unlock(&lock);
		}
	}

	const struct drm_msm_gem_submit_bo *sb = (const void *)(uintptr_t)r->bos;
	const struct drm_msm_gem_submit_cmd *sc = (const void *)(uintptr_t)r->cmds;
	if (r->nr_bos > SWEET_MAX_BOS) {
		return -EINVAL;
	}
	size_t len = 8 + r->nr_cmds * 16 + r->nr_bos * 4;
	uint8_t *buf = malloc(len);
	if (buf == NULL) {
		return -ENOMEM;
	}
	uint32_t *hdr = (uint32_t *)buf, nib = 0;
	uint8_t *ib = buf + 8;
	pthread_mutex_lock(&lock);
	int ret = 0;
	for (uint32_t i = 0; i < r->nr_cmds && ret == 0; i++) {
		if (sc[i].type == MSM_SUBMIT_CMD_CTX_RESTORE_BUF) {
			/* a descriptor-table restore for the CP to read from a
			 * base address, not an IB (Turnip 23.1 emits neither) */
			sweet_drm_log("skipping a CTX_RESTORE_BUF (%u dwords)", sc[i].size / 4);
			continue;
		}
		if (sc[i].type != MSM_SUBMIT_CMD_BUF && sc[i].type != MSM_SUBMIT_CMD_IB_TARGET_BUF) {
			ret = -EINVAL;
			break;
		}
		struct sbo *b = sc[i].submit_idx < r->nr_bos ? sbo(sb[sc[i].submit_idx].handle) : NULL;
		if (b == NULL || sc[i].nr_relocs || (sc[i].size & 3) || sc[i].size == 0 ||
		    (uint64_t)sc[i].submit_offset + sc[i].size > b->size) {
			sweet_drm_log("bad cmd %u: handle %u, offset %u, size %u (%u relocs)",
			    i, sc[i].submit_idx < r->nr_bos ? sb[sc[i].submit_idx].handle : 0,
			    sc[i].submit_offset, sc[i].size, sc[i].nr_relocs);
			ret = -EINVAL;
			break;
		}
		uint64_t va = b->iova + sc[i].submit_offset;
		uint32_t dw[4] = {(uint32_t)va, (uint32_t)(va >> 32), sc[i].size / 4, 0};
		memcpy(ib + 16 * nib++, dw, 16);
	}
	uint32_t *handles = (uint32_t *)(ib + 16 * nib);
	for (uint32_t i = 0; i < r->nr_bos && ret == 0; i++) {
		if (sbo(sb[i].handle) == NULL) {
			sweet_drm_log("submit lists unknown bo handle %u", sb[i].handle);
			ret = -EINVAL;
		}
		handles[i] = sb[i].handle;
	}
	pthread_mutex_unlock(&lock);
	if (ret == 0 && nib == 0) {
		ret = -EINVAL;
	}
	uint64_t seq = 0;
	if (ret == 0) {
		/* the kernel takes at most SWEET_MAX_IBS IBs at a time: send a
		 * bigger submit as several, in order (one GPU ring, so the
		 * order holds and the last one carries the fence) */
		for (uint32_t done = 0; done < nib && ret == 0; ) {
			uint32_t take = nib - done > SWEET_MAX_IBS ? SWEET_MAX_IBS : nib - done;
			hdr[0] = take;
			hdr[1] = r->nr_bos;
			uint32_t n = 1;
			kern_return_t kr = IOConnectCallMethod(conn, SEL_SUBMIT, NULL, 0,
			    buf + 8 + 16 * done, 8 + take * 16 + r->nr_bos * 4,
			    &seq, &n, NULL, NULL);
			if (kr != KERN_SUCCESS) {
				sweet_drm_log("submit (%u IBs, %u bos) failed: %x", take, r->nr_bos, (unsigned)kr);
				ret = kr == kIOReturnNotReady ? -ENODEV :
				      kr == kIOReturnTimeout ? -ETIMEDOUT :
				      kr == kIOReturnNotPermitted ? -EFAULT : -EINVAL;
				break;
			}
			done += take;
		}
	}
	if (ret == 0) {
		pthread_mutex_lock(&lock);
		for (uint32_t i = 0; i < r->nr_bos; i++) {
			struct sbo *b = sbo(sb[i].handle);
			if (b) {
				b->seq = (uint32_t)seq;
				b->used = 1;
			}
		}
		for (uint32_t i = 0; i < r->nr_out_syncobjs; i++) {
			struct ssobj *s = ssobj(sout[i].handle);
			if (s == NULL) {
				sweet_drm_log("submit signals unknown syncobj %u", sout[i].handle);
				ret = -EINVAL;
				break;
			}
			s->seq = (uint32_t)seq;
			s->signalled = 0;
		}
		pthread_mutex_unlock(&lock);
		r->fence = (uint32_t)seq;
	}
	free(buf);
	return ret;
}

static int
command(unsigned long index, void *data)
{
	if (!conn) {
		return -ENODEV;
	}
	switch (index) {
	case DRM_MSM_GET_PARAM:
		return get_param(data);
	case DRM_MSM_SET_PARAM:
		return 0;
	case DRM_MSM_GEM_NEW:
		return gem_new(data);
	case DRM_MSM_GEM_INFO:
		return gem_info(data);
	case DRM_MSM_GEM_CPU_PREP:
		return cpu_prep(data);
	case DRM_MSM_GEM_CPU_FINI:
		return 0;
	case DRM_MSM_GEM_SUBMIT:
		return gem_submit(data);
	case DRM_MSM_WAIT_FENCE: {
		struct drm_msm_wait_fence *r = data;
		return fence_wait(r->fence, timeout_ms(&r->timeout));
	}
	case DRM_MSM_GEM_MADVISE:
		((struct drm_msm_gem_madvise *)data)->retained = 1;
		return 0;
	case DRM_MSM_SUBMITQUEUE_NEW:
		((struct drm_msm_submitqueue *)data)->id = ++queues;
		return 0;
	case DRM_MSM_SUBMITQUEUE_CLOSE:
		return 0;
	case DRM_MSM_SUBMITQUEUE_QUERY: {
		struct drm_msm_submitqueue_query *q = data;
		if (q->param != MSM_SUBMITQUEUE_PARAM_FAULTS || q->len < 4) {
			return -EINVAL;
		}
		*(uint32_t *)(uintptr_t)q->data = 0;
		return 0;
	}
	default:
		sweet_drm_log("unhandled msm ioctl %lu", index);
		return -EINVAL;
	}
}

/* libdrm: 0 or -errno, errno set too */
int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	(void)size;
	int ret = command(index, data);
	if (ret) {
		errno = -ret;
	}
	return ret;
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	return drmCommandWrite(fd, index, data, size);
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	int ret;
	if (request == DRM_IOCTL_GEM_CLOSE) {
		ret = gem_close(((struct drm_gem_close *)arg)->handle);
	} else if (((request >> 8) & 0xff) == DRM_IOCTL_BASE && (request & 0xff) >= DRM_COMMAND_BASE &&
	    (request & 0xff) < DRM_COMMAND_END) {
		ret = command((request & 0xff) - DRM_COMMAND_BASE, arg);
	} else {
		sweet_drm_log("unhandled drm ioctl %lx", request);
		ret = -EINVAL;
	}
	if (ret) {
		errno = -ret;
		return -1;
	}
	return 0;
}

void *
sweet_drm_bo_map(int fd, uint64_t offset)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo((uint32_t)(offset >> 32));
	void *p = b && b->map ? (void *)(uintptr_t)b->map : (void *)-1;
	pthread_mutex_unlock(&lock);
	return p;
}

int
sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, uint32_t *handle, uint64_t *size)
{
	if (!conn) {
		return -ENODEV;
	}
	uint64_t in = surface_id, out[3];
	uint32_t n = 3;
	if (IOConnectCallScalarMethod(conn, SEL_BO_IMPORT, &in, 1, out, &n) != KERN_SUCCESS) {
		return -ENOENT;
	}
	pthread_mutex_lock(&lock);
	int ret = sbo_add((uint32_t)out[0], out[1], map, out[2]);
	pthread_mutex_unlock(&lock);
	if (ret) {
		IOConnectCallScalarMethod(conn, SEL_BO_FREE, out, 1, NULL, NULL);
		return ret;
	}
	*handle = (uint32_t)out[0];
	*size = out[2];
	return 0;
}

static drmVersion version = {
	.version_major = 1, .version_minor = 7, .version_patchlevel = 0,
	.name_len = 3, .name = "msm", .date_len = 0, .date = "", .desc_len = 0, .desc = "",
};

drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	return conn ? &version : NULL;
}

void
drmFreeVersion(drmVersionPtr v)
{
	(void)v;
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	switch (capability) {
	case DRM_CAP_SYNCOBJ:
		/* the emulated syncobjs are real to the only user (Turnip) */
		*value = 1;
		return 0;
	case DRM_CAP_SYNCOBJ_TIMELINE:
		/* a timeline point on the kernel side would need a msm submit
		 * that signals it; DNM: Turnip takes its own timeline path
		 * (tu_timeline_sync_type) when this is unsupported, which is
		 * the path this emulation implements. */
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

/* Device sharing is not implemented: a GEM handle is this process's (the
 * SweetGPU user client's) own, and prime fds would need a kernel dma-buf. */
int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *p) { (void)fd; (void)h; (void)f; (void)p; return -1; }
int drmPrimeFDToHandle(int fd, int p, uint32_t *h) { (void)fd; (void)p; (void)h; return -1; }

/*
 * Syncobjs. Binary only (drmGetCap(DRM_CAP_SYNCOBJ_TIMELINE) says so): a
 * syncobj is signalled by the submit told to signal it, or by
 * drmSyncobjSignal. All of these follow libdrm's convention: 0, or -1 with
 * errno set.
 */
static int
syncobj_ret(int ret)
{
	if (ret == 0) {
		return 0;
	}
	errno = -ret;
	return -1;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	if (flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) {
		return syncobj_ret(-EINVAL);
	}
	pthread_mutex_lock(&lock);
	int ret = ssobj_add(handle);
	if (ret == 0) {
		sobjs[*handle - 1].signalled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0;
	}
	pthread_mutex_unlock(&lock);
	return syncobj_ret(ret);
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct ssobj *s = ssobj(handle);
	if (s) {
		memset(s, 0, sizeof(*s));
	}
	pthread_mutex_unlock(&lock);
	return s ? 0 : syncobj_ret(-EINVAL);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	for (uint32_t i = 0; i < handle_count; i++) {
		pthread_mutex_lock(&lock);
		struct ssobj *s = ssobj(handles[i]);
		if (s) {
			s->signalled = 1;
			s->seq = 0;
		}
		pthread_mutex_unlock(&lock);
		if (!s) {
			return syncobj_ret(-EINVAL);
		}
	}
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	for (uint32_t i = 0; i < handle_count; i++) {
		pthread_mutex_lock(&lock);
		struct ssobj *s = ssobj(handles[i]);
		if (s) {
			s->signalled = 0;
			s->seq = 0;
		}
		pthread_mutex_unlock(&lock);
		if (!s) {
			return syncobj_ret(-EINVAL);
		}
	}
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	if (flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
	    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) {
		return syncobj_ret(-EINVAL);
	}
	uint64_t ms = syncobj_timeout_ms(timeout_nsec);
	int wait_all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	unsigned signalled = 0;
	if (num_handles == 0) {
		return 0;
	}
	for (unsigned i = 0; i < num_handles; i++) {
		int e = ssobj_wait(handles[i], ms, flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
		if (e == 0) {
			if (first_signaled != NULL && signalled == 0) {
				*first_signaled = i;
			}
			signalled++;
			if (!wait_all) {
				return 0;
			}
		} else if (wait_all) {
			return syncobj_ret(e);          /* -ETIME unless it is a hard error */
		}
	}
	return signalled > 0 ? 0 : syncobj_ret(-ETIME);
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	(void)fd;
	for (uint32_t i = 0; i < handle_count; i++) {
		points[i] = ssobj_signalled(handles[i], NULL) ? 1 : 0;
	}
	return 0;
}

int drmSyncobjHandleToFD(int fd, uint32_t h, int *o) { (void)fd; (void)h; (void)o; return syncobj_ret(-ENOSYS); }
int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return syncobj_ret(-ENOSYS); }
int drmSyncobjImportSyncFile(int fd, uint32_t h, int s) { (void)fd; (void)h; (void)s; return syncobj_ret(-ENOSYS); }
int drmSyncobjExportSyncFile(int fd, uint32_t h, int *s) { (void)fd; (void)h; (void)s; return syncobj_ret(-ENOSYS); }
int drmSyncobjTimelineSignal(int fd, const uint32_t *h, uint64_t *p, uint32_t n) { (void)fd; (void)h; (void)p; (void)n; return syncobj_ret(-ENOSYS); }
int drmSyncobjTimelineWait(int fd, uint32_t *h, uint64_t *p, unsigned n, int64_t t, unsigned f, uint32_t *s)
{
	(void)fd; (void)h; (void)p; (void)n; (void)t; (void)f; (void)s;
	return syncobj_ret(-ENOSYS);
}

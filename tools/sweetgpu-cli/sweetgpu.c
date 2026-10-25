/*
 * sweetgpu: poke the A630 through the SweetGPU user client (patch 0085) from a
 * shell on the phone, so GPU experiments need no kernel rebuild or reboot.
 * Boot with sweetgpu=8 (sweetgpuforce=1 keeps service mode up even when the
 * level-3 self-test fails).
 *
 *   sweetgpu info                 chip id, GMEM, max frequency
 *   sweetgpu memwrite [value]     CP_MEM_WRITE from a user-built IB into a bo
 *   sweetgpu quad [n]             the level-3 sample4 copy on user memory,
 *                                 n times, with a sentinel-filled destination
 *
 * Selectors and layouts: SweetGPUUserClient::externalMethod in 0085.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEL_MAP		0	/* addr, len[, ro] -> gva */
#define SEL_UNMAP	2	/* gva */
#define SEL_QUAD	3	/* struct sweet_gpu_quad */
#define SEL_BO_NEW	10	/* size -> handle, gva, user address */
#define SEL_BO_FREE	11	/* handle */
#define SEL_SUBMIT	13	/* struct submit + ibs + handles -> seqno */
#define SEL_WAIT	14	/* seqno, timeout ms */
#define SEL_INFO	15	/* -> chip id, gmem size, gmem base, max freq, 0 */

#define CP_WAIT_FOR_IDLE	0x26u
#define CP_MEM_WRITE		0x3du

#define SENTINEL	0x5a5aa5a5u
#define PAGE		16384u

struct sweet_gpu_quad {
	uint64_t src_va, dst_va;
	uint32_t src_w, src_h, src_pitch;
	uint32_t dst_w, dst_h, dst_pitch;
	uint32_t x0, y0, w, h;
	uint32_t m[6];
	uint32_t reserved;
	uint32_t flags;
	uint32_t color[4];
	uint32_t src_fmt;
	uint32_t src_swiz;
	uint32_t nedges;
	uint32_t edge[4][3];
};
#define QUAD_NEAREST	1u
#define QUAD_COPY	4u
#define SWIZ_XYZW	0x688u

struct submit_hdr {
	uint32_t nr_ibs, nr_bos;
};
struct submit_ib {
	uint64_t iova;
	uint32_t dwords;
	uint32_t pad;
};

static io_connect_t conn;

static uint32_t
parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^
	    (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return (7u << 28) | cnt | (parity(cnt) << 15) | ((op & 0x7fu) << 16) | (parity(op) << 23);
}

static int
open_gpu(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (svc == 0) {
		fprintf(stderr, "sweetgpu: no SweetGPU service (boot with sweetgpu=8)\n");
		return -1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sweetgpu: IOServiceOpen: 0x%x\n", kr);
		return -1;
	}
	return 0;
}

static int
info(void)
{
	uint64_t out[5];
	uint32_t n = 5;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_INFO, NULL, 0, out, &n);
	if (kr != KERN_SUCCESS) {
		printf("info: 0x%x (0xe00002d8 = service not ready)\n", kr);
		return 1;
	}
	printf("info: chip %08llx gmem %llu KiB at %llx, max %llu MHz\n", out[0], out[1] >> 10,
	    out[2], out[3] / 1000000);
	return 0;
}

struct bo {
	uint32_t handle;
	uint64_t gva;
	uint32_t *cpu;
};

static int
bo_new(uint64_t size, struct bo *b)
{
	uint64_t in = size, out[3];
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_BO_NEW, &in, 1, out, &n);
	if (kr != KERN_SUCCESS) {
		printf("bo new %llu: 0x%x\n", size, kr);
		return -1;
	}
	b->handle = (uint32_t)out[0];
	b->gva = out[1];
	b->cpu = (uint32_t *)(uintptr_t)out[2];
	return 0;
}

static void
bo_free(struct bo *b)
{
	uint64_t in = b->handle;
	IOConnectCallScalarMethod(conn, SEL_BO_FREE, &in, 1, NULL, NULL);
}

/* One IB, listing the given bos; waits up to 1 s. */
static int
run_ib(struct bo *ib, uint32_t dwords, const uint32_t *handles, uint32_t nbos)
{
	size_t len = sizeof(struct submit_hdr) + sizeof(struct submit_ib) + nbos * sizeof(uint32_t);
	uint8_t *buf = calloc(1, len);
	struct submit_hdr *h = (struct submit_hdr *)buf;
	struct submit_ib *s = (struct submit_ib *)(h + 1);
	h->nr_ibs = 1;
	h->nr_bos = nbos;
	s->iova = ib->gva;
	s->dwords = dwords;
	memcpy(s + 1, handles, nbos * sizeof(uint32_t));
	uint64_t seq = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallMethod(conn, SEL_SUBMIT, NULL, 0, buf, len, &seq, &n, NULL, NULL);
	free(buf);
	if (kr != KERN_SUCCESS) {
		printf("submit: 0x%x\n", kr);
		return -1;
	}
	uint64_t w[2] = { seq, 1000 };
	kr = IOConnectCallScalarMethod(conn, SEL_WAIT, w, 2, NULL, NULL);
	printf("submit seq %llu, wait 0x%x%s\n", seq, kr, kr == kIOReturnTimeout ? " (timeout)" : "");
	return kr == KERN_SUCCESS ? 0 : -1;
}

static int
memwrite(uint32_t value)
{
	struct bo ib, dst;
	if (bo_new(PAGE, &ib) || bo_new(PAGE, &dst)) {
		return 1;
	}
	for (uint32_t i = 0; i < PAGE / 4; i++) {
		dst.cpu[i] = SENTINEL;
	}
	uint32_t *p = ib.cpu, n = 0;
	p[n++] = pkt7(CP_MEM_WRITE, 3);
	p[n++] = (uint32_t)dst.gva;
	p[n++] = (uint32_t)(dst.gva >> 32);
	p[n++] = value;
	p[n++] = pkt7(CP_WAIT_FOR_IDLE, 0);
	uint32_t handles[2] = { ib.handle, dst.handle };
	printf("memwrite %08x to gva %llx (ib %llx, %u dwords)\n", value, dst.gva, ib.gva, n);
	int r = run_ib(&ib, n, handles, 2);
	printf("memwrite: dst[0] %08x dst[1] %08x -> %s\n", dst.cpu[0], dst.cpu[1],
	    dst.cpu[0] == value ? "OK" : "NOT WRITTEN");
	bo_free(&ib);
	bo_free(&dst);
	return r == 0 && dst.cpu[0] == value ? 0 : 1;
}

static uint64_t
map_user(void *addr, uint64_t len, int ro)
{
	uint64_t in[3] = { (uint64_t)(uintptr_t)addr, len, (uint64_t)ro }, gva = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_MAP, in, 3, &gva, &n);
	if (kr != KERN_SUCCESS) {
		printf("map %p+%llu: 0x%x\n", addr, len, kr);
		return 0;
	}
	return gva;
}

/* The level-3 self-test (selftest_level3 in 0086), from user space. */
static int
quad(int runs)
{
	uint32_t *src = NULL, *dst = NULL;
	if (posix_memalign((void **)&src, PAGE, PAGE) || posix_memalign((void **)&dst, PAGE, PAGE)) {
		return 1;
	}
	for (uint32_t i = 0; i < PAGE / 4; i++) {
		src[i] = 0xff000000u | ((i * 0x9e3779b1u) & 0xffffffu);
	}
	uint64_t sva = map_user(src, PAGE, 1), dva = map_user(dst, PAGE, 0);
	if (sva == 0 || dva == 0) {
		return 1;
	}
	int bad = 0;
	for (int r = 0; r < runs; r++) {
		for (uint32_t i = 0; i < PAGE / 4; i++) {
			dst[i] = SENTINEL;
		}
		struct sweet_gpu_quad q;
		memset(&q, 0, sizeof(q));
		q.src_va = sva;
		q.dst_va = dva;
		q.src_w = q.src_h = q.dst_w = q.dst_h = q.w = q.h = 64;
		q.src_pitch = q.dst_pitch = 256;
		q.m[0] = q.m[3] = 0x3f800000u;
		q.flags = QUAD_COPY | QUAD_NEAREST;
		q.color[0] = q.color[1] = q.color[2] = q.color[3] = 0x3f800000u;
		q.src_swiz = SWIZ_XYZW;
		kern_return_t kr = IOConnectCallStructMethod(conn, SEL_QUAD, &q, sizeof(q), NULL, NULL);
		uint32_t sent = 0, eq = 0, first = ~0u;
		for (uint32_t i = 0; i < PAGE / 4; i++) {
			sent += dst[i] == SENTINEL;
			eq += dst[i] == src[i];
			if (dst[i] != src[i] && first == ~0u) {
				first = i;
			}
		}
		printf("quad %d: 0x%x, equal %u, still sentinel %u of %u", r, kr, eq, sent, PAGE / 4);
		if (first != ~0u) {
			printf(", first diff [%u] %08x want %08x", first, dst[first], src[first]);
		}
		printf("\n");
		bad += eq != PAGE / 4;
	}
	return bad != 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: sweetgpu info | memwrite [value] | quad [n]\n");
		return 2;
	}
	if (open_gpu() != 0) {
		return 1;
	}
	if (strcmp(argv[1], "info") == 0) {
		return info();
	}
	if (strcmp(argv[1], "memwrite") == 0) {
		return memwrite(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0xc0ffee02u);
	}
	if (strcmp(argv[1], "quad") == 0) {
		return quad(argc > 2 ? atoi(argv[2]) : 1);
	}
	fprintf(stderr, "sweetgpu: unknown command %s\n", argv[1]);
	return 2;
}

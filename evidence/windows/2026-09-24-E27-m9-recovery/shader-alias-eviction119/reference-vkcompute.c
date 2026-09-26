/*
 * vkcompute.c - a portable Vulkan compute reference for the BC-250 driver project (experiment E14).
 *
 * Why this exists
 * ---------------
 * Milestone M8 puts RADV on a Windows WDDM winsys. When that stack first runs, we need a way to
 * tell "it works" from "it produces numbers". This program computes every result twice: once on
 * the GPU from SPIR-V, once on the CPU in plain C, and prints an FNV-1a-64 hash of both. The
 * inputs are generated deterministically in C, and every test is arranged so that its result is
 * exactly defined - integer arithmetic, or float32 arithmetic in which every intermediate value
 * is exactly representable, so that fused multiply-add, reassociation and a different workgroup
 * shape cannot change a single bit. A run under Linux/amdgpu/RADV therefore produces a table of
 * hashes that the Windows stack must reproduce exactly, on the same silicon, from the same SPIR-V.
 *
 * The process exits 0 only if every GPU hash equals its CPU hash.
 *
 * Portability rules followed here, because this file must build unchanged on both systems:
 *   - core Vulkan 1.1 only: no extensions, no layers, no windowing, no swapchain;
 *   - C99 plus libc; the only platform split is the monotonic clock (one #ifdef, below);
 *   - no variable length arrays (MSVC has none) and no POSIX calls;
 *   - all limits used stay inside the Vulkan 1.1 guaranteed minimums, so nothing here can fail
 *     on a conformant implementation: at most 4 storage buffers per stage (minimum 4), 64
 *     invocations per workgroup (minimum 128), 256 bytes of workgroup memory (minimum 16384),
 *     at most 32 bytes of push constants (minimum 128).
 *
 * Build (Linux):   gcc -std=c99 -O2 -Wall -o vkcompute vkcompute.c -lvulkan -lm
 * Build (Windows): cl /std:c11 /O2 vkcompute.c vulkan-1.lib
 *
 * Usage: vkcompute <shader-dir> [--only <name>] [--runs N] [--list]
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#endif
#define _CRT_SECURE_NO_WARNINGS 1 /* MSVC: fopen */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#ifdef _WIN32
#include <windows.h>
static double now_us(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}
#endif

/* ------------------------------------------------------------------ constants of the experiment */

/* Deterministic inputs: the 32-bit linear congruential generator from Numerical Recipes,
 *     state <- 1664525 * state + 1013904223  (mod 2^32).
 * Three lines, no libc, trivially reimplementable in any language we may need later. Values are
 * always taken from the HIGH bits of the state: the low bits of a power-of-two-modulus LCG have
 * very short periods (bit 0 alternates, the low 4 bits repeat every 16 draws), which would make
 * the matrices and weight tables below degenerate. */
#define LCG_MUL 1664525u
#define LCG_ADD 1013904223u

/* One seed per test, so that a change to one test's element count cannot shift another's input. */
#define SEED_INTHASH 0x13FE0001u
#define SEED_SAXPY   0x13FE0002u
#define SEED_SGEMM   0x13FE0003u
#define SEED_MLP     0x13FE0004u
#define SEED_REDUCE  0x13FE0005u

#define FILL_BYTES  (64u * 1024u)        /* mirrors the 64 KiB memset dispatch of libdrm's test */
#define FILL_N      (FILL_BYTES / 4u)
#define FILL_VALUE  0x22222222u

#define BIG_N       (1u << 20)           /* 1M elements for inthash, saxpy, reduce */
#define SAXPY_A     3.0f

#define GEMM_N      256u                 /* square matrices, GEMM_N must be a multiple of TILE */
#define GEMM_TILE   8u

#define MLP_BATCH   1024u
#define MLP_IN      64u
#define MLP_HID     128u
#define MLP_OUT     10u

#define RED_GROUPS  1024u                /* BIG_N / RED_GROUPS elements per workgroup in pass 1 */
#define RED_PER_GRP (BIG_N / RED_GROUPS)

#define WG          64u                  /* invocations per workgroup, everywhere */
#define MAX_BIND    4u                   /* storage buffers per pipeline; Vulkan 1.1 guarantees 4 */
#define MAX_STEPS   2                    /* dispatches per submission */
#define FENCE_NS    (10ull * 1000ull * 1000ull * 1000ull) /* 10 s, never an infinite wait */

/* ------------------------------------------------------------------------------ small utilities */

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "vkcompute: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(2);
}

#define VK_CHECK(expr)                                                                             \
	do {                                                                                       \
		VkResult vk_check_r = (expr);                                                      \
		if (vk_check_r != VK_SUCCESS)                                                      \
			die("%s:%d: %s failed with VkResult %d", __FILE__, __LINE__, #expr,        \
			    (int)vk_check_r);                                                      \
	} while (0)

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p)
		die("out of memory (%zu bytes)", n);
	return p;
}

static uint32_t lcg_next(uint32_t *state)
{
	*state = *state * LCG_MUL + LCG_ADD;
	return *state;
}

/* FNV-1a, 64 bit, over raw bytes. Chosen for the same reason as the LCG: it is five lines and
 * cannot be got wrong when it is reimplemented elsewhere. */
static uint64_t fnv1a64(const void *data, size_t n)
{
	const unsigned char *p = (const unsigned char *)data;
	uint64_t h = 0xcbf29ce484222325ull;
	size_t i;
	for (i = 0; i < n; i++) {
		h ^= (uint64_t)p[i];
		h *= 0x00000100000001b3ull;
	}
	return h;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

/* ------------------------------------------------------------------------------- Vulkan context */

struct ctx {
	VkInstance inst;
	VkPhysicalDevice phys;
	VkPhysicalDeviceProperties props;
	VkPhysicalDeviceMemoryProperties memprops;
	uint32_t qfam;
	VkDevice dev;
	VkQueue queue;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	const char *spvdir;
	int runs;
	int failures;
};

struct gbuf {
	VkBuffer buf;
	VkDeviceMemory mem;
	void *map;
	VkDeviceSize size;
};

struct gpipe {
	VkShaderModule mod;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout layout;
	VkPipeline pipe;
	VkDescriptorPool dpool;
	VkDescriptorSet dset;
	uint32_t nbind;
};

struct step {
	struct gpipe *p;
	uint32_t gx, gy, gz;
	const void *push;
	uint32_t push_size;
};

/* Host-visible and coherent, because every buffer here is both staging and result: on this APU all
 * memory is local anyway, and a separate upload path would only add a variable the comparison does
 * not need. DEVICE_LOCAL is preferred when a type offers it. */
static uint32_t pick_memtype(struct ctx *c, uint32_t type_bits, VkMemoryPropertyFlags want)
{
	VkMemoryPropertyFlags best = want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	uint32_t pass, i;
	for (pass = 0; pass < 2; pass++) {
		VkMemoryPropertyFlags need = pass == 0 ? best : want;
		for (i = 0; i < c->memprops.memoryTypeCount; i++) {
			if (!(type_bits & (1u << i)))
				continue;
			if ((c->memprops.memoryTypes[i].propertyFlags & need) == need)
				return i;
		}
	}
	die("no host-visible coherent memory type for type_bits 0x%x", type_bits);
	return 0;
}

static void buf_create(struct ctx *c, struct gbuf *b, VkDeviceSize size)
{
	VkBufferCreateInfo bi;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo ai;

	memset(b, 0, sizeof(*b));
	memset(&bi, 0, sizeof(bi));
	bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(c->dev, &bi, NULL, &b->buf));

	vkGetBufferMemoryRequirements(c->dev, b->buf, &req);
	memset(&ai, 0, sizeof(ai));
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = pick_memtype(c, req.memoryTypeBits,
					  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
						  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_CHECK(vkAllocateMemory(c->dev, &ai, NULL, &b->mem));
	VK_CHECK(vkBindBufferMemory(c->dev, b->buf, b->mem, 0));
	VK_CHECK(vkMapMemory(c->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
	b->size = size;
	memset(b->map, 0, (size_t)size);
}

static void buf_destroy(struct ctx *c, struct gbuf *b)
{
	if (b->map)
		vkUnmapMemory(c->dev, b->mem);
	if (b->buf)
		vkDestroyBuffer(c->dev, b->buf, NULL);
	if (b->mem)
		vkFreeMemory(c->dev, b->mem, NULL);
	memset(b, 0, sizeof(*b));
}

static uint32_t *read_spirv(const char *dir, const char *file, size_t *out_bytes)
{
	char path[1024];
	FILE *f;
	long len;
	uint32_t *code;

	snprintf(path, sizeof(path), "%s/%s", dir, file); /* '/' works on Windows too */
	f = fopen(path, "rb");
	if (!f)
		die("cannot open %s", path);
	if (fseek(f, 0, SEEK_END) != 0)
		die("cannot seek %s", path);
	len = ftell(f);
	if (len <= 0 || (len % 4) != 0)
		die("%s is %ld bytes, not a SPIR-V module", path, len);
	rewind(f);
	code = (uint32_t *)xmalloc((size_t)len);
	if (fread(code, 1, (size_t)len, f) != (size_t)len)
		die("short read on %s", path);
	fclose(f);
	if (code[0] != 0x07230203u)
		die("%s: bad SPIR-V magic 0x%08x", path, code[0]);
	*out_bytes = (size_t)len;
	return code;
}

static void pipe_create(struct ctx *c, struct gpipe *p, const char *spv, uint32_t nbind,
			uint32_t push_size)
{
	VkDescriptorSetLayoutBinding bind[MAX_BIND];
	VkDescriptorSetLayoutCreateInfo dli;
	VkPushConstantRange pcr;
	VkPipelineLayoutCreateInfo pli;
	VkShaderModuleCreateInfo smi;
	VkComputePipelineCreateInfo cpi;
	VkDescriptorPoolSize psize;
	VkDescriptorPoolCreateInfo dpi;
	VkDescriptorSetAllocateInfo dsi;
	size_t bytes;
	uint32_t *code;
	uint32_t i;

	if (nbind > MAX_BIND)
		die("%s wants %u storage buffers, the portable limit is %u", spv, nbind, MAX_BIND);
	memset(p, 0, sizeof(*p));
	p->nbind = nbind;

	for (i = 0; i < nbind; i++) {
		memset(&bind[i], 0, sizeof(bind[i]));
		bind[i].binding = i;
		bind[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bind[i].descriptorCount = 1;
		bind[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&dli, 0, sizeof(dli));
	dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dli.bindingCount = nbind;
	dli.pBindings = bind;
	VK_CHECK(vkCreateDescriptorSetLayout(c->dev, &dli, NULL, &p->dsl));

	memset(&pcr, 0, sizeof(pcr));
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.offset = 0;
	pcr.size = push_size;
	memset(&pli, 0, sizeof(pli));
	pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &p->dsl;
	pli.pushConstantRangeCount = push_size ? 1 : 0;
	pli.pPushConstantRanges = push_size ? &pcr : NULL;
	VK_CHECK(vkCreatePipelineLayout(c->dev, &pli, NULL, &p->layout));

	code = read_spirv(c->spvdir, spv, &bytes);
	memset(&smi, 0, sizeof(smi));
	smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smi.codeSize = bytes;
	smi.pCode = code;
	VK_CHECK(vkCreateShaderModule(c->dev, &smi, NULL, &p->mod));
	free(code);

	memset(&cpi, 0, sizeof(cpi));
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = p->mod;
	cpi.stage.pName = "main";
	cpi.layout = p->layout;
	VK_CHECK(vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p->pipe));

	memset(&psize, 0, sizeof(psize));
	psize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	psize.descriptorCount = nbind;
	memset(&dpi, 0, sizeof(dpi));
	dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpi.maxSets = 1;
	dpi.poolSizeCount = 1;
	dpi.pPoolSizes = &psize;
	VK_CHECK(vkCreateDescriptorPool(c->dev, &dpi, NULL, &p->dpool));

	memset(&dsi, 0, sizeof(dsi));
	dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsi.descriptorPool = p->dpool;
	dsi.descriptorSetCount = 1;
	dsi.pSetLayouts = &p->dsl;
	VK_CHECK(vkAllocateDescriptorSets(c->dev, &dsi, &p->dset));
}

static void pipe_bind(struct ctx *c, struct gpipe *p, struct gbuf **bufs)
{
	VkDescriptorBufferInfo info[MAX_BIND];
	VkWriteDescriptorSet w[MAX_BIND];
	uint32_t i;

	for (i = 0; i < p->nbind; i++) {
		memset(&info[i], 0, sizeof(info[i]));
		info[i].buffer = bufs[i]->buf;
		info[i].offset = 0;
		info[i].range = VK_WHOLE_SIZE;
		memset(&w[i], 0, sizeof(w[i]));
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = p->dset;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w[i].pBufferInfo = &info[i];
	}
	vkUpdateDescriptorSets(c->dev, p->nbind, w, 0, NULL);
}

static void pipe_destroy(struct ctx *c, struct gpipe *p)
{
	if (p->dpool)
		vkDestroyDescriptorPool(c->dev, p->dpool, NULL);
	if (p->pipe)
		vkDestroyPipeline(c->dev, p->pipe, NULL);
	if (p->layout)
		vkDestroyPipelineLayout(c->dev, p->layout, NULL);
	if (p->dsl)
		vkDestroyDescriptorSetLayout(c->dev, p->dsl, NULL);
	if (p->mod)
		vkDestroyShaderModule(c->dev, p->mod, NULL);
	memset(p, 0, sizeof(*p));
}

/*
 * Record the dispatch sequence once, then submit it c->runs times and return the median wall time
 * from just before vkQueueSubmit to just after vkWaitForFences returns. Every test here is
 * idempotent (no kernel reads a buffer it also writes across runs), so re-submitting the same
 * command buffer leaves the same result in memory and the timing loop cannot disturb the hash.
 *
 * Between two dispatches there is a full compute-to-compute barrier, and after the last one a
 * barrier to HOST_READ, which is what makes the mapped memory legal to read afterwards.
 */
static double run_steps(struct ctx *c, struct step *s, int nsteps)
{
	VkCommandBufferBeginInfo bi;
	VkMemoryBarrier mb;
	VkSubmitInfo si;
	double *t;
	double median;
	int i, r;

	memset(&bi, 0, sizeof(bi));
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(vkResetCommandBuffer(c->cmd, 0));
	VK_CHECK(vkBeginCommandBuffer(c->cmd, &bi));

	memset(&mb, 0, sizeof(mb));
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;

	for (i = 0; i < nsteps; i++) {
		vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s[i].p->pipe);
		vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s[i].p->layout, 0, 1,
					&s[i].p->dset, 0, NULL);
		if (s[i].push_size)
			vkCmdPushConstants(c->cmd, s[i].p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
					   s[i].push_size, s[i].push);
		vkCmdDispatch(c->cmd, s[i].gx, s[i].gy, s[i].gz);
		if (i + 1 < nsteps) {
			mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
					     NULL, 0, NULL);
		}
	}
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			     VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	VK_CHECK(vkEndCommandBuffer(c->cmd));

	memset(&si, 0, sizeof(si));
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c->cmd;

	t = (double *)xmalloc(sizeof(double) * (size_t)c->runs);
	for (r = 0; r < c->runs; r++) {
		double t0, t1;
		VkResult res;
		VK_CHECK(vkResetFences(c->dev, 1, &c->fence));
		t0 = now_us();
		VK_CHECK(vkQueueSubmit(c->queue, 1, &si, c->fence));
		res = vkWaitForFences(c->dev, 1, &c->fence, VK_TRUE, FENCE_NS);
		t1 = now_us();
		if (res == VK_TIMEOUT)
			die("fence still unsignalled after 10 s - the GPU is not coming back; "
			    "stopping instead of retrying");
		if (res != VK_SUCCESS)
			die("vkWaitForFences returned VkResult %d", (int)res);
		t[r] = t1 - t0;
	}
	qsort(t, (size_t)c->runs, sizeof(double), cmp_double);
	median = t[c->runs / 2];
	free(t);
	return median;
}

/* ------------------------------------------------------------------------------------ reporting */

/*
 * One line per test. `hash` is the GPU result, `cpu_hash` the reference computed in C over the
 * same number of bytes; the run only passes when they are equal. `first_words` is there so that a
 * mismatch can be recognised at a glance (all zeros = the dispatch did not run, garbage = it ran
 * on the wrong data) without having to dump a buffer.
 */
static int report(struct ctx *c, const char *name, size_t elems, const void *gpu, const void *cpu,
		  size_t bytes, double us, const char *extra)
{
	uint64_t hg = fnv1a64(gpu, bytes);
	uint64_t hc = fnv1a64(cpu, bytes);
	const uint32_t *w = (const uint32_t *)gpu;
	size_t nwords = bytes / 4 < 8 ? bytes / 4 : 8;
	size_t i;
	int ok = (hg == hc) && (memcmp(gpu, cpu, bytes) == 0);

	printf("%-12s n=%-9zu hash=0x%016llx cpu_hash=0x%016llx match=%-3s "
	       "t_submit_to_idle_us=%9.1f first_words=",
	       name, elems, (unsigned long long)hg, (unsigned long long)hc, ok ? "yes" : "NO", us);
	for (i = 0; i < nwords; i++)
		printf("%08x%s", w[i], i + 1 < nwords ? " " : "");
	if (extra && *extra)
		printf(" %s", extra);
	printf("\n");
	fflush(stdout);
	if (!ok)
		c->failures++;
	return ok;
}

/* ---------------------------------------------------------------------------------- the tests */

/*
 * fill: write a constant over 64 KiB. This mirrors the memset dispatch that libdrm's amdgpu test
 * uses and that our kernel-level work already exercises, so the two levels talk about the same
 * operation. The shader uses a grid-stride loop, which is the point of the two variants: one
 * workgroup and sixteen workgroups must produce byte-identical output and differ only in time.
 */
static void test_fill(struct ctx *c, uint32_t groups, const char *name)
{
	struct gbuf out;
	struct gbuf *bind[1];
	struct gpipe p;
	struct step s;
	struct {
		uint32_t n, value;
	} push;
	uint32_t *ref;
	double us;
	uint32_t i;

	buf_create(c, &out, FILL_BYTES);
	bind[0] = &out;
	pipe_create(c, &p, "fill.spv", 1, sizeof(push));
	pipe_bind(c, &p, bind);

	push.n = FILL_N;
	push.value = FILL_VALUE;
	memset(&s, 0, sizeof(s));
	s.p = &p;
	s.gx = groups;
	s.gy = 1;
	s.gz = 1;
	s.push = &push;
	s.push_size = sizeof(push);
	us = run_steps(c, &s, 1);

	ref = (uint32_t *)xmalloc(FILL_BYTES);
	for (i = 0; i < FILL_N; i++)
		ref[i] = FILL_VALUE;
	report(c, name, FILL_N, out.map, ref, FILL_BYTES, us, NULL);

	free(ref);
	pipe_destroy(c, &p);
	buf_destroy(c, &out);
}

static void test_fill_g1(struct ctx *c) { test_fill(c, 1, "fill_g1"); }
static void test_fill_g16(struct ctx *c) { test_fill(c, 16, "fill_g16"); }

/*
 * inthash: per-element integer mixing over 1M elements. Pure uint32 arithmetic, so the result is
 * defined to the bit by the language and nothing about the hardware can change it. The mixer is
 * the "lowbias32" xorshift-multiply constant set; any deviation shows up immediately in the hash.
 */
static uint32_t mix32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static void test_inthash(struct ctx *c)
{
	struct gbuf in, out;
	struct gbuf *bind[2];
	struct gpipe p;
	struct step s;
	uint32_t push = BIG_N;
	uint32_t *ref, *src;
	uint32_t seed = SEED_INTHASH;
	double us;
	uint32_t i;

	buf_create(c, &in, (VkDeviceSize)BIG_N * 4);
	buf_create(c, &out, (VkDeviceSize)BIG_N * 4);
	src = (uint32_t *)in.map;
	for (i = 0; i < BIG_N; i++)
		src[i] = lcg_next(&seed);

	bind[0] = &in;
	bind[1] = &out;
	pipe_create(c, &p, "inthash.spv", 2, sizeof(push));
	pipe_bind(c, &p, bind);

	memset(&s, 0, sizeof(s));
	s.p = &p;
	s.gx = BIG_N / WG;
	s.gy = 1;
	s.gz = 1;
	s.push = &push;
	s.push_size = sizeof(push);
	us = run_steps(c, &s, 1);

	ref = (uint32_t *)xmalloc((size_t)BIG_N * 4);
	for (i = 0; i < BIG_N; i++)
		ref[i] = mix32(src[i]);
	report(c, "inthash", BIG_N, out.map, ref, (size_t)BIG_N * 4, us, NULL);

	free(ref);
	pipe_destroy(c, &p);
	buf_destroy(c, &out);
	buf_destroy(c, &in);
}

/*
 * saxpy: z = a*x + y over 1M float32. x and y are integers in [0,1023] and a is 3, so every
 * product and every sum is an integer below 2^13 and exactly representable; whether the compiler
 * emits v_fma_f32 or a multiply and an add is therefore invisible in the result. Note that the
 * classic in-place form (y = a*x + y) is deliberately not used: it is not idempotent, and the
 * timing loop re-submits the same command buffer.
 */
static void test_saxpy(struct ctx *c)
{
	struct gbuf bx, by, bz;
	struct gbuf *bind[3];
	struct gpipe p;
	struct step s;
	struct {
		uint32_t n;
		float a;
	} push;
	float *x, *y, *ref;
	uint32_t seed = SEED_SAXPY;
	double us;
	uint32_t i;

	buf_create(c, &bx, (VkDeviceSize)BIG_N * 4);
	buf_create(c, &by, (VkDeviceSize)BIG_N * 4);
	buf_create(c, &bz, (VkDeviceSize)BIG_N * 4);
	x = (float *)bx.map;
	y = (float *)by.map;
	for (i = 0; i < BIG_N; i++) {
		x[i] = (float)(lcg_next(&seed) >> 22); /* 10 high bits: 0..1023 */
		y[i] = (float)(lcg_next(&seed) >> 22);
	}

	bind[0] = &bx;
	bind[1] = &by;
	bind[2] = &bz;
	pipe_create(c, &p, "saxpy.spv", 3, sizeof(push));
	pipe_bind(c, &p, bind);

	push.n = BIG_N;
	push.a = SAXPY_A;
	memset(&s, 0, sizeof(s));
	s.p = &p;
	s.gx = BIG_N / WG;
	s.gy = 1;
	s.gz = 1;
	s.push = &push;
	s.push_size = sizeof(push);
	us = run_steps(c, &s, 1);

	ref = (float *)xmalloc((size_t)BIG_N * 4);
	for (i = 0; i < BIG_N; i++)
		ref[i] = SAXPY_A * x[i] + y[i];
	report(c, "saxpy", BIG_N, bz.map, ref, (size_t)BIG_N * 4, us, NULL);

	free(ref);
	pipe_destroy(c, &p);
	buf_destroy(c, &bz);
	buf_destroy(c, &by);
	buf_destroy(c, &bx);
}

/*
 * sgemm: C = A*B, 256x256 float32. Entries of A and B are integers in [0,15], so each product is
 * at most 225 and each of the 256 partial sums is an integer below 57601 - far below 2^24, where
 * float32 still counts by ones. Every intermediate value is therefore exact, which is what lets
 * the naive and the tiled shader, with their different accumulation orders, and any use of FMA,
 * all produce the same bits as the C loop.
 */
static void test_sgemm_common(struct ctx *c, const char *spv, const char *name)
{
	struct gbuf ba, bb, bc;
	struct gbuf *bind[3];
	struct gpipe p;
	struct step s;
	uint32_t push = GEMM_N;
	float *a, *b, *ref;
	uint32_t seed = SEED_SGEMM;
	size_t nelem = (size_t)GEMM_N * GEMM_N;
	double us;
	uint32_t i, row, col, k;

	buf_create(c, &ba, (VkDeviceSize)nelem * 4);
	buf_create(c, &bb, (VkDeviceSize)nelem * 4);
	buf_create(c, &bc, (VkDeviceSize)nelem * 4);
	a = (float *)ba.map;
	b = (float *)bb.map;
	for (i = 0; i < nelem; i++)
		a[i] = (float)(lcg_next(&seed) >> 28); /* 4 high bits: 0..15 */
	for (i = 0; i < nelem; i++)
		b[i] = (float)(lcg_next(&seed) >> 28);

	bind[0] = &ba;
	bind[1] = &bb;
	bind[2] = &bc;
	pipe_create(c, &p, spv, 3, sizeof(push));
	pipe_bind(c, &p, bind);

	memset(&s, 0, sizeof(s));
	s.p = &p;
	s.gx = GEMM_N / GEMM_TILE;
	s.gy = GEMM_N / GEMM_TILE;
	s.gz = 1;
	s.push = &push;
	s.push_size = sizeof(push);
	us = run_steps(c, &s, 1);

	ref = (float *)xmalloc(nelem * 4);
	for (row = 0; row < GEMM_N; row++) {
		for (col = 0; col < GEMM_N; col++) {
			float sum = 0.0f;
			for (k = 0; k < GEMM_N; k++)
				sum += a[row * GEMM_N + k] * b[k * GEMM_N + col];
			ref[row * GEMM_N + col] = sum;
		}
	}
	report(c, name, nelem, bc.map, ref, nelem * 4, us, NULL);

	free(ref);
	pipe_destroy(c, &p);
	buf_destroy(c, &bc);
	buf_destroy(c, &bb);
	buf_destroy(c, &ba);
}

static void test_sgemm(struct ctx *c) { test_sgemm_common(c, "sgemm.spv", "sgemm"); }
static void test_sgemm_tiled(struct ctx *c)
{
	test_sgemm_common(c, "sgemm_tiled.spv", "sgemm_tiled");
}

/*
 * mlp: the smallest thing that is honestly an inference workload. A two layer perceptron,
 * 64 -> 128 -> 10 with ReLU, batch 1024, run as two dispatches with a barrier between them; the
 * second also takes the argmax per sample.
 *
 * Exactness, which is the whole point, comes from the scaling:
 *   inputs   are multiples of 2^-2 in [0, 1.75]          (3 high bits of the LCG, times 0.25)
 *   weights  are multiples of 2^-3 in [-1, 0.875]        (4 high bits, minus 8, times 0.125)
 *   biases   are multiples of 2^-2 in [-1, 0.75]         (3 high bits, minus 4, times 0.25)
 * so a layer-1 product is a multiple of 2^-5 no larger than 1.75, a layer-1 sum over 64 inputs
 * plus a bias is a multiple of 2^-5 no larger than 113 - that is 3616 units of 2^-5, well inside
 * the 2^24 integers float32 represents exactly. ReLU preserves that. A layer-2 product is then a
 * multiple of 2^-8 no larger than 113, and a sum over 128 of them is at most 14464, i.e. about
 * 3.7M units of 2^-8, still inside 2^24. Nothing rounds, anywhere.
 *
 * The bias is added first, before the dot product, in both the shader and the C reference; with
 * exact arithmetic the order does not matter, but keeping them identical means a future failure
 * cannot be blamed on it.
 *
 * Reported: the hash of the logits (1024 x 10 float32) as the main hash, plus the hash of the
 * argmax array and the class histogram, which is the human-readable half - a driver that returns
 * zeros gets a histogram of 1024 in class 0 and is recognisable without decoding a hash.
 */
static void test_mlp(struct ctx *c)
{
	struct gbuf bx, bw1, bh, bw2, blog, bam;
	struct gbuf *bind1[3], *bind2[4];
	struct gpipe p1, p2;
	struct step s[2];
	struct {
		uint32_t batch, a, b;
	} push1, push2;
	float *x, *w1, *w2, *h, *logits;
	uint32_t *am_ref;
	float *log_ref;
	uint32_t seed = SEED_MLP;
	uint32_t hist[MLP_OUT];
	char extra[256];
	size_t log_bytes = (size_t)MLP_BATCH * MLP_OUT * 4;
	double us;
	uint32_t i, j, k, bsmp;
	int len;

	/* Weights and their biases share one buffer (weights first, biases appended) so that no
	 * pipeline needs more than the four storage buffers Vulkan 1.1 guarantees. */
	buf_create(c, &bx, (VkDeviceSize)MLP_BATCH * MLP_IN * 4);
	buf_create(c, &bw1, (VkDeviceSize)(MLP_IN * MLP_HID + MLP_HID) * 4);
	buf_create(c, &bh, (VkDeviceSize)MLP_BATCH * MLP_HID * 4);
	buf_create(c, &bw2, (VkDeviceSize)(MLP_HID * MLP_OUT + MLP_OUT) * 4);
	buf_create(c, &blog, (VkDeviceSize)log_bytes);
	buf_create(c, &bam, (VkDeviceSize)MLP_BATCH * 4);

	x = (float *)bx.map;
	w1 = (float *)bw1.map;
	w2 = (float *)bw2.map;
	for (i = 0; i < MLP_BATCH * MLP_IN; i++)
		x[i] = (float)(lcg_next(&seed) >> 29) * 0.25f;
	for (i = 0; i < MLP_IN * MLP_HID; i++)
		w1[i] = ((float)(lcg_next(&seed) >> 28) - 8.0f) * 0.125f;
	for (i = 0; i < MLP_HID; i++)
		w1[MLP_IN * MLP_HID + i] = ((float)(lcg_next(&seed) >> 29) - 4.0f) * 0.25f;
	for (i = 0; i < MLP_HID * MLP_OUT; i++)
		w2[i] = ((float)(lcg_next(&seed) >> 28) - 8.0f) * 0.125f;
	for (i = 0; i < MLP_OUT; i++)
		w2[MLP_HID * MLP_OUT + i] = ((float)(lcg_next(&seed) >> 29) - 4.0f) * 0.25f;

	bind1[0] = &bx;
	bind1[1] = &bw1;
	bind1[2] = &bh;
	pipe_create(c, &p1, "mlp1.spv", 3, sizeof(push1));
	pipe_bind(c, &p1, bind1);

	bind2[0] = &bh;
	bind2[1] = &bw2;
	bind2[2] = &blog;
	bind2[3] = &bam;
	pipe_create(c, &p2, "mlp2.spv", 4, sizeof(push2));
	pipe_bind(c, &p2, bind2);

	push1.batch = MLP_BATCH;
	push1.a = MLP_IN;
	push1.b = MLP_HID;
	push2.batch = MLP_BATCH;
	push2.a = MLP_HID;
	push2.b = MLP_OUT;

	memset(s, 0, sizeof(s));
	s[0].p = &p1;
	s[0].gx = (MLP_BATCH * MLP_HID) / WG;
	s[0].gy = 1;
	s[0].gz = 1;
	s[0].push = &push1;
	s[0].push_size = sizeof(push1);
	s[1].p = &p2;
	s[1].gx = MLP_BATCH / WG;
	s[1].gy = 1;
	s[1].gz = 1;
	s[1].push = &push2;
	s[1].push_size = sizeof(push2);
	us = run_steps(c, s, 2);

	/* CPU reference, same order of operations as the two shaders. */
	h = (float *)xmalloc((size_t)MLP_BATCH * MLP_HID * 4);
	log_ref = (float *)xmalloc(log_bytes);
	am_ref = (uint32_t *)xmalloc((size_t)MLP_BATCH * 4);
	for (bsmp = 0; bsmp < MLP_BATCH; bsmp++) {
		for (j = 0; j < MLP_HID; j++) {
			float sum = w1[MLP_IN * MLP_HID + j];
			for (i = 0; i < MLP_IN; i++)
				sum += x[bsmp * MLP_IN + i] * w1[i * MLP_HID + j];
			h[bsmp * MLP_HID + j] = sum > 0.0f ? sum : 0.0f;
		}
	}
	for (bsmp = 0; bsmp < MLP_BATCH; bsmp++) {
		uint32_t best = 0;
		float bestv = 0.0f;
		for (k = 0; k < MLP_OUT; k++) {
			float sum = w2[MLP_HID * MLP_OUT + k];
			for (j = 0; j < MLP_HID; j++)
				sum += h[bsmp * MLP_HID + j] * w2[j * MLP_OUT + k];
			log_ref[bsmp * MLP_OUT + k] = sum;
			if (k == 0 || sum > bestv) { /* ties go to the lowest class index */
				bestv = sum;
				best = k;
			}
		}
		am_ref[bsmp] = best;
	}

	logits = (float *)blog.map;
	(void)logits;
	for (k = 0; k < MLP_OUT; k++)
		hist[k] = 0;
	for (bsmp = 0; bsmp < MLP_BATCH; bsmp++) {
		uint32_t cls = ((const uint32_t *)bam.map)[bsmp];
		if (cls < MLP_OUT)
			hist[cls]++;
	}

	len = snprintf(extra, sizeof(extra), "argmax_hash=0x%016llx argmax_cpu=0x%016llx hist=",
		       (unsigned long long)fnv1a64(bam.map, (size_t)MLP_BATCH * 4),
		       (unsigned long long)fnv1a64(am_ref, (size_t)MLP_BATCH * 4));
	for (k = 0; k < MLP_OUT && len > 0 && (size_t)len < sizeof(extra); k++)
		len += snprintf(extra + len, sizeof(extra) - (size_t)len, "%u%s", hist[k],
				k + 1 < MLP_OUT ? "," : "");
	report(c, "mlp", (size_t)MLP_BATCH * MLP_OUT, blog.map, log_ref, log_bytes, us, extra);
	if (memcmp(bam.map, am_ref, (size_t)MLP_BATCH * 4) != 0) {
		printf("mlp: argmax differs from the CPU reference\n");
		c->failures++;
	}

	free(am_ref);
	free(log_ref);
	free(h);
	pipe_destroy(c, &p2);
	pipe_destroy(c, &p1);
	buf_destroy(c, &bam);
	buf_destroy(c, &blog);
	buf_destroy(c, &bw2);
	buf_destroy(c, &bh);
	buf_destroy(c, &bw1);
	buf_destroy(c, &bx);
}

/*
 * reduce: sum 1M uint32 in two dispatches with a pipeline barrier between them, using workgroup
 * memory and barrier() inside the shader. Pass 1 gives one partial sum per workgroup over a
 * contiguous block of RED_PER_GRP elements; pass 2 folds the RED_GROUPS partials with a single
 * workgroup and appends the total. uint32 addition wraps modulo 2^32 and is associative, so the
 * tree in the shader and the straight loop in C must agree to the bit even though they add in
 * completely different orders. Both the partials and the total are hashed: the partials are what
 * localise a failure to a range of the input.
 */
static void test_reduce(struct ctx *c)
{
	struct gbuf in, out;
	struct gbuf *bind1[2], *bind2[1];
	struct gpipe p1, p2;
	struct step s[2];
	struct {
		uint32_t n, per_group;
	} push1;
	uint32_t push2 = RED_GROUPS;
	uint32_t *src, *ref;
	uint32_t seed = SEED_REDUCE;
	size_t out_words = RED_GROUPS + 1;
	double us;
	uint32_t g, i;

	buf_create(c, &in, (VkDeviceSize)BIG_N * 4);
	buf_create(c, &out, (VkDeviceSize)out_words * 4);
	src = (uint32_t *)in.map;
	for (i = 0; i < BIG_N; i++)
		src[i] = lcg_next(&seed);

	bind1[0] = &in;
	bind1[1] = &out;
	pipe_create(c, &p1, "reduce1.spv", 2, sizeof(push1));
	pipe_bind(c, &p1, bind1);

	bind2[0] = &out;
	pipe_create(c, &p2, "reduce2.spv", 1, sizeof(push2));
	pipe_bind(c, &p2, bind2);

	push1.n = BIG_N;
	push1.per_group = RED_PER_GRP;
	memset(s, 0, sizeof(s));
	s[0].p = &p1;
	s[0].gx = RED_GROUPS;
	s[0].gy = 1;
	s[0].gz = 1;
	s[0].push = &push1;
	s[0].push_size = sizeof(push1);
	s[1].p = &p2;
	s[1].gx = 1;
	s[1].gy = 1;
	s[1].gz = 1;
	s[1].push = &push2;
	s[1].push_size = sizeof(push2);
	us = run_steps(c, s, 2);

	ref = (uint32_t *)xmalloc(out_words * 4);
	ref[RED_GROUPS] = 0;
	for (g = 0; g < RED_GROUPS; g++) {
		uint32_t sum = 0;
		for (i = 0; i < RED_PER_GRP; i++)
			sum += src[g * RED_PER_GRP + i];
		ref[g] = sum;
		ref[RED_GROUPS] += sum;
	}
	report(c, "reduce", out_words, out.map, ref, out_words * 4, us, NULL);

	free(ref);
	pipe_destroy(c, &p2);
	pipe_destroy(c, &p1);
	buf_destroy(c, &out);
	buf_destroy(c, &in);
}

/* ------------------------------------------------------------------------------------- the list */

struct testdef {
	const char *name;
	void (*fn)(struct ctx *c);
	const char *what;
};

static const struct testdef tests[] = {
	{ "fill_g1", test_fill_g1, "64 KiB constant fill, 1 workgroup" },
	{ "fill_g16", test_fill_g16, "64 KiB constant fill, 16 workgroups (same bytes, less time)" },
	{ "inthash", test_inthash, "1M element uint32 xorshift-multiply mixing" },
	{ "saxpy", test_saxpy, "1M element float32 z = a*x + y, exact in float32" },
	{ "sgemm", test_sgemm, "256x256 float32 matrix multiply, naive" },
	{ "sgemm_tiled", test_sgemm_tiled, "256x256 float32 matrix multiply, 8x8 workgroup-memory tiles" },
	{ "mlp", test_mlp, "2-layer perceptron 64->128->10, ReLU, batch 1024, argmax" },
	{ "reduce", test_reduce, "1M element uint32 sum, workgroup memory, two dispatches" },
};
#define NTESTS ((int)(sizeof(tests) / sizeof(tests[0])))

/* -------------------------------------------------------------------------------- device set-up */

static void print_device(struct ctx *c)
{
	VkPhysicalDeviceSubgroupProperties sub;
	VkPhysicalDeviceProperties2 p2;
	const VkPhysicalDeviceLimits *l = &c->props.limits;
	uint32_t i;

	memset(&sub, 0, sizeof(sub));
	sub.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
	memset(&p2, 0, sizeof(p2));
	p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	p2.pNext = &sub;
	vkGetPhysicalDeviceProperties2(c->phys, &p2);

	printf("device        %s\n", c->props.deviceName);
	printf("ids           vendor=0x%04x device=0x%04x type=%d\n", c->props.vendorID,
	       c->props.deviceID, (int)c->props.deviceType);
	printf("driverVersion 0x%08x (%u.%u.%u)\n", c->props.driverVersion,
	       VK_VERSION_MAJOR(c->props.driverVersion), VK_VERSION_MINOR(c->props.driverVersion),
	       VK_VERSION_PATCH(c->props.driverVersion));
	printf("apiVersion    0x%08x (%u.%u.%u)\n", c->props.apiVersion,
	       VK_VERSION_MAJOR(c->props.apiVersion), VK_VERSION_MINOR(c->props.apiVersion),
	       VK_VERSION_PATCH(c->props.apiVersion));
	printf("queue family  %u\n", c->qfam);
	printf("subgroup      size=%u supportedStages=0x%x supportedOps=0x%x\n", sub.subgroupSize,
	       sub.supportedStages, sub.supportedOperations);
	printf("limits        wgInvocations=%u wgSize=%ux%ux%u sharedMem=%u pushConst=%u "
	       "storageBuffersPerStage=%u\n",
	       l->maxComputeWorkGroupInvocations, l->maxComputeWorkGroupSize[0],
	       l->maxComputeWorkGroupSize[1], l->maxComputeWorkGroupSize[2],
	       l->maxComputeSharedMemorySize, l->maxPushConstantsSize,
	       l->maxPerStageDescriptorStorageBuffers);
	printf("memory types  %u heaps %u\n", c->memprops.memoryTypeCount,
	       c->memprops.memoryHeapCount);
	for (i = 0; i < c->memprops.memoryTypeCount; i++)
		printf("  type %2u heap %u flags 0x%03x\n", i, c->memprops.memoryTypes[i].heapIndex,
		       c->memprops.memoryTypes[i].propertyFlags);
	for (i = 0; i < c->memprops.memoryHeapCount; i++)
		printf("  heap %2u size %llu MiB flags 0x%x\n", i,
		       (unsigned long long)(c->memprops.memoryHeaps[i].size >> 20),
		       c->memprops.memoryHeaps[i].flags);
	fflush(stdout);
}

static void ctx_init(struct ctx *c)
{
	VkApplicationInfo app;
	VkInstanceCreateInfo ici;
	VkPhysicalDevice *devs;
	VkQueueFamilyProperties *qf;
	VkDeviceQueueCreateInfo qci;
	VkDeviceCreateInfo dci;
	VkCommandPoolCreateInfo pci;
	VkCommandBufferAllocateInfo cbi;
	VkFenceCreateInfo fci;
	float prio = 1.0f;
	uint32_t ndev = 0, nqf = 0, i;

	memset(&app, 0, sizeof(app));
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "vkcompute";
	app.applicationVersion = 1;
	app.pEngineName = "bc250-e14";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_1;

	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	VK_CHECK(vkCreateInstance(&ici, NULL, &c->inst));

	VK_CHECK(vkEnumeratePhysicalDevices(c->inst, &ndev, NULL));
	if (ndev == 0)
		die("no Vulkan physical device");
	devs = (VkPhysicalDevice *)xmalloc(sizeof(VkPhysicalDevice) * ndev);
	VK_CHECK(vkEnumeratePhysicalDevices(c->inst, &ndev, devs));
	c->phys = devs[0]; /* the first device, deliberately: one GPU in this machine */
	printf("physical devices %u, using index 0\n", ndev);
	free(devs);

	vkGetPhysicalDeviceProperties(c->phys, &c->props);
	vkGetPhysicalDeviceMemoryProperties(c->phys, &c->memprops);

	vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nqf, NULL);
	qf = (VkQueueFamilyProperties *)xmalloc(sizeof(VkQueueFamilyProperties) * nqf);
	vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nqf, qf);
	c->qfam = UINT32_MAX;
	for (i = 0; i < nqf; i++) {
		if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			c->qfam = i;
			break;
		}
	}
	if (c->qfam == UINT32_MAX)
		die("no queue family with VK_QUEUE_COMPUTE_BIT");
	free(qf);

	print_device(c);

	memset(&qci, 0, sizeof(qci));
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = c->qfam;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	memset(&dci, 0, sizeof(dci));
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VK_CHECK(vkCreateDevice(c->phys, &dci, NULL, &c->dev));
	vkGetDeviceQueue(c->dev, c->qfam, 0, &c->queue);

	memset(&pci, 0, sizeof(pci));
	pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = c->qfam;
	VK_CHECK(vkCreateCommandPool(c->dev, &pci, NULL, &c->pool));

	memset(&cbi, 0, sizeof(cbi));
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbi.commandPool = c->pool;
	cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbi.commandBufferCount = 1;
	VK_CHECK(vkAllocateCommandBuffers(c->dev, &cbi, &c->cmd));

	memset(&fci, 0, sizeof(fci));
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(vkCreateFence(c->dev, &fci, NULL, &c->fence));
}

static void ctx_fini(struct ctx *c)
{
	vkDeviceWaitIdle(c->dev);
	vkDestroyFence(c->dev, c->fence, NULL);
	vkDestroyCommandPool(c->dev, c->pool, NULL);
	vkDestroyDevice(c->dev, NULL);
	vkDestroyInstance(c->inst, NULL);
}

/* ------------------------------------------------------------------------------------------ main */

static void usage(void)
{
	fprintf(stderr, "usage: vkcompute <shader-dir> [--only <name>] [--runs N] [--list]\n");
}

int main(int argc, char **argv)
{
	struct ctx c;
	const char *only = NULL;
	int i, selected = 0;

	memset(&c, 0, sizeof(c));
	c.runs = 5;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0) {
			int t;
			for (t = 0; t < NTESTS; t++)
				printf("%-12s %s\n", tests[t].name, tests[t].what);
			return 0;
		} else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
			only = argv[++i];
		} else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
			c.runs = atoi(argv[++i]);
			if (c.runs < 1 || c.runs > 999)
				die("--runs must be between 1 and 999");
		} else if (argv[i][0] == '-') {
			usage();
			return 2;
		} else if (!c.spvdir) {
			c.spvdir = argv[i];
		} else {
			usage();
			return 2;
		}
	}
	if (!c.spvdir) {
		usage();
		return 2;
	}
	if (only) {
		int t, found = 0;
		for (t = 0; t < NTESTS; t++)
			if (strcmp(tests[t].name, only) == 0)
				found = 1;
		if (!found)
			die("no test called '%s' (try --list)", only);
	}

	ctx_init(&c);
	printf("shaders       %s\n", c.spvdir);
	printf("runs          %d (the reported time is the median)\n", c.runs);
	printf("lcg           state = %u * state + %u, values taken from the high bits\n", LCG_MUL,
	       LCG_ADD);
	printf("hash          FNV-1a 64 over the raw output bytes\n\n");

	for (i = 0; i < NTESTS; i++) {
		if (only && strcmp(tests[i].name, only) != 0)
			continue;
		tests[i].fn(&c);
		selected++;
	}

	printf("\n%d test(s) run, %d mismatch(es)\n", selected, c.failures);
	ctx_fini(&c);
	return c.failures == 0 ? 0 : 1;
}

/* Visual correctness probe. Reuses the E14 Vulkan submission/readback path.
 * Each pixel is a GPU-computed fixed-point escape count, checked bit-for-bit
 * against independent CPU arithmetic before being exported for display.
 */
#define main e14_reference_main
#include "../E14-vulkan-compute-reference/vkcompute.c"
#undef main

static uint32_t escape_cpu(int x, int y, int mode)
{
    int cx = -2304 + x * 3072 / 512, cy = -1536 + y * 3072 / 512;
    int zx = 0, zy = 0;
    uint32_t n;
    if (mode == 1) { zx = cx; zy = cy; cx = -819; cy = 160; }
    for (n = 0; n < 96; ++n) {
        if (zx * zx + zy * zy > 4 * 1024 * 1024) break;
        if (mode == 2) { zx = abs(zx); zy = abs(zy); }
        int nx = (zx * zx - zy * zy) / 1024 + cx;
        zy = (2 * zx * zy) / 1024 + cy;
        zx = nx;
    }
    return n;
}

static void save_words(const char *dir, const char *name, const char *kind, const void *words, size_t bytes)
{
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s-%s.bin", dir, name, kind) >= (int)sizeof(path))
        die("export path too long");
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(words, 1, bytes, f) != bytes) die("cannot export %s", path);
    if (fclose(f)) die("cannot finish %s", path);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: shader-gallery-compute <shader-dir> <existing-output-dir>\n"); return 2; }
    struct ctx c = {0};
    c.spvdir = argv[1]; c.runs = 1;
    ctx_init(&c);
    const char *names[3] = {"Mandelbrot", "Julia", "Burning-ship"};
    char metadata[1024];
    snprintf(metadata, sizeof(metadata), "%s/results.json", argv[2]);
    FILE *meta = fopen(metadata, "wb");
    if (!meta) die("cannot create metadata");
    fprintf(meta, "{\"schema\":1,\"width\":512,\"height\":512,\"iterations\":96,\"cases\":[");
    for (uint32_t mode = 0; mode < 3; ++mode) {
        struct gbuf out; struct gbuf *bindings[1]; struct gpipe pipe; struct step step = {0};
        const size_t count = 512 * 512, bytes = count * 4;
        buf_create(&c, &out, bytes); bindings[0] = &out;
        pipe_create(&c, &pipe, "gallery.spv", 1, sizeof(mode)); pipe_bind(&c, &pipe, bindings);
        step.p = &pipe; step.gx = (uint32_t)(count / 64); step.gy = 1; step.gz = 1;
        step.push = &mode; step.push_size = sizeof(mode);
        double us = run_steps(&c, &step, 1);
        uint32_t *ref = xmalloc(bytes);
        size_t mismatch = 0;
        for (size_t i = 0; i < count; ++i) {
            ref[i] = escape_cpu((int)(i % 512), (int)(i / 512), (int)mode);
            if (ref[i] != ((uint32_t *)out.map)[i]) ++mismatch;
        }
        int pass = report(&c, names[mode], count, out.map, ref, bytes, us, NULL);
        save_words(argv[2], names[mode], "gpu", out.map, bytes);
        save_words(argv[2], names[mode], "cpu", ref, bytes);
        fprintf(meta, "%s{\"name\":\"%s\",\"pass\":%s,\"mismatches\":%zu,\"gpu_hash\":\"%016llx\",\"cpu_hash\":\"%016llx\",\"dispatch_us\":%.3f}",
            mode ? "," : "", names[mode], pass ? "true" : "false", mismatch,
            (unsigned long long)fnv1a64(out.map, bytes), (unsigned long long)fnv1a64(ref, bytes), us);
        free(ref); pipe_destroy(&c, &pipe); buf_destroy(&c, &out);
        if (!pass) break;
    }
    fprintf(meta, "]}\n"); if (fclose(meta)) die("metadata close failed");
    ctx_fini(&c);
    return c.failures ? 1 : 0;
}

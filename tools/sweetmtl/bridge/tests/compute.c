/*
 * The bridge's C test: the runtime's own `compute_dispatch` example driven through the C ABI,
 * against a bare Mesa ICD by path, with no Vulkan loader in the picture.
 *
 *     compute <air-bitcode> [<icd-path>]
 *
 * `air-bitcode` is what `llvm-as` writes from the translator's owned fixture,
 * `crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll`
 * (`tests/run.sh` assembles it). `icd-path` is the Vulkan ICD to open: NULL (or absent) takes
 * `SWEETMTL_VK_ICD` instead. On a test machine that is Mesa's Lavapipe
 * (`/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so` on Linux,
 * `/opt/homebrew/Cellar/mesa/…/lib/libvulkan_lvp.dylib` on macOS), which exports only
 * `vk_icdGetInstanceProcAddr`: the bridge makes its own instance and device through it.
 *
 * The kernel is `scale_and_offset` (buffer 0 in, buffer 1 out, buffer 2 the scale), COUNT threads
 * dispatched as a grid of 64-wide threadgroups, and output[i] = input[i] * scale + i = 3i.
 *
 * Exits 0 and prints what it checked; any failure is one line on stderr with the bridge's
 * last-error message, and exit 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sweetmtl_bridge.h"

/* The number of floats the kernel scales, as the example's COUNT. */
#define COUNT 1000
/* How long the test waits for the GPU: a CPU Vulkan driver is slow, but not this slow. */
#define TIMEOUT_MS 120000

/* One failure: `what` and the bridge's last error, on stderr. */
static int failed(const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", what, sweetmtl_last_error());
    return 1;
}

#define CHECK(expression)                 \
    do {                                  \
        if (!(expression)) {              \
            return failed(#expression);   \
        }                                 \
    } while (0)

/* The whole file at `path`, malloc'd and NUL-terminated (the last byte is not counted in
 * `*length`, so the caller can hand it to something that wants a C string too). */
static unsigned char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "FAIL cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    unsigned char *bytes = malloc((size_t)size + 1);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        fprintf(stderr, "FAIL cannot read %s\n", path);
        return NULL;
    }
    fclose(file);
    bytes[size] = 0;
    *length = (size_t)size;
    return bytes;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <air-bitcode> [<icd-path>]\n", argv[0]);
        return 2;
    }
    const char *icd_path = argc > 2 ? argv[2] : NULL;

    /* Unbuffered, because a driver that dies inside its own shader compiler (see README.md) would
     * otherwise take the whole progress report with it. */
    setvbuf(stdout, NULL, _IONBF, 0);

    size_t air_length = 0;
    unsigned char *air = read_file(argv[1], &air_length);
    if (air == NULL) {
        return 1;
    }

    /* `MTLCreateSystemDefaultDevice()`, on the ICD named: the bridge opens it, makes the instance
     * and the device through its `vk_icdGetInstanceProcAddr`, and adopts them with metal2vulkan. */
    SweetMTLDevice *device = sweetmtl_device_new(icd_path, getenv("SWEETMTL_CACHE"));
    if (device == NULL) {
        free(air);
        return failed("sweetmtl_device_new");
    }
    printf("device: %s\n", sweetmtl_device_name(device));

    /* `newLibraryWithData:error:` for AIR bitcode (the bridge's door for `llvm-as` output),
     * `newFunctionWithName:` and `newComputePipelineStateWithFunction:error:`: the translation to
     * SPIR-V and the compile against the adopted device happen here. */
    SweetMTLLibrary *library = sweetmtl_device_new_library_with_air(device, air, air_length);
    free(air);
    CHECK(library != NULL);
    SweetMTLFunction *function = sweetmtl_library_new_function(library, "scale_and_offset");
    CHECK(function != NULL);
    SweetMTLComputePipeline *pipeline =
        sweetmtl_device_new_compute_pipeline(device, function);
    CHECK(pipeline != NULL);
    printf("pipeline: %llu threads per threadgroup at most, SIMD width %llu\n",
           (unsigned long long)sweetmtl_compute_pipeline_max_total_threads_per_threadgroup(pipeline),
           (unsigned long long)sweetmtl_compute_pipeline_thread_execution_width(pipeline));

    /* `newBufferWithBytes:length:options:` and `newBufferWithLength:options:` in shared storage,
     * which the CPU reads and writes through `contents`. */
    float input[COUNT];
    for (int i = 0; i < COUNT; i++) {
        input[i] = (float)i;
    }
    SweetMTLBuffer *in = sweetmtl_device_new_buffer_with_bytes(
        device, (const unsigned char *)input, sizeof input);
    CHECK(in != NULL);
    SweetMTLBuffer *out = sweetmtl_device_new_buffer(device, COUNT * sizeof(float));
    CHECK(out != NULL);
    const float scale_value = 2.0f;
    SweetMTLBuffer *scale = sweetmtl_device_new_buffer_with_bytes(
        device, (const unsigned char *)&scale_value, sizeof scale_value);
    CHECK(scale != NULL);

    /* One command buffer with one compute encoder, 64 threads per threadgroup as Metal runs it. */
    SweetMTLCommandQueue *queue = sweetmtl_device_new_command_queue(device);
    CHECK(queue != NULL);
    SweetMTLCommandBuffer *command_buffer = sweetmtl_command_queue_new_command_buffer(queue);
    CHECK(command_buffer != NULL);
    SweetMTLComputeEncoder *encoder = sweetmtl_command_buffer_new_compute_encoder(command_buffer);
    CHECK(encoder != NULL);
    CHECK(sweetmtl_compute_encoder_set_pipeline(encoder, pipeline) == 0);
    CHECK(sweetmtl_compute_encoder_set_buffer(encoder, in, 0, 0) == 0);
    CHECK(sweetmtl_compute_encoder_set_buffer(encoder, out, 0, 1) == 0);
    CHECK(sweetmtl_compute_encoder_set_buffer(encoder, scale, 0, 2) == 0);
    CHECK(sweetmtl_compute_encoder_dispatch_threads(encoder, COUNT, 1, 1, 64, 1, 1) == 0);
    CHECK(sweetmtl_compute_encoder_end(encoder) == 0);
    sweetmtl_compute_encoder_release(encoder);
    CHECK(sweetmtl_command_buffer_commit(command_buffer) == 0);
    CHECK(sweetmtl_command_buffer_wait_until_completed(command_buffer, TIMEOUT_MS) == 0);
    CHECK(sweetmtl_command_buffer_completed(command_buffer));
    printf("status: %s\n",
           sweetmtl_command_buffer_status_name(sweetmtl_command_buffer_status(command_buffer)));

    /* output[i] = input[i] * 2 + i = 3i. */
    const float *output = (const float *)sweetmtl_buffer_contents(out);
    CHECK(output != NULL);
    for (int i = 0; i < COUNT; i++) {
        if (output[i] != 3.0f * (float)i) {
            fprintf(stderr, "FAIL output[%d] is %g, not %g\n", i, (double)output[i],
                    (double)(3 * i));
            return 1;
        }
    }
    printf("output[0..4] = %g %g %g %g, ..., output[%d] = %g: as expected\n", (double)output[0],
           (double)output[1], (double)output[2], (double)output[3], COUNT - 1,
           (double)output[COUNT - 1]);

    /* Every object before the device it came from: the runtime's objects are made from the
     * VkDevice the device handle owns. */
    sweetmtl_command_buffer_release(command_buffer);
    sweetmtl_command_queue_release(queue);
    sweetmtl_buffer_release(scale);
    sweetmtl_buffer_release(out);
    sweetmtl_buffer_release(in);
    sweetmtl_compute_pipeline_release(pipeline);
    sweetmtl_function_release(function);
    sweetmtl_library_release(library);
    sweetmtl_device_release(device);

    printf("sweetmtl-bridge: compute_dispatch through the C ABI: OK\n");
    return 0;
}

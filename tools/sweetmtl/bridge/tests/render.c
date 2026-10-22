/*
 * The bridge's render test: the runtime's own `render_to_texture` example driven through the C
 * ABI, against a bare ICD by path, with no Vulkan loader in the picture.
 *
 *     render <air-bitcode> [<icd-path>]
 *
 * The arguments are compute.c's: the same fixture (`runtime_examples.ll`, assembled by
 * `tests/run.sh`), whose `colored_vertex` and `colored_fragment` draw here, and the same ICD.
 *
 * An 8x8 RGBA8Unorm render target in private storage is cleared to blue; two triangles over its
 * top-left quarter are drawn in (1.0, 0.2, 0.0, 1.0), each vertex's position and color read by
 * vertex ID from buffers 0 and 1; a blit copies the texels into a shared buffer, and every texel
 * is checked: the quarter is 255 51 0 255, the rest 0 0 255 255.
 *
 * Exits 0 and prints what it checked; any failure is one line on stderr with the bridge's
 * last-error message, and exit 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sweetmtl_bridge.h"

/* The render target's side, in texels, as the example's SIDE. */
#define SIDE 8
#define TIMEOUT_MS 120000

/* Metal's raw values the ABI takes (MTLPixelFormat, MTLTextureUsage, MTLStorageMode,
 * MTLLoadAction, MTLStoreAction, MTLPrimitiveType, MTLFunctionType). */
#define PIXEL_FORMAT_RGBA8_UNORM 70
#define TEXTURE_USAGE_RENDER_TARGET 0x4
#define STORAGE_MODE_PRIVATE 2
#define LOAD_ACTION_CLEAR 2
#define STORE_ACTION_STORE 1
#define PRIMITIVE_TYPE_TRIANGLE 3
#define FUNCTION_TYPE_VERTEX 1
#define FUNCTION_TYPE_FRAGMENT 2

static const unsigned char DRAWN_TEXEL[4] = {255, 51, 0, 255};
static const unsigned char CLEARED_TEXEL[4] = {0, 0, 255, 255};

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

/* The whole file at `path`, malloc'd (compute.c's reader). */
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
    setvbuf(stdout, NULL, _IONBF, 0);

    size_t air_length = 0;
    unsigned char *air = read_file(argv[1], &air_length);
    if (air == NULL) {
        return 1;
    }
    SweetMTLDevice *device = sweetmtl_device_new(icd_path, getenv("SWEETMTL_CACHE"));
    if (device == NULL) {
        free(air);
        return failed("sweetmtl_device_new");
    }
    printf("device: %s\n", sweetmtl_device_name(device));

    SweetMTLLibrary *library = sweetmtl_device_new_library_with_air(device, air, air_length);
    free(air);
    CHECK(library != NULL);
    SweetMTLFunction *vertex = sweetmtl_library_new_function(library, "colored_vertex");
    CHECK(vertex != NULL);
    SweetMTLFunction *fragment = sweetmtl_library_new_function(library, "colored_fragment");
    CHECK(fragment != NULL);
    CHECK(sweetmtl_function_type(vertex) == FUNCTION_TYPE_VERTEX);
    CHECK(sweetmtl_function_type(fragment) == FUNCTION_TYPE_FRAGMENT);

    /* `newRenderPipelineStateWithDescriptor:error:`: both stages translated and compiled. */
    SweetMTLRenderPipeline *pipeline =
        sweetmtl_device_new_render_pipeline(device, vertex, fragment, PIXEL_FORMAT_RGBA8_UNORM);
    CHECK(pipeline != NULL);
    printf("render pipeline: colored_vertex + colored_fragment, RGBA8Unorm\n");

    SweetMTLTexture *target = sweetmtl_device_new_texture_2d(
        device, PIXEL_FORMAT_RGBA8_UNORM, SIDE, SIDE, TEXTURE_USAGE_RENDER_TARGET,
        STORAGE_MODE_PRIVATE);
    CHECK(target != NULL);
    CHECK(sweetmtl_texture_width(target) == SIDE && sweetmtl_texture_height(target) == SIDE);
    CHECK(sweetmtl_texture_pixel_format(target) == PIXEL_FORMAT_RGBA8_UNORM);

    /* The top-left quarter as two triangles, in Metal's normalized device coordinates (y up). */
    const float positions[6][2] = {
        {-1.0f, 1.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f},
        {0.0f, 1.0f},  {0.0f, 0.0f}, {-1.0f, 0.0f},
    };
    float colors[6][4];
    for (int i = 0; i < 6; i++) {
        colors[i][0] = 1.0f;
        colors[i][1] = 0.2f;
        colors[i][2] = 0.0f;
        colors[i][3] = 1.0f;
    }
    SweetMTLBuffer *position_buffer = sweetmtl_device_new_buffer_with_bytes(
        device, (const unsigned char *)positions, sizeof positions);
    CHECK(position_buffer != NULL);
    SweetMTLBuffer *color_buffer = sweetmtl_device_new_buffer_with_bytes(
        device, (const unsigned char *)colors, sizeof colors);
    CHECK(color_buffer != NULL);
    const unsigned long long row = SIDE * 4;
    SweetMTLBuffer *readback = sweetmtl_device_new_buffer(device, row * SIDE);
    CHECK(readback != NULL);

    SweetMTLCommandQueue *queue = sweetmtl_device_new_command_queue(device);
    CHECK(queue != NULL);
    SweetMTLCommandBuffer *command_buffer = sweetmtl_command_queue_new_command_buffer(queue);
    CHECK(command_buffer != NULL);

    /* Clear to blue, store what the pass draws. */
    SweetMTLRenderEncoder *encoder = sweetmtl_command_buffer_new_render_encoder(
        command_buffer, target, LOAD_ACTION_CLEAR, STORE_ACTION_STORE, 0.0, 0.0, 1.0, 1.0);
    CHECK(encoder != NULL);
    CHECK(sweetmtl_render_encoder_set_pipeline(encoder, pipeline) == 0);
    CHECK(sweetmtl_render_encoder_set_buffer(encoder, 0, position_buffer, 0, 0) == 0);
    CHECK(sweetmtl_render_encoder_set_buffer(encoder, 0, color_buffer, 0, 1) == 0);
    CHECK(sweetmtl_render_encoder_draw_primitives(encoder, PRIMITIVE_TYPE_TRIANGLE, 0, 6, 1, 0)
          == 0);
    CHECK(sweetmtl_render_encoder_end(encoder) == 0);
    sweetmtl_render_encoder_release(encoder);

    /* The texels into the shared buffer, after the pass. */
    SweetMTLBlitEncoder *blit = sweetmtl_command_buffer_new_blit_encoder(command_buffer);
    CHECK(blit != NULL);
    CHECK(sweetmtl_blit_encoder_copy_texture_to_buffer(blit, target, 0, 0, 0, 0, SIDE, SIDE,
                                                       readback, 0, row, row * SIDE) == 0);
    CHECK(sweetmtl_blit_encoder_end(blit) == 0);
    sweetmtl_blit_encoder_release(blit);

    CHECK(sweetmtl_command_buffer_commit(command_buffer) == 0);
    CHECK(sweetmtl_command_buffer_wait_until_completed(command_buffer, TIMEOUT_MS) == 0);
    CHECK(sweetmtl_command_buffer_completed(command_buffer));
    printf("status: %s\n",
           sweetmtl_command_buffer_status_name(sweetmtl_command_buffer_status(command_buffer)));

    const unsigned char *texels = (const unsigned char *)sweetmtl_buffer_contents(readback);
    CHECK(texels != NULL);
    for (int y = 0; y < SIDE; y++) {
        char line[SIDE + 1];
        for (int x = 0; x < SIDE; x++) {
            const unsigned char *texel = texels + (y * SIDE + x) * 4;
            int inside = x < SIDE / 2 && y < SIDE / 2;
            const unsigned char *expected = inside ? DRAWN_TEXEL : CLEARED_TEXEL;
            if (memcmp(texel, expected, 4) != 0) {
                fprintf(stderr, "FAIL texel (%d, %d) is %u %u %u %u, not %u %u %u %u\n", x, y,
                        texel[0], texel[1], texel[2], texel[3], expected[0], expected[1],
                        expected[2], expected[3]);
                return 1;
            }
            line[x] = inside ? '#' : '.';
        }
        line[SIDE] = 0;
        printf("%s\n", line);
    }
    printf("every texel as expected: the top-left quarter drawn, the rest cleared\n");

    sweetmtl_command_buffer_release(command_buffer);
    sweetmtl_command_queue_release(queue);
    sweetmtl_buffer_release(readback);
    sweetmtl_buffer_release(color_buffer);
    sweetmtl_buffer_release(position_buffer);
    sweetmtl_texture_release(target);
    sweetmtl_render_pipeline_release(pipeline);
    sweetmtl_function_release(fragment);
    sweetmtl_function_release(vertex);
    sweetmtl_library_release(library);
    sweetmtl_device_release(device);

    printf("sweetmtl-bridge: render_to_texture through the C ABI: OK\n");
    return 0;
}

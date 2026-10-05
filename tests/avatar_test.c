#define _POSIX_C_SOURCE 200809L
#include "aipet_avatar.h"
#include "frame_player.h"
#include "muse_pixel.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    unsigned char *bytes;
    uint32_t length;
    void *arena;
    fp_player_t *reference;
    unsigned width;
} fixture_t;

static fixture_t open_fixture(const char *path, unsigned width)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && length <= 3000000);
    rewind(file);
    fixture_t f = {.length = (uint32_t)length, .width = width};
    f.bytes = malloc(f.length + 1);
    f.arena = calloc(1, fp_arena_size());
    assert(f.bytes && f.arena);
    assert(fread(f.bytes, 1, f.length, file) == f.length);
    fclose(file);
    f.bytes[f.length] = 0;
    return f;
}

static void reset(fixture_t *f)
{
    fp_error_t error;
    f->reference = fp_player_init(f->arena, fp_arena_size(), f->bytes, f->length, 1, &error);
    assert(f->reference && error == FP_OK);
    assert(fp_canvas_width(f->reference) == f->width);
    assert(fp_canvas_height(f->reference) == f->width);
    fp_set_idle_mode(f->reference, FP_IDLE_NATURAL);
    assert(aipet_avatar_init(f->bytes, f->length));
    muse_pixel_set_size((int)f->width);
}

static void assert_native(const fixture_t *f)
{
    uint16_t pixels[FP_MAX_PIXELS];
    muse_pixel_scale(pixels, (int)f->width, 0, (int)f->width - 1, 0, (int)f->width - 1);
    assert(memcmp(pixels, fp_framebuffer(f->reference), f->width * f->width * sizeof(*pixels)) == 0);
}

static void step(fixture_t *f, muse_pose_t pose, fp_sys_state_t state,
                 uint8_t audio, bool touch, unsigned ticks)
{
    uint32_t before = muse_pixel_revision();
    uint32_t canonical_before = fp_visual_revision(f->reference);
    fp_set_sys_state(f->reference, state);
    fp_set_audio_level(f->reference, audio);
    if (touch) {
        fp_notify_touch(f->reference);
    }
    for (unsigned i = 0; i < ticks; i++) {
        fp_tick(f->reference);
    }
    muse_pixel_render(&pose);
    bool changed = fp_visual_revision(f->reference) != canonical_before;
    assert(muse_pixel_revision() == before + (changed ? 1u : 0u));
    assert_native(f);
}

static void check_timing(fixture_t *f)
{
    reset(f);
    /* Expected director steps are explicit, including a 132 ms gap and resume.
     * The first presentation initializes the director with one tick. */
    const unsigned millis[] = {0, 32, 33, 40, 66, 80, 120, 252, 253, 2000, 2040, 2073, 2073};
    const unsigned ticks[] =  {1,  0,  1,  0,  1,  0,   1,   4,   0,    1,    1,    1,    0};
    for (unsigned i = 0; i < sizeof(millis) / sizeof(*millis); i++) {
        muse_pose_t pose = {.mode = MUSE_MODE_IDLE, .t = millis[i] / 1000.0f};
        step(f, pose, FP_SYS_IDLE, 0, false, ticks[i]);
    }
    reset(f);
    unsigned elapsed_ticks = 0;
    for (unsigned i = 0; i < 300; i++) {
        unsigned total = 1 + i * 40 / 33;
        muse_pose_t pose = {.mode = MUSE_MODE_IDLE, .t = i * 0.04f};
        step(f, pose, FP_SYS_IDLE, 0, false, total - elapsed_ticks);
        elapsed_ticks = total;
    }
}

static void check_inputs(fixture_t *f)
{
    const muse_mode_t modes[] = {MUSE_MODE_BOOT, MUSE_MODE_IDLE, MUSE_MODE_LISTENING,
        MUSE_MODE_THINKING, MUSE_MODE_SPEAKING, MUSE_MODE_ERROR, MUSE_MODE_OFF, MUSE_MODE_COUNT};
    const fp_sys_state_t states[] = {FP_SYS_BOOTING, FP_SYS_IDLE, FP_SYS_LISTENING,
        FP_SYS_THINKING, FP_SYS_SPEAKING, FP_SYS_ERROR, FP_SYS_OFFLINE, FP_SYS_IDLE};
    reset(f);
    unsigned tick = 0;
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++) {
        for (unsigned level = 0; level < 101; level++) {
            muse_pose_t pose = {.mode = modes[mode], .t = tick++ * 0.033f, .level = level / 100.0f};
            step(f, pose, states[mode], (uint8_t)level, false, 1);
        }
    }
    const float input[] = {-1, NAN, INFINITY, 0.004f, 0.005f, 0.496f, 1.5f};
    const uint8_t output[] = {0, 0, 0, 0, 1, 50, 100};
    for (unsigned i = 0; i < sizeof(input) / sizeof(*input); i++) {
        muse_pose_t pose = {.mode = MUSE_MODE_SPEAKING, .t = tick++ * 0.033f, .level = input[i]};
        step(f, pose, FP_SYS_SPEAKING, output[i], false, 1);
    }
    /* Sustained/decaying happiness must not retrigger; a second rise must. */
    for (unsigned i = 0; i < 120; i++) {
        float happy = i < 30 ? 1 : (i < 60 ? 0.5f : (i < 90 ? 1 : 0));
        muse_pose_t pose = {.mode = MUSE_MODE_IDLE, .t = tick++ * 0.033f, .happy = happy};
        step(f, pose, FP_SYS_IDLE, 0, i == 0 || i == 60, 1);
    }
}

static void check_scaling(fixture_t *f)
{
    const int sizes[] = {1, 63, 120, 240, 320, 466, 512};
    const uint16_t sentinel = 0x6bad;
    const uint16_t *source = fp_framebuffer(f->reference);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        int size = sizes[i];
        muse_pixel_set_size(size);
        /* Full, interior, bottom-right: decoder destinations start at (0,0),
         * while requested source/display rectangles need not. */
        const int rects[][4] = {{0, size - 1, 0, size - 1},
            {size / 3, size / 2, size / 4, size / 2},
            {size - 1, size - 1, size - 1, size - 1}};
        for (unsigned r = 0; r < 3; r++) {
            int x0 = rects[r][0], x1 = rects[r][1], y0 = rects[r][2], y1 = rects[r][3];
            int stride = x1 - x0 + 4, rows = y1 - y0 + 1;
            size_t count = (size_t)stride * rows + 2;
            uint16_t *out = malloc(count * sizeof(*out));
            assert(out);
            for (size_t j = 0; j < count; j++) {
                out[j] = sentinel;
            }
            muse_pixel_scale(out + 1, stride, x0, x1, y0, y1);
            assert(out[0] == sentinel && out[count - 1] == sentinel);
            for (int y = 0; y < rows; y++) {
                for (int x = 0; x < stride; x++) {
                    uint16_t expected = sentinel;
                    if (x <= x1 - x0) {
                        unsigned sx = (unsigned)(x + x0) * f->width / (unsigned)size;
                        unsigned sy = (unsigned)(y + y0) * f->width / (unsigned)size;
                        expected = source[sy * f->width + sx];
                    }
                    assert(out[1 + y * stride + x] == expected);
                }
            }
            free(out);
        }
    }
    uint16_t guard = sentinel;
    muse_pixel_scale(&guard, 1, -1, 0, 0, 0);
    muse_pixel_scale(&guard, 1, 0, 1, 0, 0);
    muse_pixel_scale(&guard, 1, 512, 512, 0, 0);
    assert(guard == sentinel);
}

static void check_idle_override(fixture_t *f)
{
    const fp_sys_state_t states[] = {FP_SYS_BOOTING, FP_SYS_PROVISIONING, FP_SYS_CONNECTING,
        FP_SYS_IDLE, FP_SYS_OFFLINE, FP_SYS_ERROR};
    reset(f);
    unsigned tick = 0;
    for (unsigned i = 0; i < sizeof(states) / sizeof(*states); i++) {
        aipet_avatar_set_idle_state(states[i]);
        for (unsigned j = 0; j < 8; j++) {
            muse_pose_t pose = {.mode = MUSE_MODE_IDLE, .t = tick++ * 0.033f};
            step(f, pose, states[i], 0, false, 1);
        }
        muse_pose_t listen = {.mode = MUSE_MODE_LISTENING, .t = tick++ * 0.033f};
        step(f, listen, FP_SYS_LISTENING, 0, false, 1);
        muse_pose_t speak = {.mode = MUSE_MODE_SPEAKING, .t = tick++ * 0.033f, .level = 1};
        step(f, speak, FP_SYS_SPEAKING, 100, false, 1);
        muse_pose_t idle = {.mode = MUSE_MODE_IDLE, .t = tick++ * 0.033f};
        step(f, idle, states[i], 0, false, 1);
    }
    const fp_sys_state_t invalid[] = {FP_SYS_LISTENING, FP_SYS_THINKING, FP_SYS_SPEAKING,
        (fp_sys_state_t)-1, (fp_sys_state_t)100};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        aipet_avatar_set_idle_state(FP_SYS_OFFLINE);
        aipet_avatar_set_idle_state(invalid[i]);
        muse_pose_t pose = {.mode = MUSE_MODE_IDLE, .t = tick++ * 0.033f};
        step(f, pose, FP_SYS_IDLE, 0, false, 1);
    }
    aipet_avatar_set_idle_state(FP_SYS_ERROR);
    reset(f);
    muse_pose_t idle = {.mode = MUSE_MODE_IDLE};
    step(f, idle, FP_SYS_IDLE, 0, false, 1);
    const uint32_t accents[] = {0xa9c0ff, 0xa77dff, 0x5cb8ff, 0xe07bff,
        0x6ff0bf, 0xff5c5c, 0x7c72d0};
    for (unsigned i = 0; i < sizeof(accents) / sizeof(*accents); i++) {
        assert(muse_pixel_accent((muse_mode_t)i) == accents[i]);
    }
    assert(muse_pixel_accent((muse_mode_t)-1) == 0xa77dff);
    assert(muse_pixel_accent(MUSE_MODE_COUNT) == 0xa77dff);
}

static void check_rejection(fixture_t *f)
{
    assert(!aipet_avatar_init(NULL, f->length));
    assert(!aipet_avatar_init(f->bytes, 0));
    assert(!aipet_avatar_init(f->bytes, 3000001));
    assert(!aipet_avatar_init(f->bytes, f->length - 1));
    assert(!aipet_avatar_init(f->bytes, f->length + 1));
    f->bytes[0] ^= 1;
    assert(!aipet_avatar_init(f->bytes, f->length));
    f->bytes[0] ^= 1;
    f->bytes[f->length - 1] ^= 1;
    assert(!aipet_avatar_init(f->bytes, f->length));
    f->bytes[f->length - 1] ^= 1;
    muse_pixel_set_size(24);
    uint16_t error_pixels[24];
    muse_pixel_scale(error_pixels, 24, 0, 23, 0, 0);
    assert(error_pixels[0] == 0 && error_pixels[12] == 0xf800);
    reset(f);
}

static void check_host_load(const char *path, bool succeeds)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        aipet_avatar_shutdown();
        if (path) {
            assert(setenv("AIPET_PACK", path, 1) == 0);
        } else {
            assert(unsetenv("AIPET_PACK") == 0);
        }
        muse_pose_t pose = {.mode = MUSE_MODE_IDLE};
        muse_pixel_render(&pose);
        aipet_avatar_shutdown();
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert((WEXITSTATUS(status) == 0) == succeeds);
}

static void save_pixels(const fixture_t *f, const char *directory, unsigned stage)
{
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/stage-%u.ppm", directory, stage) < (int)sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%u %u\n255\n", f->width, f->width);
    uint16_t pixels[FP_MAX_PIXELS];
    muse_pixel_scale(pixels, (int)f->width, 0, (int)f->width - 1, 0, (int)f->width - 1);
    for (unsigned i = 0; i < f->width * f->width; i++) {
        uint16_t p = pixels[i];
        unsigned char rgb[] = {
            (unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63),
            (unsigned char)((p & 31) * 255 / 31)
        };
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(fclose(file) == 0);
}

static void check_speech(fixture_t *f, unsigned stages, const char *directory)
{
    assert(stages >= 2 && stages <= 8);
    unsigned visited = 0;
    /* Identical seeds and tick counts isolate mouth changes from blink/pose
     * timing. Compare actual strip pixels, not only the reference state. */
    for (unsigned level = 0; level <= 100; level++) {
        reset(f);
        for (unsigned i = 0; i < 6; i++) {
            muse_pose_t pose = {.mode = MUSE_MODE_SPEAKING, .t = i * 0.033f,
                .level = level / 100.0f};
            step(f, pose, FP_SYS_SPEAKING, (uint8_t)level, false, 1);
        }
        unsigned stage = fp_debug_mouth_stage(f->reference);
        assert(stage < stages);
        if (!(visited & (1u << stage))) {
            save_pixels(f, directory, stage);
        }
        visited |= 1u << stage;
        if (level == 0) {
            assert(stage == 0);
        }
        if (level == 100) {
            assert(stage == stages - 1);
        }
    }
    assert(visited == (1u << stages) - 1);
    /* Silence must close the mouth even while the reply remains SPEAKING. */
    for (unsigned i = 6; i < 26; i++) {
        muse_pose_t pose = {.mode = MUSE_MODE_SPEAKING, .t = i * 0.033f};
        step(f, pose, FP_SYS_SPEAKING, 0, false, 1);
    }
    assert(fp_debug_mouth_stage(f->reference) == 0);
    save_pixels(f, directory, stages);
    puts("speech passed: every mouth stage reached, strip pixels match, silence closes");
}

int main(int argc, char **argv)
{
    if (argc == 6 && strcmp(argv[1], "--speech") == 0) {
        fixture_t f = open_fixture(argv[2], (unsigned)atoi(argv[3]));
        check_speech(&f, (unsigned)atoi(argv[4]), argv[5]);
        aipet_avatar_shutdown();
        free(f.arena);
        free(f.bytes);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s PACK120 PACK240\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        fixture_t f = open_fixture(argv[i], i == 1 ? 120 : 240);
        check_timing(&f);
        check_inputs(&f);
        check_idle_override(&f);
        check_scaling(&f);
        check_rejection(&f);
        check_host_load(argv[i], true);
        aipet_avatar_shutdown();
        free(f.arena);
        free(f.bytes);
    }
    check_host_load(NULL, false);
    check_host_load("/missing/aipetframes/fixture", false);
    check_host_load(argv[0], false);
    puts("avatar tests passed: 120/240px timing, states, audio, touch, strips, rejection and host loading");
    return 0;
}

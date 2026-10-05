#include "aipet_avatar.h"
#include "frame_player.h"
#include "muse_pixel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "aipet_pack_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#endif

#define PACK_MAX_BYTES 3000000u
#define MAX_CATCHUP_MS (4u * FP_TICK_MS)

static fp_player_t *s_player;
static void *s_arena;
static bool s_attempted, s_clock_started;
static uint64_t s_last_ms;
static uint32_t s_remainder_ms, s_revision, s_native_revision;
static float s_happy;
static fp_sys_state_t s_idle_state = FP_SYS_IDLE;
static int s_size = 320;
#ifdef ESP_PLATFORM
static esp_partition_mmap_handle_t s_mapping;
static bool s_mapped;
#else
static void *s_owned_pack;
#endif

static bool fail(const char *message)
{
#ifdef ESP_PLATFORM
    ESP_LOGE("aipet_avatar", "%s", message);
    muse_state_set_mode(MUSE_MODE_ERROR);
    muse_state_set_caption("AIPET PACK ERROR");
#else
    fprintf(stderr, "aipet avatar: %s\n", message);
#endif
    s_revision++;
    return false;
}

void aipet_avatar_shutdown(void)
{
    free(s_arena);
    s_arena = NULL;
    s_player = NULL;
#ifdef ESP_PLATFORM
    if (s_mapped) {
        esp_partition_munmap(s_mapping);
        s_mapped = false;
    }
#else
    free(s_owned_pack);
    s_owned_pack = NULL;
#endif
    s_attempted = s_clock_started = false;
    s_remainder_ms = 0;
    s_happy = 0;
    s_idle_state = FP_SYS_IDLE;
}

bool aipet_avatar_init(const void *pack, uint32_t bytes)
{
    aipet_avatar_shutdown();
    s_attempted = true;
    if (!pack || !bytes || bytes > PACK_MAX_BYTES) {
        return fail("missing pack or invalid pack length");
    }
#ifdef ESP_PLATFORM
    s_arena = heap_caps_aligned_calloc(8, 1, fp_arena_size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    s_arena = calloc(1, fp_arena_size());
#endif
    if (!s_arena) {
        return fail("cannot allocate player arena");
    }
    fp_error_t error;
    s_player = fp_player_init(s_arena, fp_arena_size(), pack, bytes, 1, &error);
    if (!s_player) {
        free(s_arena);
        s_arena = NULL;
        return fail(fp_error_string(error));
    }
    fp_set_idle_mode(s_player, FP_IDLE_NATURAL);
    s_native_revision = fp_visual_revision(s_player);
    s_revision++;
    return true;
}

static bool load_pack(void)
{
    s_attempted = true;
#ifdef ESP_PLATFORM
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "aipet");
    if (!part || part->address != 0x830000 || AIPET_PACK_BYTES == 0 ||
        AIPET_PACK_BYTES > PACK_MAX_BYTES || AIPET_PACK_BYTES > part->size) {
        return fail("missing/mismatched aipet partition or pack length");
    }
    const void *pack;
    esp_partition_mmap_handle_t mapping;
    if (esp_partition_mmap(part, 0, AIPET_PACK_BYTES, ESP_PARTITION_MMAP_DATA, &pack, &mapping) != ESP_OK) {
        return fail("cannot map aipet partition");
    }
    unsigned char digest[32];
    char hex[65];
    size_t digest_bytes = 0;
    bool valid = psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_compute(PSA_ALG_SHA_256, pack, AIPET_PACK_BYTES, digest, sizeof(digest),
                         &digest_bytes) == PSA_SUCCESS && digest_bytes == sizeof(digest);
    if (valid) {
        for (unsigned i = 0; i < sizeof(digest); i++) {
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        valid = strcmp(hex, AIPET_PACK_SHA256) == 0;
    }
    if (!valid) {
        esp_partition_munmap(mapping);
        return fail("aipet pack SHA-256 does not match firmware");
    }
    if (!aipet_avatar_init(pack, AIPET_PACK_BYTES)) {
        esp_partition_munmap(mapping);
        return false;
    }
    s_mapping = mapping;
    s_mapped = true;
#else
    const char *path = getenv("AIPET_PACK");
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
        return fail("set AIPET_PACK to a readable .aipetframes pack");
    }
    long bytes = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        bytes = ftell(file);
    }
    if (bytes <= 0 || bytes > PACK_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return fail("invalid pack file length");
    }
    void *pack = malloc((size_t)bytes);
    bool read_ok = pack && fread(pack, 1, (size_t)bytes, file) == (size_t)bytes && !ferror(file);
    fclose(file);
    if (!read_ok || !aipet_avatar_init(pack, (uint32_t)bytes)) {
        free(pack);
        return read_ok ? false : fail("cannot read pack file");
    }
    s_owned_pack = pack;
#endif
    return true;
}

static void ensure_pack(void)
{
    if (!s_attempted && !load_pack()) {
#ifndef ESP_PLATFORM
        exit(EXIT_FAILURE);
#endif
    }
}

static fp_sys_state_t state_for(muse_mode_t mode)
{
    switch (mode) {
    case MUSE_MODE_BOOT: return FP_SYS_BOOTING;
    case MUSE_MODE_IDLE: return s_idle_state;
    case MUSE_MODE_LISTENING: return FP_SYS_LISTENING;
    case MUSE_MODE_THINKING: return FP_SYS_THINKING;
    case MUSE_MODE_SPEAKING: return FP_SYS_SPEAKING;
    case MUSE_MODE_ERROR: return FP_SYS_ERROR;
    case MUSE_MODE_OFF: return FP_SYS_OFFLINE;
    default: return FP_SYS_IDLE;
    }
}

void aipet_avatar_set_idle_state(fp_sys_state_t state)
{
    switch (state) {
    case FP_SYS_BOOTING:
    case FP_SYS_PROVISIONING:
    case FP_SYS_CONNECTING:
    case FP_SYS_IDLE:
    case FP_SYS_OFFLINE:
    case FP_SYS_ERROR:
        s_idle_state = state;
        break;
    default:
        s_idle_state = FP_SYS_IDLE;
        break;
    }
}

static float unit_level(float value)
{
    if (!isfinite(value) || value <= 0) {
        return 0;
    }
    return value < 1 ? value : 1;
}

void muse_pixel_render(const muse_pose_t *pose)
{
    ensure_pack();
    if (!s_player || !pose) {
        return;
    }
    fp_set_sys_state(s_player, state_for(pose->mode));
    fp_set_audio_level(s_player, (uint8_t)(unit_level(pose->level) * 100 + 0.5f));
    float happy = unit_level(pose->happy);
    if (happy > s_happy) {
        fp_notify_touch(s_player);
    }
    s_happy = happy;

    /* At most four ticks per call. A hidden/sleep gap over 132 ms resumes with
     * one tick and discards the backlog, avoiding a burst of obsolete actions. */
    uint64_t now = s_last_ms;
    if (isfinite(pose->t) && pose->t >= 0 && pose->t <= (float)UINT32_MAX) {
        now = (uint64_t)((double)pose->t * 1000 + 0.5);
    }
    uint64_t elapsed = s_clock_started && now >= s_last_ms ? now - s_last_ms : FP_TICK_MS;
    s_last_ms = now;
    s_clock_started = true;
    if (elapsed > MAX_CATCHUP_MS) {
        elapsed = FP_TICK_MS;
        s_remainder_ms = 0;
    }
    s_remainder_ms += (uint32_t)elapsed;
    while (s_remainder_ms >= FP_TICK_MS) {
        fp_tick(s_player);
        s_remainder_ms -= FP_TICK_MS;
    }
    uint32_t revision = fp_visual_revision(s_player);
    if (revision != s_native_revision) {
        s_native_revision = revision;
        s_revision++;
    }
}

uint32_t muse_pixel_revision(void)
{
    return s_revision;
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    static const uint32_t accents[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT] = 0xa9c0ff,
        [MUSE_MODE_IDLE] = 0xa77dff,
        [MUSE_MODE_LISTENING] = 0x5cb8ff,
        [MUSE_MODE_THINKING] = 0xe07bff,
        [MUSE_MODE_SPEAKING] = 0x6ff0bf,
        [MUSE_MODE_ERROR] = 0xff5c5c,
        [MUSE_MODE_OFF] = 0x7c72d0,
    };
    unsigned index = (unsigned)mode;
    return accents[index < MUSE_MODE_COUNT ? index : MUSE_MODE_IDLE];
}

void muse_pixel_set_size(int px)
{
    s_size = px < 1 ? 1 : (px > 512 ? 512 : px);
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    ensure_pack();
    if (!dst || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 ||
        x1 >= s_size || y1 >= s_size || stride_px < x1 - x0 + 1) {
        return;
    }
    const uint16_t *pixels = s_player ? fp_framebuffer(s_player) : NULL;
    unsigned width = s_player ? fp_canvas_width(s_player) : 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        for (int x = x0; x <= x1; x++) {
            /* A failed pack is unmistakable on-device even without a console. */
            dst[x - x0] = pixels ? pixels[(y * width / s_size) * width + x * width / s_size]
                : ((x / 12 + y / 12) % 2 ? 0xf800 : 0);
        }
    }
}

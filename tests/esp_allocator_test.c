#include "aipet_avatar.h"
#include "muse_pixel.h"
#include "esp_heap_caps.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void *s_raw, *s_allocation;
static unsigned s_ordinary_calls, s_aligned_calls;

static void *allocate(size_t alignment, size_t n, size_t size, uint32_t caps)
{
    assert(!s_raw && n == 1 && size == fp_arena_size());
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_raw = malloc(size + 16);
    assert(s_raw);
    uintptr_t address = ((uintptr_t)s_raw + 7u) & ~(uintptr_t)7u;
    // ESP's ordinary allocator may return a pointer aligned to 4, but not 8.
    s_allocation = (void *)(alignment == 8 ? address : address + 4u);
    memset(s_allocation, 0, size);
    return s_allocation;
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    s_ordinary_calls++;
    return allocate(4, n, size, caps);
}

void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)
{
    assert(alignment == 8);
    s_aligned_calls++;
    return allocate(alignment, n, size, caps);
}

void esp_test_free(void *pointer)
{
    if (pointer)
    {
        assert(pointer == s_allocation);
        free(s_raw);
        s_raw = s_allocation = NULL;
    }
}

// Partition and crypto APIs must remain unused when a caller supplies the pack.
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    abort();
}

int esp_partition_mmap(const esp_partition_t *part, size_t offset, size_t size,
    int memory, const void **pointer, esp_partition_mmap_handle_t *handle)
{
    (void)part;
    (void)offset;
    (void)size;
    (void)memory;
    (void)pointer;
    (void)handle;
    abort();
}

void esp_partition_munmap(esp_partition_mmap_handle_t handle)
{
    (void)handle;
    abort();
}

int psa_crypto_init(void)
{
    abort();
}

int psa_hash_compute(int algorithm, const uint8_t *input, size_t bytes,
    uint8_t *hash, size_t capacity, size_t *written)
{
    (void)algorithm;
    (void)input;
    (void)bytes;
    (void)hash;
    (void)capacity;
    (void)written;
    abort();
}

void muse_state_set_mode(muse_mode_t mode)
{
    assert(mode == MUSE_MODE_ERROR);
}

void muse_state_set_caption(const char *format, ...)
{
    assert(strcmp(format, "AIPET PACK ERROR") == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long bytes = ftell(file);
    assert(bytes > 0 && bytes <= 3000000);
    rewind(file);
    void *pack = malloc((size_t)bytes);
    assert(pack && fread(pack, 1, (size_t)bytes, file) == (size_t)bytes);
    fclose(file);

    void *ordinary = heap_caps_calloc(1, fp_arena_size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(((uintptr_t)ordinary & 7u) == 4u);
    esp_test_free(ordinary);
    for (unsigned i = 0; i < 2; i++)
    {
        assert(aipet_avatar_init(pack, (uint32_t)bytes));
        assert(((uintptr_t)s_allocation & 7u) == 0u);
        muse_pose_t pose = {.mode = MUSE_MODE_SPEAKING, .level = 1};
        muse_pixel_render(&pose);
        aipet_avatar_shutdown();
        assert(!s_raw && !s_allocation);
    }
    assert(s_ordinary_calls == 1 && s_aligned_calls == 2);
    free(pack);
    puts("ESP arena allocation passed: aligned player initialization, render and cleanup");
    return 0;
}

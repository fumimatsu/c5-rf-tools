#pragma once
#include <cstdlib>
#ifdef _MSC_VER
#include <malloc.h>
#endif
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned) {
#ifdef _MSC_VER
    return _aligned_malloc(size, alignment);
#else
    void *p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
#endif
}

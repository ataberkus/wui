#include "allocation_metrics.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {

thread_local bool g_sampling = false;
thread_local std::uint64_t g_calls = 0;
thread_local std::uint64_t g_bytes = 0;

inline void count_alloc(std::size_t bytes) noexcept {
    if (g_sampling) {
        ++g_calls;
        g_bytes += static_cast<std::uint64_t>(bytes);
    }
}

}  // namespace

namespace wui::example {

void begin_allocation_sample() noexcept {
    g_sampling = true;
    g_calls = 0;
    g_bytes = 0;
}

AllocationSample end_allocation_sample() noexcept {
    g_sampling = false;
    return AllocationSample{g_calls, g_bytes};
}

}  // namespace wui::example

void* operator new(std::size_t bytes) {
    count_alloc(bytes);
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t bytes) {
    count_alloc(bytes);
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc{};
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    count_alloc(bytes);
    return std::malloc(bytes ? bytes : 1);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    count_alloc(bytes);
    return std::malloc(bytes ? bytes : 1);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

#ifdef _WIN32
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    count_alloc(bytes);
    const std::size_t align = static_cast<std::size_t>(alignment);
    void* result = _aligned_malloc(bytes ? bytes : 1, align ? align : alignof(void*));
    if (!result) throw std::bad_alloc{};
    return result;
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    count_alloc(bytes);
    const std::size_t align = static_cast<std::size_t>(alignment);
    void* result = _aligned_malloc(bytes ? bytes : 1, align ? align : alignof(void*));
    if (!result) throw std::bad_alloc{};
    return result;
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    _aligned_free(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept {
    _aligned_free(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    _aligned_free(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    _aligned_free(pointer);
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    count_alloc(bytes);
    const std::size_t align = static_cast<std::size_t>(alignment);
    return _aligned_malloc(bytes ? bytes : 1, align ? align : alignof(void*));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    count_alloc(bytes);
    const std::size_t align = static_cast<std::size_t>(alignment);
    return _aligned_malloc(bytes ? bytes : 1, align ? align : alignof(void*));
}

void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    _aligned_free(pointer);
}

void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    _aligned_free(pointer);
}
#else
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    count_alloc(bytes);
    void* result = nullptr;
    const std::size_t align = static_cast<std::size_t>(alignment);
    if (::posix_memalign(&result, align ? align : alignof(void*), bytes ? bytes : 1) != 0) {
        throw std::bad_alloc{};
    }
    return result;
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    count_alloc(bytes);
    void* result = nullptr;
    const std::size_t align = static_cast<std::size_t>(alignment);
    if (::posix_memalign(&result, align ? align : alignof(void*), bytes ? bytes : 1) != 0) {
        throw std::bad_alloc{};
    }
    return result;
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
#endif

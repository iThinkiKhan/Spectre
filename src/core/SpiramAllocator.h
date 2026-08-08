#pragma once

#include <esp_heap_caps.h>
#include <cstddef>
#include <cstdlib>
#include <vector>

template <typename T>
struct SpiramStlAllocator {
    using value_type = T;

    SpiramStlAllocator() noexcept = default;

    template <class U>
    SpiramStlAllocator(const SpiramStlAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n > (SIZE_MAX / sizeof(T))) {
            std::abort();
        }
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (!p) {
            p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
        }
        if (!p) {
            std::abort();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) noexcept {
        heap_caps_free(p);
    }

    template <class U>
    struct rebind {
        using other = SpiramStlAllocator<U>;
    };
};

template <class T, class U>
bool operator==(const SpiramStlAllocator<T>&,
                const SpiramStlAllocator<U>&) noexcept {
    return true;
}

template <class T, class U>
bool operator!=(const SpiramStlAllocator<T>&,
                const SpiramStlAllocator<U>&) noexcept {
    return false;
}

template <typename T>
using SpiramVector = std::vector<T, SpiramStlAllocator<T>>;

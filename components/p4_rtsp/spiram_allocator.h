#pragma once

#include <cstddef>
#include <cstdlib>

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace p4_rtsp {

template<class T> class SpiramAllocator {
 public:
  using value_type = T;

  constexpr SpiramAllocator() noexcept = default;
  template<class U> constexpr SpiramAllocator(const SpiramAllocator<U> &) noexcept {}

  T *allocate(std::size_t n) {
    if (n == 0) {
      return nullptr;
    }
    size_t size = n * sizeof(T);
#ifdef USE_ESP32
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == nullptr) {
      ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
#else
    void *ptr = malloc(size);
#endif
    return static_cast<T *>(ptr);
  }

  void deallocate(T *ptr, std::size_t) noexcept {
#ifdef USE_ESP32
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
  }
};

template<class T, class U>
constexpr bool operator==(const SpiramAllocator<T> &, const SpiramAllocator<U> &) noexcept {
  return true;
}
template<class T, class U>
constexpr bool operator!=(const SpiramAllocator<T> &, const SpiramAllocator<U> &) noexcept {
  return false;
}

}  // namespace p4_rtsp
}  // namespace esphome

// app/debug_heap.cpp — temporary diagnostics allocator (MAG_DEBUG_HEAP=1).
//
// Interposes global new/delete with a header+canary scheme:
//   [16-byte size header][user block][32-byte canary 0xC5]
// delete checks the canary before freeing (overflow -> who was freed),
// and free fills the user block with 0xDD, so a read through a dangling
// pointer faults at an unmistakable 0xDDDD... address instead of silently
// reading whatever reused the block. Diagnosis for the resolution-change
// heap corruption; not compiled into the shipped binaries.

#if MAG_DEBUG_HEAP

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

constexpr std::size_t kHeader = 16;
constexpr std::size_t kCanary = 32;
constexpr unsigned char kByteFree = 0xDD;
constexpr unsigned char kByteNew = 0xAB;
constexpr unsigned char kByteCanary = 0xC5;

std::atomic<unsigned long long> g_allocs{0};

void check_canary(unsigned char* user, std::size_t size, void* raw) {
    const unsigned char* canary = user + size;
    for (std::size_t i = 0; i < kCanary; ++i) {
        if (canary[i] != kByteCanary) {
            fprintf(stderr,
                    "[HEAP] canary smashed: user=%p size=%zu alloc=%llu "
                    "(first bad byte +%zu, was %02X)\n",
                    (void*)user, size, g_allocs.load(), i, canary[i]);
            fflush(stderr);
            break;
        }
    }
}

void* alloc(std::size_t n) {
    unsigned char* raw = static_cast<unsigned char*>(std::malloc(n + kHeader + kCanary));
    if (raw == nullptr) {
        throw std::bad_alloc{};
    }
    *reinterpret_cast<unsigned long long*>(raw) = n;
    unsigned char* user = raw + kHeader;
    std::memset(user, kByteNew, n);
    std::memset(user + n, kByteCanary, kCanary);
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return user;
}

void free_ptr(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    unsigned char* user = static_cast<unsigned char*>(p);
    unsigned char* raw = user - kHeader;
    const std::size_t size = *reinterpret_cast<unsigned long long*>(raw);
    check_canary(user, size, raw);
    std::memset(user, kByteFree, size);
    std::memset(user + size, kByteFree, kCanary);
    std::free(raw);
}

}  // namespace

void* operator new(std::size_t n) {
    return alloc(n);
}
void* operator new[](std::size_t n) {
    return alloc(n);
}
void operator delete(void* p) noexcept {
    free_ptr(p);
}
void operator delete[](void* p) noexcept {
    free_ptr(p);
}
void operator delete(void* p, std::size_t) noexcept {
    free_ptr(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    free_ptr(p);
}

// Over-aligned allocations bypass the header (plain allocator, no checks) --
// nothing in this program is known to use them, and a wrong assumption here
// would corrupt the header arithmetic for everything else.
void* operator new(std::size_t n, const std::align_val_t) {
    void* p = std::malloc(n);
    if (p == nullptr) throw std::bad_alloc{};
    return p;
}
void operator delete(void* p, const std::align_val_t) noexcept {
    std::free(p);
}
void* operator new[](std::size_t n, const std::align_val_t) {
    void* p = std::malloc(n);
    if (p == nullptr) throw std::bad_alloc{};
    return p;
}
void operator delete[](void* p, const std::align_val_t) noexcept {
    std::free(p);
}

#endif  // MAG_DEBUG_HEAP

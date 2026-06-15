/*
 *  jit-target-cache.hpp — AArch64 JIT code cache management
 *
 *  AArch64 requires explicit cache maintenance after writing code:
 *    1. DC CVAU  — clean data cache line to Point of Unification
 *    2. DSB ISH  — ensure clean completes
 *    3. IC IVAU  — invalidate instruction cache line
 *    4. DSB ISH  — ensure invalidation completes
 *    5. ISB      — synchronize instruction stream
 */

#ifndef JIT_TARGET_CACHE_HPP
#define JIT_TARGET_CACHE_HPP

#include <stdint.h>
#include <sys/mman.h>

/* ---- W^X / write-protect policy ----------------------------------------
 *
 * macOS arm64 enforces W^X: anonymous RWX mappings are rejected with EPERM.
 * The supported route is MAP_JIT (no entitlement needed for ad-hoc-signed
 * builds) plus per-thread write-protect toggling: pthread_jit_write_protect_np(0)
 * before writing code, pthread_jit_write_protect_np(1) afterwards, then an
 * sys_icache_invalidate() for instruction-cache coherence.
 *
 * Linux arm64 keeps its existing behavior byte-for-byte: a plain
 * MAP_PRIVATE|MAP_ANONYMOUS RWX mapping and the manual DC CVAU / IC IVAU
 * cache maintenance in jit_cache_flush(). The begin/end-write hooks are
 * no-ops there, and jit_cache_flush() (called from the same write sites)
 * performs the icache maintenance exactly as before. */
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#define JIT_CACHE_MAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT)
static inline void jit_cache_begin_write(void) {
    pthread_jit_write_protect_np(0); /* make JIT region writable on this thread */
}
static inline void jit_cache_end_write(void *addr, size_t len) {
    pthread_jit_write_protect_np(1); /* make JIT region executable again */
    sys_icache_invalidate(addr, len);
}
#else
#define JIT_CACHE_MAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
static inline void jit_cache_begin_write(void) {}
static inline void jit_cache_end_write(void *addr, size_t len) {
    /* Linux arm64: icache maintenance is performed by jit_cache_flush(),
     * which the write sites already call. Nothing to do here. */
    (void)addr; (void)len;
}
#endif

static inline void jit_cache_flush(void *start, size_t length) {
#if defined(__APPLE__) && defined(__aarch64__)
    /* On Apple, instruction-cache coherence is handled by sys_icache_invalidate()
     * inside jit_cache_end_write(). Avoid issuing the privileged DC CVAU / IC IVAU
     * maintenance here (and avoid double-flushing). */
    (void)start; (void)length;
    return;
#else
    uintptr_t addr = (uintptr_t)start;
    uintptr_t end = addr + length;

    /* AArch64 cache line size is typically 64 bytes, but query it at runtime
       for correctness. For now, use 64 as a safe default. */
    const uintptr_t line_size = 64;
    const uintptr_t line_mask = ~(line_size - 1);

    /* Clean data cache */
    for (uintptr_t a = addr & line_mask; a < end; a += line_size)
        __asm__ volatile("dc cvau, %0" :: "r"(a));

    __asm__ volatile("dsb ish");

    /* Invalidate instruction cache */
    for (uintptr_t a = addr & line_mask; a < end; a += line_size)
        __asm__ volatile("ic ivau, %0" :: "r"(a));

    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
#endif
}

static inline void *jit_cache_alloc(size_t size) {
    void *p = mmap(NULL, size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   JIT_CACHE_MAP_FLAGS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

static inline void jit_cache_free(void *p, size_t size) {
    if (p) munmap(p, size);
}

#endif /* JIT_TARGET_CACHE_HPP */

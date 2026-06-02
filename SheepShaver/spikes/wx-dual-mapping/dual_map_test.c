/*
 * dual_map_test.c — empirical W^X dual-mapping spike for SheepShaver C4.
 *
 * Question: can we drop pthread_jit_write_protect_np toggling on macOS arm64
 * by keeping separate RW and RX mappings of the same physical code cache?
 *
 * Each subtest runs in a FORKED CHILD, because the likely failure mode on a
 * hardened OS is a fatal signal at execute time (SIGBUS / codesign kill),
 * not an errno — perror+continue cannot survive that. The parent reports the
 * child's exit status: exit(42) = works (incl. rewrite-coherence), other exit
 * = soft failure, killed-by-signal N = hard blocked.
 *
 * Correctness probe (the one that discriminates):
 *   1. write fn returning 42 via W alias -> invalidate X alias -> exec -> expect 42
 *   2. OVERWRITE same slot with fn returning 99 via W alias -> invalidate -> exec -> expect 99
 * The JIT's real workload is code patching, so rewrite-coherence is the property.
 *
 * Build: make    Run: ./dual_map_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>

#define SLOT 4096
typedef int (*fn_t)(void);

/* ARM64: MOV w0, #imm ; RET  -> two 32-bit little-endian words */
static void emit_ret_const(uint32_t *w, int imm) {
    /* MOVZ w0, #imm  = 0x52800000 | (imm<<5) */
    w[0] = 0x52800000u | ((uint32_t)(imm & 0xffff) << 5);
    w[1] = 0xd65f03c0u; /* RET */
}

/* ---- approach a: oaknut macOS method (vm_remap, NO MAP_JIT, NO toggle) ---- */
static int test_vm_remap(void) {
    uint32_t *wmem = mmap(NULL, SLOT, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (wmem == MAP_FAILED) { perror("a: mmap RW"); return 1; }

    uint32_t *xmem = NULL;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), (vm_address_t *)&xmem, SLOT, 0,
                                VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR,
                                mach_task_self(), (mach_vm_address_t)wmem, false,
                                &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "a: vm_remap kr=%d\n", kr); return 2; }
    if (mprotect(xmem, SLOT, PROT_READ | PROT_EXEC) != 0) { perror("a: mprotect RX"); return 3; }

    emit_ret_const(wmem, 42);
    sys_icache_invalidate(xmem, SLOT);            /* invalidate the X alias */
    int r1 = ((fn_t)xmem)();
    if (r1 != 42) { fprintf(stderr, "a: first exec got %d\n", r1); return 4; }

    emit_ret_const(wmem, 99);                     /* REWRITE via W alias */
    sys_icache_invalidate(xmem, SLOT);
    int r2 = ((fn_t)xmem)();
    if (r2 != 99) { fprintf(stderr, "a: rewrite exec got %d (no coherence)\n", r2); return 5; }
    return 42;
}

/* ---- approach b: MAP_SHARED on shm fd, two mappings ---- */
static int test_shm_shared(void) {
    char name[64];
    snprintf(name, sizeof name, "/wxdual_%d", (int)getpid());
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("b: shm_open"); return 1; }
    shm_unlink(name);
    if (ftruncate(fd, SLOT) != 0) { perror("b: ftruncate"); close(fd); return 2; }

    uint32_t *wmem = mmap(NULL, SLOT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint32_t *xmem = mmap(NULL, SLOT, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (wmem == MAP_FAILED) { perror("b: mmap RW"); close(fd); return 3; }
    if (xmem == MAP_FAILED) { perror("b: mmap RX"); close(fd); return 4; }

    emit_ret_const(wmem, 42);
    sys_icache_invalidate(xmem, SLOT);
    int r1 = ((fn_t)xmem)();
    if (r1 != 42) { fprintf(stderr, "b: first exec got %d\n", r1); return 5; }

    emit_ret_const(wmem, 99);
    sys_icache_invalidate(xmem, SLOT);
    int r2 = ((fn_t)xmem)();
    if (r2 != 99) { fprintf(stderr, "b: rewrite exec got %d\n", r2); return 6; }
    close(fd);
    return 42;
}

/* ---- approach a2: MAP_JIT RX first, vm_remap to RW alias (task's 2a wording) -- */
static int test_mapjit_remap(void) {
    uint32_t *xmem = mmap(NULL, SLOT, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (xmem == MAP_FAILED) { perror("a2: mmap MAP_JIT"); return 1; }

    uint32_t *wmem = NULL;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), (vm_address_t *)&wmem, SLOT, 0,
                                VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR,
                                mach_task_self(), (mach_vm_address_t)xmem, false,
                                &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "a2: vm_remap kr=%d\n", kr); return 2; }
    if (mprotect(wmem, SLOT, PROT_READ | PROT_WRITE) != 0) { perror("a2: mprotect RW"); return 3; }

    emit_ret_const(wmem, 42);                     /* write via RW alias, no toggle */
    sys_icache_invalidate(xmem, SLOT);
    int r1 = ((fn_t)xmem)();
    if (r1 != 42) { fprintf(stderr, "a2: first exec got %d\n", r1); return 4; }

    emit_ret_const(wmem, 99);
    sys_icache_invalidate(xmem, SLOT);
    int r2 = ((fn_t)xmem)();
    if (r2 != 99) { fprintf(stderr, "a2: rewrite exec got %d\n", r2); return 5; }
    return 42;
}

/* ---- control c: MAP_JIT + pthread_jit_write_protect_np toggle (today) ---- */
static int test_mapjit_toggle(void) {
    uint32_t *mem = mmap(NULL, SLOT, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (mem == MAP_FAILED) { perror("c: mmap MAP_JIT"); return 1; }

    pthread_jit_write_protect_np(0);
    emit_ret_const(mem, 42);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(mem, SLOT);
    int r1 = ((fn_t)mem)();
    if (r1 != 42) { fprintf(stderr, "c: first exec got %d\n", r1); return 2; }

    pthread_jit_write_protect_np(0);
    emit_ret_const(mem, 99);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(mem, SLOT);
    int r2 = ((fn_t)mem)();
    if (r2 != 99) { fprintf(stderr, "c: rewrite exec got %d\n", r2); return 3; }
    return 42;
}

/* ---- run a subtest in a forked child; classify the result ---- */
static int run_child(const char *label, int (*fn)(void), int *works_out) {
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) { _exit(fn()); }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 42) { printf("  [%s] WORKS (write+exec+rewrite-coherent)\n", label); *works_out = 1; return 0; }
        printf("  [%s] SOFT FAIL (exit code %d — see stderr above)\n", label, code);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("  [%s] BLOCKED — killed by signal %d (%s)\n", label, sig, strsignal(sig));
    } else {
        printf("  [%s] unknown wait status 0x%x\n", label, status);
    }
    *works_out = 0;
    return -1;
}

/* ---- benchmark: write-4-bytes + invalidate + execute, N iters ---- */
#define ITERS 100000

static double bench_dual(void) {
    uint32_t *wmem = mmap(NULL, SLOT, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint32_t *xmem = NULL; vm_prot_t cur, max;
    vm_remap(mach_task_self(), (vm_address_t *)&xmem, SLOT, 0,
             VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR, mach_task_self(),
             (mach_vm_address_t)wmem, false, &cur, &max, VM_INHERIT_NONE);
    mprotect(xmem, SLOT, PROT_READ | PROT_EXEC);
    fn_t f = (fn_t)xmem;
    volatile int sink = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) {
        emit_ret_const(wmem, i & 0xffff);
        sys_icache_invalidate(xmem, 8);
        sink += f();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void)sink;
    return (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
}

static double bench_toggle(void) {
    uint32_t *mem = mmap(NULL, SLOT, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    fn_t f = (fn_t)mem;
    volatile int sink = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) {
        pthread_jit_write_protect_np(0);
        emit_ret_const(mem, i & 0xffff);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(mem, 8);
        sink += f();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void)sink;
    return (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
}

int main(void) {
    printf("=== W^X dual-mapping spike (macOS arm64) ===\n");
    printf("ITERS per bench = %d, slot = %d bytes\n\n", ITERS, SLOT);

    printf("Correctness (each in forked child):\n");
    int wa = 0, wb = 0, wa2 = 0, wc = 0;
    run_child("a  vm_remap (oaknut, no MAP_JIT, no toggle)", test_vm_remap, &wa);
    run_child("b  shm MAP_SHARED dual mmap", test_shm_shared, &wb);
    run_child("a2 MAP_JIT RX + vm_remap RW alias", test_mapjit_remap, &wa2);
    run_child("c  CONTROL: MAP_JIT + jit_write_protect toggle", test_mapjit_toggle, &wc);

    printf("\nPerformance (only meaningful for approaches that WORK):\n");
    if (wc) {
        double tg = bench_toggle();
        printf("  toggle (today):    %.1f ms total, %.1f ns/iter\n", tg / 1e6, tg / ITERS);
    } else {
        printf("  toggle control did not work; skipping its bench\n");
    }
    if (wa) {
        double td = bench_dual();
        printf("  dual vm_remap:     %.1f ms total, %.1f ns/iter\n", td / 1e6, td / ITERS);
    } else {
        printf("  vm_remap approach did not work; skipping its bench\n");
    }
    printf("\nNote: both approaches pay sys_icache_invalidate per iter; the only\n");
    printf("delta dual-mapping removes is the two pthread_jit_write_protect_np calls.\n");

    printf("\nHEADLINE: vm_remap=%s shm=%s mapjit_remap=%s control=%s\n",
           wa ? "WORKS" : "no", wb ? "WORKS" : "no",
           wa2 ? "WORKS" : "no", wc ? "WORKS" : "no");
    return 0;
}

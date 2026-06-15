// Spike S2 — Mach fault-decode keystone proof (MACHINE-LAYER-PLAN.md §3 / §2b).
//
// Proves end-to-end on macOS arm64:
//   1. An unmapped (PROT_NONE) page inside a reservation traps to a Mach
//      exception handler thread (EXC_BAD_ACCESS, EXCEPTION_DEFAULT |
//      MACH_EXCEPTION_CODES — same shape as sigsegv.cpp's machinery).
//   2. The handler reads the faulting thread's ARM_THREAD_STATE64, fetches the
//      instruction word at PC, decodes the JIT-emitted access form
//      LDR Wt, [Xn, Wm, UXTW]   (0xB8604800 | rm<<16 | rn<<5 | rt)
//      exactly as emitted by ppc-jit.cpp / ppc-codegen-aarch64.h.
//   3. It injects a RAW BIG-ENDIAN value into Wt via thread_set_state,
//      advances PC by 4 (past the LDR only — the following REV must execute),
//      and resumes.
//   4. The main thread observes bswap32(raw) — proving the §2b endianness
//      contract ("injected value must be raw big-endian, because the JIT's
//      REV executes after the resumed load").
//   5. Benchmarks N faulting round-trips vs the same loop on a mapped page.
//   6. Repeats the single-shot test with the load executing from a
//      MAP_JIT + pthread_jit_write_protect_np page (the real JIT's home).
//
// Standalone: does NOT modify or link any SheepShaver source.
// Build: see Makefile (clang++ + MIG-generated mach_excServer.c).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <atomic>

#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <time.h>

extern "C" {
#include <mach/mach.h>
#include <mach/mach_error.h>
// MIG-generated (see Makefile): dispatches EXC messages to our catch_* below.
boolean_t mach_exc_server(mach_msg_header_t *in, mach_msg_header_t *out);
}

#define CHECK_MACH(name, kr)                                                  \
    do {                                                                      \
        if ((kr) != KERN_SUCCESS) {                                           \
            fprintf(stderr, "FATAL: %s: %s (%d)\n", name,                     \
                    mach_error_string(kr), (int)(kr));                        \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// The JIT-form code under test (page-table / linker text section variant).
// Mirrors ppc-jit.cpp case 32 (lwz): LDR Wt,[Xn,Wm,UXTW] ; REV Wt,Wt.
//   x0 = host base (RMEMBASE analogue), w1 = 32-bit guest EA (RTMP0 analogue)
//   returns w0 = REV(loaded word)
// Hand-assembled with .inst so the bytes are EXACTLY the JIT encodings.
// ---------------------------------------------------------------------------
extern "C" uint32_t jit_form_load(const void *base, uint32_t off);
__asm__(
    ".text\n"
    ".globl _jit_form_load\n"
    ".p2align 2\n"
    "_jit_form_load:\n"
    ".inst 0xB8614800\n" // ldr w0, [x0, w1, uxtw]   (a64_ldr_w_reg(0,0,1))
    ".inst 0x5AC00800\n" // rev w0, w0               (JIT's bswap)
    "ret\n");

// Same three instructions, copied into a MAP_JIT page at runtime.
static const uint32_t kJitFormInsns[3] = {0xB8614800, 0x5AC00800, 0xD65F03C0};

// ---------------------------------------------------------------------------
// Globals shared with the exception handler.
// ---------------------------------------------------------------------------
static mach_port_t g_exc_port = MACH_PORT_NULL;
static const uint32_t kInjectRaw = 0x12345678;     // raw value the "device" returns
static std::atomic<uint64_t> g_fault_count{0};
static std::atomic<uint64_t> g_decode_fail{0};
static uintptr_t g_trap_lo = 0, g_trap_hi = 0;     // trapped range, for sanity

// ---------------------------------------------------------------------------
// MIG server callbacks. Behavior is EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
// so only catch_mach_exception_raise is live; the other two are link stubs
// (same layout as sigsegv.cpp).
// ---------------------------------------------------------------------------
extern "C" kern_return_t catch_mach_exception_raise(
    mach_port_t exception_port, mach_port_t thread, mach_port_t task,
    exception_type_t exception, mach_exception_data_t code,
    mach_msg_type_number_t code_count)
{
    (void)exception_port; (void)task;
    if (exception != EXC_BAD_ACCESS)
        return KERN_FAILURE; // not ours — let it crash loudly

    // Sanity: code[1] is the faulting data address under MACH_EXCEPTION_CODES.
    uint64_t fault_addr = (code_count >= 2) ? (uint64_t)code[1] : 0;
    if (g_trap_lo && (fault_addr < g_trap_lo || fault_addr >= g_trap_hi)) {
        fprintf(stderr, "FATAL: EXC_BAD_ACCESS outside trap window: %#" PRIx64 "\n",
                fault_addr);
        return KERN_FAILURE;
    }

    // --- thread_get_state (exactly the sigsegv.cpp pattern) ---
    arm_thread_state64_t ts;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(thread, ARM_THREAD_STATE64,
                                        (thread_state_t)&ts, &count);
    if (kr != KERN_SUCCESS) return KERN_FAILURE;

    // --- fetch + decode the faulting instruction ---
    // Same task: the handler thread can read the code page directly.
    // (arm_thread_state64_get_pc strips PAC if present; plain __pc also works
    //  for our unsigned spike, but use the API for correctness.)
    uint64_t pc = arm_thread_state64_get_pc(ts);
    uint32_t insn = *(const uint32_t *)pc;

    // Decode LDR Wt, [Xn, Wm, UXTW]: fixed bits mask 0xFFE0FC00 == 0xB8604800.
    // (size=10 opc=01 register-offset, option=010 UXTW, S=0)
    if ((insn & 0xFFE0FC00) == 0xB8604800) {
        unsigned rt = insn & 0x1F;
        // unsigned rn = (insn >> 5) & 0x1F;   // base — bus would use these +
        // unsigned rm = (insn >> 16) & 0x1F;  // offset to compute device addr
        if (rt != 31) {
            // LDR W zero-extends into the X register: write 64-bit zero-extended.
            // ENDIANNESS CONTRACT: inject RAW (big-endian) — the REV at pc+4
            // executes after resume and performs the byte swap.
            ts.__x[rt] = (uint64_t)kInjectRaw;
        }
        arm_thread_state64_set_pc_fptr(ts, (void *)(pc + 4)); // skip LDR ONLY
        g_fault_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_decode_fail.fetch_add(1, std::memory_order_relaxed);
        fprintf(stderr, "FATAL: undecodable insn %#010x at pc=%#" PRIx64
                        " (fault addr %#" PRIx64 ")\n", insn, pc, fault_addr);
        return KERN_FAILURE; // abort-loudly rule (§2b host-accessor contract)
    }

    // --- thread_set_state, then KERN_SUCCESS => kernel resumes the thread ---
    kr = thread_set_state(thread, ARM_THREAD_STATE64, (thread_state_t)&ts,
                          ARM_THREAD_STATE64_COUNT);
    if (kr != KERN_SUCCESS) return KERN_FAILURE;
    return KERN_SUCCESS;
}

extern "C" kern_return_t catch_mach_exception_raise_state(
    mach_port_t, exception_type_t, const mach_exception_data_t,
    mach_msg_type_number_t, int *, const thread_state_t,
    mach_msg_type_number_t, thread_state_t, mach_msg_type_number_t *)
{
    return KERN_FAILURE; // not used (EXCEPTION_DEFAULT behavior)
}

extern "C" kern_return_t catch_mach_exception_raise_state_identity(
    mach_port_t, mach_port_t, mach_port_t, exception_type_t,
    mach_exception_data_t, mach_msg_type_number_t, int *, thread_state_t,
    mach_msg_type_number_t, thread_state_t, mach_msg_type_number_t *)
{
    return KERN_FAILURE;
}

// Handler thread loop — same shape as sigsegv.cpp handleExceptions().
static void *exception_thread(void *)
{
    static char msgbuf[512], replybuf[512];
    mach_msg_header_t *msg = (mach_msg_header_t *)msgbuf;
    mach_msg_header_t *reply = (mach_msg_header_t *)replybuf;
    for (;;) {
        kern_return_t kr = mach_msg(msg, MACH_RCV_MSG, 0, sizeof(msgbuf),
                                    g_exc_port, MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        CHECK_MACH("mach_msg(recv)", kr);
        if (!mach_exc_server(msg, reply)) {
            fprintf(stderr, "FATAL: mach_exc_server rejected message\n");
            exit(1);
        }
        kr = mach_msg(reply, MACH_SEND_MSG, reply->msgh_size, 0,
                      msg->msgh_remote_port, MACH_MSG_TIMEOUT_NONE,
                      MACH_PORT_NULL);
        CHECK_MACH("mach_msg(reply)", kr);
    }
    return nullptr;
}

static void install_handler_for_current_thread()
{
    kern_return_t kr;
    if (g_exc_port == MACH_PORT_NULL) {
        kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                                &g_exc_port);
        CHECK_MACH("mach_port_allocate", kr);
        kr = mach_port_insert_right(mach_task_self(), g_exc_port, g_exc_port,
                                    MACH_MSG_TYPE_MAKE_SEND);
        CHECK_MACH("mach_port_insert_right", kr);
        pthread_t t;
        if (pthread_create(&t, nullptr, exception_thread, nullptr) != 0) {
            perror("pthread_create");
            exit(1);
        }
        pthread_detach(t);
    }
    // Thread-level port, like sigsegv.cpp (not task-level — avoids fights
    // with crash reporters/debugger task ports).
    kr = thread_set_exception_ports(
        mach_thread_self(), EXC_MASK_BAD_ACCESS, g_exc_port,
        EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    CHECK_MACH("thread_set_exception_ports", kr);
}

static uint64_t now_ns()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

int main(int argc, char **argv)
{
    const size_t page = (size_t)getpagesize();
    uint64_t N = (argc > 1) ? strtoull(argv[1], nullptr, 0) : 100000;
    printf("S2 Mach fault-decode spike — page size %zu, N=%" PRIu64 "\n",
           page, N);

    // (1) Reservation: 3 pages. Page 0 mapped RW (baseline), page 1 PROT_NONE
    // (the "device" page — mimics an unmapped MMIO hole in NATMEM), page 2 RW.
    uint8_t *resv = (uint8_t *)mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE,
                                    MAP_ANON | MAP_PRIVATE, -1, 0);
    if (resv == MAP_FAILED) { perror("mmap reservation"); return 1; }
    if (mprotect(resv + page, page, PROT_NONE) != 0) {
        perror("mprotect PROT_NONE"); return 1;
    }
    g_trap_lo = (uintptr_t)(resv + page);
    g_trap_hi = (uintptr_t)(resv + 2 * page);

    // Seed the mapped baseline page with a known big-endian word.
    const uint32_t baseline_raw = 0xAABBCCDD;
    memcpy(resv + 0x40, &baseline_raw, 4);

    // (2) Mach exception handler for EXC_BAD_ACCESS on this (main) thread.
    install_handler_for_current_thread();

    // ---- Test A: mapped page, no fault (sanity for the asm form itself) ----
    uint32_t v = jit_form_load(resv, 0x40);
    printf("A. mapped-load sanity:    got %#010x expect %#010x  %s\n",
           v, __builtin_bswap32(baseline_raw),
           v == __builtin_bswap32(baseline_raw) ? "PASS" : "FAIL");
    if (v != __builtin_bswap32(baseline_raw)) return 1;

    // ---- Test B: the keystone — fault, decode, inject, resume, observe ----
    // (3)(4)(5): load targets the PROT_NONE page; handler injects raw
    // 0x12345678 into Wt and skips the LDR; the REV then runs for real.
    const uint32_t expect = __builtin_bswap32(kInjectRaw); // 0x78563412
    v = jit_form_load(resv, (uint32_t)page + 0x10);
    printf("B. fault+inject (text):   got %#010x expect %#010x  %s "
           "(faults=%" PRIu64 ")\n",
           v, expect, v == expect ? "PASS" : "FAIL",
           g_fault_count.load());
    if (v != expect) return 1;

    // ---- Test C: same, executing from a MAP_JIT page (known unknown) ----
    void *jitpage = mmap(nullptr, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
    if (jitpage == MAP_FAILED) {
        perror("mmap MAP_JIT");
        printf("C. MAP_JIT:               FAIL (mmap MAP_JIT refused — "
               "entitlement/hardened-runtime issue?)\n");
        return 1;
    }
    pthread_jit_write_protect_np(0);
    memcpy(jitpage, kJitFormInsns, sizeof(kJitFormInsns));
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(jitpage, sizeof(kJitFormInsns));
    auto jit_load = (uint32_t (*)(const void *, uint32_t))jitpage;

    v = jit_load(resv, 0x40); // mapped sanity from MAP_JIT
    bool c1 = (v == __builtin_bswap32(baseline_raw));
    v = jit_load(resv, (uint32_t)page + 0x10); // faulting from MAP_JIT
    bool c2 = (v == expect);
    printf("C. MAP_JIT mapped/fault:  %s / %s (got %#010x)\n",
           c1 ? "PASS" : "FAIL", c2 ? "PASS" : "FAIL", v);
    if (!c1 || !c2) return 1;

    // ---- (6) Benchmark: N faulting round-trips vs mapped baseline ----
    // Warm up.
    for (int i = 0; i < 1000; i++) (void)jit_form_load(resv, 0x40);

    uint64_t t0 = now_ns();
    uint64_t acc = 0;
    for (uint64_t i = 0; i < N; i++)
        acc += jit_form_load(resv, 0x40);
    uint64_t mapped_ns = now_ns() - t0;

    uint64_t faults_before = g_fault_count.load();
    t0 = now_ns();
    for (uint64_t i = 0; i < N; i++)
        acc += jit_form_load(resv, (uint32_t)page + 0x10);
    uint64_t fault_ns = now_ns() - t0;
    uint64_t faults_done = g_fault_count.load() - faults_before;

    printf("D. bench (N=%" PRIu64 "):\n", N);
    printf("   mapped baseline:  %8.1f ns/load  (total %.3f ms)\n",
           (double)mapped_ns / N, mapped_ns / 1e6);
    printf("   fault round-trip: %8.1f ns/fault (total %.3f ms, faults "
           "serviced=%" PRIu64 ")\n",
           (double)fault_ns / N, fault_ns / 1e6, faults_done);
    printf("   slowdown:         %8.0fx\n",
           (double)fault_ns / (double)(mapped_ns ? mapped_ns : 1));
    if (faults_done != N) {
        printf("   WARNING: fault count mismatch (%" PRIu64 " != %" PRIu64 ")\n",
               faults_done, N);
        return 1;
    }
    (void)acc;

    printf("ALL PASS (decode failures: %" PRIu64 ")\n", g_decode_fail.load());
    return 0;
}

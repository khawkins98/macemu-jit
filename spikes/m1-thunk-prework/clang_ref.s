// Reference instructions for the clang cross-check. Order MUST match the
// xchecks[] table in thunk_ref.c. Assembled by the Makefile; the 32-bit words
// are extracted and diffed against the helper-produced encodings.
.text
.globl _xcheck_ref
.p2align 2
_xcheck_ref:
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x26, x27, [sp, #208]
    ldp x0, x1, [sp, #0]
    ldp x26, x27, [sp, #208]
    str x28, [sp, #224]
    ldr x28, [sp, #224]
    stp q0, q1, [sp, #240]
    stp q30, q31, [sp, #592]
    ldp q0, q1, [sp, #240]
    sub sp, sp, #624
    add sp, sp, #624
    mov x0, sp
    sub x1, x30, #4
    stp x29, x30, [sp, #-16]!
    ldp x29, x30, [sp], #16
    blr x16
    ret
    // --- sketch-expression checks: clang assembles the INTENDED instruction;
    //     the helper column carries the sketch's literal value. MATCH => the
    //     sketch line was already correct; MISMATCH => the sketch line is buggy.
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x26, x27, [sp, #208]
    str x28, [sp, #224]
    stp q0, q1, [sp, #240]

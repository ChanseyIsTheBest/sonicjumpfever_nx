/* sj_calltrace_stubs.s -- one stub per imported function (sj_calltrace.c).
 * MIT licensed, see LICENSE.
 *
 * The game's PLT loads its GOT slot into x17 and branches to it, so a stub
 * runs with the caller's arguments in x0-x7 / q0-q7, an indirect-result
 * pointer in x8, stack arguments at [sp] and the return address in x30.
 * Every one of those is saved, sj_ct_hit(index, saved) logs the first call
 * and returns the real target, everything is restored, and the stub BRANCHES
 * (never calls) to the target: the real function sees exactly the frame it
 * would have seen without us -- which is what setjmp and variadic functions
 * need. x16/x17 are the AAPCS64 intra-call scratch registers the PLT already
 * clobbers, so using x16 costs the caller nothing. */

    .text
    .align 4
    .global sj_ct_stubs
sj_ct_stubs:
    .set i, 0
    .rept 512
    mov  x16, #i
    b    sj_ct_common
    .set i, i + 1
    .endr

    .align 4
    .type sj_ct_common, %function
sj_ct_common:
    sub  sp, sp, #208
    stp  x0, x1, [sp, #0]
    stp  x2, x3, [sp, #16]
    stp  x4, x5, [sp, #32]
    stp  x6, x7, [sp, #48]
    stp  x8, x30, [sp, #64]
    stp  q0, q1, [sp, #80]
    stp  q2, q3, [sp, #112]
    stp  q4, q5, [sp, #144]
    stp  q6, q7, [sp, #176]
    mov  x0, x16
    mov  x1, sp
    bl   sj_ct_hit
    mov  x16, x0
    ldp  q6, q7, [sp, #176]
    ldp  q4, q5, [sp, #144]
    ldp  q2, q3, [sp, #112]
    ldp  q0, q1, [sp, #80]
    ldp  x8, x30, [sp, #64]
    ldp  x6, x7, [sp, #48]
    ldp  x4, x5, [sp, #32]
    ldp  x2, x3, [sp, #16]
    ldp  x0, x1, [sp, #0]
    add  sp, sp, #208
    br   x16
    .size sj_ct_common, . - sj_ct_common

/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * crt_setjmp and crt_longjmp (crt.h) for the x64 calling convention of UEFI and Windows: the first argument is in
 * rcx, the second in rdx. The callee-saved registers are rbx, rbp, rdi, rsi, r12-r15 and xmm6-xmm15. The slots of
 * crt_jmp_buf: 0-7 the general registers, 8 the stack pointer after the return, 9 the return address, 10-29 the
 * ten xmm registers (two slots each).
 */
#include "crt.h"

__attribute__((naked, returns_twice)) int crt_setjmp(__attribute__((unused)) crt_jmp_buf *buffer)
{
	__asm__ volatile(
		"movq %rbx, 0(%rcx)\n\t"
		"movq %rbp, 8(%rcx)\n\t"
		"movq %rdi, 16(%rcx)\n\t"
		"movq %rsi, 24(%rcx)\n\t"
		"movq %r12, 32(%rcx)\n\t"
		"movq %r13, 40(%rcx)\n\t"
		"movq %r14, 48(%rcx)\n\t"
		"movq %r15, 56(%rcx)\n\t"
		"leaq 8(%rsp), %rdx\n\t"
		"movq %rdx, 64(%rcx)\n\t"
		"movq (%rsp), %rdx\n\t"
		"movq %rdx, 72(%rcx)\n\t"
		"movdqu %xmm6, 80(%rcx)\n\t"
		"movdqu %xmm7, 96(%rcx)\n\t"
		"movdqu %xmm8, 112(%rcx)\n\t"
		"movdqu %xmm9, 128(%rcx)\n\t"
		"movdqu %xmm10, 144(%rcx)\n\t"
		"movdqu %xmm11, 160(%rcx)\n\t"
		"movdqu %xmm12, 176(%rcx)\n\t"
		"movdqu %xmm13, 192(%rcx)\n\t"
		"movdqu %xmm14, 208(%rcx)\n\t"
		"movdqu %xmm15, 224(%rcx)\n\t"
		"xorl %eax, %eax\n\t"
		"ret\n\t");
}

__attribute__((naked)) _Noreturn void crt_longjmp(__attribute__((unused)) crt_jmp_buf *buffer, __attribute__((unused)) int value)
{
	__asm__ volatile(
		"movl %edx, %eax\n\t"
		"testl %eax, %eax\n\t"
		"jnz 1f\n\t"
		"movl $1, %eax\n"
		"1:\n\t"
		"movq 0(%rcx), %rbx\n\t"
		"movq 8(%rcx), %rbp\n\t"
		"movq 16(%rcx), %rdi\n\t"
		"movq 24(%rcx), %rsi\n\t"
		"movq 32(%rcx), %r12\n\t"
		"movq 40(%rcx), %r13\n\t"
		"movq 48(%rcx), %r14\n\t"
		"movq 56(%rcx), %r15\n\t"
		"movdqu 80(%rcx), %xmm6\n\t"
		"movdqu 96(%rcx), %xmm7\n\t"
		"movdqu 112(%rcx), %xmm8\n\t"
		"movdqu 128(%rcx), %xmm9\n\t"
		"movdqu 144(%rcx), %xmm10\n\t"
		"movdqu 160(%rcx), %xmm11\n\t"
		"movdqu 176(%rcx), %xmm12\n\t"
		"movdqu 192(%rcx), %xmm13\n\t"
		"movdqu 208(%rcx), %xmm14\n\t"
		"movdqu 224(%rcx), %xmm15\n\t"
		"movq 64(%rcx), %rsp\n\t"
		"jmpq *72(%rcx)\n\t");
}

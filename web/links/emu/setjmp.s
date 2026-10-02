@ setjmp.s - setjmp/longjmp for the emulator build (on the Psion, ESTLIB
@ has them). jmp_buf is 16 words (ESTLIB's setjmp.h); r4-r11, sp and lr
@ are saved, as APCS-32 requires.
	.text
	.align	2
	.global	setjmp
setjmp:
	stmia	r0, {r4-r11, sp, lr}
	mov	r0, #0
	mov	pc, lr

	.global	longjmp
longjmp:
	ldmia	r0, {r4-r11, sp, lr}
	movs	r0, r1
	moveq	r0, #1
	mov	pc, lr

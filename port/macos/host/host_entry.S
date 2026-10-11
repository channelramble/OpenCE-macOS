/*
HOST_ENTRY.S

The host's ways into guest code (port/macos/README.md). Guest code addresses
its window through x28, which holds the window's base while it runs
(tools/macos_sfi.py); host code the guest calls keeps x28 as it keeps any
callee-saved register, so only these entries need to set it.
*/

	.text

/* uint64_t host_enter_guest(uint64_t function, uint64_t a, uint64_t b,
uint64_t c, uint64_t d, uint64_t base): function (a host address) with a to
d, and x28 = base for its duration */
	.globl _host_enter_guest
	.p2align 2
_host_enter_guest:
	stp	x29, x30, [sp, #-32]!
	stp	x27, x28, [sp, #16]
	mov	x29, sp
	mov	x28, x5
	mov	x16, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	mov	x3, x4
	blr	x16
	ldp	x27, x28, [sp, #16]
	ldp	x29, x30, [sp], #32
	ret

/* void host_enter_guest_on_stack(uint64_t function, uint64_t argument,
uint64_t stack_top, uint64_t base): function(argument) on the stack ending
at stack_top, with x28 = base; the guest's main never returns (it exits
through host_exit), and the frame chain ends here */
	.globl _host_enter_guest_on_stack
	.p2align 2
_host_enter_guest_on_stack:
	mov	sp, x2
	mov	x29, #0
	mov	x30, #0
	mov	x28, x3
	mov	x16, x0
	mov	x0, x1
	blr	x16
	brk	#1

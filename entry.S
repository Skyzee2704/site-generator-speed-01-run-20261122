# entry.S — the freestanding entry point.
# The kernel starts us with %rsp pointing at argc, then argv[], then
# envp[]. Hand that stack pointer to entry_main as its argument; when it
# returns, exit with its return code (syscall 60).
	.globl _start
	.text
_start:
	mov %rsp, %rdi
	call entry_main
	mov %rax, %rdi
	mov $60, %rax
	syscall

	.section .note.GNU-stack,"",@progbits

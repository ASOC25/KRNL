.section .text

.global _start
_start:
	mov $main, %rsi
	mov %rsp, %rdi          /* rdi = stack ptr (-> argc); saved before alignment */
	andq $-16, %rsp         /* align rsp to 16 bytes (SysV AMD64 ABI) */
	call __mlibc_entry

.size _start, . - _start
.section .note.GNU-stack,"",%progbits
# Homebrew test executable: exercises CALL/RET, the guest stack, and LEA.
# Returns rdi + 7 from a callee and exits with the result (12).
# Original ARMSX5 code.
.global _start
.text
_start:
    mov $5, %rdi
    call add7
    mov %rax, %rdi
    mov $1, %rax               # FreeBSD SYS_exit
    syscall

add7:
    lea 7(%rdi), %rax
    ret

# Homebrew test executable: prints a greeting through the emulated kernel
# (FreeBSD/amd64 write syscall) and exits with code 42. Original ARMSX5 code.
.global _start
.text
_start:
    lea msg(%rip), %rsi        # buffer
    mov $msg_len, %rdx         # length
    mov $1, %rdi               # fd = 1 (stdout)
    mov $4, %rax               # FreeBSD SYS_write
    syscall
    mov $42, %rdi              # exit code
    mov $1, %rax               # FreeBSD SYS_exit
    syscall
.section .rodata
msg:
    .ascii "Hello from ARMSX5 guest\n"
msg_len = . - msg

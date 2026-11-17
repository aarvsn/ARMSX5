# Homebrew test executable: loop-sums 1..100 (ALU, flags, backward branch)
# and exits with 5050. Original ARMSX5 code.
.global _start
.text
_start:
    xor %rax, %rax             # sum = 0
    mov $1, %rcx               # i = 1
1:
    add %rcx, %rax             # sum += i
    inc %rcx                   # ++i
    cmp $100, %rcx             # i <= 100?
    jbe 1b                     # loop
    mov %rax, %rdi             # exit(sum)
    mov $1, %rax               # FreeBSD SYS_exit
    syscall

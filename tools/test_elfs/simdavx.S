# Homebrew test executable: AVX (VEX) end-to-end path.
#
# Exercises the emulated AVX pipeline: VEX.256 integer and float compute,
# both 2-byte (C5) and 3-byte (C4) VEX encodings, high YMM registers
# (ymm8-15 via VEX.R/X/B), packed-doubleword and packed-word adds, integer-
# to-float and truncate-float-to-int conversions, aligned 32-byte VMOVDQA
# loads/stores, and the GPR ALU loop that reduces the results.
#
# YMM lane sums (dword view):
#   ymm1 = const_a + const_b = {11,22,...,88}   sum = 396
#   ymm3 = cvttps2dq(2 * cvtdq2ps(a))           sum = 72
#   ymm8 = ymm9(0) + const_a = {1,...,8}        sum = 36   (3-byte VEX, ymm8/9)
#   ymm4 = vpaddw(a, a) = {2,4,...,16}          sum = 72
# Total = 396 + 72 + 36 + 72 = 576 -> exit(576). Original ARMSX5 code.
.global _start
.text
_start:
    vmovdqa const_a(%rip), %ymm0        # ymm0 = {1,2,3,4,5,6,7,8}
    vpaddd const_b(%rip), %ymm0, %ymm1  # ymm1 = {11,22,...,88}

    vcvtdq2ps %ymm0, %ymm2              # ymm2 = {1.0f..8.0f}
    vaddps %ymm2, %ymm2, %ymm3          # ymm3 = {2.0f..16.0f}
    vcvttps2dq %ymm3, %ymm3             # ymm3 = {2,4,...,16}

    # Hand-encoded 3-byte VEX (C4) to force the 3-byte path + high registers:
    #   vxorps ymm9, ymm9, ymm9      C4 41 34 57 C9
    #   vpaddd ymm8, ymm9, ymm0      C4 61 35 FE C0
    .byte 0xC4, 0x41, 0x34, 0x57, 0xC9
    .byte 0xC4, 0x61, 0x35, 0xFE, 0xC0

    vpaddw %ymm0, %ymm0, %ymm4          # ymm4 = word-wise a+a = {2,4,...,16}

    vmovdqa %ymm1, buf(%rip)            # 32-byte aligned stores
    vmovdqa %ymm3, buf+32(%rip)
    vmovdqa %ymm8, buf+64(%rip)
    vmovdqa %ymm4, buf+96(%rip)

    lea buf(%rip), %rsi                 # GPR reduction over 32 dwords
    xor %rax, %rax
    mov $32, %ecx
1:
    add (%rsi), %rax
    add $4, %rsi
    dec %ecx
    jnz 1b

    mov %rax, %rdi                      # exit(576)
    mov $1, %rax                        # FreeBSD SYS_exit
    syscall

.section .rodata
    .balign 32
const_a:
    .long 1, 2, 3, 4, 5, 6, 7, 8
    .balign 32
const_b:
    .long 10, 20, 30, 40, 50, 60, 70, 80

.data
    .balign 32
buf:
    .space 128

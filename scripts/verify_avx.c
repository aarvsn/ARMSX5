// Ground-truth verification of AVX (VEX) semantics on the sandbox CPU.
// Uses fixed registers (xmm0/ymm0..ymm7) with explicit memory outputs.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>

typedef union { __m128 v; uint64_t u64[2]; } M128;
typedef union { __m256 v; uint64_t u64[4]; } M256;

static void dump128(const char* name, M128 m) {
    printf("%-30s %016llx %016llx\n", name,
           (unsigned long long)m.u64[1], (unsigned long long)m.u64[0]);
}
static void dump256(const char* name, M256 m) {
    printf("%-30s %016llx %016llx %016llx %016llx\n", name,
           (unsigned long long)m.u64[3], (unsigned long long)m.u64[2],
           (unsigned long long)m.u64[1], (unsigned long long)m.u64[0]);
}

static inline M128 set128(uint64_t lo, uint64_t hi) {
    M128 m; m.u64[0] = lo; m.u64[1] = hi; return m;
}
static inline M256 seeded256(void) {
    M256 m;
    m.u64[0] = 0x1111111111111111ULL; m.u64[1] = 0x2222222222222222ULL;
    m.u64[2] = 0x3333333333333333ULL; m.u64[3] = 0x4444444444444444ULL;
    return m;
}

int main(void) { setbuf(stdout, NULL);
    printf("== VADDSS xmm0,xmm1,xmm2 : scalar upper from SRC1? ==\n");
    {
        M128 s1 = set128(0, 0xAAAA111122223333ULL); // vvvv operand
        M128 s2 = set128(0, 0);                     // rm operand, lane0=0.0f
        M128 r;
        __asm__ volatile("vaddss %2, %1, %%xmm0 \n\t"
                         "vmovups %%xmm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(s1.v), "x"(s2.v)
                         : "xmm0");
        dump128("vaddss dst128", r);
    }
    printf("== VADDSS with YMM upper garbage in dst ==\n");
    {
        M128 s1 = set128(0xAAAA111122223333ULL, 0);
        M128 s2 = set128(0, 0);
        M256 seeded = seeded256();
        M256 r;
        __asm__ volatile("vmovups %2, %%ymm0 \n\t"
                         "vaddss %4, %3, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "m"(seeded), "x"(s1.v), "x"(s2.v)
                         : "ymm0");
        dump256("vaddss dst256", r);
    }
    printf("== VADDPS xmm0,xmm1,xmm2 (128) : YMM upper of dst ==\n");
    {
        M128 s1 = set128(0, 0xAAAA111122223333ULL);
        M128 s2 = set128(0, 0);
        M256 seeded = seeded256();
        M256 r;
        __asm__ volatile("vmovups %2, %%ymm0 \n\t"
                         "vaddps %4, %3, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "m"(seeded), "x"(s1.v), "x"(s2.v)
                         : "ymm0");
        dump256("vaddps dst256", r);
    }
    printf("== VSQRTPS xmm 2-op (gas: C5 F8 51 C2), all lanes from src? ==\n");
    {
        float s1[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        float s2[4] = {4.0f, 9.0f, 16.0f, 25.0f};
        M128 m1, m2, r;
        memcpy(&m1.v, s1, 16);
        memcpy(&m2.v, s2, 16);
        __asm__ volatile("vmovups %1, %%xmm0 \n\t"
                         "vsqrtps %2, %%xmm0 \n\t"
                         "vmovups %%xmm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(m1.v), "x"(m2.v)
                         : "xmm0");
        dump128("vsqrtps dst(seed=s1)", r);
    }
    printf("== VSQRTSS xmm0,xmm1,xmm2 (dst!=src1, gas 3-op) ==\n");
    {
        float s1[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        float s2[4] = {4.0f, 9.0f, 16.0f, 25.0f};
        M128 m1, m2, r;
        memcpy(&m1.v, s1, 16);
        memcpy(&m2.v, s2, 16);
        __asm__ volatile("vsqrtss %2, %1, %%xmm0 \n\t"
                         "vmovups %%xmm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(m1.v), "x"(m2.v)
                         : "xmm0");
        dump128("vsqrtss dst(dst!=src1)", r);
    }
    printf("== VSQRTPS ymm 256 (2-op) ==\n");
    {
        float s1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        float s2[8] = {4, 9, 16, 25, 36, 49, 64, 81};
        M256 m1, m2, r;
        memcpy(&m1.v, s1, 32);
        memcpy(&m2.v, s2, 32);
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vsqrtps %2, %%ymm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(m1.v), "x"(m2.v)
                         : "ymm0");
        dump256("vsqrtps ymm dst(seed=s1)", r);
    }
    printf("== VSQRTSS with YMM upper garbage in dst ==\n");
    {
        M128 s1 = set128(0xAAAA111122223333ULL, 0);
        M128 s2 = set128(0x40000000ULL, 0); // 2.0f
        M256 seeded = seeded256();
        M256 r;
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vsqrtss %3, %2, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "x"(s1.v), "x"(s2.v)
                         : "ymm0");
        dump256("vsqrtss dst256", r);
    }
    printf("== VCVTSI2SS xmm0,xmm1,r (dst!=src1) + YMM upper ==\n");
    {
        M128 s1 = set128(0, 0xAAAA111122223333ULL);
        M256 seeded = seeded256();
        M256 r;
        long v = 12345;
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vcvtsi2ss %3, %2, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "x"(s1.v), "r"(v)
                         : "ymm0");
        dump256("vcvtsi2ss dst256", r);
    }
    printf("== VCVTDQ2PS xmm0,xmm0 ==\n");
    {
        int32_t s[4] = {-3, 7, 1000000, -1000000};
        M128 m, r;
        memcpy(&m.v, s, 16);
        __asm__ volatile("vcvtdq2ps %1, %%xmm0 \n\t"
                         "vmovups %%xmm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(m.v)
                         : "xmm0");
        dump128("vcvtdq2ps dst", r);
    }
    printf("== VMOVSS mem-load : zeroing ==\n");
    {
        float val = 2.5f;
        M256 seeded = seeded256();
        M256 r;
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vmovss %2, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "m"(val)
                         : "ymm0");
        dump256("vmovss mem-load dst256", r);
    }
    printf("== VMOVSS reg,reg,reg (dst!=src1) + YMM upper ==\n");
    {
        M128 s1 = set128(0xAAAA111122223333ULL, 0xCCCC0000CCCC0000ULL);
        M128 s2 = set128(0x3FC00000ULL, 0); // lane0 = 1.5f
        M256 seeded = seeded256();
        M256 r;
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vmovss %3, %2, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "x"(s1.v), "x"(s2.v)
                         : "ymm0");
        dump256("vmovss reg dst256", r);
    }
    printf("== VMOVAPS VEX.128 : YMM upper ==\n");
    {
        M128 src = set128(0xAAAA111122223333ULL, 0xCCCC0000CCCC0000ULL);
        M256 seeded = seeded256();
        M256 r;
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vmovaps %2, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "x"(src.v)
                         : "ymm0");
        dump256("vmovaps dst256", r);
    }
    printf("== VMOVD (W0 66 6E) xmm <- r32 : zeroing ==\n");
    {
        M256 seeded = seeded256();
        M256 r;
        long v = 0x87654321;
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vmovd %2, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "r"(v)
                         : "ymm0");
        dump256("vmovd dst256", r);
    }
    printf("== VMOVQ (F3 7E) xmm <- r64 : zeroing ==\n");
    {
        M256 seeded = seeded256();
        M256 r;
        long v = 0x8899AABBCCDDEEFF;
        __asm__ volatile("vmovups %1, %%ymm0 \n\t"
                         "vmovq %2, %%xmm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "m"(seeded), "r"(v)
                         : "ymm0");
        dump256("vmovq F3 dst256", r);
    }
    printf("== VPSHUFD ymm : two independent halves? ==\n");
    {
        uint32_t s[8] = {10, 11, 12, 13, 20, 21, 22, 23};
        M256 m, r;
        memcpy(&m.v, s, 32);
        __asm__ volatile("vpshufd $0x1B, %1, %%ymm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(m.v)
                         : "ymm0");
        dump256("vpshufd 1B dst256", r);
    }
    printf("== VUCOMISS flag test (eq / below / unordered) ==\n");
    {
        float a = 1.0f, b = 1.0f;
        unsigned long flags;
        __asm__ volatile("vucomiss %2, %1 \n\t"
                         "pushfq \n\t popq %0 \n\t"
                         : "=r"(flags)
                         : "x"(a), "x"(b)
                         : "cc");
        printf("vucomiss eq: zf=%lu cf=%lu\n", (flags >> 6) & 1, (flags >> 0) & 1);
        float a2 = 0.5f;
        __asm__ volatile("vucomiss %2, %1 \n\t"
                         "pushfq \n\t popq %0 \n\t"
                         : "=r"(flags)
                         : "x"(a2), "x"(b)
                         : "cc");
        printf("vucomiss below: zf=%lu cf=%lu\n", (flags >> 6) & 1, (flags >> 0) & 1);
        float nanv;
        memset(&nanv, 0xFF, 4);
        __asm__ volatile("vucomiss %2, %1 \n\t"
                         "pushfq \n\t popq %0 \n\t"
                         : "=r"(flags)
                         : "x"(nanv), "x"(b)
                         : "cc");
        printf("vucomiss unord: zf=%lu cf=%lu\n", (flags >> 6) & 1, (flags >> 0) & 1);
    }
    printf("== VADDPS ymm (256) + VANDNPS ==\n");
    {
        float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        float b[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
        M256 ma, mb, r;
        memcpy(&ma.v, a, 32);
        memcpy(&mb.v, b, 32);
        __asm__ volatile("vaddps %2, %1, %%ymm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(ma.v), "x"(mb.v)
                         : "ymm0");
        dump256("vaddps ymm", r);
        __asm__ volatile("vandnps %2, %1, %%ymm0 \n\t"
                         "vmovups %%ymm0, %0 \n\t"
                         : "=m"(r)
                         : "x"(ma.v), "x"(mb.v)
                         : "ymm0");
        dump256("vandnps ymm", r);
    }
    return 0;
}

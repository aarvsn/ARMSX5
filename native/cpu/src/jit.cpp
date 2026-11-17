#include "armsx5/cpu/jit.h"

#include "armsx5/core/log.h"
#include "armsx5/cpu/arm64/encoder.h"
#include "armsx5/cpu/x86/decoder.h"

#include <cstddef>
#include <cstring>

namespace armsx5::cpu {
namespace {
constexpr const char* TAG = "Jit";

// Result kinds communicated from helpers to execute_block.
constexpr uint8_t kResOk = 0;
constexpr uint8_t kResFault = 1;
constexpr uint8_t kResStop = 2;

thread_local uint8_t t_result = kResOk;
thread_local Status t_fault_code = Status::Ok;
thread_local char t_fault_msg[256] = {0};
thread_local bool t_invalidate_requested = false;

void set_fault(Status code, const char* msg) {
    t_result = kResFault;
    t_fault_code = code;
    snprintf(t_fault_msg, sizeof(t_fault_msg), "%s", msg);
}

uint8_t parity_of(uint64_t v) {
    int n = 0;
    for (int i = 0; i < 8; ++i)
        if (v & (1u << i)) ++n;
    return (n % 2) == 0 ? 1 : 0;
}

// ---- host helpers invoked from generated code (AArch64 C ABI) ----
// x0..x7 argument registers; return value in x0/w0.

// ALU with flags. kind: 0=add 1=sub 2=and 3=or 4=xor 5=cmp.
// Args: x0=ctx, w1=dst, x2=a, x3=b, w4=size, w5=kind.
uint64_t jit_helper_bin(CpuState* ctx, uint64_t dst, uint64_t a, uint64_t b, uint64_t size,
                        uint64_t kind) {
    const uint8_t bits = static_cast<uint8_t>(size * 8);
    const uint64_t mask = size == 8 ? ~0ULL : ((1ULL << bits) - 1);
    const uint64_t sign = 1ULL << (bits - 1);
    const uint64_t am = a & mask, bm = b & mask;
    uint64_t r = 0;
    switch (kind) {
    case 0: {
        const __uint128_t t = static_cast<__uint128_t>(am) + bm;
        r = static_cast<uint64_t>(t) & mask;
        ctx->flags.cf = (t >> bits) != 0;
        const bool sa = (am & sign) != 0, sb = (bm & sign) != 0, sr = (r & sign) != 0;
        ctx->flags.of = (sa == sb) && (sa != sr);
        ctx->flags.af = ((am ^ bm ^ r) & 0x10) != 0;
        ctx->flags.zf = r == 0;
        ctx->flags.sf = (r & sign) != 0;
        ctx->flags.pf = parity_of(r) != 0;
        break;
    }
    case 1:
    case 5: {
        const __int128_t t = static_cast<__int128_t>(am) - bm;
        r = static_cast<uint64_t>(t) & mask;
        ctx->flags.cf = t < 0;
        const bool sa = (am & sign) != 0, sb = (bm & sign) != 0, sr = (r & sign) != 0;
        ctx->flags.of = (sa != sb) && (sa != sr);
        ctx->flags.af = ((am ^ bm ^ r) & 0x10) != 0;
        ctx->flags.zf = r == 0;
        ctx->flags.sf = (r & sign) != 0;
        ctx->flags.pf = parity_of(r) != 0;
        if (kind == 5) return 1; // CMP: flags only
        break;
    }
    case 2:
    case 3:
    case 4: {
        r = kind == 2 ? (am & bm) : kind == 3 ? (am | bm) : (am ^ bm);
        ctx->flags.cf = false;
        ctx->flags.of = false;
        ctx->flags.af = false;
        ctx->flags.zf = r == 0;
        ctx->flags.sf = (r & sign) != 0;
        ctx->flags.pf = parity_of(r) != 0;
        break;
    }
    default:
        set_fault(Status::JitError, "unknown ALU kind");
        return 0;
    }
    ctx->gpr[dst & 15] = r;
    return 1;
}

// Guest memory load. Args: x0=ctx, x1=GuestMemory*, x2=addr, w3=size,
// w4=dst reg, x5=fault_rip. Returns 0 on fault.
uint64_t jit_helper_load(CpuState* ctx, void* mem_impl, uint64_t addr, uint64_t size,
                         uint64_t dst, uint64_t fault_rip) {
    auto* mem = static_cast<memory::GuestMemory*>(mem_impl);
    auto ptr = mem->translate(addr, static_cast<size_t>(size), memory::Prot::R);
    if (!ptr) {
        set_fault(ptr.error().code, ("load fault: " + ptr.error().message).c_str());
        ctx->rip = fault_rip;
        return 0;
    }
    uint64_t v = 0;
    memcpy(&v, *ptr, static_cast<size_t>(size));
    ctx->gpr[dst & 15] = v;
    return 1;
}

// Guest memory store. Args: x0=ctx, x1=GuestMemory*, x2=addr, w3=size,
// x4=value, x5=fault_rip. Returns 0 on fault.
uint64_t jit_helper_store(CpuState* ctx, void* mem_impl, uint64_t addr, uint64_t size,
                          uint64_t value, uint64_t fault_rip) {
    auto* mem = static_cast<memory::GuestMemory*>(mem_impl);
    auto ptr = mem->translate(addr, static_cast<size_t>(size), memory::Prot::W);
    if (!ptr) {
        set_fault(ptr.error().code, ("store fault: " + ptr.error().message).c_str());
        ctx->rip = fault_rip;
        return 0;
    }
    memcpy(*ptr, &value, static_cast<size_t>(size));
    if (mem->is_exec_page(addr)) t_invalidate_requested = true;
    return 1;
}

// Jcc/Jmp. Args: x0=ctx, w1=cond, x2=rel, x3=next_rip, w4=mode (0=Jcc, 1=Jmp).
void jit_helper_branch(CpuState* ctx, uint64_t cond, int64_t rel, uint64_t next_rip,
                       uint64_t mode) {
    const Flags& f = ctx->flags;
    if (mode == 1) {
        ctx->rip = next_rip + static_cast<uint64_t>(rel);
        return;
    }
    bool taken = false;
    switch (static_cast<uint8_t>(cond)) {
    case 0: taken = f.of; break;
    case 1: taken = !f.of; break;
    case 2: taken = f.cf; break;
    case 3: taken = !f.cf; break;
    case 4: taken = f.zf; break;
    case 5: taken = !f.zf; break;
    case 6: taken = f.cf || f.zf; break;
    case 7: taken = !f.cf && !f.zf; break;
    case 8: taken = f.sf; break;
    case 9: taken = !f.sf; break;
    case 10: taken = f.pf; break;
    case 11: taken = !f.pf; break;
    case 12: taken = f.sf != f.of; break;
    case 13: taken = f.sf == f.of; break;
    case 14: taken = f.zf || (f.sf != f.of); break;
    case 15: taken = !f.zf && (f.sf == f.of); break;
    }
    ctx->rip = taken ? next_rip + static_cast<uint64_t>(rel) : next_rip;
}

// CALL rel. Args: x0=ctx, x1=GuestMemory*, x2=target, x3=return_rip.
uint64_t jit_helper_call(CpuState* ctx, void* mem_impl, uint64_t target, uint64_t return_rip) {
    auto* mem = static_cast<memory::GuestMemory*>(mem_impl);
    const uint64_t rsp = ctx->gpr[reg::SP] - 8;
    auto ptr = mem->translate(rsp, 8, memory::Prot::W);
    if (!ptr) {
        set_fault(ptr.error().code, ("call stack fault: " + ptr.error().message).c_str());
        ctx->rip = return_rip;
        return 0;
    }
    memcpy(*ptr, &return_rip, 8);
    ctx->gpr[reg::SP] = rsp;
    ctx->rip = target;
    return 1;
}

// RET [imm]. Args: x0=ctx, x1=GuestMemory*, x2=stack_add.
uint64_t jit_helper_ret(CpuState* ctx, void* mem_impl, uint64_t stack_add) {
    auto* mem = static_cast<memory::GuestMemory*>(mem_impl);
    auto ptr = mem->translate(ctx->gpr[reg::SP], 8, memory::Prot::R);
    if (!ptr) {
        set_fault(ptr.error().code, ("ret stack fault: " + ptr.error().message).c_str());
        return 0;
    }
    uint64_t target = 0;
    memcpy(&target, *ptr, 8);
    ctx->gpr[reg::SP] += 8 + stack_add;
    ctx->rip = target;
    return 1;
}

// SYSCALL. Args: x0=ctx, x1=ISyscallHandler*, x2=next_rip.
uint64_t jit_helper_syscall(CpuState* ctx, void* handler, uint64_t next_rip) {
    auto* h = static_cast<ISyscallHandler*>(handler);
    if (!h) {
        set_fault(Status::KernelError, "no syscall handler for JIT");
        return 0;
    }
    ctx->rip = next_rip;
    bool stop = false;
    const Status r = h->on_syscall(*ctx, stop);
    if (r != Status::Ok) {
        set_fault(r, "syscall handler failed");
        return 0;
    }
    if (stop) t_result = kResStop;
    return 1;
}

} // namespace

Jit::Jit(memory::GuestMemory& mem, ISyscallHandler* syscall_handler)
    : mem_(mem), syscall_handler_(syscall_handler) {}

std::expected<void, Error> Jit::init() { return arena_.init(); }

bool Jit::can_execute_natively() {
#if defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

const char* Jit::result_kind_name(uint8_t kind) {
    switch (kind) {
    case kResOk: return "Ok";
    case kResFault: return "Fault";
    case kResStop: return "Stop";
    }
    return "?";
}

std::expected<ir::Block, Error> Jit::build_ir(uint64_t rip) {
    using namespace x86;
    ir::Block block;
    block.start_rip = rip;
    uint64_t cur = rip;
    for (size_t n = 0; n < kMaxBlockInstrs; ++n) {
        if (cur - rip >= kMaxBlockBytes) break;
        uint8_t buf[15] = {};
        size_t filled = 0;
        for (; filled < sizeof(buf); ++filled) {
            auto b = mem_.read(cur + filled, &buf[filled], 1);
            if (!b) break;
        }
        if (filled == 0)
            return std::unexpected(Error{Status::GuestFault, "cannot fetch at block start"});

        auto d = decode(buf, filled);
        if (!d) break; // ends block; the interpreter executes this instruction
        const x86::Decoded& dec = *d;

        auto push = [&](ir::Instr i) {
            i.guest_len = dec.length;
            block.instrs.push_back(i);
        };

        switch (dec.cls) {
        case InstrClass::Nop:
            push(ir::Instr{ir::Op::Nop});
            break;
        case InstrClass::MovImm:
            push(ir::Instr{ir::Op::Imm, dec.dst.reg, 0xFF, 0xFF, dec.opsize, dec.src.imm});
            break;
        case InstrClass::Mov:
            if (dec.src.kind != OperandKind::Reg || dec.dst.kind != OperandKind::Reg) break;
            push(ir::Instr{ir::Op::Mov, dec.dst.reg, dec.src.reg, 0xFF, dec.opsize});
            break;
        case InstrClass::Alu: {
            if (dec.dst.kind != OperandKind::Reg) break; // memory ALU -> interpreter
            const bool src_reg = dec.src.kind == OperandKind::Reg;
            const bool src_imm = dec.src.kind == OperandKind::Imm;
            if (!src_reg && !src_imm) break;
            if (dec.alu_op == AluOp::Adc || dec.alu_op == AluOp::Sbb) break;
            uint64_t kind = 0;
            switch (dec.alu_op) {
            case AluOp::Add: kind = 0; break;
            case AluOp::Sub: kind = 1; break;
            case AluOp::And: kind = 2; break;
            case AluOp::Or: kind = 3; break;
            case AluOp::Xor: kind = 4; break;
            case AluOp::Cmp: kind = 5; break;
            default: break;
            }
            ir::Instr i{ir::Op::Bin};
            i.dst = dec.dst.reg;
            i.a = dec.dst.reg; // reads the destination register value
            i.b = src_reg ? dec.src.reg : 0xFF;
            i.imm = src_imm ? dec.src.imm : 0;
            i.size = dec.opsize;
            i.bin = static_cast<ir::BinOp>(kind);
            push(i);
            break;
        }
        case InstrClass::Lea: {
            if (dec.src.kind != OperandKind::Mem) break;
            const auto& m = dec.src.mem;
            if (!m.valid || m.index != 0xFF || m.rip_relative) break;
            ir::Instr i{ir::Op::Lea};
            i.dst = dec.dst.reg;
            i.a = m.base;
            i.disp = m.disp;
            i.size = dec.opsize;
            push(i);
            break;
        }
        case InstrClass::Jcc: {
            ir::Instr i{ir::Op::Jcc};
            i.cond = dec.cond;
            i.rel = dec.rel;
            i.disp = static_cast<int64_t>(cur + dec.length);
            push(i);
            cur += dec.length;
            block.end_rip = cur;
            return block;
        }
        case InstrClass::JmpRel: {
            ir::Instr i{ir::Op::Jmp};
            i.rel = dec.rel;
            i.disp = static_cast<int64_t>(cur + dec.length);
            push(i);
            cur += dec.length;
            block.end_rip = cur;
            return block;
        }
        case InstrClass::Ret: {
            ir::Instr i{ir::Op::Ret};
            i.imm = dec.src.imm;
            push(i);
            cur += dec.length;
            block.end_rip = cur;
            return block;
        }
        case InstrClass::CallRel: {
            ir::Instr i{ir::Op::CallRel};
            i.rel = dec.rel;
            i.disp = static_cast<int64_t>(cur + dec.length);
            push(i);
            cur += dec.length;
            block.end_rip = cur;
            return block;
        }
        case InstrClass::Syscall: {
            ir::Instr i{ir::Op::Syscall};
            push(i);
            cur += dec.length;
            block.end_rip = cur;
            return block;
        }
        default:
            // Unsupported instruction: end the block before it. The
            // interpreter executes it; a new block may start after it.
            block.end_rip = cur;
            return block;
        }
        cur += dec.length;
    }
    block.end_rip = cur;
    return block;
}

std::expected<size_t, Error> Jit::translate(const ir::Block& ir_block) {
    using arm64::Assembler;
    Assembler a;

    const int epilogue = a.new_label();

    // Host helper addresses (runtime constants, loaded via movz/movk + blr).
    const uint64_t addr_bin = reinterpret_cast<uint64_t>(&jit_helper_bin);
    const uint64_t addr_load = reinterpret_cast<uint64_t>(&jit_helper_load);
    const uint64_t addr_store = reinterpret_cast<uint64_t>(&jit_helper_store);
    const uint64_t addr_branch = reinterpret_cast<uint64_t>(&jit_helper_branch);
    const uint64_t addr_call = reinterpret_cast<uint64_t>(&jit_helper_call);
    const uint64_t addr_ret = reinterpret_cast<uint64_t>(&jit_helper_ret);
    const uint64_t addr_syscall = reinterpret_cast<uint64_t>(&jit_helper_syscall);
    const uint64_t mem_ptr = reinterpret_cast<uint64_t>(&mem_);
    const uint64_t handler_ptr = reinterpret_cast<uint64_t>(syscall_handler_);

    auto call_helper = [&](uint64_t fn) {
        a.mov_imm64(9, fn);
        a.blr(9);
    };

    // Prologue: callee-saved + frame, x19=ctx, x20=mem, x21=handler.
    a.sub_imm64(31, 31, 48);
    a.stp64(19, 20, 31, 0);
    a.stp64(21, 22, 31, 16);
    a.stp64(29, 30, 31, 32);
    a.mov_reg64(19, 0);
    a.mov_imm64(20, mem_ptr);
    a.mov_imm64(21, handler_ptr);

    const auto off_gpr = [&](uint8_t r) {
        return static_cast<uint32_t>(offsetof(CpuState, gpr) + r * sizeof(uint64_t));
    };
    const auto off_rip = [] {
        return static_cast<uint32_t>(offsetof(CpuState, rip));
    };
    auto load_op_value = [&](int reg, uint8_t guest_reg, int64_t imm, bool use_imm) {
        if (use_imm) a.mov_imm64(reg, static_cast<uint64_t>(imm));
        else a.ldr64(reg, 19, off_gpr(guest_reg));
    };

    for (const ir::Instr& i : ir_block.instrs) {
        switch (i.op) {
        case ir::Op::Nop:
            a.nop();
            break;
        case ir::Op::Imm:
            a.mov_imm64(8, static_cast<uint64_t>(i.imm));
            a.str64(8, 19, off_gpr(i.dst));
            break;
        case ir::Op::Mov:
            a.ldr64(8, 19, off_gpr(i.a));
            a.str64(8, 19, off_gpr(i.dst));
            break;
        case ir::Op::Bin:
            a.mov_reg64(0, 19);
            a.movz(1, i.dst, 0);
            a.ldr64(2, 19, off_gpr(i.a));
            if (i.b != 0xFF) a.ldr64(3, 19, off_gpr(i.b));
            else a.mov_imm64(3, static_cast<uint64_t>(i.imm));
            a.movz(4, i.size, 0);
            a.movz(5, static_cast<uint16_t>(i.bin), 0);
            call_helper(addr_bin);
            break;
        case ir::Op::Lea:
            a.ldr64(8, 19, off_gpr(i.a));
            a.mov_imm64(9, static_cast<uint64_t>(i.disp));
            a.add_reg64(8, 8, 9);
            a.str64(8, 19, off_gpr(i.dst));
            break;
        case ir::Op::LoadMem:
            a.mov_reg64(0, 19);
            a.mov_reg64(1, 20);
            a.ldr64(2, 19, off_gpr(i.a));
            a.mov_imm64(9, static_cast<uint64_t>(i.disp));
            a.add_reg64(2, 2, 9);
            a.movz(3, i.size, 0);
            a.movz(4, i.dst, 0);
            a.mov_imm64(5, i.next_rip);
            call_helper(addr_load);
            a.cbnz64(0, epilogue); // 0 = fault -> exit block
            break;
        case ir::Op::StoreMem:
            a.mov_reg64(0, 19);
            a.mov_reg64(1, 20);
            a.ldr64(2, 19, off_gpr(i.a));
            a.mov_imm64(9, static_cast<uint64_t>(i.disp));
            a.add_reg64(2, 2, 9);
            a.movz(3, i.size, 0);
            a.ldr64(4, 19, off_gpr(i.b));
            a.mov_imm64(5, i.next_rip);
            call_helper(addr_store);
            a.cbnz64(0, epilogue);
            break;
        case ir::Op::Jcc:
            a.mov_reg64(0, 19);
            a.movz(1, i.cond, 0);
            a.mov_imm64(2, static_cast<uint64_t>(i.rel));
            a.mov_imm64(3, static_cast<uint64_t>(i.disp));
            a.movz(4, 0, 0);
            call_helper(addr_branch);
            a.b(epilogue);
            break;
        case ir::Op::Jmp:
            a.mov_reg64(0, 19);
            a.movz(1, 0, 0);
            a.mov_imm64(2, static_cast<uint64_t>(i.rel));
            a.mov_imm64(3, static_cast<uint64_t>(i.disp));
            a.movz(4, 1, 0);
            call_helper(addr_branch);
            a.b(epilogue);
            break;
        case ir::Op::Ret:
            a.mov_reg64(0, 19);
            a.mov_reg64(1, 20);
            a.mov_imm64(2, static_cast<uint64_t>(i.imm));
            call_helper(addr_ret);
            a.b(epilogue);
            break;
        case ir::Op::CallRel:
            a.mov_reg64(0, 19);
            a.mov_reg64(1, 20);
            a.mov_imm64(2, static_cast<uint64_t>(static_cast<int64_t>(i.disp) + i.rel));
            a.mov_imm64(3, static_cast<uint64_t>(i.disp));
            call_helper(addr_call);
            a.b(epilogue);
            break;
        case ir::Op::Syscall:
            a.mov_reg64(0, 19);
            a.mov_reg64(1, 21);
            a.mov_imm64(2, static_cast<uint64_t>(i.disp));
            call_helper(addr_syscall);
            a.b(epilogue);
            break;
        case ir::Op::Shift:
        case ir::Op::CmpImm:
        case ir::Op::CmpReg:
            // Generated only via older paths; treated as block end.
            a.brk();
            break;
        }
    }

    // Blocks that do not end in a control-flow terminator must store the
    // fall-through RIP so the epilogue can return it.
    const bool ends_with_terminator = [&] {
        if (ir_block.instrs.empty()) return false;
        switch (ir_block.instrs.back().op) {
        case ir::Op::Jcc:
        case ir::Op::Jmp:
        case ir::Op::Ret:
        case ir::Op::CallRel:
        case ir::Op::Syscall:
            return true;
        default:
            return false;
        }
    }();
    if (!ends_with_terminator) {
        a.mov_imm64(8, ir_block.end_rip);
        a.str64(8, 19, off_rip());
    }

    a.bind(epilogue);
    a.ldr64(0, 19, off_rip());
    a.ldp64(19, 20, 31, 0);
    a.ldp64(21, 22, 31, 16);
    a.ldp64(29, 30, 31, 32);
    a.add_imm64(31, 31, 48);
    a.ret();

    if (!a.finalize())
        return std::unexpected(Error{Status::JitError, "branch fixup failed"});

    const size_t words = a.code().size();
    auto buf = arena_.reserve(words * sizeof(uint32_t));
    if (!buf) return std::unexpected(buf.error());
    memcpy(*buf, a.code().data(), words * sizeof(uint32_t));
    return words;
}

std::expected<const Jit::Block*, Error> Jit::get_block(uint64_t rip) {
    if (!mem_.is_exec_page(rip))
        return std::unexpected(Error{Status::GuestFault, "block start is not executable"});
    auto it = cache_.find(rip);
    if (it != cache_.end()) return &it->second;

    auto irb = build_ir(rip);
    if (!irb) return std::unexpected(irb.error());
    if (irb->instrs.empty())
        return std::unexpected(Error{Status::Unsupported, "empty block (interpreter required)"});

    const size_t start_words = arena_.size() / sizeof(uint32_t);
    auto words = translate(*irb);
    if (!words) return std::unexpected(words.error());

    Block b;
    b.guest_start = irb->start_rip;
    b.guest_end = irb->end_rip;
    b.code_index = start_words;
    b.code_words = *words;
    auto [ins, inserted] = cache_.emplace(rip, b);
    ++stats_.translations;
    stats_.blocks_cached = cache_.size();
    return &ins->second;
}

std::expected<uint64_t, Error> Jit::execute_block(const Block& block, CpuState& state) {
#if !defined(__aarch64__)
    (void)block;
    (void)state;
    return std::unexpected(
        Error{Status::Unsupported,
              "JIT execution requires an aarch64 host; interpreter handles execution here"});
#else
    t_result = kResOk;
    using Fn = uint64_t (*)(CpuState*);
    const auto fn = reinterpret_cast<Fn>(const_cast<uint8_t*>(arena_.exec_ptr()) +
                                         block.code_index * sizeof(uint32_t));
    const uint64_t new_rip = fn(&state);
    ++stats_.helper_exits;
    if (t_invalidate_requested) {
        t_invalidate_requested = false;
        invalidate_all();
    }
    if (t_result == kResFault) {
        return std::unexpected(Error{t_fault_code, t_fault_msg});
    }
    last_run_stopped_ = (t_result == kResStop);
    return new_rip;
#endif
}

void Jit::invalidate_all() {
    if (!cache_.empty()) ++stats_.invalidations;
    cache_.clear();
}

void Jit::invalidate_range(uint64_t base, size_t size) {
    // Conservative v1 policy: any write into guest-executable pages flushes
    // the whole cache (docs/architecture/cpu.md).
    (void)base;
    (void)size;
    invalidate_all();
}

} // namespace armsx5::cpu

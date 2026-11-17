#include "armsx5/gpu/shader/spirv.h"
#include "armsx5/gpu/shader/rdna2.h"

#include <array>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace armsx5::gpu::spirv {
namespace {

// SPIR-V core opcode numbers (verified against SPIRV-Headers spirv.h).
enum : uint16_t {
    kOpExtInstImport = 11,
    kOpExtInst = 12,
    kOpMemoryModel = 14,
    kOpEntryPoint = 15,
    kOpExecutionMode = 16,
    kOpCapability = 17,
    kOpTypeVoid = 19,
    kOpTypeInt = 21,
    kOpTypeFloat = 22,
    kOpTypeVector = 23,
    kOpTypeStruct = 30,
    kOpTypePointer = 32,
    kOpTypeFunction = 33,
    kOpConstant = 43,
    kOpFunction = 54,
    kOpFunctionEnd = 56,
    kOpVariable = 59,
    kOpLoad = 61,
    kOpStore = 62,
    kOpAccessChain = 65,
    kOpDecorate = 71,
    kOpMemberDecorate = 72,
    kOpCompositeConstruct = 80,
    kOpCompositeExtract = 81,
    kOpConvertFToU = 109,
    kOpConvertFToS = 110,
    kOpConvertSToF = 111,
    kOpConvertUToF = 112,
    kOpBitcast = 124,
    kOpIAdd = 128,
    kOpFAdd = 129,
    kOpISub = 130,
    kOpFSub = 131,
    kOpIMul = 132,
    kOpFMul = 133,
    kOpFDiv = 136,
    kOpShiftRightLogical = 194,
    kOpShiftRightArithmetic = 195,
    kOpShiftLeftLogical = 196,
    kOpBitwiseOr = 197,
    kOpBitwiseXor = 198,
    kOpBitwiseAnd = 199,
    kOpNot = 200,
    kOpLabel = 248,
    kOpReturn = 253,
};

// GLSL.std.450 extended instructions (verified against SPIRV-Headers
// GLSL.std.450.h).
enum : uint16_t {
    kGlslRoundEven = 2,
    kGlslTrunc = 3,
    kGlslFloor = 8,
    kGlslCeil = 9,
    kGlslSqrt = 31,
    kGlslInverseSqrt = 32,
    kGlslFMin = 37,
    kGlslUMin = 38,
    kGlslSMin = 39,
    kGlslFMax = 40,
    kGlslUMax = 41,
    kGlslSMax = 42,
    kGlslFma = 50,
};

enum : uint32_t {
    kStorageInput = 1,
    kStorageOutput = 3,
    kStoragePushConstant = 12,
    kDecorationBlock = 2,
    kDecorationOffset = 3,
    kDecorationLocation = 30,
    kExecModelFragment = 4,
    kExecutionModeOriginUpperLeft = 7,
    kCapabilityShader = 1,
    kMemoryModelLogical = 0,
    kMemoryModelGLSL450 = 1,
    kFunctionControlNone = 0,
};

constexpr uint32_t kScalarSgprMax = 102; // s0-s101
constexpr uint32_t kVgprCount = 256;
constexpr uint32_t kSopkEndpgm = 1; // SOPP S_ENDPGM opcode (verified)

struct Value {
    uint32_t id = 0;    // 0 = unset
    uint32_t type = 0;  // SPIR-V type id
};

// Instruction size in words for an already-decoded instruction.
uint32_t instr_size(const rdna2::Instr& ins) {
    uint32_t n = rdna2::format_word_count(ins.format);
    if (n == 1 && ins.literal_src) n += 1; // inline literal dword
    if (ins.format == rdna2::Format::VOP3 && ins.literal_src) n += 1;
    return n == 0 ? 1 : n;
}

class Emitter {
public:
    explicit Emitter(const ShaderInterface& iface) : iface_(iface) {}

    std::optional<EmitResult> run(std::span<const uint32_t> code, Error& err) {
        if (code.empty()) {
            err = Error{Status::ParseError, "empty shader code"};
            return std::nullopt;
        }
        if (iface_.input_locations == 0 || iface_.output_locations == 0 ||
            iface_.sgpr_push_count > kScalarSgprMax) {
            err = Error{Status::InvalidArgument, "invalid shader interface"};
            return std::nullopt;
        }

        emit_module_header();
        translate_body(code);      // types/constants land in module_, body in body_
        append_function();

        EmitResult result;
        result.words = std::move(module_); // body spliced by append_function()
        result.words[header_bound_index_] = next_id_;
        result.id_bound = next_id_;
        result.stats = stats_;
        return result;
    }

private:
    // --- binary building ---------------------------------------------------
    uint32_t make_id() { return next_id_++; }

    void push(std::vector<uint32_t>& out, uint16_t op,
              std::initializer_list<uint32_t> ops) {
        const size_t start = out.size();
        out.push_back(0); // wordcount|opcode placeholder
        for (uint32_t o : ops) out.push_back(o);
        out[start] = (static_cast<uint32_t>(out.size() - start) << 16) | op;
    }

    void pushv(std::vector<uint32_t>& out, uint16_t op,
               const std::vector<uint32_t>& ops) {
        const size_t start = out.size();
        out.push_back(0);
        for (uint32_t o : ops) out.push_back(o);
        out[start] = (static_cast<uint32_t>(out.size() - start) << 16) | op;
    }

    void push_str(std::vector<uint32_t>& out, uint16_t op,
                  const uint32_t* operands, size_t count, const char* str) {
        const size_t start = out.size();
        out.push_back(0);
        for (size_t i = 0; i < count; ++i) out.push_back(operands[i]);
        const size_t chars = std::strlen(str) + 1; // include NUL
        size_t k = 0;
        for (; k + 4 <= chars; k += 4) {
            uint32_t w = 0;
            std::memcpy(&w, str + k, 4);
            out.push_back(w);
        }
        if (k < chars) {
            uint32_t w = 0;
            std::memcpy(&w, str + k, chars - k);
            out.push_back(w);
        }
        out[start] = (static_cast<uint32_t>(out.size() - start) << 16) | op;
    }

    // Module-section instructions (types, constants, variables, decorations).
    void mod(uint16_t op, std::initializer_list<uint32_t> ops = {}) {
        push(module_, op, ops);
    }
    void mod_str(uint16_t op, std::initializer_list<uint32_t> ops,
                 const char* str) {
        push_str(module_, op, ops.begin(), ops.size(), str);
    }
    // Function-body instructions.
    void body(uint16_t op, std::initializer_list<uint32_t> ops = {}) {
        push(body_, op, ops);
    }

    // --- types and constants (module section) ------------------------------
    uint32_t type_void() {
        if (!type_void_) { type_void_ = make_id(); mod(kOpTypeVoid, {type_void_}); }
        return type_void_;
    }
    uint32_t type_f32() {
        if (!type_f32_) { type_f32_ = make_id(); mod(kOpTypeFloat, {type_f32_, 32}); }
        return type_f32_;
    }
    uint32_t type_u32() {
        if (!type_u32_) { type_u32_ = make_id(); mod(kOpTypeInt, {type_u32_, 32, 0}); }
        return type_u32_;
    }
    uint32_t type_i32() {
        if (!type_i32_) { type_i32_ = make_id(); mod(kOpTypeInt, {type_i32_, 32, 1}); }
        return type_i32_;
    }
    uint32_t type_vec4_f32() {
        if (!type_vec4_) {
            type_vec4_ = make_id();
            mod(kOpTypeVector, {type_vec4_, type_f32(), 4});
        }
        return type_vec4_;
    }
    uint32_t type_fn_main() {
        if (!type_fn_) {
            type_fn_ = make_id();
            mod(kOpTypeFunction, {type_fn_, type_void()});
        }
        return type_fn_;
    }
    uint32_t type_ptr_u32_pc() {
        if (!type_ptr_pc_) {
            type_ptr_pc_ = make_id();
            mod(kOpTypePointer, {type_ptr_pc_, kStoragePushConstant, type_u32()});
        }
        return type_ptr_pc_;
    }
    uint32_t type_ptr_input() {
        if (!type_ptr_in_) {
            type_ptr_in_ = make_id();
            mod(kOpTypePointer, {type_ptr_in_, kStorageInput, type_vec4_f32()});
        }
        return type_ptr_in_;
    }
    uint32_t type_ptr_output() {
        if (!type_ptr_out_) {
            type_ptr_out_ = make_id();
            mod(kOpTypePointer, {type_ptr_out_, kStorageOutput, type_vec4_f32()});
        }
        return type_ptr_out_;
    }

    uint32_t const_u32(uint32_t v) {
        auto it = u32_consts_.find(v);
        if (it != u32_consts_.end()) return it->second;
        const uint32_t id = make_id();
        mod(kOpConstant, {type_u32(), id, v});
        u32_consts_.emplace(v, id);
        return id;
    }
    uint32_t const_f32(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        auto it = f32_consts_.find(bits);
        if (it != f32_consts_.end()) return it->second;
        const uint32_t id = make_id();
        mod(kOpConstant, {type_f32(), id, bits});
        f32_consts_.emplace(bits, id);
        return id;
    }

    // --- module header + declarations ---------------------------------------
    void emit_module_header() {
        module_.push_back(kSpirvMagic);
        module_.push_back(kSpirvVersion10);
        module_.push_back(0);            // generator: unregistered (ARMSX5)
        module_.push_back(0);            // id bound, patched at the end
        module_.push_back(0);            // schema
        header_bound_index_ = 3;

        mod(kOpCapability, {kCapabilityShader});
        glsl_ext_ = make_id();
        mod_str(kOpExtInstImport, {glsl_ext_}, "GLSL.std.450");
        mod(kOpMemoryModel, {kMemoryModelLogical, kMemoryModelGLSL450});

        declare_vars();
        fn_id_ = make_id();
        emit_entry_point();
        mod(kOpExecutionMode, {fn_id_, kExecutionModeOriginUpperLeft});
    }

    // OpEntryPoint: [wc|op][model][fn][name words][interface ids...].
    void emit_entry_point() {
        const size_t start = module_.size();
        module_.push_back(0);
        module_.push_back(kExecModelFragment);
        module_.push_back(fn_id_);
        const char* kName = "main";
        const size_t kChars = 5; // "main" + NUL
        uint32_t w = 0;
        std::memcpy(&w, kName, 4);
        module_.push_back(w);
        w = 0;
        std::memcpy(&w, kName + 4, 1);
        module_.push_back(w);
        for (uint32_t v : input_vars_) module_.push_back(v);
        for (uint32_t v : output_vars_) module_.push_back(v);
        module_[start] = (static_cast<uint32_t>(module_.size() - start) << 16) |
                         kOpEntryPoint;
    }

    void declare_vars() {
        for (uint32_t i = 0; i < iface_.input_locations; ++i) {
            const uint32_t var = make_id();
            mod(kOpVariable, {type_ptr_input(), var, kStorageInput});
            mod(kOpDecorate, {var, kDecorationLocation, i});
            input_vars_.push_back(var);
        }
        for (uint32_t i = 0; i < iface_.output_locations; ++i) {
            const uint32_t var = make_id();
            mod(kOpVariable, {type_ptr_output(), var, kStorageOutput});
            mod(kOpDecorate, {var, kDecorationLocation, i});
            output_vars_.push_back(var);
        }
        if (iface_.sgpr_push_count > 0) {
            const uint32_t n = iface_.sgpr_push_count;
            pc_struct_ = make_id();
            std::vector<uint32_t> members{pc_struct_};
            members.insert(members.end(), n, type_u32());
            pushv(module_, kOpTypeStruct, members);
            for (uint32_t i = 0; i < n; ++i) {
                mod(kOpMemberDecorate,
                    {pc_struct_, i, kDecorationOffset, i * 4});
            }
            mod(kOpDecorate, {pc_struct_, kDecorationBlock});
            pc_var_ = make_id();
            mod(kOpVariable, {type_ptr_u32_pc(), pc_var_, kStoragePushConstant});
        }
    }

    // --- body ---------------------------------------------------------------
    // Seeds VGPR v[4*i .. 4*i+3] from input variable i (vec4<f32>). Runs
    // inside the function frame: append_function() splices body_ between the
    // OpLabel and OpReturn, so these loads execute before the program ops.
    void seed_inputs() {
        for (uint32_t loc = 0; loc < iface_.input_locations; ++loc) {
            const uint32_t loaded = make_id();
            body(kOpLoad, {type_vec4_f32(), loaded, input_vars_[loc]});
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t comp = make_id();
                body(kOpCompositeExtract, {type_f32(), comp, loaded, c});
                set_vgpr(loc * 4 + c, Value{comp, type_f32()});
            }
        }
    }

    void translate_body(std::span<const uint32_t> code) {
        seed_inputs();
        size_t i = 0;
        bool ended = false;
        while (i < code.size() && !ended) {
            const auto ins = rdna2::decode(code.subspan(i));
            if (!ins.has_value()) {
                ++stats_.decode_errors;
                i += 1;
                continue;
            }
            const uint32_t size = instr_size(*ins);
            if (i + size > code.size()) {
                ++stats_.decode_errors;
                break;
            }
            ++stats_.decoded;
            i += size;
            const bool ok = translate_one(*ins);
            if (ok) ++stats_.translated;
            else ++stats_.skipped;
            if (ok && ins->format == rdna2::Format::SOPP &&
                ins->opcode == kSopkEndpgm) {
                ++stats_.endpgm;
                ended = true;
            }
        }
    }

    void append_function() {
        std::vector<uint32_t> fn;
        push(fn, kOpFunction,
             {type_void(), fn_id_, kFunctionControlNone, type_fn_main()});
        const uint32_t label = make_id();
        push(fn, kOpLabel, {label});
        // splice body between label and return
        module_.insert(module_.end(), fn.begin(), fn.end());
        module_.insert(module_.end(), body_.begin(), body_.end());
        push(module_, kOpReturn, {});
        push(module_, kOpFunctionEnd, {});
    }

    // --- value helpers -------------------------------------------------------
    void set_vgpr(uint32_t n, Value v) {
        if (n < kVgprCount) vgpr_[n] = v;
    }

    Value vgpr_value(uint32_t n) {
        if (n < kVgprCount && vgpr_[n].id != 0) return vgpr_[n];
        return Value{const_f32(0.0f), type_f32()};
    }

    Value sgpr_value(uint32_t n, bool& ok) {
        if (n >= kScalarSgprMax) { ok = false; return Value{}; }
        if (sgpr_[n].id != 0) return sgpr_[n];
        if (n < iface_.sgpr_push_count) {
            const uint32_t ptr = make_id();
            body(kOpAccessChain,
                 {type_ptr_u32_pc(), ptr, pc_var_, const_u32(n)});
            const uint32_t loaded = make_id();
            body(kOpLoad, {type_u32(), loaded, ptr});
            sgpr_[n] = Value{loaded, type_u32()};
            return sgpr_[n];
        }
        ok = false;
        return Value{};
    }

    Value set_sgpr(uint32_t n, Value v) {
        if (n < kScalarSgprMax) sgpr_[n] = v;
        return v;
    }

    Value bitcast(Value v, uint32_t want) {
        if (v.type == want) return v;
        const uint32_t id = make_id();
        body(kOpBitcast, {want, id, v.id});
        return Value{id, want};
    }
    Value as_float(Value v) { return bitcast(v, type_f32()); }
    Value as_uint(Value v) { return bitcast(v, type_u32()); }
    Value as_int(Value v) { return bitcast(v, type_i32()); }

    // RDNA2 scalar literal table (SOP sources, VOP scalar-mode sources).
    Value scalar_operand(uint32_t raw8, bool& ok) {
        ok = true;
        const uint32_t v = raw8 & 0xFF;
        if (v < kScalarSgprMax) return sgpr_value(v, ok);
        if (v == 125) return Value{const_u32(0), type_u32()}; // null
        if (v >= 128 && v <= 192) return Value{const_u32(v - 128), type_u32()};
        if (v >= 193 && v <= 208) {
            const uint32_t mag = v - 192; // -1..-16
            return Value{const_u32(0u - mag), type_u32()};
        }
        if (v >= 240 && v <= 247) {
            return Value{const_f32(rdna2::float_const_table()[v - 240]), type_f32()};
        }
        ok = false; // vcc / m0 / exec / literal-255 not handled here
        return Value{};
    }

    // VOP source: raw 9-bit field, bit8 = VGPR select. Literal sources take
    // the imm32 word carried in the instruction.
    Value vop_src(uint32_t raw9, uint32_t imm32, bool& ok) {
        ok = true;
        if ((raw9 >> 8) & 1) return vgpr_value(raw9 & 0xFF);
        if ((raw9 & 0xFF) == 255) {
            return Value{const_u32(imm32), type_u32()};
        }
        return scalar_operand(raw9 & 0xFF, ok);
    }

    // VOP2 e32 src1 field [16:9] is 8 bits and VGPR-only on GFX10 (scalar
    // sources force the e64 form).
    Value vop2_src1(uint32_t raw8) { return vgpr_value(raw8 & 0xFF); }

    // --- instruction emission -------------------------------------------------
    Value binop(uint16_t op, uint32_t type, Value a, Value b) {
        const uint32_t id = make_id();
        body(op, {type, id, a.id, b.id});
        return Value{id, type};
    }
    Value ext1(uint16_t glsl_op, uint32_t type, Value a) {
        const uint32_t id = make_id();
        body(kOpExtInst, {type, id, glsl_ext_, glsl_op, a.id});
        return Value{id, type};
    }
    Value ext2(uint16_t glsl_op, uint32_t type, Value a, Value b) {
        const uint32_t id = make_id();
        body(kOpExtInst, {type, id, glsl_ext_, glsl_op, a.id, b.id});
        return Value{id, type};
    }
    Value ext3(uint16_t glsl_op, uint32_t type, Value a, Value b, Value c) {
        const uint32_t id = make_id();
        body(kOpExtInst, {type, id, glsl_ext_, glsl_op, a.id, b.id, c.id});
        return Value{id, type};
    }

    bool translate_one(const rdna2::Instr& ins) {
        // EXP has no semantic-table entry; its fields are the instruction.
        if (ins.format == rdna2::Format::EXP) return translate_exp(ins);
        const auto* info = rdna2::lookup_op(ins.format, ins.opcode);
        if (info == nullptr) return false;
        if (!info->translatable) return false;

        switch (ins.format) {
        case rdna2::Format::SOPP: return true; // S_NOP; ENDPGM handled by caller
        case rdna2::Format::SOPK: return translate_sopk(ins);
        case rdna2::Format::SOP1: return translate_sop1(ins, *info);
        case rdna2::Format::SOP2: return translate_sop2(ins);
        case rdna2::Format::VOP1: return translate_vop1(ins);
        case rdna2::Format::VOP2:
            return emit_alu(ins, ins.opcode, ins.dst, false);
        case rdna2::Format::VOP3: return translate_vop3(ins);
        case rdna2::Format::EXP: return translate_exp(ins);
        default: return false;
        }
    }

    bool translate_sopk(const rdna2::Instr& ins) {
        // S_MOVK_I32: sdst = sign-extended simm16
        const uint32_t imm = ins.imm16;
        const uint32_t sext = (imm & 0x8000) ? (0xFFFF0000u | imm) : imm;
        set_sgpr(ins.dst, Value{const_u32(sext), type_u32()});
        return true;
    }

    bool translate_sop1(const rdna2::Instr& ins, const rdna2::OpInfo& info) {
        bool ok = true;
        Value src;
        if (ins.literal_src) {
            src = Value{const_u32(ins.imm32), type_u32()};
        } else {
            src = scalar_operand(ins.src0, ok);
            if (!ok) return false;
        }
        if (info.cls == rdna2::OpClass::ScalarMove) {
            set_sgpr(ins.dst, src);
            return true;
        }
        if (info.cls == rdna2::OpClass::ScalarArith && ins.opcode == 0x07) {
            // S_NOT_B32
            const uint32_t id = make_id();
            body(kOpNot, {type_u32(), id, as_uint(src).id});
            set_sgpr(ins.dst, Value{id, type_u32()});
            return true;
        }
        return false;
    }

    bool translate_sop2(const rdna2::Instr& ins) {
        bool ok0 = true, ok1 = true;
        Value a, b;
        if (ins.src0 == 255 && !ins.src0_is_vgpr) {
            a = Value{const_u32(ins.imm32), type_u32()};
        } else {
            a = scalar_operand(ins.src0, ok0);
        }
        if (ins.src1 == 255) {
            b = Value{const_u32(ins.imm32), type_u32()};
        } else {
            b = scalar_operand(ins.src1, ok1);
        }
        if (!ok0 || !ok1) return false;
        const Value au = as_uint(a), bu = as_uint(b);
        Value r;
        switch (ins.opcode) {
        case 0x00: case 0x02: r = binop(kOpIAdd, type_u32(), au, bu); break;
        case 0x01: case 0x03: r = binop(kOpISub, type_u32(), au, bu); break;
        case 0x06: r = ext2(kGlslSMin, type_i32(), as_int(au), as_int(bu)); break;
        case 0x07: r = ext2(kGlslUMin, type_u32(), au, bu); break;
        case 0x08: r = ext2(kGlslSMax, type_i32(), as_int(au), as_int(bu)); break;
        case 0x09: r = ext2(kGlslUMax, type_u32(), au, bu); break;
        case 0x0E: r = binop(kOpBitwiseAnd, type_u32(), au, bu); break;
        case 0x10: r = binop(kOpBitwiseOr, type_u32(), au, bu); break;
        case 0x12: r = binop(kOpBitwiseXor, type_u32(), au, bu); break;
        case 0x1E: r = binop(kOpShiftLeftLogical, type_u32(), bu, au); break;
        case 0x20: r = binop(kOpShiftRightLogical, type_u32(), bu, au); break;
        case 0x22:
            r = binop(kOpShiftRightArithmetic, type_i32(), as_int(bu), as_int(au));
            break;
        case 0x26: r = binop(kOpIMul, type_u32(), au, bu); break;
        default: return false;
        }
        set_sgpr(ins.dst, Value{r.id, type_u32()});
        return true;
    }

    bool translate_vop1(const rdna2::Instr& ins) {
        if (ins.opcode == 0x00) return true; // V_NOP
        bool ok = true;
        const Value src = vop_src(ins.src0, ins.imm32, ok);
        if (!ok) return false;
        switch (ins.opcode) {
        case 0x01: // V_MOV_B32: type-preserving copy
            set_vgpr(ins.dst, src);
            return true;
        case 0x05: { // V_CVT_F32_I32
            const uint32_t id = make_id();
            body(kOpConvertSToF, {type_f32(), id, as_int(src).id});
            set_vgpr(ins.dst, Value{id, type_f32()});
            return true;
        }
        case 0x06: { // V_CVT_F32_U32
            const uint32_t id = make_id();
            body(kOpConvertUToF, {type_f32(), id, as_uint(src).id});
            set_vgpr(ins.dst, Value{id, type_f32()});
            return true;
        }
        case 0x07: { // V_CVT_U32_F32
            const uint32_t id = make_id();
            body(kOpConvertFToU, {type_u32(), id, as_float(src).id});
            set_vgpr(ins.dst, Value{id, type_u32()});
            return true;
        }
        case 0x08: { // V_CVT_I32_F32
            const uint32_t id = make_id();
            body(kOpConvertFToS, {type_i32(), id, as_float(src).id});
            set_vgpr(ins.dst, Value{id, type_i32()});
            return true;
        }
        case 0x21: case 0x22: case 0x23: case 0x24: { // trunc/ceil/rndne/floor
            const uint16_t glsl = ins.opcode == 0x21 ? kGlslTrunc
                                : ins.opcode == 0x22 ? kGlslCeil
                                : ins.opcode == 0x23 ? kGlslRoundEven
                                : kGlslFloor;
            set_vgpr(ins.dst, ext1(glsl, type_f32(), as_float(src)));
            return true;
        }
        case 0x2A: { // V_RCP_F32 = 1/x
            const Value one{const_f32(1.0f), type_f32()};
            set_vgpr(ins.dst, binop(kOpFDiv, type_f32(), one, as_float(src)));
            return true;
        }
        case 0x33: // V_SQRT_F32
            set_vgpr(ins.dst, ext1(kGlslSqrt, type_f32(), as_float(src)));
            return true;
        case 0x37: { // V_NOT_B32
            const uint32_t id = make_id();
            body(kOpNot, {type_u32(), id, as_uint(src).id});
            set_vgpr(ins.dst, Value{id, type_u32()});
            return true;
        }
        default:
            return false;
        }
    }

    // VOP2 (and VOP2-in-VOP3) ALU dispatch. Shifts/rev ops take the operands
    // in RDNA order: dst = src1 <op> src0 for the *_REV forms. `e64` selects
    // the 9-bit src1 layout (VOP3) instead of the 8-bit VGPR-only field
    // (VOP2 e32).
    bool emit_alu(const rdna2::Instr& ins, uint32_t opcode, uint32_t dst_reg,
                  bool e64) {
        bool ok0 = true, ok1 = true;
        const Value a = vop_src(ins.src0, ins.imm32, ok0);
        const Value b = e64 ? vop_src(ins.src1, 0, ok1)
                            : Value{vop2_src1(ins.src1)};
        if (!ok0 || !ok1) return false;
        Value r;
        switch (opcode) {
        case 0x03: r = binop(kOpFAdd, type_f32(), as_float(a), as_float(b)); break;
        case 0x04: r = binop(kOpFSub, type_f32(), as_float(a), as_float(b)); break;
        case 0x05: r = binop(kOpFSub, type_f32(), as_float(b), as_float(a)); break;
        case 0x07: case 0x08:
            r = binop(kOpFMul, type_f32(), as_float(a), as_float(b));
            break;
        case 0x0F: r = ext2(kGlslFMin, type_f32(), as_float(a), as_float(b)); break;
        case 0x10: r = ext2(kGlslFMax, type_f32(), as_float(a), as_float(b)); break;
        case 0x11: r = ext2(kGlslSMin, type_i32(), as_int(a), as_int(b)); break;
        case 0x12: r = ext2(kGlslSMax, type_i32(), as_int(a), as_int(b)); break;
        case 0x13: r = ext2(kGlslUMin, type_u32(), as_uint(a), as_uint(b)); break;
        case 0x14: r = ext2(kGlslUMax, type_u32(), as_uint(a), as_uint(b)); break;
        case 0x16: r = binop(kOpShiftRightLogical, type_u32(), as_uint(b), as_uint(a)); break;
        case 0x18: r = binop(kOpShiftRightArithmetic, type_i32(), as_int(b), as_int(a)); break;
        case 0x1A: r = binop(kOpShiftLeftLogical, type_u32(), as_uint(b), as_uint(a)); break;
        case 0x1B: r = binop(kOpBitwiseAnd, type_u32(), as_uint(a), as_uint(b)); break;
        case 0x1C: r = binop(kOpBitwiseOr, type_u32(), as_uint(a), as_uint(b)); break;
        case 0x1D: r = binop(kOpBitwiseXor, type_u32(), as_uint(a), as_uint(b)); break;
        case 0x1E: {
            const Value x = binop(kOpBitwiseXor, type_u32(), as_uint(a), as_uint(b));
            const uint32_t id = make_id();
            body(kOpNot, {type_u32(), id, x.id});
            r = Value{id, type_u32()};
            break;
        }
        case 0x25: r = binop(kOpIAdd, type_u32(), as_uint(a), as_uint(b)); break;
        case 0x26: r = binop(kOpISub, type_u32(), as_uint(a), as_uint(b)); break;
        case 0x27: r = binop(kOpISub, type_u32(), as_uint(b), as_uint(a)); break;
        default: return false;
        }
        set_vgpr(dst_reg, r);
        return true;
    }

    bool translate_vop3(const rdna2::Instr& ins) {
        const uint32_t op = ins.opcode;
        if (op >= 0x100 && op <= 0x13F) {
            // VOP2-class ops in three-operand e64 form occupy 0x100-0x13F
            // (0x100 + the 6-bit VOP2 opcode); src2 is not read by them.
            // VOP3-only ops (V_FMA_F32 0x14B, V_MIN3 0x151, ...) live above.
            return emit_alu(ins, op - 0x100, ins.dst, true);
        }
        const auto* info = rdna2::lookup_op(rdna2::Format::VOP3, op);
        if (info == nullptr || !info->translatable) return false;
        bool ok0 = true, ok1 = true, ok2 = true;
        const Value a = vop_src(ins.src0, ins.imm32, ok0);
        const Value b = vop_src(ins.src1, 0, ok1);
        const Value c = vop_src(ins.src2, 0, ok2);
        if (!ok0 || !ok1 || !ok2) return false;
        if (op == 0x14B) { // V_FMA_F32
            set_vgpr(ins.dst,
                     ext3(kGlslFma, type_f32(), as_float(a), as_float(b),
                          as_float(c)));
            return true;
        }
        if (op == 0x151 || op == 0x154) { // V_MIN3_F32 / V_MAX3_F32
            const Value lo =
                ext2(op == 0x151 ? kGlslFMin : kGlslFMax, type_f32(),
                     as_float(a), as_float(b));
            set_vgpr(ins.dst,
                     ext2(op == 0x151 ? kGlslFMin : kGlslFMax, type_f32(), lo,
                          as_float(c)));
            return true;
        }
        return false;
    }

    bool translate_exp(const rdna2::Instr& ins) {
        if (ins.compr) return false;
        if (ins.target >= iface_.output_locations) return false;
        if (ins.en != 0xF) return false; // partial exports deferred
        std::array<Value, 4> comps;
        const uint32_t srcs[4] = {ins.src0, ins.src1, ins.src2, ins.dst};
        for (uint32_t c = 0; c < 4; ++c) {
            comps[c] = as_float(vgpr_value(srcs[c]));
        }
        const uint32_t vec = make_id();
        body(kOpCompositeConstruct,
             {type_vec4_f32(), vec, comps[0].id, comps[1].id, comps[2].id,
              comps[3].id});
        body(kOpStore, {output_vars_[ins.target], vec});
        return true;
    }

    const ShaderInterface& iface_;
    std::vector<uint32_t> module_;
    std::vector<uint32_t> body_;
    uint32_t next_id_ = 1;
    size_t header_bound_index_ = 0;

    uint32_t type_void_ = 0, type_f32_ = 0, type_u32_ = 0, type_i32_ = 0;
    uint32_t type_vec4_ = 0, type_fn_ = 0, pc_struct_ = 0;
    uint32_t type_ptr_pc_ = 0, type_ptr_in_ = 0, type_ptr_out_ = 0;
    uint32_t glsl_ext_ = 0, fn_id_ = 0, pc_var_ = 0;
    std::vector<uint32_t> input_vars_, output_vars_;
    std::unordered_map<uint32_t, uint32_t> u32_consts_, f32_consts_;

    std::array<Value, kVgprCount> vgpr_{};
    std::array<Value, kScalarSgprMax> sgpr_{};

    EmitStats stats_;
};

} // namespace

std::expected<EmitResult, Error> emit_fragment(std::span<const uint32_t> code,
                                               const ShaderInterface& iface) {
    Error err{Status::InternalError, ""};
    Emitter em(iface);
    auto result = em.run(code, err);
    if (!result.has_value()) return std::unexpected(err);
    return std::move(*result);
}

// ---------------------------------------------------------------------------
// inspect(): light SPIR-V binary walker used by tests and self-checks.
// ---------------------------------------------------------------------------
std::expected<SpirvInfo, Error> inspect(std::span<const uint32_t> words) {
    SpirvInfo info;
    if (words.size() < 5) {
        return std::unexpected(Error{Status::ParseError, "SPIR-V header truncated"});
    }
    if (words[0] != kSpirvMagic) {
        return std::unexpected(Error{Status::ParseError, "bad SPIR-V magic"});
    }
    const uint32_t version = words[1];
    info.version_major = static_cast<uint8_t>((version >> 16) & 0xFF);
    info.version_minor = static_cast<uint8_t>(version & 0xFF);
    info.generator = words[2];
    info.id_bound = words[3];
    info.schema = words[4];
    if (info.schema != 0) {
        return std::unexpected(Error{Status::ParseError, "schema must be 0"});
    }
    if (info.id_bound == 0) {
        return std::unexpected(Error{Status::ParseError, "id bound must be >= 1"});
    }

    std::unordered_map<uint32_t, uint32_t> var_class; // id -> storage class

    size_t i = 5;
    while (i < words.size()) {
        const uint16_t op = static_cast<uint16_t>(words[i] & 0xFFFF);
        const uint32_t wc = words[i] >> 16;
        if (wc == 0 || i + wc > words.size()) {
            info.errors.push_back("instruction word count overruns module");
            return std::unexpected(
                Error{Status::ParseError, "instruction overruns module"});
        }
        const std::span<const uint32_t> ins = words.subspan(i, wc);
        ++info.instruction_count;
        switch (op) {
        case 17: ++info.capability_count; break;
        case 14:
            info.memory_model_logical_glsl450 =
                ins[1] == 0 /* Logical */ && ins[2] == 1 /* GLSL450 */;
            break;
        case 15:
            info.entry_point_fragment = ins[1] == 4 /* Fragment */;
            break;
        case 11: {
            const char* s = reinterpret_cast<const char*>(&ins[2]);
            if (std::strncmp(s, "GLSL.std.450", 12) == 0) info.has_glsl_ext = true;
            break;
        }
        case 59:
            if (wc >= 4) var_class.emplace(ins[2], ins[3]);
            break;
        case 71:
            if (wc >= 4 && ins[2] == kDecorationLocation) {
                auto it = var_class.find(ins[1]);
                if (it != var_class.end()) {
                    if (it->second == kStorageInput) {
                        info.input_locations.push_back(ins[3]);
                    } else if (it->second == kStorageOutput) {
                        info.output_locations.push_back(ins[3]);
                    }
                }
            }
            break;
        case 72:
            if (wc >= 4 && ins[3] == kDecorationOffset) {
                info.push_constant_block = true;
            }
            break;
        case 54: ++info.function_count; break;
        case 248: ++info.label_count; break;
        case 253: ++info.return_count; break;
        default: break;
        }
        if (op == 12 || (op >= 109 && op <= 136) || (op >= 194 && op <= 200)) {
            info.body_opcodes.push_back(op);
        }
        i += wc;
    }
    return info;
}

const char* opcode_name(uint16_t op) {
    switch (op) {
    case 12: return "OpExtInst";
    case 61: return "OpLoad";
    case 62: return "OpStore";
    case 65: return "OpAccessChain";
    case 80: return "OpCompositeConstruct";
    case 81: return "OpCompositeExtract";
    case 109: return "OpConvertFToU";
    case 110: return "OpConvertFToS";
    case 111: return "OpConvertSToF";
    case 112: return "OpConvertUToF";
    case 124: return "OpBitcast";
    case 128: return "OpIAdd";
    case 129: return "OpFAdd";
    case 130: return "OpISub";
    case 131: return "OpFSub";
    case 132: return "OpIMul";
    case 133: return "OpFMul";
    case 136: return "OpFDiv";
    case 194: return "OpShiftRightLogical";
    case 195: return "OpShiftRightArithmetic";
    case 196: return "OpShiftLeftLogical";
    case 197: return "OpBitwiseOr";
    case 198: return "OpBitwiseXor";
    case 199: return "OpBitwiseAnd";
    case 200: return "OpNot";
    default: return "?";
    }
}

} // namespace armsx5::gpu::spirv

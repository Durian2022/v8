// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/assembler-inl.h"
#include "src/codegen/callable.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/compiler/backend/code-generator-impl.h"
#include "src/compiler/backend/code-generator.h"
#include "src/compiler/backend/gap-resolver.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/osr.h"
#include "src/heap/memory-chunk.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-code-manager.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {
namespace compiler {

#define __ tasm()->

// TODO(plind): consider renaming these macros.
#define TRACE_MSG(msg)                                                      \
  PrintF("code_gen: \'%s\' in function %s at line %d\n", msg, __FUNCTION__, \
         __LINE__)

#define TRACE_UNIMPL()                                                       \
  PrintF("UNIMPLEMENTED code_generator_mips: %s at line %d\n", __FUNCTION__, \
         __LINE__)

// Adds Mips-specific methods to convert InstructionOperands.
class MipsOperandConverter final : public InstructionOperandConverter {
 public:
  MipsOperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}

  FloatRegister OutputSingleRegister(size_t index = 0) {
    return ToSingleRegister(instr_->OutputAt(index));
  }

  FloatRegister InputSingleRegister(size_t index) {
    return ToSingleRegister(instr_->InputAt(index));
  }

  FloatRegister ToSingleRegister(InstructionOperand* op) {
    // Single (Float) and Double register namespace is same on MIPS,
    // both are typedefs of FPURegister.
    return ToDoubleRegister(op);
  }

  Register InputOrZeroRegister(size_t index) {
    if (instr_->InputAt(index)->IsImmediate()) {
      DCHECK_EQ(0, InputInt32(index));
      return zero_reg;
    }
    return InputRegister(index);
  }

  DoubleRegister InputOrZeroDoubleRegister(size_t index) {
    if (instr_->InputAt(index)->IsImmediate()) return kDoubleRegZero;

    return InputDoubleRegister(index);
  }

  DoubleRegister InputOrZeroSingleRegister(size_t index) {
    if (instr_->InputAt(index)->IsImmediate()) return kDoubleRegZero;

    return InputSingleRegister(index);
  }

  Operand InputImmediate(size_t index) {
    Constant constant = ToConstant(instr_->InputAt(index));
    switch (constant.type()) {
      case Constant::kInt32:
        return Operand(constant.ToInt32());
      case Constant::kFloat32:
        return Operand::EmbeddedNumber(constant.ToFloat32());
      case Constant::kFloat64:
        return Operand::EmbeddedNumber(constant.ToFloat64().value());
      case Constant::kInt64:
      case Constant::kExternalReference:
      case Constant::kCompressedHeapObject:
      case Constant::kHeapObject:
        // TODO(plind): Maybe we should handle ExtRef & HeapObj here?
        //    maybe not done on arm due to const pool ??
        break;
      case Constant::kDelayedStringConstant:
        return Operand::EmbeddedStringConstant(
            constant.ToDelayedStringConstant());
      case Constant::kRpoNumber:
        UNREACHABLE();  // TODO(titzer): RPO immediates on mips?
        break;
    }
    UNREACHABLE();
  }

  Operand InputOperand(size_t index) {
    InstructionOperand* op = instr_->InputAt(index);
    if (op->IsRegister()) {
      return Operand(ToRegister(op));
    }
    return InputImmediate(index);
  }

  MemOperand MemoryOperand(size_t* first_index) {
    const size_t index = *first_index;
    switch (AddressingModeField::decode(instr_->opcode())) {
      case kMode_None:
        break;
      case kMode_MRI:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputInt32(index + 1));
      case kMode_MRR:
        // TODO(plind): r6 address mode, to be implemented ...
        UNREACHABLE();
    }
    UNREACHABLE();
  }

  MemOperand MemoryOperand(size_t index = 0) { return MemoryOperand(&index); }

  MemOperand ToMemOperand(InstructionOperand* op) const {
    DCHECK_NOT_NULL(op);
    DCHECK(op->IsStackSlot() || op->IsFPStackSlot());
    return SlotToMemOperand(AllocatedOperand::cast(op)->index());
  }

  MemOperand SlotToMemOperand(int slot) const {
    FrameOffset offset = frame_access_state()->GetFrameOffset(slot);
    return MemOperand(offset.from_stack_pointer() ? sp : fp, offset.offset());
  }
};

static inline bool HasRegisterInput(Instruction* instr, size_t index) {
  return instr->InputAt(index)->IsRegister();
}

namespace {

class OutOfLineRecordWrite final : public OutOfLineCode {
 public:
  OutOfLineRecordWrite(CodeGenerator* gen, Register object, Register index,
                       Register value, Register scratch0, Register scratch1,
                       RecordWriteMode mode, StubCallMode stub_mode)
      : OutOfLineCode(gen),
        object_(object),
        index_(index),
        value_(value),
        scratch0_(scratch0),
        scratch1_(scratch1),
        mode_(mode),
#if V8_ENABLE_WEBASSEMBLY
        stub_mode_(stub_mode),
#endif  // V8_ENABLE_WEBASSEMBLY
        must_save_lr_(!gen->frame_access_state()->has_frame()),
        zone_(gen->zone()) {
    DCHECK(!AreAliased(object, index, scratch0, scratch1));
    DCHECK(!AreAliased(value, index, scratch0, scratch1));
  }

  void Generate() final {
    __ CheckPageFlag(value_, scratch0_,
                     MemoryChunk::kPointersToHereAreInterestingMask, eq,
                     exit());
    __ Addu(scratch1_, object_, index_);
    RememberedSetAction const remembered_set_action =
        mode_ > RecordWriteMode::kValueIsMap ? RememberedSetAction::kEmit
                                             : RememberedSetAction::kOmit;
    SaveFPRegsMode const save_fp_mode = frame()->DidAllocateDoubleRegisters()
                                            ? SaveFPRegsMode::kSave
                                            : SaveFPRegsMode::kIgnore;
    if (must_save_lr_) {
      // We need to save and restore ra if the frame was elided.
      __ Push(ra);
    }

    if (mode_ == RecordWriteMode::kValueIsEphemeronKey) {
      __ CallEphemeronKeyBarrier(object_, scratch1_, save_fp_mode);
#if V8_ENABLE_WEBASSEMBLY
    } else if (stub_mode_ == StubCallMode::kCallWasmRuntimeStub) {
      // A direct call to a wasm runtime stub defined in this module.
      // Just encode the stub index. This will be patched when the code
      // is added to the native module and copied into wasm code space.
      __ CallRecordWriteStubSaveRegisters(object_, scratch1_,
                                          remembered_set_action, save_fp_mode,
                                          StubCallMode::kCallWasmRuntimeStub);
#endif  // V8_ENABLE_WEBASSEMBLY
    } else {
      __ CallRecordWriteStubSaveRegisters(object_, scratch1_,
                                          remembered_set_action, save_fp_mode);
    }
    if (must_save_lr_) {
      __ Pop(ra);
    }
  }

 private:
  Register const object_;
  Register const index_;
  Register const value_;
  Register const scratch0_;
  Register const scratch1_;
  RecordWriteMode const mode_;
#if V8_ENABLE_WEBASSEMBLY
  StubCallMode const stub_mode_;
#endif  // V8_ENABLE_WEBASSEMBLY
  bool must_save_lr_;
  Zone* zone_;
};

#define CREATE_OOL_CLASS(ool_name, tasm_ool_name, T)                 \
  class ool_name final : public OutOfLineCode {                      \
   public:                                                           \
    ool_name(CodeGenerator* gen, T dst, T src1, T src2)              \
        : OutOfLineCode(gen), dst_(dst), src1_(src1), src2_(src2) {} \
                                                                     \
    void Generate() final { __ tasm_ool_name(dst_, src1_, src2_); }  \
                                                                     \
   private:                                                          \
    T const dst_;                                                    \
    T const src1_;                                                   \
    T const src2_;                                                   \
  }

CREATE_OOL_CLASS(OutOfLineFloat32Max, Float32MaxOutOfLine, FPURegister);
CREATE_OOL_CLASS(OutOfLineFloat32Min, Float32MinOutOfLine, FPURegister);
CREATE_OOL_CLASS(OutOfLineFloat64Max, Float64MaxOutOfLine, DoubleRegister);
CREATE_OOL_CLASS(OutOfLineFloat64Min, Float64MinOutOfLine, DoubleRegister);

#undef CREATE_OOL_CLASS

Condition FlagsConditionToConditionCmp(FlagsCondition condition) {
  switch (condition) {
    case kEqual:
      return eq;
    case kNotEqual:
      return ne;
    case kSignedLessThan:
      return lt;
    case kSignedGreaterThanOrEqual:
      return ge;
    case kSignedLessThanOrEqual:
      return le;
    case kSignedGreaterThan:
      return gt;
    case kUnsignedLessThan:
      return lo;
    case kUnsignedGreaterThanOrEqual:
      return hs;
    case kUnsignedLessThanOrEqual:
      return ls;
    case kUnsignedGreaterThan:
      return hi;
    case kUnorderedEqual:
    case kUnorderedNotEqual:
      break;
    default:
      break;
  }
  UNREACHABLE();
}

Condition FlagsConditionToConditionTst(FlagsCondition condition) {
  switch (condition) {
    case kNotEqual:
      return ne;
    case kEqual:
      return eq;
    default:
      break;
  }
  UNREACHABLE();
}

FPUCondition FlagsConditionToConditionCmpFPU(bool* predicate,
                                             FlagsCondition condition) {
  switch (condition) {
    case kEqual:
      *predicate = true;
      return EQ;
    case kNotEqual:
      *predicate = false;
      return EQ;
    case kUnsignedLessThan:
      *predicate = true;
      return OLT;
    case kUnsignedGreaterThanOrEqual:
      *predicate = false;
      return OLT;
    case kUnsignedLessThanOrEqual:
      *predicate = true;
      return OLE;
    case kUnsignedGreaterThan:
      *predicate = false;
      return OLE;
    case kUnorderedEqual:
    case kUnorderedNotEqual:
      *predicate = true;
      break;
    default:
      *predicate = true;
      break;
  }
  UNREACHABLE();
}

#define UNSUPPORTED_COND(opcode, condition)                                    \
  StdoutStream{} << "Unsupported " << #opcode << " condition: \"" << condition \
                 << "\"";                                                      \
  UNIMPLEMENTED();

void EmitWordLoadPoisoningIfNeeded(CodeGenerator* codegen,
                                   InstructionCode opcode, Instruction* instr,
                                   MipsOperandConverter const& i) {
  const MemoryAccessMode access_mode = AccessModeField::decode(opcode);
  if (access_mode == kMemoryAccessPoisoned) {
    Register value = i.OutputRegister();
    codegen->tasm()->And(value, value, kSpeculationPoisonRegister);
  }
}

}  // namespace

#define ASSEMBLE_ATOMIC_LOAD_INTEGER(asm_instr)          \
  do {                                                   \
    __ asm_instr(i.OutputRegister(), i.MemoryOperand()); \
    __ sync();                                           \
  } while (0)

#define ASSEMBLE_ATOMIC_STORE_INTEGER(asm_instr)               \
  do {                                                         \
    __ sync();                                                 \
    __ asm_instr(i.InputOrZeroRegister(2), i.MemoryOperand()); \
    __ sync();                                                 \
  } while (0)

#define ASSEMBLE_ATOMIC_BINOP(bin_instr)                                \
  do {                                                                  \
    Label binop;                                                        \
    __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1)); \
    __ sync();                                                          \
    __ bind(&binop);                                                    \
    __ Ll(i.OutputRegister(0), MemOperand(i.TempRegister(0), 0));       \
    __ bin_instr(i.TempRegister(1), i.OutputRegister(0),                \
                 Operand(i.InputRegister(2)));                          \
    __ Sc(i.TempRegister(1), MemOperand(i.TempRegister(0), 0));         \
    __ BranchShort(&binop, eq, i.TempRegister(1), Operand(zero_reg));   \
    __ sync();                                                          \
  } while (0)

#define ASSEMBLE_ATOMIC64_LOGIC_BINOP(bin_instr, external)                     \
  do {                                                                         \
    if (IsMipsArchVariant(kMips32r6)) {                                        \
      Label binop;                                                             \
      Register oldval_low =                                                    \
          instr->OutputCount() >= 1 ? i.OutputRegister(0) : i.TempRegister(1); \
      Register oldval_high =                                                   \
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(2); \
      __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));      \
      __ sync();                                                               \
      __ bind(&binop);                                                         \
      __ llx(oldval_high, MemOperand(i.TempRegister(0), 4));                   \
      __ ll(oldval_low, MemOperand(i.TempRegister(0), 0));                     \
      __ bin_instr(i.TempRegister(1), i.TempRegister(2), oldval_low,           \
                   oldval_high, i.InputRegister(2), i.InputRegister(3));       \
      __ scx(i.TempRegister(2), MemOperand(i.TempRegister(0), 4));             \
      __ sc(i.TempRegister(1), MemOperand(i.TempRegister(0), 0));              \
      __ BranchShort(&binop, eq, i.TempRegister(1), Operand(zero_reg));        \
      __ sync();                                                               \
    } else {                                                                   \
      FrameScope scope(tasm(), StackFrame::MANUAL);                            \
      __ Addu(a0, i.InputRegister(0), i.InputRegister(1));                     \
      __ PushCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);                     \
      __ PrepareCallCFunction(3, 0, kScratchReg);                              \
      __ CallCFunction(ExternalReference::external(), 3, 0);                   \
      __ PopCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);                      \
    }                                                                          \
  } while (0)

#define ASSEMBLE_ATOMIC64_ARITH_BINOP(bin_instr, external)                     \
  do {                                                                         \
    if (IsMipsArchVariant(kMips32r6)) {                                        \
      Label binop;                                                             \
      Register oldval_low =                                                    \
          instr->OutputCount() >= 1 ? i.OutputRegister(0) : i.TempRegister(1); \
      Register oldval_high =                                                   \
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(2); \
      __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));      \
      __ sync();                                                               \
      __ bind(&binop);                                                         \
      __ llx(oldval_high, MemOperand(i.TempRegister(0), 4));                   \
      __ ll(oldval_low, MemOperand(i.TempRegister(0), 0));                     \
      __ bin_instr(i.TempRegister(1), i.TempRegister(2), oldval_low,           \
                   oldval_high, i.InputRegister(2), i.InputRegister(3),        \
                   kScratchReg, kScratchReg2);                                 \
      __ scx(i.TempRegister(2), MemOperand(i.TempRegister(0), 4));             \
      __ sc(i.TempRegister(1), MemOperand(i.TempRegister(0), 0));              \
      __ BranchShort(&binop, eq, i.TempRegister(1), Operand(zero_reg));        \
      __ sync();                                                               \
    } else {                                                                   \
      FrameScope scope(tasm(), StackFrame::MANUAL);                            \
      __ Addu(a0, i.InputRegister(0), i.InputRegister(1));                     \
      __ PushCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);                     \
      __ PrepareCallCFunction(3, 0, kScratchReg);                              \
      __ CallCFunction(ExternalReference::external(), 3, 0);                   \
      __ PopCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);                      \
    }                                                                          \
  } while (0)

#define ASSEMBLE_ATOMIC_BINOP_EXT(sign_extend, size, bin_instr)                \
  do {                                                                         \
    Label binop;                                                               \
    __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));        \
    __ andi(i.TempRegister(3), i.TempRegister(0), 0x3);                        \
    __ Subu(i.TempRegister(0), i.TempRegister(0), Operand(i.TempRegister(3))); \
    __ sll(i.TempRegister(3), i.TempRegister(3), 3);                           \
    __ sync();                                                                 \
    __ bind(&binop);                                                           \
    __ Ll(i.TempRegister(1), MemOperand(i.TempRegister(0), 0));                \
    __ ExtractBits(i.OutputRegister(0), i.TempRegister(1), i.TempRegister(3),  \
                   size, sign_extend);                                         \
    __ bin_instr(i.TempRegister(2), i.OutputRegister(0),                       \
                 Operand(i.InputRegister(2)));                                 \
    __ InsertBits(i.TempRegister(1), i.TempRegister(2), i.TempRegister(3),     \
                  size);                                                       \
    __ Sc(i.TempRegister(1), MemOperand(i.TempRegister(0), 0));                \
    __ BranchShort(&binop, eq, i.TempRegister(1), Operand(zero_reg));          \
    __ sync();                                                                 \
  } while (0)

#define ASSEMBLE_ATOMIC_EXCHANGE_INTEGER()                               \
  do {                                                                   \
    Label exchange;                                                      \
    __ sync();                                                           \
    __ bind(&exchange);                                                  \
    __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));  \
    __ Ll(i.OutputRegister(0), MemOperand(i.TempRegister(0), 0));        \
    __ mov(i.TempRegister(1), i.InputRegister(2));                       \
    __ Sc(i.TempRegister(1), MemOperand(i.TempRegister(0), 0));          \
    __ BranchShort(&exchange, eq, i.TempRegister(1), Operand(zero_reg)); \
    __ sync();                                                           \
  } while (0)

#define ASSEMBLE_ATOMIC_EXCHANGE_INTEGER_EXT(sign_extend, size)                \
  do {                                                                         \
    Label exchange;                                                            \
    __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));        \
    __ andi(i.TempRegister(1), i.TempRegister(0), 0x3);                        \
    __ Subu(i.TempRegister(0), i.TempRegister(0), Operand(i.TempRegister(1))); \
    __ sll(i.TempRegister(1), i.TempRegister(1), 3);                           \
    __ sync();                                                                 \
    __ bind(&exchange);                                                        \
    __ Ll(i.TempRegister(2), MemOperand(i.TempRegister(0), 0));                \
    __ ExtractBits(i.OutputRegister(0), i.TempRegister(2), i.TempRegister(1),  \
                   size, sign_extend);                                         \
    __ InsertBits(i.TempRegister(2), i.InputRegister(2), i.TempRegister(1),    \
                  size);                                                       \
    __ Sc(i.TempRegister(2), MemOperand(i.TempRegister(0), 0));                \
    __ BranchShort(&exchange, eq, i.TempRegister(2), Operand(zero_reg));       \
    __ sync();                                                                 \
  } while (0)

#define ASSEMBLE_ATOMIC_COMPARE_EXCHANGE_INTEGER()                      \
  do {                                                                  \
    Label compareExchange;                                              \
    Label exit;                                                         \
    __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1)); \
    __ sync();                                                          \
    __ bind(&compareExchange);                                          \
    __ Ll(i.OutputRegister(0), MemOperand(i.TempRegister(0), 0));       \
    __ BranchShort(&exit, ne, i.InputRegister(2),                       \
                   Operand(i.OutputRegister(0)));                       \
    __ mov(i.TempRegister(2), i.InputRegister(3));                      \
    __ Sc(i.TempRegister(2), MemOperand(i.TempRegister(0), 0));         \
    __ BranchShort(&compareExchange, eq, i.TempRegister(2),             \
                   Operand(zero_reg));                                  \
    __ bind(&exit);                                                     \
    __ sync();                                                          \
  } while (0)

#define ASSEMBLE_ATOMIC_COMPARE_EXCHANGE_INTEGER_EXT(sign_extend, size)        \
  do {                                                                         \
    Label compareExchange;                                                     \
    Label exit;                                                                \
    __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));        \
    __ andi(i.TempRegister(1), i.TempRegister(0), 0x3);                        \
    __ Subu(i.TempRegister(0), i.TempRegister(0), Operand(i.TempRegister(1))); \
    __ sll(i.TempRegister(1), i.TempRegister(1), 3);                           \
    __ sync();                                                                 \
    __ bind(&compareExchange);                                                 \
    __ Ll(i.TempRegister(2), MemOperand(i.TempRegister(0), 0));                \
    __ ExtractBits(i.OutputRegister(0), i.TempRegister(2), i.TempRegister(1),  \
                   size, sign_extend);                                         \
    __ ExtractBits(i.InputRegister(2), i.InputRegister(2), zero_reg, size,     \
                   sign_extend);                                               \
    __ BranchShort(&exit, ne, i.InputRegister(2),                              \
                   Operand(i.OutputRegister(0)));                              \
    __ InsertBits(i.TempRegister(2), i.InputRegister(3), i.TempRegister(1),    \
                  size);                                                       \
    __ Sc(i.TempRegister(2), MemOperand(i.TempRegister(0), 0));                \
    __ BranchShort(&compareExchange, eq, i.TempRegister(2),                    \
                   Operand(zero_reg));                                         \
    __ bind(&exit);                                                            \
    __ sync();                                                                 \
  } while (0)

#define ASSEMBLE_IEEE754_BINOP(name)                                        \
  do {                                                                      \
    FrameScope scope(tasm(), StackFrame::MANUAL);                           \
    __ PrepareCallCFunction(0, 2, kScratchReg);                             \
    __ MovToFloatParameters(i.InputDoubleRegister(0),                       \
                            i.InputDoubleRegister(1));                      \
    __ CallCFunction(ExternalReference::ieee754_##name##_function(), 0, 2); \
    /* Move the result in the double result register. */                    \
    __ MovFromFloatResult(i.OutputDoubleRegister());                        \
  } while (0)

#define ASSEMBLE_IEEE754_UNOP(name)                                         \
  do {                                                                      \
    FrameScope scope(tasm(), StackFrame::MANUAL);                           \
    __ PrepareCallCFunction(0, 1, kScratchReg);                             \
    __ MovToFloatParameter(i.InputDoubleRegister(0));                       \
    __ CallCFunction(ExternalReference::ieee754_##name##_function(), 0, 1); \
    /* Move the result in the double result register. */                    \
    __ MovFromFloatResult(i.OutputDoubleRegister());                        \
  } while (0)

#define ASSEMBLE_F64X2_ARITHMETIC_BINOP(op)                     \
  do {                                                          \
    __ op(i.OutputSimd128Register(), i.InputSimd128Register(0), \
          i.InputSimd128Register(1));                           \
  } while (0)

#define ASSEMBLE_SIMD_EXTENDED_MULTIPLY(op0, op1)                           \
  do {                                                                      \
    CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);                           \
    __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);            \
    __ op0(kSimd128ScratchReg, kSimd128RegZero, i.InputSimd128Register(0)); \
    __ op0(kSimd128RegZero, kSimd128RegZero, i.InputSimd128Register(1));    \
    __ op1(i.OutputSimd128Register(), kSimd128ScratchReg, kSimd128RegZero); \
  } while (0)

void CodeGenerator::AssembleDeconstructFrame() {
  __ mov(sp, fp);
  __ Pop(ra, fp);
}

void CodeGenerator::AssemblePrepareTailCall() {
  if (frame_access_state()->has_frame()) {
    __ lw(ra, MemOperand(fp, StandardFrameConstants::kCallerPCOffset));
    __ lw(fp, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  }
  frame_access_state()->SetFrameAccessToSP();
}
namespace {

void AdjustStackPointerForTailCall(TurboAssembler* tasm,
                                   FrameAccessState* state,
                                   int new_slot_above_sp,
                                   bool allow_shrinkage = true) {
  int current_sp_offset = state->GetSPToFPSlotCount() +
                          StandardFrameConstants::kFixedSlotCountAboveFp;
  int stack_slot_delta = new_slot_above_sp - current_sp_offset;
  if (stack_slot_delta > 0) {
    tasm->Subu(sp, sp, stack_slot_delta * kSystemPointerSize);
    state->IncreaseSPDelta(stack_slot_delta);
  } else if (allow_shrinkage && stack_slot_delta < 0) {
    tasm->Addu(sp, sp, -stack_slot_delta * kSystemPointerSize);
    state->IncreaseSPDelta(stack_slot_delta);
  }
}

}  // namespace

void CodeGenerator::AssembleTailCallBeforeGap(Instruction* instr,
                                              int first_unused_slot_offset) {
  AdjustStackPointerForTailCall(tasm(), frame_access_state(),
                                first_unused_slot_offset, false);
}

void CodeGenerator::AssembleTailCallAfterGap(Instruction* instr,
                                             int first_unused_slot_offset) {
  AdjustStackPointerForTailCall(tasm(), frame_access_state(),
                                first_unused_slot_offset);
}

// Check that {kJavaScriptCallCodeStartRegister} is correct.
void CodeGenerator::AssembleCodeStartRegisterCheck() {
  __ ComputeCodeStartAddress(kScratchReg);
  __ Assert(eq, AbortReason::kWrongFunctionCodeStart,
            kJavaScriptCallCodeStartRegister, Operand(kScratchReg));
}

// Check if the code object is marked for deoptimization. If it is, then it
// jumps to the CompileLazyDeoptimizedCode builtin. In order to do this we need
// to:
//    1. read from memory the word that contains that bit, which can be found in
//       the flags in the referenced {CodeDataContainer} object;
//    2. test kMarkedForDeoptimizationBit in those flags; and
//    3. if it is not zero then it jumps to the builtin.
void CodeGenerator::BailoutIfDeoptimized() {
  int offset = Code::kCodeDataContainerOffset - Code::kHeaderSize;
  __ lw(kScratchReg, MemOperand(kJavaScriptCallCodeStartRegister, offset));
  __ lw(kScratchReg,
        FieldMemOperand(kScratchReg,
                        CodeDataContainer::kKindSpecificFlagsOffset));
  __ And(kScratchReg, kScratchReg,
         Operand(1 << Code::kMarkedForDeoptimizationBit));
  __ Jump(BUILTIN_CODE(isolate(), CompileLazyDeoptimizedCode),
          RelocInfo::CODE_TARGET, ne, kScratchReg, Operand(zero_reg));
}

void CodeGenerator::GenerateSpeculationPoisonFromCodeStartRegister() {
  // Calculate a mask which has all bits set in the normal case, but has all
  // bits cleared if we are speculatively executing the wrong PC.
  //    difference = (current - expected) | (expected - current)
  //    poison = ~(difference >> (kBitsPerSystemPointer - 1))
  __ ComputeCodeStartAddress(kScratchReg);
  __ Move(kSpeculationPoisonRegister, kScratchReg);
  __ subu(kSpeculationPoisonRegister, kSpeculationPoisonRegister,
          kJavaScriptCallCodeStartRegister);
  __ subu(kJavaScriptCallCodeStartRegister, kJavaScriptCallCodeStartRegister,
          kScratchReg);
  __ or_(kSpeculationPoisonRegister, kSpeculationPoisonRegister,
         kJavaScriptCallCodeStartRegister);
  __ sra(kSpeculationPoisonRegister, kSpeculationPoisonRegister,
         kBitsPerSystemPointer - 1);
  __ nor(kSpeculationPoisonRegister, kSpeculationPoisonRegister,
         kSpeculationPoisonRegister);
}

void CodeGenerator::AssembleRegisterArgumentPoisoning() {
  __ And(kJSFunctionRegister, kJSFunctionRegister, kSpeculationPoisonRegister);
  __ And(kContextRegister, kContextRegister, kSpeculationPoisonRegister);
  __ And(sp, sp, kSpeculationPoisonRegister);
}

// Assembles an instruction after register allocation, producing machine code.
CodeGenerator::CodeGenResult CodeGenerator::AssembleArchInstruction(
    Instruction* instr) {
  MipsOperandConverter i(this, instr);
  InstructionCode opcode = instr->opcode();
  ArchOpcode arch_opcode = ArchOpcodeField::decode(opcode);
  switch (arch_opcode) {
    case kArchCallCodeObject: {
      if (instr->InputAt(0)->IsImmediate()) {
        __ Call(i.InputCode(0), RelocInfo::CODE_TARGET);
      } else {
        Register reg = i.InputRegister(0);
        DCHECK_IMPLIES(
            instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister),
            reg == kJavaScriptCallCodeStartRegister);
        __ Call(reg, reg, Code::kHeaderSize - kHeapObjectTag);
      }
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchCallBuiltinPointer: {
      DCHECK(!instr->InputAt(0)->IsImmediate());
      Register builtin_index = i.InputRegister(0);
      __ CallBuiltinByIndex(builtin_index);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
#if V8_ENABLE_WEBASSEMBLY
    case kArchCallWasmFunction: {
      if (instr->InputAt(0)->IsImmediate()) {
        Constant constant = i.ToConstant(instr->InputAt(0));
        Address wasm_code = static_cast<Address>(constant.ToInt32());
        __ Call(wasm_code, constant.rmode());
      } else {
        __ Call(i.InputRegister(0));
      }
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchTailCallWasm: {
      if (instr->InputAt(0)->IsImmediate()) {
        Constant constant = i.ToConstant(instr->InputAt(0));
        Address wasm_code = static_cast<Address>(constant.ToInt32());
        __ Jump(wasm_code, constant.rmode());
      } else {
        __ Jump(i.InputRegister(0));
      }
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
#endif  // V8_ENABLE_WEBASSEMBLY
    case kArchTailCallCodeObject: {
      if (instr->InputAt(0)->IsImmediate()) {
        __ Jump(i.InputCode(0), RelocInfo::CODE_TARGET);
      } else {
        Register reg = i.InputRegister(0);
        DCHECK_IMPLIES(
            instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister),
            reg == kJavaScriptCallCodeStartRegister);
        __ Addu(reg, reg, Code::kHeaderSize - kHeapObjectTag);
        __ Jump(reg);
      }
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchTailCallAddress: {
      CHECK(!instr->InputAt(0)->IsImmediate());
      Register reg = i.InputRegister(0);
      DCHECK_IMPLIES(
          instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister),
          reg == kJavaScriptCallCodeStartRegister);
      __ Jump(reg);
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchCallJSFunction: {
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ lw(kScratchReg, FieldMemOperand(func, JSFunction::kContextOffset));
        __ Assert(eq, AbortReason::kWrongFunctionContext, cp,
                  Operand(kScratchReg));
      }
      static_assert(kJavaScriptCallCodeStartRegister == a2, "ABI mismatch");
      __ lw(a2, FieldMemOperand(func, JSFunction::kCodeOffset));
      __ Addu(a2, a2, Code::kHeaderSize - kHeapObjectTag);
      __ Call(a2);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchPrepareCallCFunction: {
      int const num_parameters = MiscField::decode(instr->opcode());
      __ PrepareCallCFunction(num_parameters, kScratchReg);
      // Frame alignment requires using FP-relative frame addressing.
      frame_access_state()->SetFrameAccessToFP();
      break;
    }
    case kArchSaveCallerRegisters: {
      fp_mode_ =
          static_cast<SaveFPRegsMode>(MiscField::decode(instr->opcode()));
      DCHECK(fp_mode_ == SaveFPRegsMode::kIgnore ||
             fp_mode_ == SaveFPRegsMode::kSave);
      // kReturnRegister0 should have been saved before entering the stub.
      int bytes = __ PushCallerSaved(fp_mode_, kReturnRegister0);
      DCHECK(IsAligned(bytes, kSystemPointerSize));
      DCHECK_EQ(0, frame_access_state()->sp_delta());
      frame_access_state()->IncreaseSPDelta(bytes / kSystemPointerSize);
      DCHECK(!caller_registers_saved_);
      caller_registers_saved_ = true;
      break;
    }
    case kArchRestoreCallerRegisters: {
      DCHECK(fp_mode_ ==
             static_cast<SaveFPRegsMode>(MiscField::decode(instr->opcode())));
      DCHECK(fp_mode_ == SaveFPRegsMode::kIgnore ||
             fp_mode_ == SaveFPRegsMode::kSave);
      // Don't overwrite the returned value.
      int bytes = __ PopCallerSaved(fp_mode_, kReturnRegister0);
      frame_access_state()->IncreaseSPDelta(-(bytes / kSystemPointerSize));
      DCHECK_EQ(0, frame_access_state()->sp_delta());
      DCHECK(caller_registers_saved_);
      caller_registers_saved_ = false;
      break;
    }
    case kArchPrepareTailCall:
      AssemblePrepareTailCall();
      break;
    case kArchCallCFunction: {
      int const num_parameters = MiscField::decode(instr->opcode());
#if V8_ENABLE_WEBASSEMBLY
      Label start_call;
      bool isWasmCapiFunction =
          linkage()->GetIncomingDescriptor()->IsWasmCapiFunction();
      // from start_call to return address.
      int offset = __ root_array_available() ? 64 : 88;
#endif  // V8_ENABLE_WEBASSEMBLY
#if V8_HOST_ARCH_MIPS
      if (FLAG_debug_code) {
        offset += 16;
      }
#endif

#if V8_ENABLE_WEBASSEMBLY
      if (isWasmCapiFunction) {
        // Put the return address in a stack slot.
        __ mov(kScratchReg, ra);
        __ bind(&start_call);
        __ nal();
        __ nop();
        __ Addu(ra, ra, offset - 8);  // 8 = nop + nal
        __ sw(ra, MemOperand(fp, WasmExitFrameConstants::kCallingPCOffset));
        __ mov(ra, kScratchReg);
      }
#endif  // V8_ENABLE_WEBASSEMBLY

      if (instr->InputAt(0)->IsImmediate()) {
        ExternalReference ref = i.InputExternalReference(0);
        __ CallCFunction(ref, num_parameters);
      } else {
        Register func = i.InputRegister(0);
        __ CallCFunction(func, num_parameters);
      }

#if V8_ENABLE_WEBASSEMBLY
      if (isWasmCapiFunction) {
        CHECK_EQ(offset, __ SizeOfCodeGeneratedSince(&start_call));
        RecordSafepoint(instr->reference_map());
      }
#endif  // V8_ENABLE_WEBASSEMBLY

      frame_access_state()->SetFrameAccessToDefault();
      // Ideally, we should decrement SP delta to match the change of stack
      // pointer in CallCFunction. However, for certain architectures (e.g.
      // ARM), there may be more strict alignment requirement, causing old SP
      // to be saved on the stack. In those cases, we can not calculate the SP
      // delta statically.
      frame_access_state()->ClearSPDelta();
      if (caller_registers_saved_) {
        // Need to re-sync SP delta introduced in kArchSaveCallerRegisters.
        // Here, we assume the sequence to be:
        //   kArchSaveCallerRegisters;
        //   kArchCallCFunction;
        //   kArchRestoreCallerRegisters;
        int bytes =
            __ RequiredStackSizeForCallerSaved(fp_mode_, kReturnRegister0);
        frame_access_state()->IncreaseSPDelta(bytes / kSystemPointerSize);
      }
      break;
    }
    case kArchJmp:
      AssembleArchJump(i.InputRpo(0));
      break;
    case kArchBinarySearchSwitch:
      AssembleArchBinarySearchSwitch(instr);
      break;
    case kArchTableSwitch:
      AssembleArchTableSwitch(instr);
      break;
    case kArchAbortCSAAssert:
      DCHECK(i.InputRegister(0) == a0);
      {
        // We don't actually want to generate a pile of code for this, so just
        // claim there is a stack frame, without generating one.
        FrameScope scope(tasm(), StackFrame::NONE);
        __ Call(isolate()->builtins()->builtin_handle(Builtin::kAbortCSAAssert),
                RelocInfo::CODE_TARGET);
      }
      __ stop();
      break;
    case kArchDebugBreak:
      __ DebugBreak();
      break;
    case kArchComment:
      __ RecordComment(reinterpret_cast<const char*>(i.InputInt32(0)));
      break;
    case kArchNop:
    case kArchThrowTerminator:
      // don't emit code for nops.
      break;
    case kArchDeoptimize: {
      DeoptimizationExit* exit =
          BuildTranslation(instr, -1, 0, 0, OutputFrameStateCombine::Ignore());
      __ Branch(exit->label());
      break;
    }
    case kArchRet:
      AssembleReturn(instr->InputAt(0));
      break;
    case kArchStackPointerGreaterThan: {
      Register lhs_register = sp;
      uint32_t offset;
      if (ShouldApplyOffsetToStackCheck(instr, &offset)) {
        lhs_register = i.TempRegister(1);
        __ Subu(lhs_register, sp, offset);
      }
      __ Sltu(i.TempRegister(0), i.InputRegister(0), lhs_register);
      break;
    }
    case kArchStackCheckOffset:
      __ Move(i.OutputRegister(), Smi::FromInt(GetStackCheckOffset()));
      break;
    case kArchFramePointer:
      __ mov(i.OutputRegister(), fp);
      break;
    case kArchParentFramePointer:
      if (frame_access_state()->has_frame()) {
        __ lw(i.OutputRegister(), MemOperand(fp, 0));
      } else {
        __ mov(i.OutputRegister(), fp);
      }
      break;
    case kArchTruncateDoubleToI:
      __ TruncateDoubleToI(isolate(), zone(), i.OutputRegister(),
                           i.InputDoubleRegister(0), DetermineStubCallMode());
      break;
    case kArchStoreWithWriteBarrier: {
      RecordWriteMode mode =
          static_cast<RecordWriteMode>(MiscField::decode(instr->opcode()));
      Register object = i.InputRegister(0);
      Register index = i.InputRegister(1);
      Register value = i.InputRegister(2);
      Register scratch0 = i.TempRegister(0);
      Register scratch1 = i.TempRegister(1);
      auto ool = zone()->New<OutOfLineRecordWrite>(this, object, index, value,
                                                   scratch0, scratch1, mode,
                                                   DetermineStubCallMode());
      __ Addu(kScratchReg, object, index);
      __ sw(value, MemOperand(kScratchReg));
      if (mode > RecordWriteMode::kValueIsPointer) {
        __ JumpIfSmi(value, ool->exit());
      }
      __ CheckPageFlag(object, scratch0,
                       MemoryChunk::kPointersFromHereAreInterestingMask, ne,
                       ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kArchStackSlot: {
      FrameOffset offset =
          frame_access_state()->GetFrameOffset(i.InputInt32(0));
      Register base_reg = offset.from_stack_pointer() ? sp : fp;
      __ Addu(i.OutputRegister(), base_reg, Operand(offset.offset()));
      if (FLAG_debug_code > 0) {
        // Verify that the output_register is properly aligned
        __ And(kScratchReg, i.OutputRegister(),
               Operand(kSystemPointerSize - 1));
        __ Assert(eq, AbortReason::kAllocationIsNotDoubleAligned, kScratchReg,
                  Operand(zero_reg));
      }
      break;
    }
    case kArchWordPoisonOnSpeculation:
      __ And(i.OutputRegister(), i.InputRegister(0),
             kSpeculationPoisonRegister);
      break;
    case kIeee754Float64Acos:
      ASSEMBLE_IEEE754_UNOP(acos);
      break;
    case kIeee754Float64Acosh:
      ASSEMBLE_IEEE754_UNOP(acosh);
      break;
    case kIeee754Float64Asin:
      ASSEMBLE_IEEE754_UNOP(asin);
      break;
    case kIeee754Float64Asinh:
      ASSEMBLE_IEEE754_UNOP(asinh);
      break;
    case kIeee754Float64Atan:
      ASSEMBLE_IEEE754_UNOP(atan);
      break;
    case kIeee754Float64Atanh:
      ASSEMBLE_IEEE754_UNOP(atanh);
      break;
    case kIeee754Float64Atan2:
      ASSEMBLE_IEEE754_BINOP(atan2);
      break;
    case kIeee754Float64Cos:
      ASSEMBLE_IEEE754_UNOP(cos);
      break;
    case kIeee754Float64Cosh:
      ASSEMBLE_IEEE754_UNOP(cosh);
      break;
    case kIeee754Float64Cbrt:
      ASSEMBLE_IEEE754_UNOP(cbrt);
      break;
    case kIeee754Float64Exp:
      ASSEMBLE_IEEE754_UNOP(exp);
      break;
    case kIeee754Float64Expm1:
      ASSEMBLE_IEEE754_UNOP(expm1);
      break;
    case kIeee754Float64Log:
      ASSEMBLE_IEEE754_UNOP(log);
      break;
    case kIeee754Float64Log1p:
      ASSEMBLE_IEEE754_UNOP(log1p);
      break;
    case kIeee754Float64Log10:
      ASSEMBLE_IEEE754_UNOP(log10);
      break;
    case kIeee754Float64Log2:
      ASSEMBLE_IEEE754_UNOP(log2);
      break;
    case kIeee754Float64Pow:
      ASSEMBLE_IEEE754_BINOP(pow);
      break;
    case kIeee754Float64Sin:
      ASSEMBLE_IEEE754_UNOP(sin);
      break;
    case kIeee754Float64Sinh:
      ASSEMBLE_IEEE754_UNOP(sinh);
      break;
    case kIeee754Float64Tan:
      ASSEMBLE_IEEE754_UNOP(tan);
      break;
    case kIeee754Float64Tanh:
      ASSEMBLE_IEEE754_UNOP(tanh);
      break;
    case kMipsAdd:
      __ Addu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsAddOvf:
      __ AddOverflow(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1),
                     kScratchReg);
      break;
    case kMipsSub:
      __ Subu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsSubOvf:
      __ SubOverflow(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1),
                     kScratchReg);
      break;
    case kMipsMul:
      __ Mul(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsMulOvf:
      __ MulOverflow(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1),
                     kScratchReg);
      break;
    case kMipsMulHigh:
      __ Mulh(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsMulHighU:
      __ Mulhu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsDiv:
      __ Div(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      if (IsMipsArchVariant(kMips32r6)) {
        __ selnez(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      } else {
        __ Movz(i.OutputRegister(), i.InputRegister(1), i.InputRegister(1));
      }
      break;
    case kMipsDivU:
      __ Divu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      if (IsMipsArchVariant(kMips32r6)) {
        __ selnez(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      } else {
        __ Movz(i.OutputRegister(), i.InputRegister(1), i.InputRegister(1));
      }
      break;
    case kMipsMod:
      __ Mod(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsModU:
      __ Modu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsAnd:
      __ And(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsOr:
      __ Or(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsNor:
      if (instr->InputAt(1)->IsRegister()) {
        __ Nor(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      } else {
        DCHECK_EQ(0, i.InputOperand(1).immediate());
        __ Nor(i.OutputRegister(), i.InputRegister(0), zero_reg);
      }
      break;
    case kMipsXor:
      __ Xor(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsClz:
      __ Clz(i.OutputRegister(), i.InputRegister(0));
      break;
    case kMipsCtz: {
      Register src = i.InputRegister(0);
      Register dst = i.OutputRegister();
      __ Ctz(dst, src);
    } break;
    case kMipsPopcnt: {
      Register src = i.InputRegister(0);
      Register dst = i.OutputRegister();
      __ Popcnt(dst, src);
    } break;
    case kMipsShl:
      if (instr->InputAt(1)->IsRegister()) {
        __ sllv(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      } else {
        int32_t imm = i.InputOperand(1).immediate();
        __ sll(i.OutputRegister(), i.InputRegister(0), imm);
      }
      break;
    case kMipsShr:
      if (instr->InputAt(1)->IsRegister()) {
        __ srlv(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      } else {
        int32_t imm = i.InputOperand(1).immediate();
        __ srl(i.OutputRegister(), i.InputRegister(0), imm);
      }
      break;
    case kMipsSar:
      if (instr->InputAt(1)->IsRegister()) {
        __ srav(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      } else {
        int32_t imm = i.InputOperand(1).immediate();
        __ sra(i.OutputRegister(), i.InputRegister(0), imm);
      }
      break;
    case kMipsShlPair: {
      Register second_output =
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(0);
      if (instr->InputAt(2)->IsRegister()) {
        __ ShlPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                   i.InputRegister(1), i.InputRegister(2), kScratchReg,
                   kScratchReg2);
      } else {
        uint32_t imm = i.InputOperand(2).immediate();
        __ ShlPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                   i.InputRegister(1), imm, kScratchReg);
      }
    } break;
    case kMipsShrPair: {
      Register second_output =
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(0);
      if (instr->InputAt(2)->IsRegister()) {
        __ ShrPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                   i.InputRegister(1), i.InputRegister(2), kScratchReg,
                   kScratchReg2);
      } else {
        uint32_t imm = i.InputOperand(2).immediate();
        __ ShrPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                   i.InputRegister(1), imm, kScratchReg);
      }
    } break;
    case kMipsSarPair: {
      Register second_output =
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(0);
      if (instr->InputAt(2)->IsRegister()) {
        __ SarPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                   i.InputRegister(1), i.InputRegister(2), kScratchReg,
                   kScratchReg2);
      } else {
        uint32_t imm = i.InputOperand(2).immediate();
        __ SarPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                   i.InputRegister(1), imm, kScratchReg);
      }
    } break;
    case kMipsExt:
      __ Ext(i.OutputRegister(), i.InputRegister(0), i.InputInt8(1),
             i.InputInt8(2));
      break;
    case kMipsIns:
      if (instr->InputAt(1)->IsImmediate() && i.InputInt8(1) == 0) {
        __ Ins(i.OutputRegister(), zero_reg, i.InputInt8(1), i.InputInt8(2));
      } else {
        __ Ins(i.OutputRegister(), i.InputRegister(0), i.InputInt8(1),
               i.InputInt8(2));
      }
      break;
    case kMipsRor:
      __ Ror(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsTst:
      __ And(kScratchReg, i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsCmp:
      // Pseudo-instruction used for cmp/branch. No opcode emitted here.
      break;
    case kMipsMov:
      // TODO(plind): Should we combine mov/li like this, or use separate instr?
      //    - Also see x64 ASSEMBLE_BINOP & RegisterOrOperandType
      if (HasRegisterInput(instr, 0)) {
        __ mov(i.OutputRegister(), i.InputRegister(0));
      } else {
        __ li(i.OutputRegister(), i.InputOperand(0));
      }
      break;
    case kMipsLsa:
      DCHECK(instr->InputAt(2)->IsImmediate());
      __ Lsa(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1),
             i.InputInt8(2));
      break;
    case kMipsCmpS: {
      FPURegister left = i.InputOrZeroSingleRegister(0);
      FPURegister right = i.InputOrZeroSingleRegister(1);
      bool predicate;
      FPUCondition cc =
          FlagsConditionToConditionCmpFPU(&predicate, instr->flags_condition());

      if ((left == kDoubleRegZero || right == kDoubleRegZero) &&
          !__ IsDoubleZeroRegSet()) {
        __ Move(kDoubleRegZero, 0.0);
      }

      __ CompareF32(cc, left, right);
    } break;
    case kMipsAddS:
      // TODO(plind): add special case: combine mult & add.
      __ add_s(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsSubS:
      __ sub_s(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsMulS:
      // TODO(plind): add special case: right op is -1.0, see arm port.
      __ mul_s(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsDivS:
      __ div_s(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsAbsS:
      if (IsMipsArchVariant(kMips32r6)) {
        __ abs_s(i.OutputSingleRegister(), i.InputSingleRegister(0));
      } else {
        __ mfc1(kScratchReg, i.InputSingleRegister(0));
        __ Ins(kScratchReg, zero_reg, 31, 1);
        __ mtc1(kScratchReg, i.OutputSingleRegister());
      }
      break;
    case kMipsSqrtS: {
      __ sqrt_s(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    }
    case kMipsMaxS:
      __ max_s(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsMinS:
      __ min_s(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsCmpD: {
      FPURegister left = i.InputOrZeroDoubleRegister(0);
      FPURegister right = i.InputOrZeroDoubleRegister(1);
      bool predicate;
      FPUCondition cc =
          FlagsConditionToConditionCmpFPU(&predicate, instr->flags_condition());
      if ((left == kDoubleRegZero || right == kDoubleRegZero) &&
          !__ IsDoubleZeroRegSet()) {
        __ Move(kDoubleRegZero, 0.0);
      }
      __ CompareF64(cc, left, right);
    } break;
    case kMipsAddPair:
      __ AddPair(i.OutputRegister(0), i.OutputRegister(1), i.InputRegister(0),
                 i.InputRegister(1), i.InputRegister(2), i.InputRegister(3),
                 kScratchReg, kScratchReg2);
      break;
    case kMipsSubPair:
      __ SubPair(i.OutputRegister(0), i.OutputRegister(1), i.InputRegister(0),
                 i.InputRegister(1), i.InputRegister(2), i.InputRegister(3),
                 kScratchReg, kScratchReg2);
      break;
    case kMipsMulPair: {
      __ MulPair(i.OutputRegister(0), i.OutputRegister(1), i.InputRegister(0),
                 i.InputRegister(1), i.InputRegister(2), i.InputRegister(3),
                 kScratchReg, kScratchReg2);
    } break;
    case kMipsAddD:
      // TODO(plind): add special case: combine mult & add.
      __ add_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsSubD:
      __ sub_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsMaddS:
      __ Madd_s(i.OutputFloatRegister(), i.InputFloatRegister(0),
                i.InputFloatRegister(1), i.InputFloatRegister(2),
                kScratchDoubleReg);
      break;
    case kMipsMaddD:
      __ Madd_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputDoubleRegister(1), i.InputDoubleRegister(2),
                kScratchDoubleReg);
      break;
    case kMipsMsubS:
      __ Msub_s(i.OutputFloatRegister(), i.InputFloatRegister(0),
                i.InputFloatRegister(1), i.InputFloatRegister(2),
                kScratchDoubleReg);
      break;
    case kMipsMsubD:
      __ Msub_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputDoubleRegister(1), i.InputDoubleRegister(2),
                kScratchDoubleReg);
      break;
    case kMipsMulD:
      // TODO(plind): add special case: right op is -1.0, see arm port.
      __ mul_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsDivD:
      __ div_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsModD: {
      // TODO(bmeurer): We should really get rid of this special instruction,
      // and generate a CallAddress instruction instead.
      FrameScope scope(tasm(), StackFrame::MANUAL);
      __ PrepareCallCFunction(0, 2, kScratchReg);
      __ MovToFloatParameters(i.InputDoubleRegister(0),
                              i.InputDoubleRegister(1));
      __ CallCFunction(ExternalReference::mod_two_doubles_operation(), 0, 2);
      // Move the result in the double result register.
      __ MovFromFloatResult(i.OutputDoubleRegister());
      break;
    }
    case kMipsAbsD: {
      FPURegister src = i.InputDoubleRegister(0);
      FPURegister dst = i.OutputDoubleRegister();
      if (IsMipsArchVariant(kMips32r6)) {
        __ abs_d(dst, src);
      } else {
        __ Move(dst, src);
        __ mfhc1(kScratchReg, src);
        __ Ins(kScratchReg, zero_reg, 31, 1);
        __ mthc1(kScratchReg, dst);
      }
      break;
    }
    case kMipsNegS:
      __ Neg_s(i.OutputSingleRegister(), i.InputSingleRegister(0));
      break;
    case kMipsNegD:
      __ Neg_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    case kMipsSqrtD: {
      __ sqrt_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    }
    case kMipsMaxD:
      __ max_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsMinD:
      __ min_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputDoubleRegister(1));
      break;
    case kMipsFloat64RoundDown: {
      __ Floor_d_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    }
    case kMipsFloat32RoundDown: {
      __ Floor_s_s(i.OutputSingleRegister(), i.InputSingleRegister(0));
      break;
    }
    case kMipsFloat64RoundTruncate: {
      __ Trunc_d_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    }
    case kMipsFloat32RoundTruncate: {
      __ Trunc_s_s(i.OutputSingleRegister(), i.InputSingleRegister(0));
      break;
    }
    case kMipsFloat64RoundUp: {
      __ Ceil_d_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    }
    case kMipsFloat32RoundUp: {
      __ Ceil_s_s(i.OutputSingleRegister(), i.InputSingleRegister(0));
      break;
    }
    case kMipsFloat64RoundTiesEven: {
      __ Round_d_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    }
    case kMipsFloat32RoundTiesEven: {
      __ Round_s_s(i.OutputSingleRegister(), i.InputSingleRegister(0));
      break;
    }
    case kMipsFloat32Max: {
      FPURegister dst = i.OutputSingleRegister();
      FPURegister src1 = i.InputSingleRegister(0);
      FPURegister src2 = i.InputSingleRegister(1);
      auto ool = zone()->New<OutOfLineFloat32Max>(this, dst, src1, src2);
      __ Float32Max(dst, src1, src2, ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kMipsFloat64Max: {
      DoubleRegister dst = i.OutputDoubleRegister();
      DoubleRegister src1 = i.InputDoubleRegister(0);
      DoubleRegister src2 = i.InputDoubleRegister(1);
      auto ool = zone()->New<OutOfLineFloat64Max>(this, dst, src1, src2);
      __ Float64Max(dst, src1, src2, ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kMipsFloat32Min: {
      FPURegister dst = i.OutputSingleRegister();
      FPURegister src1 = i.InputSingleRegister(0);
      FPURegister src2 = i.InputSingleRegister(1);
      auto ool = zone()->New<OutOfLineFloat32Min>(this, dst, src1, src2);
      __ Float32Min(dst, src1, src2, ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kMipsFloat64Min: {
      DoubleRegister dst = i.OutputDoubleRegister();
      DoubleRegister src1 = i.InputDoubleRegister(0);
      DoubleRegister src2 = i.InputDoubleRegister(1);
      auto ool = zone()->New<OutOfLineFloat64Min>(this, dst, src1, src2);
      __ Float64Min(dst, src1, src2, ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kMipsCvtSD: {
      __ cvt_s_d(i.OutputSingleRegister(), i.InputDoubleRegister(0));
      break;
    }
    case kMipsCvtDS: {
      __ cvt_d_s(i.OutputDoubleRegister(), i.InputSingleRegister(0));
      break;
    }
    case kMipsCvtDW: {
      FPURegister scratch = kScratchDoubleReg;
      __ mtc1(i.InputRegister(0), scratch);
      __ cvt_d_w(i.OutputDoubleRegister(), scratch);
      break;
    }
    case kMipsCvtSW: {
      FPURegister scratch = kScratchDoubleReg;
      __ mtc1(i.InputRegister(0), scratch);
      __ cvt_s_w(i.OutputDoubleRegister(), scratch);
      break;
    }
    case kMipsCvtSUw: {
      FPURegister scratch = kScratchDoubleReg;
      __ Cvt_d_uw(i.OutputDoubleRegister(), i.InputRegister(0), scratch);
      __ cvt_s_d(i.OutputDoubleRegister(), i.OutputDoubleRegister());
      break;
    }
    case kMipsCvtDUw: {
      FPURegister scratch = kScratchDoubleReg;
      __ Cvt_d_uw(i.OutputDoubleRegister(), i.InputRegister(0), scratch);
      break;
    }
    case kMipsFloorWD: {
      FPURegister scratch = kScratchDoubleReg;
      __ Floor_w_d(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsCeilWD: {
      FPURegister scratch = kScratchDoubleReg;
      __ Ceil_w_d(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsRoundWD: {
      FPURegister scratch = kScratchDoubleReg;
      __ Round_w_d(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsTruncWD: {
      FPURegister scratch = kScratchDoubleReg;
      // Other arches use round to zero here, so we follow.
      __ Trunc_w_d(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsFloorWS: {
      FPURegister scratch = kScratchDoubleReg;
      __ floor_w_s(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsCeilWS: {
      FPURegister scratch = kScratchDoubleReg;
      __ ceil_w_s(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsRoundWS: {
      FPURegister scratch = kScratchDoubleReg;
      __ round_w_s(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsTruncWS: {
      FPURegister scratch = kScratchDoubleReg;
      __ trunc_w_s(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      // Avoid INT32_MAX as an overflow indicator and use INT32_MIN instead,
      // because INT32_MIN allows easier out-of-bounds detection.
      bool set_overflow_to_min_i32 = MiscField::decode(instr->opcode());
      if (set_overflow_to_min_i32) {
        __ Addu(kScratchReg, i.OutputRegister(), 1);
        __ Slt(kScratchReg2, kScratchReg, i.OutputRegister());
        __ Movn(i.OutputRegister(), kScratchReg, kScratchReg2);
      }
      break;
    }
    case kMipsTruncUwD: {
      FPURegister scratch = kScratchDoubleReg;
      __ Trunc_uw_d(i.OutputRegister(), i.InputDoubleRegister(0), scratch);
      break;
    }
    case kMipsTruncUwS: {
      FPURegister scratch = kScratchDoubleReg;
      __ Trunc_uw_s(i.OutputRegister(), i.InputDoubleRegister(0), scratch);
      // Avoid UINT32_MAX as an overflow indicator and use 0 instead,
      // because 0 allows easier out-of-bounds detection.
      bool set_overflow_to_min_i32 = MiscField::decode(instr->opcode());
      if (set_overflow_to_min_i32) {
        __ Addu(kScratchReg, i.OutputRegister(), 1);
        __ Movz(i.OutputRegister(), zero_reg, kScratchReg);
      }
      break;
    }
    case kMipsFloat64ExtractLowWord32:
      __ FmoveLow(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kMipsFloat64ExtractHighWord32:
      __ FmoveHigh(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kMipsFloat64InsertLowWord32:
      __ FmoveLow(i.OutputDoubleRegister(), i.InputRegister(1));
      break;
    case kMipsFloat64InsertHighWord32:
      __ FmoveHigh(i.OutputDoubleRegister(), i.InputRegister(1));
      break;
    case kMipsFloat64SilenceNaN:
      __ FPUCanonicalizeNaN(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;

    // ... more basic instructions ...
    case kMipsSeb:
      __ Seb(i.OutputRegister(), i.InputRegister(0));
      break;
    case kMipsSeh:
      __ Seh(i.OutputRegister(), i.InputRegister(0));
      break;
    case kMipsLbu:
      __ lbu(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsLb:
      __ lb(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsSb:
      __ sb(i.InputOrZeroRegister(2), i.MemoryOperand());
      break;
    case kMipsLhu:
      __ lhu(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsUlhu:
      __ Ulhu(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsLh:
      __ lh(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsUlh:
      __ Ulh(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsSh:
      __ sh(i.InputOrZeroRegister(2), i.MemoryOperand());
      break;
    case kMipsUsh:
      __ Ush(i.InputOrZeroRegister(2), i.MemoryOperand(), kScratchReg);
      break;
    case kMipsLw:
      __ lw(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsUlw:
      __ Ulw(i.OutputRegister(), i.MemoryOperand());
      EmitWordLoadPoisoningIfNeeded(this, opcode, instr, i);
      break;
    case kMipsSw:
      __ sw(i.InputOrZeroRegister(2), i.MemoryOperand());
      break;
    case kMipsUsw:
      __ Usw(i.InputOrZeroRegister(2), i.MemoryOperand());
      break;
    case kMipsLwc1: {
      __ lwc1(i.OutputSingleRegister(), i.MemoryOperand());
      break;
    }
    case kMipsUlwc1: {
      __ Ulwc1(i.OutputSingleRegister(), i.MemoryOperand(), kScratchReg);
      break;
    }
    case kMipsSwc1: {
      size_t index = 0;
      MemOperand operand = i.MemoryOperand(&index);
      FPURegister ft = i.InputOrZeroSingleRegister(index);
      if (ft == kDoubleRegZero && !__ IsDoubleZeroRegSet()) {
        __ Move(kDoubleRegZero, 0.0);
      }
      __ swc1(ft, operand);
      break;
    }
    case kMipsUswc1: {
      size_t index = 0;
      MemOperand operand = i.MemoryOperand(&index);
      FPURegister ft = i.InputOrZeroSingleRegister(index);
      if (ft == kDoubleRegZero && !__ IsDoubleZeroRegSet()) {
        __ Move(kDoubleRegZero, 0.0);
      }
      __ Uswc1(ft, operand, kScratchReg);
      break;
    }
    case kMipsLdc1:
      __ Ldc1(i.OutputDoubleRegister(), i.MemoryOperand());
      break;
    case kMipsUldc1:
      __ Uldc1(i.OutputDoubleRegister(), i.MemoryOperand(), kScratchReg);
      break;
    case kMipsSdc1: {
      FPURegister ft = i.InputOrZeroDoubleRegister(2);
      if (ft == kDoubleRegZero && !__ IsDoubleZeroRegSet()) {
        __ Move(kDoubleRegZero, 0.0);
      }
      __ Sdc1(ft, i.MemoryOperand());
      break;
    }
    case kMipsUsdc1: {
      FPURegister ft = i.InputOrZeroDoubleRegister(2);
      if (ft == kDoubleRegZero && !__ IsDoubleZeroRegSet()) {
        __ Move(kDoubleRegZero, 0.0);
      }
      __ Usdc1(ft, i.MemoryOperand(), kScratchReg);
      break;
    }
    case kMipsSync: {
      __ sync();
      break;
    }
    case kMipsPush:
      if (instr->InputAt(0)->IsFPRegister()) {
        LocationOperand* op = LocationOperand::cast(instr->InputAt(0));
        switch (op->representation()) {
          case MachineRepresentation::kFloat32:
            __ swc1(i.InputFloatRegister(0), MemOperand(sp, -kFloatSize));
            __ Subu(sp, sp, Operand(kFloatSize));
            frame_access_state()->IncreaseSPDelta(kFloatSize /
                                                  kSystemPointerSize);
            break;
          case MachineRepresentation::kFloat64:
            __ Sdc1(i.InputDoubleRegister(0), MemOperand(sp, -kDoubleSize));
            __ Subu(sp, sp, Operand(kDoubleSize));
            frame_access_state()->IncreaseSPDelta(kDoubleSize /
                                                  kSystemPointerSize);
            break;
          default: {
            UNREACHABLE();
            break;
          }
        }
      } else {
        __ Push(i.InputRegister(0));
        frame_access_state()->IncreaseSPDelta(1);
      }
      break;
    case kMipsPeek: {
      int reverse_slot = i.InputInt32(0);
      int offset =
          FrameSlotToFPOffset(frame()->GetTotalFrameSlotCount() - reverse_slot);
      if (instr->OutputAt(0)->IsFPRegister()) {
        LocationOperand* op = LocationOperand::cast(instr->OutputAt(0));
        if (op->representation() == MachineRepresentation::kFloat64) {
          __ Ldc1(i.OutputDoubleRegister(), MemOperand(fp, offset));
        } else if (op->representation() == MachineRepresentation::kFloat32) {
          __ lwc1(i.OutputSingleRegister(0), MemOperand(fp, offset));
        } else {
          DCHECK_EQ(op->representation(), MachineRepresentation::kSimd128);
          __ ld_b(i.OutputSimd128Register(), MemOperand(fp, offset));
        }
      } else {
        __ lw(i.OutputRegister(0), MemOperand(fp, offset));
      }
      break;
    }
    case kMipsStackClaim: {
      __ Subu(sp, sp, Operand(i.InputInt32(0)));
      frame_access_state()->IncreaseSPDelta(i.InputInt32(0) /
                                            kSystemPointerSize);
      break;
    }
    case kMipsStoreToStackSlot: {
      if (instr->InputAt(0)->IsFPRegister()) {
        LocationOperand* op = LocationOperand::cast(instr->InputAt(0));
        if (op->representation() == MachineRepresentation::kFloat64) {
          __ Sdc1(i.InputDoubleRegister(0), MemOperand(sp, i.InputInt32(1)));
        } else if (op->representation() == MachineRepresentation::kFloat32) {
          __ swc1(i.InputSingleRegister(0), MemOperand(sp, i.InputInt32(1)));
        } else {
          DCHECK_EQ(MachineRepresentation::kSimd128, op->representation());
          CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
          __ st_b(i.InputSimd128Register(0), MemOperand(sp, i.InputInt32(1)));
        }
      } else {
        __ sw(i.InputRegister(0), MemOperand(sp, i.InputInt32(1)));
      }
      break;
    }
    case kMipsByteSwap32: {
      __ ByteSwapSigned(i.OutputRegister(0), i.InputRegister(0), 4);
      break;
    }
    case kMipsS128Load8Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ lb(kScratchReg, i.MemoryOperand());
      __ fill_b(i.OutputSimd128Register(), kScratchReg);
      break;
    }
    case kMipsS128Load16Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ lh(kScratchReg, i.MemoryOperand());
      __ fill_h(i.OutputSimd128Register(), kScratchReg);
      break;
    }
    case kMipsS128Load32Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ Lw(kScratchReg, i.MemoryOperand());
      __ fill_w(i.OutputSimd128Register(), kScratchReg);
      break;
    }
    case kMipsS128Load64Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      MemOperand memLow = i.MemoryOperand();
      MemOperand memHigh = MemOperand(memLow.rm(), memLow.offset() + 4);
      __ Lw(kScratchReg, memLow);
      __ fill_w(dst, kScratchReg);
      __ Lw(kScratchReg, memHigh);
      __ fill_w(kSimd128ScratchReg, kScratchReg);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      break;
    }
    case kMipsS128Load8x8S: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      MemOperand memLow = i.MemoryOperand();
      MemOperand memHigh = MemOperand(memLow.rm(), memLow.offset() + 4);
      __ Lw(kScratchReg, memLow);
      __ fill_w(dst, kScratchReg);
      __ Lw(kScratchReg, memHigh);
      __ fill_w(kSimd128ScratchReg, kScratchReg);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      __ clti_s_b(kSimd128ScratchReg, dst, 0);
      __ ilvr_b(dst, kSimd128ScratchReg, dst);
      break;
    }
    case kMipsS128Load8x8U: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      MemOperand memLow = i.MemoryOperand();
      MemOperand memHigh = MemOperand(memLow.rm(), memLow.offset() + 4);
      __ Lw(kScratchReg, memLow);
      __ fill_w(dst, kScratchReg);
      __ Lw(kScratchReg, memHigh);
      __ fill_w(kSimd128ScratchReg, kScratchReg);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      __ ilvr_b(dst, kSimd128RegZero, dst);
      break;
    }
    case kMipsS128Load16x4S: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      MemOperand memLow = i.MemoryOperand();
      MemOperand memHigh = MemOperand(memLow.rm(), memLow.offset() + 4);
      __ Lw(kScratchReg, memLow);
      __ fill_w(dst, kScratchReg);
      __ Lw(kScratchReg, memHigh);
      __ fill_w(kSimd128ScratchReg, kScratchReg);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      __ clti_s_h(kSimd128ScratchReg, dst, 0);
      __ ilvr_h(dst, kSimd128ScratchReg, dst);
      break;
    }
    case kMipsS128Load16x4U: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      MemOperand memLow = i.MemoryOperand();
      MemOperand memHigh = MemOperand(memLow.rm(), memLow.offset() + 4);
      __ Lw(kScratchReg, memLow);
      __ fill_w(dst, kScratchReg);
      __ Lw(kScratchReg, memHigh);
      __ fill_w(kSimd128ScratchReg, kScratchReg);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      __ ilvr_h(dst, kSimd128RegZero, dst);
      break;
    }
    case kMipsS128Load32x2S: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      MemOperand memLow = i.MemoryOperand();
      MemOperand memHigh = MemOperand(memLow.rm(), memLow.offset() + 4);
      __ Lw(kScratchReg, memLow);
      __ fill_w(dst, kScratchReg);
      __ Lw(kScratchReg, memHigh);
      __ fill_w(kSimd128ScratchReg, kScratchReg);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      __ clti_s_w(kSimd128ScratchReg, dst, 0);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      break;
    }
    case kMipsS128Load32x2U: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      MemOperand memLow = i.MemoryOperand();
      MemOperand memHigh = MemOperand(memLow.rm(), memLow.offset() + 4);
      __ Lw(kScratchReg, memLow);
      __ fill_w(dst, kScratchReg);
      __ Lw(kScratchReg, memHigh);
      __ fill_w(kSimd128ScratchReg, kScratchReg);
      __ ilvr_w(dst, kSimd128ScratchReg, dst);
      __ ilvr_w(dst, kSimd128RegZero, dst);
      break;
    }
    case kWord32AtomicLoadInt8:
      ASSEMBLE_ATOMIC_LOAD_INTEGER(lb);
      break;
    case kWord32AtomicLoadUint8:
      ASSEMBLE_ATOMIC_LOAD_INTEGER(lbu);
      break;
    case kWord32AtomicLoadInt16:
      ASSEMBLE_ATOMIC_LOAD_INTEGER(lh);
      break;
    case kWord32AtomicLoadUint16:
      ASSEMBLE_ATOMIC_LOAD_INTEGER(lhu);
      break;
    case kWord32AtomicLoadWord32:
      ASSEMBLE_ATOMIC_LOAD_INTEGER(lw);
      break;
    case kWord32AtomicStoreWord8:
      ASSEMBLE_ATOMIC_STORE_INTEGER(sb);
      break;
    case kWord32AtomicStoreWord16:
      ASSEMBLE_ATOMIC_STORE_INTEGER(sh);
      break;
    case kWord32AtomicStoreWord32:
      ASSEMBLE_ATOMIC_STORE_INTEGER(sw);
      break;
    case kWord32AtomicExchangeInt8:
      ASSEMBLE_ATOMIC_EXCHANGE_INTEGER_EXT(true, 8);
      break;
    case kWord32AtomicExchangeUint8:
      ASSEMBLE_ATOMIC_EXCHANGE_INTEGER_EXT(false, 8);
      break;
    case kWord32AtomicExchangeInt16:
      ASSEMBLE_ATOMIC_EXCHANGE_INTEGER_EXT(true, 16);
      break;
    case kWord32AtomicExchangeUint16:
      ASSEMBLE_ATOMIC_EXCHANGE_INTEGER_EXT(false, 16);
      break;
    case kWord32AtomicExchangeWord32:
      ASSEMBLE_ATOMIC_EXCHANGE_INTEGER();
      break;
    case kWord32AtomicCompareExchangeInt8:
      ASSEMBLE_ATOMIC_COMPARE_EXCHANGE_INTEGER_EXT(true, 8);
      break;
    case kWord32AtomicCompareExchangeUint8:
      ASSEMBLE_ATOMIC_COMPARE_EXCHANGE_INTEGER_EXT(false, 8);
      break;
    case kWord32AtomicCompareExchangeInt16:
      ASSEMBLE_ATOMIC_COMPARE_EXCHANGE_INTEGER_EXT(true, 16);
      break;
    case kWord32AtomicCompareExchangeUint16:
      ASSEMBLE_ATOMIC_COMPARE_EXCHANGE_INTEGER_EXT(false, 16);
      break;
    case kWord32AtomicCompareExchangeWord32:
      ASSEMBLE_ATOMIC_COMPARE_EXCHANGE_INTEGER();
      break;
#define ATOMIC_BINOP_CASE(op, inst)             \
  case kWord32Atomic##op##Int8:                 \
    ASSEMBLE_ATOMIC_BINOP_EXT(true, 8, inst);   \
    break;                                      \
  case kWord32Atomic##op##Uint8:                \
    ASSEMBLE_ATOMIC_BINOP_EXT(false, 8, inst);  \
    break;                                      \
  case kWord32Atomic##op##Int16:                \
    ASSEMBLE_ATOMIC_BINOP_EXT(true, 16, inst);  \
    break;                                      \
  case kWord32Atomic##op##Uint16:               \
    ASSEMBLE_ATOMIC_BINOP_EXT(false, 16, inst); \
    break;                                      \
  case kWord32Atomic##op##Word32:               \
    ASSEMBLE_ATOMIC_BINOP(inst);                \
    break;
      ATOMIC_BINOP_CASE(Add, Addu)
      ATOMIC_BINOP_CASE(Sub, Subu)
      ATOMIC_BINOP_CASE(And, And)
      ATOMIC_BINOP_CASE(Or, Or)
      ATOMIC_BINOP_CASE(Xor, Xor)
#undef ATOMIC_BINOP_CASE
    case kMipsWord32AtomicPairLoad: {
      if (IsMipsArchVariant(kMips32r6)) {
        if (instr->OutputCount() > 0) {
          Register second_output = instr->OutputCount() == 2
                                       ? i.OutputRegister(1)
                                       : i.TempRegister(1);
          __ Addu(a0, i.InputRegister(0), i.InputRegister(1));
          __ llx(second_output, MemOperand(a0, 4));
          __ ll(i.OutputRegister(0), MemOperand(a0, 0));
          __ sync();
        }
      } else {
        FrameScope scope(tasm(), StackFrame::MANUAL);
        __ Addu(a0, i.InputRegister(0), i.InputRegister(1));
        __ PushCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);
        __ PrepareCallCFunction(1, 0, kScratchReg);
        __ CallCFunction(ExternalReference::atomic_pair_load_function(), 1, 0);
        __ PopCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);
      }
      break;
    }
    case kMipsWord32AtomicPairStore: {
      if (IsMipsArchVariant(kMips32r6)) {
        Label store;
        __ Addu(a0, i.InputRegister(0), i.InputRegister(1));
        __ sync();
        __ bind(&store);
        __ llx(i.TempRegister(2), MemOperand(a0, 4));
        __ ll(i.TempRegister(1), MemOperand(a0, 0));
        __ Move(i.TempRegister(1), i.InputRegister(2));
        __ scx(i.InputRegister(3), MemOperand(a0, 4));
        __ sc(i.TempRegister(1), MemOperand(a0, 0));
        __ BranchShort(&store, eq, i.TempRegister(1), Operand(zero_reg));
        __ sync();
      } else {
        FrameScope scope(tasm(), StackFrame::MANUAL);
        __ Addu(a0, i.InputRegister(0), i.InputRegister(1));
        __ PushCallerSaved(SaveFPRegsMode::kIgnore);
        __ PrepareCallCFunction(3, 0, kScratchReg);
        __ CallCFunction(ExternalReference::atomic_pair_store_function(), 3, 0);
        __ PopCallerSaved(SaveFPRegsMode::kIgnore);
      }
      break;
    }
#define ATOMIC64_BINOP_ARITH_CASE(op, instr, external) \
  case kMipsWord32AtomicPair##op:                      \
    ASSEMBLE_ATOMIC64_ARITH_BINOP(instr, external);    \
    break;
      ATOMIC64_BINOP_ARITH_CASE(Add, AddPair, atomic_pair_add_function)
      ATOMIC64_BINOP_ARITH_CASE(Sub, SubPair, atomic_pair_sub_function)
#undef ATOMIC64_BINOP_ARITH_CASE
#define ATOMIC64_BINOP_LOGIC_CASE(op, instr, external) \
  case kMipsWord32AtomicPair##op:                      \
    ASSEMBLE_ATOMIC64_LOGIC_BINOP(instr, external);    \
    break;
      ATOMIC64_BINOP_LOGIC_CASE(And, AndPair, atomic_pair_and_function)
      ATOMIC64_BINOP_LOGIC_CASE(Or, OrPair, atomic_pair_or_function)
      ATOMIC64_BINOP_LOGIC_CASE(Xor, XorPair, atomic_pair_xor_function)
#undef ATOMIC64_BINOP_LOGIC_CASE
    case kMipsWord32AtomicPairExchange:
      if (IsMipsArchVariant(kMips32r6)) {
        Label binop;
        Register oldval_low =
            instr->OutputCount() >= 1 ? i.OutputRegister(0) : i.TempRegister(1);
        Register oldval_high =
            instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(2);
        __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));
        __ sync();
        __ bind(&binop);
        __ llx(oldval_high, MemOperand(i.TempRegister(0), 4));
        __ ll(oldval_low, MemOperand(i.TempRegister(0), 0));
        __ Move(i.TempRegister(1), i.InputRegister(2));
        __ scx(i.InputRegister(3), MemOperand(i.TempRegister(0), 4));
        __ sc(i.TempRegister(1), MemOperand(i.TempRegister(0), 0));
        __ BranchShort(&binop, eq, i.TempRegister(1), Operand(zero_reg));
        __ sync();
      } else {
        FrameScope scope(tasm(), StackFrame::MANUAL);
        __ PushCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);
        __ PrepareCallCFunction(3, 0, kScratchReg);
        __ Addu(a0, i.InputRegister(0), i.InputRegister(1));
        __ CallCFunction(ExternalReference::atomic_pair_exchange_function(), 3,
                         0);
        __ PopCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);
      }
      break;
    case kMipsWord32AtomicPairCompareExchange: {
      if (IsMipsArchVariant(kMips32r6)) {
        Label compareExchange, exit;
        Register oldval_low =
            instr->OutputCount() >= 1 ? i.OutputRegister(0) : kScratchReg;
        Register oldval_high =
            instr->OutputCount() >= 2 ? i.OutputRegister(1) : kScratchReg2;
        __ Addu(i.TempRegister(0), i.InputRegister(0), i.InputRegister(1));
        __ sync();
        __ bind(&compareExchange);
        __ llx(oldval_high, MemOperand(i.TempRegister(0), 4));
        __ ll(oldval_low, MemOperand(i.TempRegister(0), 0));
        __ BranchShort(&exit, ne, i.InputRegister(2), Operand(oldval_low));
        __ BranchShort(&exit, ne, i.InputRegister(3), Operand(oldval_high));
        __ mov(kScratchReg, i.InputRegister(4));
        __ scx(i.InputRegister(5), MemOperand(i.TempRegister(0), 4));
        __ sc(kScratchReg, MemOperand(i.TempRegister(0), 0));
        __ BranchShort(&compareExchange, eq, kScratchReg, Operand(zero_reg));
        __ bind(&exit);
        __ sync();
      } else {
        FrameScope scope(tasm(), StackFrame::MANUAL);
        __ PushCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);
        __ PrepareCallCFunction(5, 0, kScratchReg);
        __ addu(a0, i.InputRegister(0), i.InputRegister(1));
        __ sw(i.InputRegister(5), MemOperand(sp, 16));
        __ CallCFunction(
            ExternalReference::atomic_pair_compare_exchange_function(), 5, 0);
        __ PopCallerSaved(SaveFPRegsMode::kIgnore, v0, v1);
      }
      break;
    }
    case kMipsS128Zero: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(i.OutputSimd128Register(), i.OutputSimd128Register(),
               i.OutputSimd128Register());
      break;
    }
    case kMipsI32x4Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fill_w(i.OutputSimd128Register(), i.InputRegister(0));
      break;
    }
    case kMipsI32x4ExtractLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ copy_s_w(i.OutputRegister(), i.InputSimd128Register(0),
                  i.InputInt8(1));
      break;
    }
    case kMipsI32x4ReplaceLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register dst = i.OutputSimd128Register();
      if (src != dst) {
        __ move_v(dst, src);
      }
      __ insert_w(dst, i.InputInt8(1), i.InputRegister(2));
      break;
    }
    case kMipsI32x4Add: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ addv_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4Sub: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subv_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4ExtAddPairwiseI16x8S: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ hadd_s_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4ExtAddPairwiseI16x8U: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ hadd_u_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(0));
      break;
    }
    case kMipsF64x2Abs: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ bclri_d(i.OutputSimd128Register(), i.InputSimd128Register(0), 63);
      break;
    }
    case kMipsF64x2Neg: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ bnegi_d(i.OutputSimd128Register(), i.InputSimd128Register(0), 63);
      break;
    }
    case kMipsF64x2Sqrt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fsqrt_d(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsF64x2Add: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      ASSEMBLE_F64X2_ARITHMETIC_BINOP(fadd_d);
      break;
    }
    case kMipsF64x2Sub: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      ASSEMBLE_F64X2_ARITHMETIC_BINOP(fsub_d);
      break;
    }
    case kMipsF64x2Mul: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      ASSEMBLE_F64X2_ARITHMETIC_BINOP(fmul_d);
      break;
    }
    case kMipsF64x2Div: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      ASSEMBLE_F64X2_ARITHMETIC_BINOP(fdiv_d);
      break;
    }
    case kMipsF64x2Min: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      ASSEMBLE_F64X2_ARITHMETIC_BINOP(fmin_d);
      break;
    }
    case kMipsF64x2Max: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      ASSEMBLE_F64X2_ARITHMETIC_BINOP(fmax_d);
      break;
    }
    case kMipsF64x2Eq: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fceq_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF64x2Ne: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fcne_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF64x2Lt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fclt_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF64x2Le: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fcle_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF64x2Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      __ FmoveLow(kScratchReg, i.InputDoubleRegister(0));
      __ insert_w(dst, 0, kScratchReg);
      __ insert_w(dst, 2, kScratchReg);
      __ FmoveHigh(kScratchReg, i.InputDoubleRegister(0));
      __ insert_w(dst, 1, kScratchReg);
      __ insert_w(dst, 3, kScratchReg);
      break;
    }
    case kMipsF64x2ExtractLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ copy_u_w(kScratchReg, i.InputSimd128Register(0), i.InputInt8(1) * 2);
      __ FmoveLow(i.OutputDoubleRegister(), kScratchReg);
      __ copy_u_w(kScratchReg, i.InputSimd128Register(0),
                  i.InputInt8(1) * 2 + 1);
      __ FmoveHigh(i.OutputDoubleRegister(), kScratchReg);
      break;
    }
    case kMipsF64x2ReplaceLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register dst = i.OutputSimd128Register();
      if (src != dst) {
        __ move_v(dst, src);
      }
      __ FmoveLow(kScratchReg, i.InputDoubleRegister(2));
      __ insert_w(dst, i.InputInt8(1) * 2, kScratchReg);
      __ FmoveHigh(kScratchReg, i.InputDoubleRegister(2));
      __ insert_w(dst, i.InputInt8(1) * 2 + 1, kScratchReg);
      break;
    }
    case kMipsF64x2Pmin: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register lhs = i.InputSimd128Register(0);
      Simd128Register rhs = i.InputSimd128Register(1);
      // dst = rhs < lhs ? rhs : lhs
      __ fclt_d(dst, rhs, lhs);
      __ bsel_v(dst, lhs, rhs);
      break;
    }
    case kMipsF64x2Pmax: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register lhs = i.InputSimd128Register(0);
      Simd128Register rhs = i.InputSimd128Register(1);
      // dst = lhs < rhs ? rhs : lhs
      __ fclt_d(dst, lhs, rhs);
      __ bsel_v(dst, lhs, rhs);
      break;
    }
    case kMipsF64x2Ceil: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      __ li(kScratchReg2, kRoundToPlusInf);
      __ ctcmsa(MSACSR, kScratchReg2);
      __ frint_d(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsF64x2Floor: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      __ li(kScratchReg2, kRoundToMinusInf);
      __ ctcmsa(MSACSR, kScratchReg2);
      __ frint_d(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsF64x2Trunc: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      __ li(kScratchReg2, kRoundToZero);
      __ ctcmsa(MSACSR, kScratchReg2);
      __ frint_d(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsF64x2NearestInt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      // kRoundToNearest == 0
      __ ctcmsa(MSACSR, zero_reg);
      __ frint_d(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsF64x2ConvertLowI32x4S: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvr_w(kSimd128RegZero, kSimd128RegZero, i.InputSimd128Register(0));
      __ slli_d(kSimd128RegZero, kSimd128RegZero, 32);
      __ srai_d(kSimd128RegZero, kSimd128RegZero, 32);
      __ ffint_s_d(i.OutputSimd128Register(), kSimd128RegZero);
      break;
    }
    case kMipsF64x2ConvertLowI32x4U: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvr_w(kSimd128RegZero, kSimd128RegZero, i.InputSimd128Register(0));
      __ ffint_u_d(i.OutputSimd128Register(), kSimd128RegZero);
      break;
    }
    case kMipsF64x2PromoteLowF32x4: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fexupr_d(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsI64x2Add: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ addv_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI64x2Sub: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subv_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI64x2Mul: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ mulv_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI64x2Neg: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ subv_d(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI64x2Shl: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ slli_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt6(1));
      break;
    }
    case kMipsI64x2ShrS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srai_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt6(1));
      break;
    }
    case kMipsI64x2ShrU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srli_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt6(1));
      break;
    }
    case kMipsI64x2BitMask: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register scratch0 = kSimd128RegZero;
      Simd128Register scratch1 = kSimd128ScratchReg;
      __ srli_d(scratch0, src, 63);
      __ shf_w(scratch1, scratch0, 0x02);
      __ slli_d(scratch1, scratch1, 1);
      __ or_v(scratch0, scratch0, scratch1);
      __ copy_u_b(dst, scratch0, 0);
      break;
    }
    case kMipsI64x2Eq: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ceq_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(1));
      break;
    }
    case kMipsI64x2Ne: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ceq_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(1));
      __ nor_v(i.OutputSimd128Register(), i.OutputSimd128Register(),
               i.OutputSimd128Register());
      break;
    }
    case kMipsI64x2GtS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ clt_s_d(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI64x2GeS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cle_s_d(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI64x2Abs: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ adds_a_d(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  kSimd128RegZero);
      break;
    }
    case kMipsI64x2SConvertI32x4Low: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src = i.InputSimd128Register(0);
      __ ilvr_w(kSimd128ScratchReg, src, src);
      __ slli_d(dst, kSimd128ScratchReg, 32);
      __ srai_d(dst, dst, 32);
      break;
    }
    case kMipsI64x2SConvertI32x4High: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src = i.InputSimd128Register(0);
      __ ilvl_w(kSimd128ScratchReg, src, src);
      __ slli_d(dst, kSimd128ScratchReg, 32);
      __ srai_d(dst, dst, 32);
      break;
    }
    case kMipsI64x2UConvertI32x4Low: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvr_w(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI64x2UConvertI32x4High: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvl_w(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI64x2ExtMulLowI32x4S:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvr_w, dotp_s_d);
      break;
    case kMipsI64x2ExtMulHighI32x4S:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvl_w, dotp_s_d);
      break;
    case kMipsI64x2ExtMulLowI32x4U:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvr_w, dotp_u_d);
      break;
    case kMipsI64x2ExtMulHighI32x4U:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvl_w, dotp_u_d);
      break;
    case kMipsI32x4ExtMulLowI16x8S:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvr_h, dotp_s_w);
      break;
    case kMipsI32x4ExtMulHighI16x8S:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvl_h, dotp_s_w);
      break;
    case kMipsI32x4ExtMulLowI16x8U:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvr_h, dotp_u_w);
      break;
    case kMipsI32x4ExtMulHighI16x8U:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvl_h, dotp_u_w);
      break;
    case kMipsI16x8ExtMulLowI8x16S:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvr_b, dotp_s_h);
      break;
    case kMipsI16x8ExtMulHighI8x16S:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvl_b, dotp_s_h);
      break;
    case kMipsI16x8ExtMulLowI8x16U:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvr_b, dotp_u_h);
      break;
    case kMipsI16x8ExtMulHighI8x16U:
      ASSEMBLE_SIMD_EXTENDED_MULTIPLY(ilvl_b, dotp_u_h);
      break;
    case kMipsF32x4Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ FmoveLow(kScratchReg, i.InputSingleRegister(0));
      __ fill_w(i.OutputSimd128Register(), kScratchReg);
      break;
    }
    case kMipsF32x4ExtractLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ copy_u_w(kScratchReg, i.InputSimd128Register(0), i.InputInt8(1));
      __ FmoveLow(i.OutputSingleRegister(), kScratchReg);
      break;
    }
    case kMipsF32x4ReplaceLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register dst = i.OutputSimd128Register();
      if (src != dst) {
        __ move_v(dst, src);
      }
      __ FmoveLow(kScratchReg, i.InputSingleRegister(2));
      __ insert_w(dst, i.InputInt8(1), kScratchReg);
      break;
    }
    case kMipsF32x4SConvertI32x4: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ffint_s_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsF32x4UConvertI32x4: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ffint_u_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsF32x4DemoteF64x2Zero: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ fexdo_w(i.OutputSimd128Register(), kSimd128RegZero,
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4Mul: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ mulv_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4MaxS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ max_s_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4MinS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ min_s_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4Eq: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ceq_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4Ne: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      __ ceq_w(dst, i.InputSimd128Register(0), i.InputSimd128Register(1));
      __ nor_v(dst, dst, dst);
      break;
    }
    case kMipsI32x4Shl: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ slli_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt5(1));
      break;
    }
    case kMipsI32x4ShrS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srai_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt5(1));
      break;
    }
    case kMipsI32x4ShrU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srli_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt5(1));
      break;
    }
    case kMipsI32x4MaxU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ max_u_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4MinU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ min_u_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsS128Select: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      DCHECK(i.OutputSimd128Register() == i.InputSimd128Register(0));
      __ bsel_v(i.OutputSimd128Register(), i.InputSimd128Register(2),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsS128AndNot: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      __ nor_v(dst, i.InputSimd128Register(1), i.InputSimd128Register(1));
      __ and_v(dst, dst, i.InputSimd128Register(0));
      break;
    }
    case kMipsF32x4Abs: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ bclri_w(i.OutputSimd128Register(), i.InputSimd128Register(0), 31);
      break;
    }
    case kMipsF32x4Neg: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ bnegi_w(i.OutputSimd128Register(), i.InputSimd128Register(0), 31);
      break;
    }
    case kMipsF32x4Sqrt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fsqrt_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsF32x4RecipApprox: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ frcp_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsF32x4RecipSqrtApprox: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ frsqrt_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsF32x4Add: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fadd_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Sub: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fsub_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Mul: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fmul_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Div: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fdiv_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Max: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fmax_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Min: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fmin_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Eq: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fceq_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Ne: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fcne_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Lt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fclt_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Le: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fcle_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsF32x4Pmin: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register lhs = i.InputSimd128Register(0);
      Simd128Register rhs = i.InputSimd128Register(1);
      // dst = rhs < lhs ? rhs : lhs
      __ fclt_w(dst, rhs, lhs);
      __ bsel_v(dst, lhs, rhs);
      break;
    }
    case kMipsF32x4Pmax: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register lhs = i.InputSimd128Register(0);
      Simd128Register rhs = i.InputSimd128Register(1);
      // dst = lhs < rhs ? rhs : lhs
      __ fclt_w(dst, lhs, rhs);
      __ bsel_v(dst, lhs, rhs);
      break;
    }
    case kMipsF32x4Ceil: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      __ li(kScratchReg2, kRoundToPlusInf);
      __ ctcmsa(MSACSR, kScratchReg2);
      __ frint_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsF32x4Floor: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      __ li(kScratchReg2, kRoundToMinusInf);
      __ ctcmsa(MSACSR, kScratchReg2);
      __ frint_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsF32x4Trunc: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      __ li(kScratchReg2, kRoundToZero);
      __ ctcmsa(MSACSR, kScratchReg2);
      __ frint_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsF32x4NearestInt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cfcmsa(kScratchReg, MSACSR);
      // kRoundToNearest == 0
      __ ctcmsa(MSACSR, zero_reg);
      __ frint_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ ctcmsa(MSACSR, kScratchReg);
      break;
    }
    case kMipsI32x4SConvertF32x4: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ftrunc_s_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4UConvertF32x4: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ftrunc_u_w(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4Neg: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ subv_w(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4GtS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ clt_s_w(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4GeS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cle_s_w(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4GtU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ clt_u_w(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4GeU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cle_u_w(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4Abs: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ asub_s_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  kSimd128RegZero);
      break;
    }
    case kMipsI32x4BitMask: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register scratch0 = kSimd128RegZero;
      Simd128Register scratch1 = kSimd128ScratchReg;
      __ srli_w(scratch0, src, 31);
      __ srli_d(scratch1, scratch0, 31);
      __ or_v(scratch0, scratch0, scratch1);
      __ shf_w(scratch1, scratch0, 0x0E);
      __ slli_d(scratch1, scratch1, 2);
      __ or_v(scratch0, scratch0, scratch1);
      __ copy_u_b(dst, scratch0, 0);
      break;
    }
    case kMipsI32x4DotI16x8S: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ dotp_s_w(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI32x4TruncSatF64x2SZero: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ftrunc_s_d(kSimd128ScratchReg, i.InputSimd128Register(0));
      __ sat_s_d(kSimd128ScratchReg, kSimd128ScratchReg, 31);
      __ pckev_w(i.OutputSimd128Register(), kSimd128RegZero,
                 kSimd128ScratchReg);
      break;
    }
    case kMipsI32x4TruncSatF64x2UZero: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ftrunc_u_d(kSimd128ScratchReg, i.InputSimd128Register(0));
      __ sat_u_d(kSimd128ScratchReg, kSimd128ScratchReg, 31);
      __ pckev_w(i.OutputSimd128Register(), kSimd128RegZero,
                 kSimd128ScratchReg);
      break;
    }
    case kMipsI16x8Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fill_h(i.OutputSimd128Register(), i.InputRegister(0));
      break;
    }
    case kMipsI16x8ExtractLaneU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ copy_u_h(i.OutputRegister(), i.InputSimd128Register(0),
                  i.InputInt8(1));
      break;
    }
    case kMipsI16x8ExtractLaneS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ copy_s_h(i.OutputRegister(), i.InputSimd128Register(0),
                  i.InputInt8(1));
      break;
    }
    case kMipsI16x8ReplaceLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register dst = i.OutputSimd128Register();
      if (src != dst) {
        __ move_v(dst, src);
      }
      __ insert_h(dst, i.InputInt8(1), i.InputRegister(2));
      break;
    }
    case kMipsI16x8Neg: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ subv_h(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8Shl: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ slli_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt4(1));
      break;
    }
    case kMipsI16x8ShrS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srai_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt4(1));
      break;
    }
    case kMipsI16x8ShrU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srli_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt4(1));
      break;
    }
    case kMipsI16x8Add: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ addv_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8AddSatS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ adds_s_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8Sub: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subv_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8SubSatS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subs_s_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8Mul: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ mulv_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8MaxS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ max_s_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8MinS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ min_s_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8Eq: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ceq_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8Ne: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      __ ceq_h(dst, i.InputSimd128Register(0), i.InputSimd128Register(1));
      __ nor_v(dst, dst, dst);
      break;
    }
    case kMipsI16x8GtS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ clt_s_h(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8GeS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cle_s_h(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8AddSatU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ adds_u_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8SubSatU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subs_u_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8MaxU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ max_u_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8MinU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ min_u_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8GtU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ clt_u_h(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8GeU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cle_u_h(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8RoundingAverageU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ aver_u_h(i.OutputSimd128Register(), i.InputSimd128Register(1),
                  i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8Abs: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ asub_s_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  kSimd128RegZero);
      break;
    }
    case kMipsI16x8BitMask: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register scratch0 = kSimd128RegZero;
      Simd128Register scratch1 = kSimd128ScratchReg;
      __ srli_h(scratch0, src, 15);
      __ srli_w(scratch1, scratch0, 15);
      __ or_v(scratch0, scratch0, scratch1);
      __ srli_d(scratch1, scratch0, 30);
      __ or_v(scratch0, scratch0, scratch1);
      __ shf_w(scratch1, scratch0, 0x0E);
      __ slli_d(scratch1, scratch1, 4);
      __ or_v(scratch0, scratch0, scratch1);
      __ copy_u_b(dst, scratch0, 0);
      break;
    }
    case kMipsI16x8Q15MulRSatS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ mulr_q_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI16x8ExtAddPairwiseI8x16S: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ hadd_s_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8ExtAddPairwiseI8x16U: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ hadd_u_h(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16Splat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ fill_b(i.OutputSimd128Register(), i.InputRegister(0));
      break;
    }
    case kMipsI8x16ExtractLaneU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ copy_u_b(i.OutputRegister(), i.InputSimd128Register(0),
                  i.InputInt8(1));
      break;
    }
    case kMipsI8x16ExtractLaneS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ copy_s_b(i.OutputRegister(), i.InputSimd128Register(0),
                  i.InputInt8(1));
      break;
    }
    case kMipsI8x16ReplaceLane: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register dst = i.OutputSimd128Register();
      if (src != dst) {
        __ move_v(dst, src);
      }
      __ insert_b(dst, i.InputInt8(1), i.InputRegister(2));
      break;
    }
    case kMipsI8x16Neg: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ subv_b(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16Shl: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ slli_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt3(1));
      break;
    }
    case kMipsI8x16ShrS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srai_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt3(1));
      break;
    }
    case kMipsI8x16Add: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ addv_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16AddSatS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ adds_s_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16Sub: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subv_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16SubSatS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subs_s_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16MaxS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ max_s_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16MinS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ min_s_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16Eq: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ceq_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16Ne: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      __ ceq_b(dst, i.InputSimd128Register(0), i.InputSimd128Register(1));
      __ nor_v(dst, dst, dst);
      break;
    }
    case kMipsI8x16GtS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ clt_s_b(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16GeS: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cle_s_b(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16ShrU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ srli_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputInt3(1));
      break;
    }
    case kMipsI8x16AddSatU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ adds_u_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16SubSatU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ subs_u_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16MaxU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ max_u_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16MinU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ min_u_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kMipsI8x16GtU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ clt_u_b(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16GeU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ cle_u_b(i.OutputSimd128Register(), i.InputSimd128Register(1),
                 i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16RoundingAverageU: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ aver_u_b(i.OutputSimd128Register(), i.InputSimd128Register(1),
                  i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16Abs: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ asub_s_b(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  kSimd128RegZero);
      break;
    }
    case kMipsI8x16Popcnt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ pcnt_b(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16BitMask: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Simd128Register src = i.InputSimd128Register(0);
      Simd128Register scratch0 = kSimd128RegZero;
      Simd128Register scratch1 = kSimd128ScratchReg;
      __ srli_b(scratch0, src, 7);
      __ srli_h(scratch1, scratch0, 7);
      __ or_v(scratch0, scratch0, scratch1);
      __ srli_w(scratch1, scratch0, 14);
      __ or_v(scratch0, scratch0, scratch1);
      __ srli_d(scratch1, scratch0, 28);
      __ or_v(scratch0, scratch0, scratch1);
      __ shf_w(scratch1, scratch0, 0x0E);
      __ ilvev_b(scratch0, scratch1, scratch0);
      __ copy_u_h(dst, scratch0, 0);
      break;
    }
    case kMipsS128And: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ and_v(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(1));
      break;
    }
    case kMipsS128Or: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ or_v(i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputSimd128Register(1));
      break;
    }
    case kMipsS128Xor: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(1));
      break;
    }
    case kMipsS128Not: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ nor_v(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputSimd128Register(0));
      break;
    }
    case kMipsV128AnyTrue: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Label all_false;

      __ BranchMSA(&all_false, MSA_BRANCH_V, all_zero,
                   i.InputSimd128Register(0), USE_DELAY_SLOT);
      __ li(dst, 0);  // branch delay slot
      __ li(dst, -1);
      __ bind(&all_false);
      break;
    }
    case kMipsI64x2AllTrue: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Label all_true;
      __ BranchMSA(&all_true, MSA_BRANCH_D, all_not_zero,
                   i.InputSimd128Register(0), USE_DELAY_SLOT);
      __ li(dst, -1);  // branch delay slot
      __ li(dst, 0);
      __ bind(&all_true);
      break;
    }
    case kMipsI32x4AllTrue: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Label all_true;
      __ BranchMSA(&all_true, MSA_BRANCH_W, all_not_zero,
                   i.InputSimd128Register(0), USE_DELAY_SLOT);
      __ li(dst, -1);  // branch delay slot
      __ li(dst, 0);
      __ bind(&all_true);
      break;
    }
    case kMipsI16x8AllTrue: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Label all_true;
      __ BranchMSA(&all_true, MSA_BRANCH_H, all_not_zero,
                   i.InputSimd128Register(0), USE_DELAY_SLOT);
      __ li(dst, -1);  // branch delay slot
      __ li(dst, 0);
      __ bind(&all_true);
      break;
    }
    case kMipsI8x16AllTrue: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Register dst = i.OutputRegister();
      Label all_true;
      __ BranchMSA(&all_true, MSA_BRANCH_B, all_not_zero,
                   i.InputSimd128Register(0), USE_DELAY_SLOT);
      __ li(dst, -1);  // branch delay slot
      __ li(dst, 0);
      __ bind(&all_true);
      break;
    }
    case kMipsMsaLd: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ ld_b(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kMipsMsaSt: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ st_b(i.InputSimd128Register(2), i.MemoryOperand());
      break;
    }
    case kMipsS32x4InterleaveRight: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [7, 6, 5, 4], src0 = [3, 2, 1, 0]
      // dst = [5, 1, 4, 0]
      __ ilvr_w(dst, src1, src0);
      break;
    }
    case kMipsS32x4InterleaveLeft: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [7, 6, 5, 4], src0 = [3, 2, 1, 0]
      // dst = [7, 3, 6, 2]
      __ ilvl_w(dst, src1, src0);
      break;
    }
    case kMipsS32x4PackEven: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [7, 6, 5, 4], src0 = [3, 2, 1, 0]
      // dst = [6, 4, 2, 0]
      __ pckev_w(dst, src1, src0);
      break;
    }
    case kMipsS32x4PackOdd: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [7, 6, 5, 4], src0 = [3, 2, 1, 0]
      // dst = [7, 5, 3, 1]
      __ pckod_w(dst, src1, src0);
      break;
    }
    case kMipsS32x4InterleaveEven: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [7, 6, 5, 4], src0 = [3, 2, 1, 0]
      // dst = [6, 2, 4, 0]
      __ ilvev_w(dst, src1, src0);
      break;
    }
    case kMipsS32x4InterleaveOdd: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [7, 6, 5, 4], src0 = [3, 2, 1, 0]
      // dst = [7, 3, 5, 1]
      __ ilvod_w(dst, src1, src0);
      break;
    }
    case kMipsS32x4Shuffle: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);

      int32_t shuffle = i.InputInt32(2);

      if (src0 == src1) {
        // Unary S32x4 shuffles are handled with shf.w instruction
        unsigned lane = shuffle & 0xFF;
        if (FLAG_debug_code) {
          // range of all four lanes, for unary instruction,
          // should belong to the same range, which can be one of these:
          // [0, 3] or [4, 7]
          if (lane >= 4) {
            int32_t shuffle_helper = shuffle;
            for (int i = 0; i < 4; ++i) {
              lane = shuffle_helper & 0xFF;
              CHECK_GE(lane, 4);
              shuffle_helper >>= 8;
            }
          }
        }
        uint32_t i8 = 0;
        for (int i = 0; i < 4; i++) {
          lane = shuffle & 0xFF;
          if (lane >= 4) {
            lane -= 4;
          }
          DCHECK_GT(4, lane);
          i8 |= lane << (2 * i);
          shuffle >>= 8;
        }
        __ shf_w(dst, src0, i8);
      } else {
        // For binary shuffles use vshf.w instruction
        if (dst == src0) {
          __ move_v(kSimd128ScratchReg, src0);
          src0 = kSimd128ScratchReg;
        } else if (dst == src1) {
          __ move_v(kSimd128ScratchReg, src1);
          src1 = kSimd128ScratchReg;
        }

        __ li(kScratchReg, i.InputInt32(2));
        __ insert_w(dst, 0, kScratchReg);
        __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
        __ ilvr_b(dst, kSimd128RegZero, dst);
        __ ilvr_h(dst, kSimd128RegZero, dst);
        __ vshf_w(dst, src1, src0);
      }
      break;
    }
    case kMipsS16x8InterleaveRight: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [15, ... 11, 10, 9, 8], src0 = [7, ... 3, 2, 1, 0]
      // dst = [11, 3, 10, 2, 9, 1, 8, 0]
      __ ilvr_h(dst, src1, src0);
      break;
    }
    case kMipsS16x8InterleaveLeft: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [15, ... 11, 10, 9, 8], src0 = [7, ... 3, 2, 1, 0]
      // dst = [15, 7, 14, 6, 13, 5, 12, 4]
      __ ilvl_h(dst, src1, src0);
      break;
    }
    case kMipsS16x8PackEven: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [15, ... 11, 10, 9, 8], src0 = [7, ... 3, 2, 1, 0]
      // dst = [14, 12, 10, 8, 6, 4, 2, 0]
      __ pckev_h(dst, src1, src0);
      break;
    }
    case kMipsS16x8PackOdd: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [15, ... 11, 10, 9, 8], src0 = [7, ... 3, 2, 1, 0]
      // dst = [15, 13, 11, 9, 7, 5, 3, 1]
      __ pckod_h(dst, src1, src0);
      break;
    }
    case kMipsS16x8InterleaveEven: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [15, ... 11, 10, 9, 8], src0 = [7, ... 3, 2, 1, 0]
      // dst = [14, 6, 12, 4, 10, 2, 8, 0]
      __ ilvev_h(dst, src1, src0);
      break;
    }
    case kMipsS16x8InterleaveOdd: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [15, ... 11, 10, 9, 8], src0 = [7, ... 3, 2, 1, 0]
      // dst = [15, 7, ... 11, 3, 9, 1]
      __ ilvod_h(dst, src1, src0);
      break;
    }
    case kMipsS16x4Reverse: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      // src = [7, 6, 5, 4, 3, 2, 1, 0], dst = [4, 5, 6, 7, 0, 1, 2, 3]
      // shf.df imm field: 0 1 2 3 = 00011011 = 0x1B
      __ shf_h(i.OutputSimd128Register(), i.InputSimd128Register(0), 0x1B);
      break;
    }
    case kMipsS16x2Reverse: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      // src = [7, 6, 5, 4, 3, 2, 1, 0], dst = [6, 7, 4, 5, 3, 2, 0, 1]
      // shf.df imm field: 2 3 0 1 = 10110001 = 0xB1
      __ shf_h(i.OutputSimd128Register(), i.InputSimd128Register(0), 0xB1);
      break;
    }
    case kMipsS8x16InterleaveRight: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [31, ... 19, 18, 17, 16], src0 = [15, ... 3, 2, 1, 0]
      // dst = [23, 7, ... 17, 1, 16, 0]
      __ ilvr_b(dst, src1, src0);
      break;
    }
    case kMipsS8x16InterleaveLeft: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [31, ... 19, 18, 17, 16], src0 = [15, ... 3, 2, 1, 0]
      // dst = [31, 15, ... 25, 9, 24, 8]
      __ ilvl_b(dst, src1, src0);
      break;
    }
    case kMipsS8x16PackEven: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [31, ... 19, 18, 17, 16], src0 = [15, ... 3, 2, 1, 0]
      // dst = [30, 28, ... 6, 4, 2, 0]
      __ pckev_b(dst, src1, src0);
      break;
    }
    case kMipsS8x16PackOdd: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [31, ... 19, 18, 17, 16], src0 = [15, ... 3, 2, 1, 0]
      // dst = [31, 29, ... 7, 5, 3, 1]
      __ pckod_b(dst, src1, src0);
      break;
    }
    case kMipsS8x16InterleaveEven: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [31, ... 19, 18, 17, 16], src0 = [15, ... 3, 2, 1, 0]
      // dst = [30, 14, ... 18, 2, 16, 0]
      __ ilvev_b(dst, src1, src0);
      break;
    }
    case kMipsS8x16InterleaveOdd: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);
      // src1 = [31, ... 19, 18, 17, 16], src0 = [15, ... 3, 2, 1, 0]
      // dst = [31, 15, ... 19, 3, 17, 1]
      __ ilvod_b(dst, src1, src0);
      break;
    }
    case kMipsS8x16Concat: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      DCHECK(dst == i.InputSimd128Register(0));
      __ sldi_b(dst, i.InputSimd128Register(1), i.InputInt4(2));
      break;
    }
    case kMipsI8x16Shuffle: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register(),
                      src0 = i.InputSimd128Register(0),
                      src1 = i.InputSimd128Register(1);

      if (dst == src0) {
        __ move_v(kSimd128ScratchReg, src0);
        src0 = kSimd128ScratchReg;
      } else if (dst == src1) {
        __ move_v(kSimd128ScratchReg, src1);
        src1 = kSimd128ScratchReg;
      }

      __ li(kScratchReg, i.InputInt32(2));
      __ insert_w(dst, 0, kScratchReg);
      __ li(kScratchReg, i.InputInt32(3));
      __ insert_w(dst, 1, kScratchReg);
      __ li(kScratchReg, i.InputInt32(4));
      __ insert_w(dst, 2, kScratchReg);
      __ li(kScratchReg, i.InputInt32(5));
      __ insert_w(dst, 3, kScratchReg);
      __ vshf_b(dst, src1, src0);
      break;
    }
    case kMipsI8x16Swizzle: {
      Simd128Register dst = i.OutputSimd128Register(),
                      tbl = i.InputSimd128Register(0),
                      ctl = i.InputSimd128Register(1);
      DCHECK(dst != ctl && dst != tbl);
      Simd128Register zeroReg = i.TempSimd128Register(0);
      __ fill_w(zeroReg, zero_reg);
      __ move_v(dst, ctl);
      __ vshf_b(dst, tbl, zeroReg);
      break;
    }
    case kMipsS8x8Reverse: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      // src = [15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0]
      // dst = [8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7]
      // [A B C D] => [B A D C]: shf.w imm: 2 3 0 1 = 10110001 = 0xB1
      // C: [7, 6, 5, 4] => A': [4, 5, 6, 7]: shf.b imm: 00011011 = 0x1B
      __ shf_w(kSimd128ScratchReg, i.InputSimd128Register(0), 0xB1);
      __ shf_b(i.OutputSimd128Register(), kSimd128ScratchReg, 0x1B);
      break;
    }
    case kMipsS8x4Reverse: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      // src = [15, 14, ... 3, 2, 1, 0], dst = [12, 13, 14, 15, ... 0, 1, 2, 3]
      // shf.df imm field: 0 1 2 3 = 00011011 = 0x1B
      __ shf_b(i.OutputSimd128Register(), i.InputSimd128Register(0), 0x1B);
      break;
    }
    case kMipsS8x2Reverse: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      // src = [15, 14, ... 3, 2, 1, 0], dst = [14, 15, 12, 13, ... 2, 3, 0, 1]
      // shf.df imm field: 2 3 0 1 = 10110001 = 0xB1
      __ shf_b(i.OutputSimd128Register(), i.InputSimd128Register(0), 0xB1);
      break;
    }
    case kMipsI32x4SConvertI16x8Low: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src = i.InputSimd128Register(0);
      __ ilvr_h(kSimd128ScratchReg, src, src);
      __ slli_w(dst, kSimd128ScratchReg, 16);
      __ srai_w(dst, dst, 16);
      break;
    }
    case kMipsI32x4SConvertI16x8High: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src = i.InputSimd128Register(0);
      __ ilvl_h(kSimd128ScratchReg, src, src);
      __ slli_w(dst, kSimd128ScratchReg, 16);
      __ srai_w(dst, dst, 16);
      break;
    }
    case kMipsI32x4UConvertI16x8Low: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvr_h(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI32x4UConvertI16x8High: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvl_h(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8SConvertI8x16Low: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src = i.InputSimd128Register(0);
      __ ilvr_b(kSimd128ScratchReg, src, src);
      __ slli_h(dst, kSimd128ScratchReg, 8);
      __ srai_h(dst, dst, 8);
      break;
    }
    case kMipsI16x8SConvertI8x16High: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src = i.InputSimd128Register(0);
      __ ilvl_b(kSimd128ScratchReg, src, src);
      __ slli_h(dst, kSimd128ScratchReg, 8);
      __ srai_h(dst, dst, 8);
      break;
    }
    case kMipsI16x8SConvertI32x4: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src0 = i.InputSimd128Register(0);
      Simd128Register src1 = i.InputSimd128Register(1);
      __ sat_s_w(kSimd128ScratchReg, src0, 15);
      __ sat_s_w(kSimd128RegZero, src1, 15);  // kSimd128RegZero as scratch
      __ pckev_h(dst, kSimd128RegZero, kSimd128ScratchReg);
      break;
    }
    case kMipsI16x8UConvertI32x4: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src0 = i.InputSimd128Register(0);
      Simd128Register src1 = i.InputSimd128Register(1);
      __ sat_u_w(kSimd128ScratchReg, src0, 15);
      __ sat_u_w(kSimd128RegZero, src1, 15);  // kSimd128RegZero as scratch
      __ pckev_h(dst, kSimd128RegZero, kSimd128ScratchReg);
      break;
    }
    case kMipsI16x8UConvertI8x16Low: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvr_b(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI16x8UConvertI8x16High: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      __ xor_v(kSimd128RegZero, kSimd128RegZero, kSimd128RegZero);
      __ ilvl_b(i.OutputSimd128Register(), kSimd128RegZero,
                i.InputSimd128Register(0));
      break;
    }
    case kMipsI8x16SConvertI16x8: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src0 = i.InputSimd128Register(0);
      Simd128Register src1 = i.InputSimd128Register(1);
      __ sat_s_h(kSimd128ScratchReg, src0, 7);
      __ sat_s_h(kSimd128RegZero, src1, 7);  // kSimd128RegZero as scratch
      __ pckev_b(dst, kSimd128RegZero, kSimd128ScratchReg);
      break;
    }
    case kMipsI8x16UConvertI16x8: {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      Simd128Register dst = i.OutputSimd128Register();
      Simd128Register src0 = i.InputSimd128Register(0);
      Simd128Register src1 = i.InputSimd128Register(1);
      __ sat_u_h(kSimd128ScratchReg, src0, 7);
      __ sat_u_h(kSimd128RegZero, src1, 7);  // kSimd128RegZero as scratch
      __ pckev_b(dst, kSimd128RegZero, kSimd128ScratchReg);
      break;
    }
  }
  return kSuccess;
}

void AssembleBranchToLabels(CodeGenerator* gen, TurboAssembler* tasm,
                            Instruction* instr, FlagsCondition condition,
                            Label* tlabel, Label* flabel, bool fallthru) {
#undef __
#define __ tasm->

  Condition cc = kNoCondition;
  // MIPS does not have condition code flags, so compare and branch are
  // implemented differently than on the other arch's. The compare operations
  // emit mips pseudo-instructions, which are handled here by branch
  // instructions that do the actual comparison. Essential that the input
  // registers to compare pseudo-op are not modified before this branch op, as
  // they are tested here.

  MipsOperandConverter i(gen, instr);
  if (instr->arch_opcode() == kMipsTst) {
    cc = FlagsConditionToConditionTst(condition);
    __ Branch(tlabel, cc, kScratchReg, Operand(zero_reg));
  } else if (instr->arch_opcode() == kMipsAddOvf ||
             instr->arch_opcode() == kMipsSubOvf) {
    // Overflow occurs if overflow register is negative
    switch (condition) {
      case kOverflow:
        __ Branch(tlabel, lt, kScratchReg, Operand(zero_reg));
        break;
      case kNotOverflow:
        __ Branch(tlabel, ge, kScratchReg, Operand(zero_reg));
        break;
      default:
        UNSUPPORTED_COND(instr->arch_opcode(), condition);
        break;
    }
  } else if (instr->arch_opcode() == kMipsMulOvf) {
    // Overflow occurs if overflow register is not zero
    switch (condition) {
      case kOverflow:
        __ Branch(tlabel, ne, kScratchReg, Operand(zero_reg));
        break;
      case kNotOverflow:
        __ Branch(tlabel, eq, kScratchReg, Operand(zero_reg));
        break;
      default:
        UNSUPPORTED_COND(kMipsMulOvf, condition);
        break;
    }
  } else if (instr->arch_opcode() == kMipsCmp) {
    cc = FlagsConditionToConditionCmp(condition);
    __ Branch(tlabel, cc, i.InputRegister(0), i.InputOperand(1));
  } else if (instr->arch_opcode() == kArchStackPointerGreaterThan) {
    cc = FlagsConditionToConditionCmp(condition);
    DCHECK((cc == ls) || (cc == hi));
    if (cc == ls) {
      __ xori(i.TempRegister(0), i.TempRegister(0), 1);
    }
    __ Branch(tlabel, ne, i.TempRegister(0), Operand(zero_reg));
  } else if (instr->arch_opcode() == kMipsCmpS ||
             instr->arch_opcode() == kMipsCmpD) {
    bool predicate;
    FlagsConditionToConditionCmpFPU(&predicate, condition);
    if (predicate) {
      __ BranchTrueF(tlabel);
    } else {
      __ BranchFalseF(tlabel);
    }
  } else {
    PrintF("AssembleArchBranch Unimplemented arch_opcode: %d\n",
           instr->arch_opcode());
    UNIMPLEMENTED();
  }
  if (!fallthru) __ Branch(flabel);  // no fallthru to flabel.
#undef __
#define __ tasm()->
}

// Assembles branches after an instruction.
void CodeGenerator::AssembleArchBranch(Instruction* instr, BranchInfo* branch) {
  Label* tlabel = branch->true_label;
  Label* flabel = branch->false_label;
  AssembleBranchToLabels(this, tasm(), instr, branch->condition, tlabel, flabel,
                         branch->fallthru);
}

void CodeGenerator::AssembleBranchPoisoning(FlagsCondition condition,
                                            Instruction* instr) {
  // TODO(jarin) Handle float comparisons (kUnordered[Not]Equal).
  if (condition == kUnorderedEqual || condition == kUnorderedNotEqual) {
    return;
  }

  MipsOperandConverter i(this, instr);
  condition = NegateFlagsCondition(condition);

  switch (instr->arch_opcode()) {
    case kMipsCmp: {
      __ LoadZeroOnCondition(kSpeculationPoisonRegister, i.InputRegister(0),
                             i.InputOperand(1),
                             FlagsConditionToConditionCmp(condition));
    }
      return;
    case kMipsTst: {
      switch (condition) {
        case kEqual:
          __ LoadZeroIfConditionZero(kSpeculationPoisonRegister, kScratchReg);
          break;
        case kNotEqual:
          __ LoadZeroIfConditionNotZero(kSpeculationPoisonRegister,
                                        kScratchReg);
          break;
        default:
          UNREACHABLE();
      }
    }
      return;
    case kMipsAddOvf:
    case kMipsSubOvf: {
      // Overflow occurs if overflow register is negative
      __ Slt(kScratchReg2, kScratchReg, zero_reg);
      switch (condition) {
        case kOverflow:
          __ LoadZeroIfConditionNotZero(kSpeculationPoisonRegister,
                                        kScratchReg2);
          break;
        case kNotOverflow:
          __ LoadZeroIfConditionZero(kSpeculationPoisonRegister, kScratchReg2);
          break;
        default:
          UNSUPPORTED_COND(instr->arch_opcode(), condition);
      }
    }
      return;
    case kMipsMulOvf: {
      // Overflow occurs if overflow register is not zero
      switch (condition) {
        case kOverflow:
          __ LoadZeroIfConditionNotZero(kSpeculationPoisonRegister,
                                        kScratchReg);
          break;
        case kNotOverflow:
          __ LoadZeroIfConditionZero(kSpeculationPoisonRegister, kScratchReg);
          break;
        default:
          UNSUPPORTED_COND(instr->arch_opcode(), condition);
      }
    }
      return;
    case kMipsCmpS:
    case kMipsCmpD: {
      bool predicate;
      FlagsConditionToConditionCmpFPU(&predicate, condition);
      if (predicate) {
        __ LoadZeroIfFPUCondition(kSpeculationPoisonRegister);
      } else {
        __ LoadZeroIfNotFPUCondition(kSpeculationPoisonRegister);
      }
    }
      return;
    default:
      UNREACHABLE();
  }
}

void CodeGenerator::AssembleArchDeoptBranch(Instruction* instr,
                                            BranchInfo* branch) {
  AssembleArchBranch(instr, branch);
}

void CodeGenerator::AssembleArchJump(RpoNumber target) {
  if (!IsNextInAssemblyOrder(target)) __ Branch(GetLabel(target));
}

#if V8_ENABLE_WEBASSEMBLY
void CodeGenerator::AssembleArchTrap(Instruction* instr,
                                     FlagsCondition condition) {
  class OutOfLineTrap final : public OutOfLineCode {
   public:
    OutOfLineTrap(CodeGenerator* gen, Instruction* instr)
        : OutOfLineCode(gen), instr_(instr), gen_(gen) {}

    void Generate() final {
      MipsOperandConverter i(gen_, instr_);
      TrapId trap_id =
          static_cast<TrapId>(i.InputInt32(instr_->InputCount() - 1));
      GenerateCallToTrap(trap_id);
    }

   private:
    void GenerateCallToTrap(TrapId trap_id) {
      if (trap_id == TrapId::kInvalid) {
        // We cannot test calls to the runtime in cctest/test-run-wasm.
        // Therefore we emit a call to C here instead of a call to the runtime.
        // We use the context register as the scratch register, because we do
        // not have a context here.
        __ PrepareCallCFunction(0, 0, cp);
        __ CallCFunction(
            ExternalReference::wasm_call_trap_callback_for_testing(), 0);
        __ LeaveFrame(StackFrame::WASM);
        auto call_descriptor = gen_->linkage()->GetIncomingDescriptor();
        int pop_count = static_cast<int>(call_descriptor->ParameterSlotCount());
        __ Drop(pop_count);
        __ Ret();
      } else {
        gen_->AssembleSourcePosition(instr_);
        // A direct call to a wasm runtime stub defined in this module.
        // Just encode the stub index. This will be patched when the code
        // is added to the native module and copied into wasm code space.
        __ Call(static_cast<Address>(trap_id), RelocInfo::WASM_STUB_CALL);
        ReferenceMap* reference_map =
            gen_->zone()->New<ReferenceMap>(gen_->zone());
        gen_->RecordSafepoint(reference_map);
        if (FLAG_debug_code) {
          __ stop();
        }
      }
    }

    Instruction* instr_;
    CodeGenerator* gen_;
  };
  auto ool = zone()->New<OutOfLineTrap>(this, instr);
  Label* tlabel = ool->entry();
  AssembleBranchToLabels(this, tasm(), instr, condition, tlabel, nullptr, true);
}
#endif  // V8_ENABLE_WEBASSEMBLY

// Assembles boolean materializations after an instruction.
void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  MipsOperandConverter i(this, instr);

  // Materialize a full 32-bit 1 or 0 value. The result register is always the
  // last output of the instruction.
  DCHECK_NE(0u, instr->OutputCount());
  Register result = i.OutputRegister(instr->OutputCount() - 1);
  Condition cc = kNoCondition;
  // MIPS does not have condition code flags, so compare and branch are
  // implemented differently than on the other arch's. The compare operations
  // emit mips pseudo-instructions, which are checked and handled here.

  if (instr->arch_opcode() == kMipsTst) {
    cc = FlagsConditionToConditionTst(condition);
    if (cc == eq) {
      __ Sltu(result, kScratchReg, 1);
    } else {
      __ Sltu(result, zero_reg, kScratchReg);
    }
    return;
  } else if (instr->arch_opcode() == kMipsAddOvf ||
             instr->arch_opcode() == kMipsSubOvf) {
    // Overflow occurs if overflow register is negative
    __ slt(result, kScratchReg, zero_reg);
  } else if (instr->arch_opcode() == kMipsMulOvf) {
    // Overflow occurs if overflow register is not zero
    __ Sgtu(result, kScratchReg, zero_reg);
  } else if (instr->arch_opcode() == kMipsCmp) {
    cc = FlagsConditionToConditionCmp(condition);
    switch (cc) {
      case eq:
      case ne: {
        Register left = i.InputRegister(0);
        Operand right = i.InputOperand(1);
        if (instr->InputAt(1)->IsImmediate()) {
          if (is_int16(-right.immediate())) {
            if (right.immediate() == 0) {
              if (cc == eq) {
                __ Sltu(result, left, 1);
              } else {
                __ Sltu(result, zero_reg, left);
              }
            } else {
              __ Addu(result, left, -right.immediate());
              if (cc == eq) {
                __ Sltu(result, result, 1);
              } else {
                __ Sltu(result, zero_reg, result);
              }
            }
          } else {
            if (is_uint16(right.immediate())) {
              __ Xor(result, left, right);
            } else {
              __ li(kScratchReg, right);
              __ Xor(result, left, kScratchReg);
            }
            if (cc == eq) {
              __ Sltu(result, result, 1);
            } else {
              __ Sltu(result, zero_reg, result);
            }
          }
        } else {
          __ Xor(result, left, right);
          if (cc == eq) {
            __ Sltu(result, result, 1);
          } else {
            __ Sltu(result, zero_reg, result);
          }
        }
      } break;
      case lt:
      case ge: {
        Register left = i.InputRegister(0);
        Operand right = i.InputOperand(1);
        __ Slt(result, left, right);
        if (cc == ge) {
          __ xori(result, result, 1);
        }
      } break;
      case gt:
      case le: {
        Register left = i.InputRegister(1);
        Operand right = i.InputOperand(0);
        __ Slt(result, left, right);
        if (cc == le) {
          __ xori(result, result, 1);
        }
      } break;
      case lo:
      case hs: {
        Register left = i.InputRegister(0);
        Operand right = i.InputOperand(1);
        __ Sltu(result, left, right);
        if (cc == hs) {
          __ xori(result, result, 1);
        }
      } break;
      case hi:
      case ls: {
        Register left = i.InputRegister(1);
        Operand right = i.InputOperand(0);
        __ Sltu(result, left, right);
        if (cc == ls) {
          __ xori(result, result, 1);
        }
      } break;
      default:
        UNREACHABLE();
    }
    return;
  } else if (instr->arch_opcode() == kMipsCmpD ||
             instr->arch_opcode() == kMipsCmpS) {
    FPURegister left = i.InputOrZeroDoubleRegister(0);
    FPURegister right = i.InputOrZeroDoubleRegister(1);
    if ((left == kDoubleRegZero || right == kDoubleRegZero) &&
        !__ IsDoubleZeroRegSet()) {
      __ Move(kDoubleRegZero, 0.0);
    }
    bool predicate;
    FlagsConditionToConditionCmpFPU(&predicate, condition);
    if (!IsMipsArchVariant(kMips32r6)) {
      __ li(result, Operand(1));
      if (predicate) {
        __ Movf(result, zero_reg);
      } else {
        __ Movt(result, zero_reg);
      }
    } else {
      __ mfc1(result, kDoubleCompareReg);
      if (predicate) {
        __ And(result, result, 1);  // cmp returns all 1's/0's, use only LSB.
      } else {
        __ Addu(result, result, 1);  // Toggle result for not equal.
      }
    }
    return;
  } else if (instr->arch_opcode() == kArchStackPointerGreaterThan) {
    cc = FlagsConditionToConditionCmp(condition);
    DCHECK((cc == ls) || (cc == hi));
    if (cc == ls) {
      __ xori(i.OutputRegister(), i.TempRegister(0), 1);
    }
    return;
  } else {
    PrintF("AssembleArchBoolean Unimplemented arch_opcode is : %d\n",
           instr->arch_opcode());
    TRACE_UNIMPL();
    UNIMPLEMENTED();
  }
}

void CodeGenerator::AssembleArchBinarySearchSwitch(Instruction* instr) {
  MipsOperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  std::vector<std::pair<int32_t, Label*>> cases;
  for (size_t index = 2; index < instr->InputCount(); index += 2) {
    cases.push_back({i.InputInt32(index + 0), GetLabel(i.InputRpo(index + 1))});
  }
  AssembleArchBinarySearchSwitchRange(input, i.InputRpo(1), cases.data(),
                                      cases.data() + cases.size());
}

void CodeGenerator::AssembleArchTableSwitch(Instruction* instr) {
  MipsOperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  size_t const case_count = instr->InputCount() - 2;
  __ Branch(GetLabel(i.InputRpo(1)), hs, input, Operand(case_count));
  __ GenerateSwitchTable(input, case_count, [&i, this](size_t index) {
    return GetLabel(i.InputRpo(index + 2));
  });
}

void CodeGenerator::AssembleArchSelect(Instruction* instr,
                                       FlagsCondition condition) {
  UNIMPLEMENTED();
}

void CodeGenerator::FinishFrame(Frame* frame) {
  auto call_descriptor = linkage()->GetIncomingDescriptor();

  const RegList saves_fpu = call_descriptor->CalleeSavedFPRegisters();
  if (saves_fpu != 0) {
    frame->AlignSavedCalleeRegisterSlots();
  }

  if (saves_fpu != 0) {
    int count = base::bits::CountPopulation(saves_fpu);
    DCHECK_EQ(kNumCalleeSavedFPU, count);
    frame->AllocateSavedCalleeRegisterSlots(count *
                                            (kDoubleSize / kSystemPointerSize));
  }

  const RegList saves = call_descriptor->CalleeSavedRegisters();
  if (saves != 0) {
    int count = base::bits::CountPopulation(saves);
    DCHECK_EQ(kNumCalleeSaved, count + 1);
    frame->AllocateSavedCalleeRegisterSlots(count);
  }
}

void CodeGenerator::AssembleConstructFrame() {
  auto call_descriptor = linkage()->GetIncomingDescriptor();
  if (frame_access_state()->has_frame()) {
    if (call_descriptor->IsCFunctionCall()) {
#if V8_ENABLE_WEBASSEMBLY
      if (info()->GetOutputStackFrameType() == StackFrame::C_WASM_ENTRY) {
        __ StubPrologue(StackFrame::C_WASM_ENTRY);
        // Reserve stack space for saving the c_entry_fp later.
        __ Subu(sp, sp, Operand(kSystemPointerSize));
#else
      // For balance.
      if (false) {
#endif  // V8_ENABLE_WEBASSEMBLY
      } else {
        __ Push(ra, fp);
        __ mov(fp, sp);
      }
    } else if (call_descriptor->IsJSFunctionCall()) {
      __ Prologue();
    } else {
      __ StubPrologue(info()->GetOutputStackFrameType());
#if V8_ENABLE_WEBASSEMBLY
      if (call_descriptor->IsWasmFunctionCall()) {
        __ Push(kWasmInstanceRegister);
      } else if (call_descriptor->IsWasmImportWrapper() ||
                 call_descriptor->IsWasmCapiFunction()) {
        // Wasm import wrappers are passed a tuple in the place of the instance.
        // Unpack the tuple into the instance and the target callable.
        // This must be done here in the codegen because it cannot be expressed
        // properly in the graph.
        __ lw(kJSFunctionRegister,
              FieldMemOperand(kWasmInstanceRegister, Tuple2::kValue2Offset));
        __ lw(kWasmInstanceRegister,
              FieldMemOperand(kWasmInstanceRegister, Tuple2::kValue1Offset));
        __ Push(kWasmInstanceRegister);
        if (call_descriptor->IsWasmCapiFunction()) {
          // Reserve space for saving the PC later.
          __ Subu(sp, sp, Operand(kSystemPointerSize));
        }
      }
#endif  // V8_ENABLE_WEBASSEMBLY
    }
  }

  int required_slots =
      frame()->GetTotalFrameSlotCount() - frame()->GetFixedSlotCount();

  if (info()->is_osr()) {
    // TurboFan OSR-compiled functions cannot be entered directly.
    __ Abort(AbortReason::kShouldNotDirectlyEnterOsrFunction);

    // Unoptimized code jumps directly to this entrypoint while the unoptimized
    // frame is still on the stack. Optimized code uses OSR values directly from
    // the unoptimized frame. Thus, all that needs to be done is to allocate the
    // remaining stack slots.
    __ RecordComment("-- OSR entrypoint --");
    osr_pc_offset_ = __ pc_offset();
    required_slots -= osr_helper()->UnoptimizedFrameSlots();
    ResetSpeculationPoison();
  }

  const RegList saves = call_descriptor->CalleeSavedRegisters();
  const RegList saves_fpu = call_descriptor->CalleeSavedFPRegisters();

  if (required_slots > 0) {
    DCHECK(frame_access_state()->has_frame());
#if V8_ENABLE_WEBASSEMBLY
    if (info()->IsWasm() && required_slots > 128) {
      // For WebAssembly functions with big frames we have to do the stack
      // overflow check before we construct the frame. Otherwise we may not
      // have enough space on the stack to call the runtime for the stack
      // overflow.
      Label done;

      // If the frame is bigger than the stack, we throw the stack overflow
      // exception unconditionally. Thereby we can avoid the integer overflow
      // check in the condition code.
      if ((required_slots * kSystemPointerSize) < (FLAG_stack_size * 1024)) {
        __ Lw(
             kScratchReg,
             FieldMemOperand(kWasmInstanceRegister,
                             WasmInstanceObject::kRealStackLimitAddressOffset));
        __ Lw(kScratchReg, MemOperand(kScratchReg));
        __ Addu(kScratchReg, kScratchReg,
                      Operand(required_slots * kSystemPointerSize));
        __ Branch(&done, uge, sp, Operand(kScratchReg));
      }

      __ Call(wasm::WasmCode::kWasmStackOverflow, RelocInfo::WASM_STUB_CALL);
      // We come from WebAssembly, there are no references for the GC.
      ReferenceMap* reference_map = zone()->New<ReferenceMap>(zone());
      RecordSafepoint(reference_map);
      if (FLAG_debug_code) {
        __ stop();
      }

      __ bind(&done);
    }
#endif  // V8_ENABLE_WEBASSEMBLY
  }

  const int returns = frame()->GetReturnSlotCount();

  // Skip callee-saved and return slots, which are pushed below.
  required_slots -= base::bits::CountPopulation(saves);
  required_slots -= 2 * base::bits::CountPopulation(saves_fpu);
  required_slots -= returns;
  if (required_slots > 0) {
    __ Subu(sp, sp, Operand(required_slots * kSystemPointerSize));
  }

  // Save callee-saved FPU registers.
  if (saves_fpu != 0) {
    __ MultiPushFPU(saves_fpu);
  }

  if (saves != 0) {
    // Save callee-saved registers.
    __ MultiPush(saves);
    DCHECK_EQ(kNumCalleeSaved, base::bits::CountPopulation(saves) + 1);
  }

  if (returns != 0) {
    // Create space for returns.
    __ Subu(sp, sp, Operand(returns * kSystemPointerSize));
  }
}

void CodeGenerator::AssembleReturn(InstructionOperand* additional_pop_count) {
  auto call_descriptor = linkage()->GetIncomingDescriptor();

  const int returns = frame()->GetReturnSlotCount();
  if (returns != 0) {
    __ Addu(sp, sp, Operand(returns * kSystemPointerSize));
  }

  // Restore GP registers.
  const RegList saves = call_descriptor->CalleeSavedRegisters();
  if (saves != 0) {
    __ MultiPop(saves);
  }

  // Restore FPU registers.
  const RegList saves_fpu = call_descriptor->CalleeSavedFPRegisters();
  if (saves_fpu != 0) {
    __ MultiPopFPU(saves_fpu);
  }

  MipsOperandConverter g(this, nullptr);
  const int parameter_slots =
      static_cast<int>(call_descriptor->ParameterSlotCount());

  // {aditional_pop_count} is only greater than zero if {parameter_slots = 0}.
  // Check RawMachineAssembler::PopAndReturn.
  if (parameter_slots != 0) {
    if (additional_pop_count->IsImmediate()) {
      DCHECK_EQ(g.ToConstant(additional_pop_count).ToInt32(), 0);
    } else if (FLAG_debug_code) {
      __ Assert(eq, AbortReason::kUnexpectedAdditionalPopValue,
                g.ToRegister(additional_pop_count),
                Operand(static_cast<int64_t>(0)));
    }
  }
  // Functions with JS linkage have at least one parameter (the receiver).
  // If {parameter_slots} == 0, it means it is a builtin with
  // kDontAdaptArgumentsSentinel, which takes care of JS arguments popping
  // itself.
  const bool drop_jsargs = frame_access_state()->has_frame() &&
                           call_descriptor->IsJSFunctionCall() &&
                           parameter_slots != 0;

  if (call_descriptor->IsCFunctionCall()) {
    AssembleDeconstructFrame();
  } else if (frame_access_state()->has_frame()) {
    // Canonicalize JSFunction return sites for now unless they have an variable
    // number of stack slot pops.
    if (additional_pop_count->IsImmediate() &&
        g.ToConstant(additional_pop_count).ToInt32() == 0) {
      if (return_label_.is_bound()) {
        __ Branch(&return_label_);
        return;
      } else {
        __ bind(&return_label_);
      }
    }
    if (drop_jsargs) {
      // Get the actual argument count
      __ Lw(t0, MemOperand(fp, StandardFrameConstants::kArgCOffset));
    }
    AssembleDeconstructFrame();
  }

  if (drop_jsargs) {
    // We must pop all arguments from the stack (including the receiver). This
    // number of arguments is given by max(1 + argc_reg, parameter_slots).
    __ Addu(t0, t0, Operand(1));  // Also pop the receiver.
    if (parameter_slots > 1) {
      __ li(kScratchReg, parameter_slots);
      __ slt(kScratchReg2, t0, kScratchReg);
      __ movn(t0, kScratchReg, kScratchReg2);
    }
    __ sll(t0, t0, kSystemPointerSizeLog2);
    __ Addu(sp, sp, t0);
  } else if (additional_pop_count->IsImmediate()) {
    DCHECK_EQ(Constant::kInt32, g.ToConstant(additional_pop_count).type());
    int additional_count = g.ToConstant(additional_pop_count).ToInt32();
    __ Drop(parameter_slots + additional_count);
  } else {
    Register pop_reg = g.ToRegister(additional_pop_count);
    __ Drop(parameter_slots);
    __ sll(pop_reg, pop_reg, kSystemPointerSizeLog2);
    __ Addu(sp, sp, pop_reg);
  }
  __ Ret();
}

void CodeGenerator::FinishCode() {}

void CodeGenerator::PrepareForDeoptimizationExits(
    ZoneDeque<DeoptimizationExit*>* exits) {}

void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  MipsOperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      __ mov(g.ToRegister(destination), src);
    } else {
      __ sw(src, g.ToMemOperand(destination));
    }
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsRegister()) {
      __ lw(g.ToRegister(destination), src);
    } else {
      Register temp = kScratchReg;
      __ lw(temp, src);
      __ sw(temp, g.ToMemOperand(destination));
    }
  } else if (source->IsConstant()) {
    Constant src = g.ToConstant(source);
    if (destination->IsRegister() || destination->IsStackSlot()) {
      Register dst =
          destination->IsRegister() ? g.ToRegister(destination) : kScratchReg;
      switch (src.type()) {
        case Constant::kInt32:
#if V8_ENABLE_WEBASSEMBLY
          if (RelocInfo::IsWasmReference(src.rmode()))
            __ li(dst, Operand(src.ToInt32(), src.rmode()));
          else
#endif  // V8_ENABLE_WEBASSEMBLY
            __ li(dst, Operand(src.ToInt32()));
          break;
        case Constant::kFloat32:
          __ li(dst, Operand::EmbeddedNumber(src.ToFloat32()));
          break;
        case Constant::kInt64:
          UNREACHABLE();
          break;
        case Constant::kFloat64:
          __ li(dst, Operand::EmbeddedNumber(src.ToFloat64().value()));
          break;
        case Constant::kExternalReference:
          __ li(dst, src.ToExternalReference());
          break;
        case Constant::kDelayedStringConstant:
          __ li(dst, src.ToDelayedStringConstant());
          break;
        case Constant::kHeapObject: {
          Handle<HeapObject> src_object = src.ToHeapObject();
          RootIndex index;
          if (IsMaterializableFromRoot(src_object, &index)) {
            __ LoadRoot(dst, index);
          } else {
            __ li(dst, src_object);
          }
          break;
        }
        case Constant::kCompressedHeapObject:
          UNREACHABLE();
        case Constant::kRpoNumber:
          UNREACHABLE();  // TODO(titzer): loading RPO numbers on mips.
          break;
      }
      if (destination->IsStackSlot()) __ sw(dst, g.ToMemOperand(destination));
    } else if (src.type() == Constant::kFloat32) {
      if (destination->IsFPStackSlot()) {
        MemOperand dst = g.ToMemOperand(destination);
        if (bit_cast<int32_t>(src.ToFloat32()) == 0) {
          __ sw(zero_reg, dst);
        } else {
          __ li(kScratchReg, Operand(bit_cast<int32_t>(src.ToFloat32())));
          __ sw(kScratchReg, dst);
        }
      } else {
        DCHECK(destination->IsFPRegister());
        FloatRegister dst = g.ToSingleRegister(destination);
        __ Move(dst, src.ToFloat32());
      }
    } else {
      DCHECK_EQ(Constant::kFloat64, src.type());
      DoubleRegister dst = destination->IsFPRegister()
                               ? g.ToDoubleRegister(destination)
                               : kScratchDoubleReg;
      __ Move(dst, src.ToFloat64().value());
      if (destination->IsFPStackSlot()) {
        __ Sdc1(dst, g.ToMemOperand(destination));
      }
    }
  } else if (source->IsFPRegister()) {
    MachineRepresentation rep = LocationOperand::cast(source)->representation();
    if (rep == MachineRepresentation::kSimd128) {
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      MSARegister src = g.ToSimd128Register(source);
      if (destination->IsSimd128Register()) {
        MSARegister dst = g.ToSimd128Register(destination);
        __ move_v(dst, src);
      } else {
        DCHECK(destination->IsSimd128StackSlot());
        __ st_b(src, g.ToMemOperand(destination));
      }
    } else {
      FPURegister src = g.ToDoubleRegister(source);
      if (destination->IsFPRegister()) {
        FPURegister dst = g.ToDoubleRegister(destination);
        __ Move(dst, src);
      } else {
        DCHECK(destination->IsFPStackSlot());
        MachineRepresentation rep =
            LocationOperand::cast(source)->representation();
        if (rep == MachineRepresentation::kFloat64) {
          __ Sdc1(src, g.ToMemOperand(destination));
        } else if (rep == MachineRepresentation::kFloat32) {
          __ swc1(src, g.ToMemOperand(destination));
        } else {
          UNREACHABLE();
        }
      }
    }
  } else if (source->IsFPStackSlot()) {
    DCHECK(destination->IsFPRegister() || destination->IsFPStackSlot());
    MemOperand src = g.ToMemOperand(source);
    MachineRepresentation rep = LocationOperand::cast(source)->representation();
    if (destination->IsFPRegister()) {
      if (rep == MachineRepresentation::kFloat64) {
        __ Ldc1(g.ToDoubleRegister(destination), src);
      } else if (rep == MachineRepresentation::kFloat32) {
        __ lwc1(g.ToDoubleRegister(destination), src);
      } else {
        DCHECK_EQ(MachineRepresentation::kSimd128, rep);
        CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
        __ ld_b(g.ToSimd128Register(destination), src);
      }
    } else {
      FPURegister temp = kScratchDoubleReg;
      if (rep == MachineRepresentation::kFloat64) {
        __ Ldc1(temp, src);
        __ Sdc1(temp, g.ToMemOperand(destination));
      } else if (rep == MachineRepresentation::kFloat32) {
        __ lwc1(temp, src);
        __ swc1(temp, g.ToMemOperand(destination));
      } else {
        DCHECK_EQ(MachineRepresentation::kSimd128, rep);
        CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
        MSARegister temp = kSimd128ScratchReg;
        __ ld_b(temp, src);
        __ st_b(temp, g.ToMemOperand(destination));
      }
    }
  } else {
    UNREACHABLE();
  }
}

void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  MipsOperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    // Register-register.
    Register temp = kScratchReg;
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      Register dst = g.ToRegister(destination);
      __ Move(temp, src);
      __ Move(src, dst);
      __ Move(dst, temp);
    } else {
      DCHECK(destination->IsStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ mov(temp, src);
      __ lw(src, dst);
      __ sw(temp, dst);
    }
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsStackSlot());
    Register temp_0 = kScratchReg;
    Register temp_1 = kScratchReg2;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    __ lw(temp_0, src);
    __ lw(temp_1, dst);
    __ sw(temp_0, dst);
    __ sw(temp_1, src);
  } else if (source->IsFPRegister()) {
    if (destination->IsFPRegister()) {
      MachineRepresentation rep =
          LocationOperand::cast(source)->representation();
      if (rep == MachineRepresentation::kSimd128) {
        CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
        MSARegister temp = kSimd128ScratchReg;
        MSARegister src = g.ToSimd128Register(source);
        MSARegister dst = g.ToSimd128Register(destination);
        __ move_v(temp, src);
        __ move_v(src, dst);
        __ move_v(dst, temp);
      } else {
        FPURegister temp = kScratchDoubleReg;
        FPURegister src = g.ToDoubleRegister(source);
        FPURegister dst = g.ToDoubleRegister(destination);
        __ Move(temp, src);
        __ Move(src, dst);
        __ Move(dst, temp);
      }
    } else {
      DCHECK(destination->IsFPStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      MachineRepresentation rep =
          LocationOperand::cast(source)->representation();
      if (rep == MachineRepresentation::kFloat64) {
        FPURegister temp = kScratchDoubleReg;
        FPURegister src = g.ToDoubleRegister(source);
        __ Move(temp, src);
        __ Ldc1(src, dst);
        __ Sdc1(temp, dst);
      } else if (rep == MachineRepresentation::kFloat32) {
        FPURegister temp = kScratchDoubleReg;
        FPURegister src = g.ToFloatRegister(source);
        __ Move(temp, src);
        __ lwc1(src, dst);
        __ swc1(temp, dst);
      } else {
        DCHECK_EQ(MachineRepresentation::kSimd128, rep);
        CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
        MSARegister temp = kSimd128ScratchReg;
        MSARegister src = g.ToSimd128Register(source);
        __ move_v(temp, src);
        __ ld_b(src, dst);
        __ st_b(temp, dst);
      }
    }
  } else if (source->IsFPStackSlot()) {
    DCHECK(destination->IsFPStackSlot());
    Register temp_0 = kScratchReg;
    FPURegister temp_1 = kScratchDoubleReg;
    MemOperand src0 = g.ToMemOperand(source);
    MemOperand dst0 = g.ToMemOperand(destination);
    MachineRepresentation rep = LocationOperand::cast(source)->representation();
    if (rep == MachineRepresentation::kFloat64) {
      MemOperand src1(src0.rm(), src0.offset() + kIntSize);
      MemOperand dst1(dst0.rm(), dst0.offset() + kIntSize);
      __ Ldc1(temp_1, dst0);  // Save destination in temp_1.
      __ lw(temp_0, src0);    // Then use temp_0 to copy source to destination.
      __ sw(temp_0, dst0);
      __ lw(temp_0, src1);
      __ sw(temp_0, dst1);
      __ Sdc1(temp_1, src0);
    } else if (rep == MachineRepresentation::kFloat32) {
      __ lwc1(temp_1, dst0);  // Save destination in temp_1.
      __ lw(temp_0, src0);    // Then use temp_0 to copy source to destination.
      __ sw(temp_0, dst0);
      __ swc1(temp_1, src0);
    } else {
      DCHECK_EQ(MachineRepresentation::kSimd128, rep);
      MemOperand src1(src0.rm(), src0.offset() + kIntSize);
      MemOperand dst1(dst0.rm(), dst0.offset() + kIntSize);
      MemOperand src2(src0.rm(), src0.offset() + 2 * kIntSize);
      MemOperand dst2(dst0.rm(), dst0.offset() + 2 * kIntSize);
      MemOperand src3(src0.rm(), src0.offset() + 3 * kIntSize);
      MemOperand dst3(dst0.rm(), dst0.offset() + 3 * kIntSize);
      CpuFeatureScope msa_scope(tasm(), MIPS_SIMD);
      MSARegister temp_1 = kSimd128ScratchReg;
      __ ld_b(temp_1, dst0);  // Save destination in temp_1.
      __ lw(temp_0, src0);    // Then use temp_0 to copy source to destination.
      __ sw(temp_0, dst0);
      __ lw(temp_0, src1);
      __ sw(temp_0, dst1);
      __ lw(temp_0, src2);
      __ sw(temp_0, dst2);
      __ lw(temp_0, src3);
      __ sw(temp_0, dst3);
      __ st_b(temp_1, src0);
    }
  } else {
    // No other combinations are possible.
    UNREACHABLE();
  }
}

void CodeGenerator::AssembleJumpTable(Label** targets, size_t target_count) {
  // On 32-bit MIPS we emit the jump tables inline.
  UNREACHABLE();
}

#undef __
#undef ASSEMBLE_F64X2_ARITHMETIC_BINOP
#undef ASSEMBLE_SIMD_EXTENDED_MULTIPLY

}  // namespace compiler
}  // namespace internal
}  // namespace v8

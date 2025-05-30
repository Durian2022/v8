// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INCLUDED_FROM_MACRO_ASSEMBLER_H
#error This header must be included via macro-assembler.h
#endif

#ifndef V8_CODEGEN_MIPS_MACRO_ASSEMBLER_MIPS_H_
#define V8_CODEGEN_MIPS_MACRO_ASSEMBLER_MIPS_H_

#include "src/codegen/assembler.h"
#include "src/codegen/mips/assembler-mips.h"
#include "src/common/globals.h"
#include "src/objects/contexts.h"

namespace v8 {
namespace internal {

// Forward declarations
enum class AbortReason : uint8_t;

// Reserved Register Usage Summary.
//
// Registers t8, t9, and at are reserved for use by the MacroAssembler.
//
// The programmer should know that the MacroAssembler may clobber these three,
// but won't touch other registers except in special cases.
//
// Per the MIPS ABI, register t9 must be used for indirect function call
// via 'jalr t9' or 'jr t9' instructions. This is relied upon by gcc when
// trying to update gp register for position-independent-code. Whenever
// MIPS generated code calls C code, it must be via t9 register.

// Flags used for LeaveExitFrame function.
enum LeaveExitFrameMode { EMIT_RETURN = true, NO_EMIT_RETURN = false };

// Flags used for the li macro-assembler function.
enum LiFlags {
  // If the constant value can be represented in just 16 bits, then
  // optimize the li to use a single instruction, rather than lui/ori pair.
  OPTIMIZE_SIZE = 0,
  // Always use 2 instructions (lui/ori pair), even if the constant could
  // be loaded with just one, so that this value is patchable later.
  CONSTANT_SIZE = 1
};

enum RAStatus { kRAHasNotBeenSaved, kRAHasBeenSaved };

Register GetRegisterThatIsNotOneOf(Register reg1, Register reg2 = no_reg,
                                   Register reg3 = no_reg,
                                   Register reg4 = no_reg,
                                   Register reg5 = no_reg,
                                   Register reg6 = no_reg);

// -----------------------------------------------------------------------------
// Static helper functions.
// Generate a MemOperand for loading a field from an object.
inline MemOperand FieldMemOperand(Register object, int offset) {
  return MemOperand(object, offset - kHeapObjectTag);
}

// Generate a MemOperand for storing arguments 5..N on the stack
// when calling CallCFunction().
inline MemOperand CFunctionArgumentOperand(int index) {
  DCHECK_GT(index, kCArgSlotCount);
  // Argument 5 takes the slot just past the four Arg-slots.
  int offset = (index - 5) * kPointerSize + kCArgsSlotsSize;
  return MemOperand(sp, offset);
}

class V8_EXPORT_PRIVATE TurboAssembler : public TurboAssemblerBase {
 public:
  using TurboAssemblerBase::TurboAssemblerBase;

  // Activation support.
  void EnterFrame(StackFrame::Type type);
  void EnterFrame(StackFrame::Type type, bool load_constant_pool_pointer_reg) {
    // Out-of-line constant pool not implemented on mips.
    UNREACHABLE();
  }
  void LeaveFrame(StackFrame::Type type);

  void AllocateStackSpace(Register bytes) { Subu(sp, sp, bytes); }
  void AllocateStackSpace(int bytes) {
    DCHECK_GE(bytes, 0);
    if (bytes == 0) return;
    Subu(sp, sp, Operand(bytes));
  }

  // Generates function and stub prologue code.
  void StubPrologue(StackFrame::Type type);
  void Prologue();

  void InitializeRootRegister() {
    ExternalReference isolate_root = ExternalReference::isolate_root(isolate());
    li(kRootRegister, Operand(isolate_root));
  }

  // Jump unconditionally to given label.
  // We NEED a nop in the branch delay slot, as it used by v8, for example in
  // CodeGenerator::ProcessDeferred().
  // Currently the branch delay slot is filled by the MacroAssembler.
  // Use rather b(Label) for code generation.
  void jmp(Label* L) { Branch(L); }

  // -------------------------------------------------------------------------
  // Debugging.

  void Trap();
  void DebugBreak();

  // Calls Abort(msg) if the condition cc is not satisfied.
  // Use --debug_code to enable.
  void Assert(Condition cc, AbortReason reason, Register rs, Operand rt);

  // Like Assert(), but always enabled.
  void Check(Condition cc, AbortReason reason, Register rs, Operand rt);

  // Print a message to stdout and abort execution.
  void Abort(AbortReason msg);

  // Arguments macros.
#define COND_TYPED_ARGS Condition cond, Register r1, const Operand &r2
#define COND_ARGS cond, r1, r2

  // Cases when relocation is not needed.
#define DECLARE_NORELOC_PROTOTYPE(Name, target_type)                          \
  void Name(target_type target, BranchDelaySlot bd = PROTECT);                \
  inline void Name(BranchDelaySlot bd, target_type target) {                  \
    Name(target, bd);                                                         \
  }                                                                           \
  void Name(target_type target, COND_TYPED_ARGS,                              \
            BranchDelaySlot bd = PROTECT);                                    \
  inline void Name(BranchDelaySlot bd, target_type target, COND_TYPED_ARGS) { \
    Name(target, COND_ARGS, bd);                                              \
  }

#define DECLARE_BRANCH_PROTOTYPES(Name)   \
  DECLARE_NORELOC_PROTOTYPE(Name, Label*) \
  DECLARE_NORELOC_PROTOTYPE(Name, int32_t)

  DECLARE_BRANCH_PROTOTYPES(Branch)
  DECLARE_BRANCH_PROTOTYPES(BranchAndLink)
  DECLARE_BRANCH_PROTOTYPES(BranchShort)

#undef DECLARE_BRANCH_PROTOTYPES
#undef COND_TYPED_ARGS
#undef COND_ARGS

  // Floating point branches
  void CompareF32(FPUCondition cc, FPURegister cmp1, FPURegister cmp2) {
    CompareF(S, cc, cmp1, cmp2);
  }

  void CompareIsNanF32(FPURegister cmp1, FPURegister cmp2) {
    CompareIsNanF(S, cmp1, cmp2);
  }

  void CompareF64(FPUCondition cc, FPURegister cmp1, FPURegister cmp2) {
    CompareF(D, cc, cmp1, cmp2);
  }

  void CompareIsNanF64(FPURegister cmp1, FPURegister cmp2) {
    CompareIsNanF(D, cmp1, cmp2);
  }

  void BranchTrueShortF(Label* target, BranchDelaySlot bd = PROTECT);
  void BranchFalseShortF(Label* target, BranchDelaySlot bd = PROTECT);

  void BranchTrueF(Label* target, BranchDelaySlot bd = PROTECT);
  void BranchFalseF(Label* target, BranchDelaySlot bd = PROTECT);

  // MSA Branches
  void BranchMSA(Label* target, MSABranchDF df, MSABranchCondition cond,
                 MSARegister wt, BranchDelaySlot bd = PROTECT);

  void Branch(Label* L, Condition cond, Register rs, RootIndex index,
              BranchDelaySlot bdslot = PROTECT);

  // Load int32 in the rd register.
  void li(Register rd, Operand j, LiFlags mode = OPTIMIZE_SIZE);
  inline void li(Register rd, int32_t j, LiFlags mode = OPTIMIZE_SIZE) {
    li(rd, Operand(j), mode);
  }
  void li(Register dst, Handle<HeapObject> value, LiFlags mode = OPTIMIZE_SIZE);
  void li(Register dst, ExternalReference value, LiFlags mode = OPTIMIZE_SIZE);
  void li(Register dst, const StringConstantBase* string,
          LiFlags mode = OPTIMIZE_SIZE);

  void LoadFromConstantsTable(Register destination, int constant_index) final;
  void LoadRootRegisterOffset(Register destination, intptr_t offset) final;
  void LoadRootRelative(Register destination, int32_t offset) final;

// Jump, Call, and Ret pseudo instructions implementing inter-working.
#define COND_ARGS                                  \
  Condition cond = al, Register rs = zero_reg,     \
            const Operand &rt = Operand(zero_reg), \
            BranchDelaySlot bd = PROTECT

  void Jump(Register target, int16_t offset = 0, COND_ARGS);
  void Jump(Register target, Register base, int16_t offset = 0, COND_ARGS);
  void Jump(Register target, const Operand& offset, COND_ARGS);
  void Jump(intptr_t target, RelocInfo::Mode rmode, COND_ARGS);
  void Jump(Address target, RelocInfo::Mode rmode, COND_ARGS);
  // Deffer from li, this method save target to the memory, and then load
  // it to register use lw, it can be used in wasm jump table for concurrent
  // patching.
  void PatchAndJump(Address target);
  void Jump(Handle<Code> code, RelocInfo::Mode rmode, COND_ARGS);
  void Jump(const ExternalReference& reference);
  void Call(Register target, int16_t offset = 0, COND_ARGS);
  void Call(Register target, Register base, int16_t offset = 0, COND_ARGS);
  void Call(Address target, RelocInfo::Mode rmode, COND_ARGS);
  void Call(Handle<Code> code, RelocInfo::Mode rmode = RelocInfo::CODE_TARGET,
            COND_ARGS);
  void Call(Label* target);
  void LoadAddress(Register dst, Label* target);

  // Load the builtin given by the Smi in |builtin_index| into the same
  // register.
  void LoadEntryFromBuiltin(Register builtin_index);
  void CallBuiltinByIndex(Register builtin_index);

  void LoadCodeObjectEntry(Register destination, Register code_object) {
    // TODO(mips): Implement.
    UNIMPLEMENTED();
  }
  void CallCodeObject(Register code_object) {
    // TODO(mips): Implement.
    UNIMPLEMENTED();
  }
  void JumpCodeObject(Register code_object,
                      JumpMode jump_mode = JumpMode::kJump) {
    // TODO(mips): Implement.
    UNIMPLEMENTED();
  }

  // Generates an instruction sequence s.t. the return address points to the
  // instruction following the call.
  // The return address on the stack is used by frame iteration.
  void StoreReturnAddressAndCall(Register target);

  void CallForDeoptimization(Builtin target, int deopt_id, Label* exit,
                             DeoptimizeKind kind, Label* ret,
                             Label* jump_deoptimization_entry_label);

  void Ret(COND_ARGS);
  inline void Ret(BranchDelaySlot bd, Condition cond = al,
                  Register rs = zero_reg,
                  const Operand& rt = Operand(zero_reg)) {
    Ret(cond, rs, rt, bd);
  }

  // Emit code to discard a non-negative number of pointer-sized elements
  // from the stack, clobbering only the sp register.
  void Drop(int count, Condition cond = cc_always, Register reg = no_reg,
            const Operand& op = Operand(no_reg));

  // Trivial case of DropAndRet that utilizes the delay slot.
  void DropAndRet(int drop);

  void DropAndRet(int drop, Condition cond, Register reg, const Operand& op);

  void Lw(Register rd, const MemOperand& rs);
  void Sw(Register rd, const MemOperand& rs);

  void push(Register src) {
    Addu(sp, sp, Operand(-kPointerSize));
    sw(src, MemOperand(sp, 0));
  }

  void Push(Register src) { push(src); }
  void Push(Handle<HeapObject> handle);
  void Push(Smi smi);

  // Push two registers. Pushes leftmost register first (to highest address).
  void Push(Register src1, Register src2) {
    Subu(sp, sp, Operand(2 * kPointerSize));
    sw(src1, MemOperand(sp, 1 * kPointerSize));
    sw(src2, MemOperand(sp, 0 * kPointerSize));
  }

  // Push three registers. Pushes leftmost register first (to highest address).
  void Push(Register src1, Register src2, Register src3) {
    Subu(sp, sp, Operand(3 * kPointerSize));
    sw(src1, MemOperand(sp, 2 * kPointerSize));
    sw(src2, MemOperand(sp, 1 * kPointerSize));
    sw(src3, MemOperand(sp, 0 * kPointerSize));
  }

  // Push four registers. Pushes leftmost register first (to highest address).
  void Push(Register src1, Register src2, Register src3, Register src4) {
    Subu(sp, sp, Operand(4 * kPointerSize));
    sw(src1, MemOperand(sp, 3 * kPointerSize));
    sw(src2, MemOperand(sp, 2 * kPointerSize));
    sw(src3, MemOperand(sp, 1 * kPointerSize));
    sw(src4, MemOperand(sp, 0 * kPointerSize));
  }

  // Push five registers. Pushes leftmost register first (to highest address).
  void Push(Register src1, Register src2, Register src3, Register src4,
            Register src5) {
    Subu(sp, sp, Operand(5 * kPointerSize));
    sw(src1, MemOperand(sp, 4 * kPointerSize));
    sw(src2, MemOperand(sp, 3 * kPointerSize));
    sw(src3, MemOperand(sp, 2 * kPointerSize));
    sw(src4, MemOperand(sp, 1 * kPointerSize));
    sw(src5, MemOperand(sp, 0 * kPointerSize));
  }

  void Push(Register src, Condition cond, Register tst1, Register tst2) {
    // Since we don't have conditional execution we use a Branch.
    Branch(3, cond, tst1, Operand(tst2));
    Subu(sp, sp, Operand(kPointerSize));
    sw(src, MemOperand(sp, 0));
  }

  enum PushArrayOrder { kNormal, kReverse };
  void PushArray(Register array, Register size, Register scratch,
                 Register scratch2, PushArrayOrder order = kNormal);

  void MaybeSaveRegisters(RegList registers);
  void MaybeRestoreRegisters(RegList registers);

  void CallEphemeronKeyBarrier(Register object, Register slot_address,
                               SaveFPRegsMode fp_mode);

  void CallRecordWriteStubSaveRegisters(
      Register object, Register slot_address,
      RememberedSetAction remembered_set_action, SaveFPRegsMode fp_mode,
      StubCallMode mode = StubCallMode::kCallBuiltinPointer);
  void CallRecordWriteStub(
      Register object, Register slot_address,
      RememberedSetAction remembered_set_action, SaveFPRegsMode fp_mode,
      StubCallMode mode = StubCallMode::kCallBuiltinPointer);

  // Push multiple registers on the stack.
  // Registers are saved in numerical order, with higher numbered registers
  // saved in higher memory addresses.
  void MultiPush(RegList regs);
  void MultiPushFPU(RegList regs);

  // Calculate how much stack space (in bytes) are required to store caller
  // registers excluding those specified in the arguments.
  int RequiredStackSizeForCallerSaved(SaveFPRegsMode fp_mode,
                                      Register exclusion1 = no_reg,
                                      Register exclusion2 = no_reg,
                                      Register exclusion3 = no_reg) const;

  // Push caller saved registers on the stack, and return the number of bytes
  // stack pointer is adjusted.
  int PushCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1 = no_reg,
                      Register exclusion2 = no_reg,
                      Register exclusion3 = no_reg);
  // Restore caller saved registers from the stack, and return the number of
  // bytes stack pointer is adjusted.
  int PopCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1 = no_reg,
                     Register exclusion2 = no_reg,
                     Register exclusion3 = no_reg);

  void pop(Register dst) {
    lw(dst, MemOperand(sp, 0));
    Addu(sp, sp, Operand(kPointerSize));
  }

  void Pop(Register dst) { pop(dst); }

  // Pop two registers. Pops rightmost register first (from lower address).
  void Pop(Register src1, Register src2) {
    DCHECK(src1 != src2);
    lw(src2, MemOperand(sp, 0 * kPointerSize));
    lw(src1, MemOperand(sp, 1 * kPointerSize));
    Addu(sp, sp, 2 * kPointerSize);
  }

  // Pop three registers. Pops rightmost register first (from lower address).
  void Pop(Register src1, Register src2, Register src3) {
    lw(src3, MemOperand(sp, 0 * kPointerSize));
    lw(src2, MemOperand(sp, 1 * kPointerSize));
    lw(src1, MemOperand(sp, 2 * kPointerSize));
    Addu(sp, sp, 3 * kPointerSize);
  }

  void Pop(uint32_t count = 1) { Addu(sp, sp, Operand(count * kPointerSize)); }

  // Pops multiple values from the stack and load them in the
  // registers specified in regs. Pop order is the opposite as in MultiPush.
  void MultiPop(RegList regs);
  void MultiPopFPU(RegList regs);

  // Load Scaled Address instructions. Parameter sa (shift argument) must be
  // between [1, 31] (inclusive). On pre-r6 architectures the scratch register
  // may be clobbered.
  void Lsa(Register rd, Register rs, Register rt, uint8_t sa,
           Register scratch = at);

#define DEFINE_INSTRUCTION(instr)                          \
  void instr(Register rd, Register rs, const Operand& rt); \
  void instr(Register rd, Register rs, Register rt) {      \
    instr(rd, rs, Operand(rt));                            \
  }                                                        \
  void instr(Register rs, Register rt, int32_t j) { instr(rs, rt, Operand(j)); }

#define DEFINE_INSTRUCTION2(instr)                                 \
  void instr(Register rs, const Operand& rt);                      \
  void instr(Register rs, Register rt) { instr(rs, Operand(rt)); } \
  void instr(Register rs, int32_t j) { instr(rs, Operand(j)); }

#define DEFINE_INSTRUCTION3(instr)                                            \
  void instr(Register rd_hi, Register rd_lo, Register rs, const Operand& rt); \
  void instr(Register rd_hi, Register rd_lo, Register rs, Register rt) {      \
    instr(rd_hi, rd_lo, rs, Operand(rt));                                     \
  }                                                                           \
  void instr(Register rd_hi, Register rd_lo, Register rs, int32_t j) {        \
    instr(rd_hi, rd_lo, rs, Operand(j));                                      \
  }

  DEFINE_INSTRUCTION(Addu)
  DEFINE_INSTRUCTION(Subu)
  DEFINE_INSTRUCTION(Mul)
  DEFINE_INSTRUCTION(Div)
  DEFINE_INSTRUCTION(Divu)
  DEFINE_INSTRUCTION(Mod)
  DEFINE_INSTRUCTION(Modu)
  DEFINE_INSTRUCTION(Mulh)
  DEFINE_INSTRUCTION2(Mult)
  DEFINE_INSTRUCTION(Mulhu)
  DEFINE_INSTRUCTION2(Multu)
  DEFINE_INSTRUCTION2(Div)
  DEFINE_INSTRUCTION2(Divu)

  DEFINE_INSTRUCTION3(Div)
  DEFINE_INSTRUCTION3(Mul)
  DEFINE_INSTRUCTION3(Mulu)

  DEFINE_INSTRUCTION(And)
  DEFINE_INSTRUCTION(Or)
  DEFINE_INSTRUCTION(Xor)
  DEFINE_INSTRUCTION(Nor)
  DEFINE_INSTRUCTION2(Neg)

  DEFINE_INSTRUCTION(Slt)
  DEFINE_INSTRUCTION(Sltu)
  DEFINE_INSTRUCTION(Sle)
  DEFINE_INSTRUCTION(Sleu)
  DEFINE_INSTRUCTION(Sgt)
  DEFINE_INSTRUCTION(Sgtu)
  DEFINE_INSTRUCTION(Sge)
  DEFINE_INSTRUCTION(Sgeu)

  // MIPS32 R2 instruction macro.
  DEFINE_INSTRUCTION(Ror)

#undef DEFINE_INSTRUCTION
#undef DEFINE_INSTRUCTION2
#undef DEFINE_INSTRUCTION3

  void SmiUntag(Register reg) { sra(reg, reg, kSmiTagSize); }

  void SmiUntag(Register dst, Register src) { sra(dst, src, kSmiTagSize); }

  // Removes current frame and its arguments from the stack preserving
  // the arguments and a return address pushed to the stack for the next call.
  // Both |callee_args_count| and |caller_args_count| do not include
  // receiver. |callee_args_count| is not modified. |caller_args_count|
  // is trashed.
  void PrepareForTailCall(Register callee_args_count,
                          Register caller_args_count, Register scratch0,
                          Register scratch1);

  int CalculateStackPassedWords(int num_reg_arguments,
                                int num_double_arguments);

  // Before calling a C-function from generated code, align arguments on stack
  // and add space for the four mips argument slots.
  // After aligning the frame, non-register arguments must be stored on the
  // stack, after the argument-slots using helper: CFunctionArgumentOperand().
  // The argument count assumes all arguments are word sized.
  // Some compilers/platforms require the stack to be aligned when calling
  // C++ code.
  // Needs a scratch register to do some arithmetic. This register will be
  // trashed.
  void PrepareCallCFunction(int num_reg_arguments, int num_double_registers,
                            Register scratch);
  void PrepareCallCFunction(int num_reg_arguments, Register scratch);

  // Arguments 1-4 are placed in registers a0 through a3 respectively.
  // Arguments 5..n are stored to stack using following:
  //  sw(t0, CFunctionArgumentOperand(5));

  // Calls a C function and cleans up the space for arguments allocated
  // by PrepareCallCFunction. The called function is not allowed to trigger a
  // garbage collection, since that might move the code and invalidate the
  // return address (unless this is somehow accounted for by the called
  // function).
  void CallCFunction(ExternalReference function, int num_arguments);
  void CallCFunction(Register function, int num_arguments);
  void CallCFunction(ExternalReference function, int num_reg_arguments,
                     int num_double_arguments);
  void CallCFunction(Register function, int num_reg_arguments,
                     int num_double_arguments);
  void MovFromFloatResult(DoubleRegister dst);
  void MovFromFloatParameter(DoubleRegister dst);

  // There are two ways of passing double arguments on MIPS, depending on
  // whether soft or hard floating point ABI is used. These functions
  // abstract parameter passing for the three different ways we call
  // C functions from generated code.
  void MovToFloatParameter(DoubleRegister src);
  void MovToFloatParameters(DoubleRegister src1, DoubleRegister src2);
  void MovToFloatResult(DoubleRegister src);

  // See comments at the beginning of Builtins::Generate_CEntry.
  inline void PrepareCEntryArgs(int num_args) { li(a0, num_args); }
  inline void PrepareCEntryFunction(const ExternalReference& ref) {
    li(a1, ref);
  }

  void CheckPageFlag(Register object, Register scratch, int mask, Condition cc,
                     Label* condition_met);
#undef COND_ARGS

  // Performs a truncating conversion of a floating point number as used by
  // the JS bitwise operations. See ECMA-262 9.5: ToInt32.
  // Exits with 'result' holding the answer.
  void TruncateDoubleToI(Isolate* isolate, Zone* zone, Register result,
                         DoubleRegister double_input, StubCallMode stub_mode);

  // Conditional move.
  void Movz(Register rd, Register rs, Register rt);
  void Movn(Register rd, Register rs, Register rt);
  void Movt(Register rd, Register rs, uint16_t cc = 0);
  void Movf(Register rd, Register rs, uint16_t cc = 0);

  void LoadZeroIfFPUCondition(Register dest);
  void LoadZeroIfNotFPUCondition(Register dest);

  void LoadZeroIfConditionNotZero(Register dest, Register condition);
  void LoadZeroIfConditionZero(Register dest, Register condition);
  void LoadZeroOnCondition(Register rd, Register rs, const Operand& rt,
                           Condition cond);

  void Clz(Register rd, Register rs);
  void Ctz(Register rd, Register rs);
  void Popcnt(Register rd, Register rs);

  // Int64Lowering instructions
  void AddPair(Register dst_low, Register dst_high, Register left_low,
               Register left_high, Register right_low, Register right_high,
               Register scratch1, Register scratch2);

  void AddPair(Register dst_low, Register dst_high, Register left_low,
               Register left_high, int32_t imm, Register scratch1,
               Register scratch2);

  void SubPair(Register dst_low, Register dst_high, Register left_low,
               Register left_high, Register right_low, Register right_high,
               Register scratch1, Register scratch2);

  void AndPair(Register dst_low, Register dst_high, Register left_low,
               Register left_high, Register right_low, Register right_high);

  void OrPair(Register dst_low, Register dst_high, Register left_low,
              Register left_high, Register right_low, Register right_high);

  void XorPair(Register dst_low, Register dst_high, Register left_low,
               Register left_high, Register right_low, Register right_high);

  void MulPair(Register dst_low, Register dst_high, Register left_low,
               Register left_high, Register right_low, Register right_high,
               Register scratch1, Register scratch2);

  void ShlPair(Register dst_low, Register dst_high, Register src_low,
               Register src_high, Register shift, Register scratch1,
               Register scratch2);

  void ShlPair(Register dst_low, Register dst_high, Register src_low,
               Register src_high, uint32_t shift, Register scratch);

  void ShrPair(Register dst_low, Register dst_high, Register src_low,
               Register src_high, Register shift, Register scratch1,
               Register scratch2);

  void ShrPair(Register dst_low, Register dst_high, Register src_low,
               Register src_high, uint32_t shift, Register scratch);

  void SarPair(Register dst_low, Register dst_high, Register src_low,
               Register src_high, Register shift, Register scratch1,
               Register scratch2);

  void SarPair(Register dst_low, Register dst_high, Register src_low,
               Register src_high, uint32_t shift, Register scratch);

  // MIPS32 R2 instruction macro.
  void Ins(Register rt, Register rs, uint16_t pos, uint16_t size);
  void Ext(Register rt, Register rs, uint16_t pos, uint16_t size);
  void ExtractBits(Register dest, Register source, Register pos, int size,
                   bool sign_extend = false);
  void InsertBits(Register dest, Register source, Register pos, int size);

  void Seb(Register rd, Register rt);
  void Seh(Register rd, Register rt);
  void Neg_s(FPURegister fd, FPURegister fs);
  void Neg_d(FPURegister fd, FPURegister fs);

  // MIPS32 R6 instruction macros.
  void Bovc(Register rt, Register rs, Label* L);
  void Bnvc(Register rt, Register rs, Label* L);

  // Convert single to unsigned word.
  void Trunc_uw_s(FPURegister fd, FPURegister fs, FPURegister scratch);
  void Trunc_uw_s(Register rd, FPURegister fs, FPURegister scratch);

  void Trunc_w_d(FPURegister fd, FPURegister fs);
  void Round_w_d(FPURegister fd, FPURegister fs);
  void Floor_w_d(FPURegister fd, FPURegister fs);
  void Ceil_w_d(FPURegister fd, FPURegister fs);

  // Round double functions
  void Trunc_d_d(FPURegister fd, FPURegister fs);
  void Round_d_d(FPURegister fd, FPURegister fs);
  void Floor_d_d(FPURegister fd, FPURegister fs);
  void Ceil_d_d(FPURegister fd, FPURegister fs);

  // Round float functions
  void Trunc_s_s(FPURegister fd, FPURegister fs);
  void Round_s_s(FPURegister fd, FPURegister fs);
  void Floor_s_s(FPURegister fd, FPURegister fs);
  void Ceil_s_s(FPURegister fd, FPURegister fs);

  // FP32 mode: Move the general purpose register into
  // the high part of the double-register pair.
  // FP64 mode: Move the general-purpose register into
  // the higher 32 bits of the 64-bit coprocessor register,
  // while leaving the low bits unchanged.
  void Mthc1(Register rt, FPURegister fs);

  // FP32 mode: move the high part of the double-register pair into
  // general purpose register.
  // FP64 mode: Move the higher 32 bits of the 64-bit coprocessor register into
  // general-purpose register.
  void Mfhc1(Register rt, FPURegister fs);

  void Madd_s(FPURegister fd, FPURegister fr, FPURegister fs, FPURegister ft,
              FPURegister scratch);
  void Madd_d(FPURegister fd, FPURegister fr, FPURegister fs, FPURegister ft,
              FPURegister scratch);
  void Msub_s(FPURegister fd, FPURegister fr, FPURegister fs, FPURegister ft,
              FPURegister scratch);
  void Msub_d(FPURegister fd, FPURegister fr, FPURegister fs, FPURegister ft,
              FPURegister scratch);

  // Change endianness
  void ByteSwapSigned(Register dest, Register src, int operand_size);
  void ByteSwapUnsigned(Register dest, Register src, int operand_size);

  void Ulh(Register rd, const MemOperand& rs);
  void Ulhu(Register rd, const MemOperand& rs);
  void Ush(Register rd, const MemOperand& rs, Register scratch);

  void Ulw(Register rd, const MemOperand& rs);
  void Usw(Register rd, const MemOperand& rs);

  void Ulwc1(FPURegister fd, const MemOperand& rs, Register scratch);
  void Uswc1(FPURegister fd, const MemOperand& rs, Register scratch);

  void Uldc1(FPURegister fd, const MemOperand& rs, Register scratch);
  void Usdc1(FPURegister fd, const MemOperand& rs, Register scratch);

  void Ldc1(FPURegister fd, const MemOperand& src);
  void Sdc1(FPURegister fs, const MemOperand& dst);

  void Ll(Register rd, const MemOperand& rs);
  void Sc(Register rd, const MemOperand& rs);

  // Perform a floating-point min or max operation with the
  // (IEEE-754-compatible) semantics of MIPS32's Release 6 MIN.fmt/MAX.fmt.
  // Some cases, typically NaNs or +/-0.0, are expected to be rare and are
  // handled in out-of-line code. The specific behaviour depends on supported
  // instructions.
  //
  // These functions assume (and assert) that src1!=src2. It is permitted
  // for the result to alias either input register.
  void Float32Max(FPURegister dst, FPURegister src1, FPURegister src2,
                  Label* out_of_line);
  void Float32Min(FPURegister dst, FPURegister src1, FPURegister src2,
                  Label* out_of_line);
  void Float64Max(DoubleRegister dst, DoubleRegister src1, DoubleRegister src2,
                  Label* out_of_line);
  void Float64Min(DoubleRegister dst, DoubleRegister src1, DoubleRegister src2,
                  Label* out_of_line);

  // Generate out-of-line cases for the macros above.
  void Float32MaxOutOfLine(FPURegister dst, FPURegister src1, FPURegister src2);
  void Float32MinOutOfLine(FPURegister dst, FPURegister src1, FPURegister src2);
  void Float64MaxOutOfLine(DoubleRegister dst, DoubleRegister src1,
                           DoubleRegister src2);
  void Float64MinOutOfLine(DoubleRegister dst, DoubleRegister src1,
                           DoubleRegister src2);

  bool IsDoubleZeroRegSet() { return has_double_zero_reg_set_; }

  void mov(Register rd, Register rt) { or_(rd, rt, zero_reg); }

  inline void Move(Register dst, Handle<HeapObject> handle) { li(dst, handle); }
  inline void Move(Register dst, Smi smi) { li(dst, Operand(smi)); }

  inline void Move(Register dst, Register src) {
    if (dst != src) {
      mov(dst, src);
    }
  }

  inline void Move_d(FPURegister dst, FPURegister src) {
    if (dst != src) {
      mov_d(dst, src);
    }
  }

  inline void Move_s(FPURegister dst, FPURegister src) {
    if (dst != src) {
      mov_s(dst, src);
    }
  }

  inline void Move(FPURegister dst, FPURegister src) { Move_d(dst, src); }

  inline void Move(Register dst_low, Register dst_high, FPURegister src) {
    mfc1(dst_low, src);
    Mfhc1(dst_high, src);
  }

  inline void FmoveHigh(Register dst_high, FPURegister src) {
    Mfhc1(dst_high, src);
  }

  inline void FmoveHigh(FPURegister dst, Register src_high) {
    Mthc1(src_high, dst);
  }

  inline void FmoveLow(Register dst_low, FPURegister src) {
    mfc1(dst_low, src);
  }

  void FmoveLow(FPURegister dst, Register src_low);

  inline void Move(FPURegister dst, Register src_low, Register src_high) {
    mtc1(src_low, dst);
    Mthc1(src_high, dst);
  }

  void Move(FPURegister dst, float imm) { Move(dst, bit_cast<uint32_t>(imm)); }
  void Move(FPURegister dst, double imm) { Move(dst, bit_cast<uint64_t>(imm)); }
  void Move(FPURegister dst, uint32_t src);
  void Move(FPURegister dst, uint64_t src);

  // -------------------------------------------------------------------------
  // Overflow operations.

  // AddOverflow sets overflow register to a negative value if
  // overflow occured, otherwise it is zero or positive
  void AddOverflow(Register dst, Register left, const Operand& right,
                   Register overflow);
  // SubOverflow sets overflow register to a negative value if
  // overflow occured, otherwise it is zero or positive
  void SubOverflow(Register dst, Register left, const Operand& right,
                   Register overflow);
  // MulOverflow sets overflow register to zero if no overflow occured
  void MulOverflow(Register dst, Register left, const Operand& right,
                   Register overflow);

// Number of instructions needed for calculation of switch table entry address
#ifdef _MIPS_ARCH_MIPS32R6
  static constexpr int kSwitchTablePrologueSize = 5;
#else
  static constexpr int kSwitchTablePrologueSize = 10;
#endif
  // GetLabelFunction must be lambda '[](size_t index) -> Label*' or a
  // functor/function with 'Label *func(size_t index)' declaration.
  template <typename Func>
  void GenerateSwitchTable(Register index, size_t case_count,
                           Func GetLabelFunction);

  // Load an object from the root table.
  void LoadRoot(Register destination, RootIndex index) final;
  void LoadRoot(Register destination, RootIndex index, Condition cond,
                Register src1, const Operand& src2);

  void LoadMap(Register destination, Register object);

  // If the value is a NaN, canonicalize the value else, do nothing.
  void FPUCanonicalizeNaN(const DoubleRegister dst, const DoubleRegister src);

  // ---------------------------------------------------------------------------
  // FPU macros. These do not handle special cases like NaN or +- inf.

  // Convert unsigned word to double.
  void Cvt_d_uw(FPURegister fd, Register rs, FPURegister scratch);

  // Convert double to unsigned word.
  void Trunc_uw_d(FPURegister fd, FPURegister fs, FPURegister scratch);
  void Trunc_uw_d(Register rd, FPURegister fs, FPURegister scratch);

  // Jump the register contains a smi.
  void JumpIfSmi(Register value, Label* smi_label, Register scratch = at,
                 BranchDelaySlot bd = PROTECT);

  void JumpIfEqual(Register a, int32_t b, Label* dest) {
    li(kScratchReg, Operand(b));
    Branch(dest, eq, a, Operand(kScratchReg));
  }

  void JumpIfLessThan(Register a, int32_t b, Label* dest) {
    li(kScratchReg, Operand(b));
    Branch(dest, lt, a, Operand(kScratchReg));
  }

  // Push a standard frame, consisting of ra, fp, context and JS function.
  void PushStandardFrame(Register function_reg);

  // Get the actual activation frame alignment for target environment.
  static int ActivationFrameAlignment();

  // Compute the start of the generated instruction stream from the current PC.
  // This is an alternative to embedding the {CodeObject} handle as a reference.
  void ComputeCodeStartAddress(Register dst);

  void ResetSpeculationPoisonRegister();

  // Control-flow integrity:

  // Define a function entrypoint. This doesn't emit any code for this
  // architecture, as control-flow integrity is not supported for it.
  void CodeEntry() {}
  // Define an exception handler.
  void ExceptionHandler() {}
  // Define an exception handler and bind a label.
  void BindExceptionHandler(Label* label) { bind(label); }

 protected:
  void BranchLong(Label* L, BranchDelaySlot bdslot);

  inline Register GetRtAsRegisterHelper(const Operand& rt, Register scratch);

  inline int32_t GetOffset(int32_t offset, Label* L, OffsetSize bits);

 private:
  bool has_double_zero_reg_set_ = false;

  // Performs a truncating conversion of a floating point number as used by
  // the JS bitwise operations. See ECMA-262 9.5: ToInt32. Goes to 'done' if it
  // succeeds, otherwise falls through if result is saturated. On return
  // 'result' either holds answer, or is clobbered on fall through.
  void TryInlineTruncateDoubleToI(Register result, DoubleRegister input,
                                  Label* done);

  void CallCFunctionHelper(Register function_base, int16_t function_offset,
                           int num_reg_arguments, int num_double_arguments);

  void CompareF(SecondaryField sizeField, FPUCondition cc, FPURegister cmp1,
                FPURegister cmp2);

  void CompareIsNanF(SecondaryField sizeField, FPURegister cmp1,
                     FPURegister cmp2);

  void BranchShortMSA(MSABranchDF df, Label* target, MSABranchCondition cond,
                      MSARegister wt, BranchDelaySlot bd = PROTECT);

  // TODO(mips) Reorder parameters so out parameters come last.
  bool CalculateOffset(Label* L, int32_t* offset, OffsetSize bits);
  bool CalculateOffset(Label* L, int32_t* offset, OffsetSize bits,
                       Register* scratch, const Operand& rt);

  void BranchShortHelperR6(int32_t offset, Label* L);
  void BranchShortHelper(int16_t offset, Label* L, BranchDelaySlot bdslot);
  bool BranchShortHelperR6(int32_t offset, Label* L, Condition cond,
                           Register rs, const Operand& rt);
  bool BranchShortHelper(int16_t offset, Label* L, Condition cond, Register rs,
                         const Operand& rt, BranchDelaySlot bdslot);
  bool BranchShortCheck(int32_t offset, Label* L, Condition cond, Register rs,
                        const Operand& rt, BranchDelaySlot bdslot);

  void BranchAndLinkShortHelperR6(int32_t offset, Label* L);
  void BranchAndLinkShortHelper(int16_t offset, Label* L,
                                BranchDelaySlot bdslot);
  void BranchAndLinkShort(int32_t offset, BranchDelaySlot bdslot = PROTECT);
  void BranchAndLinkShort(Label* L, BranchDelaySlot bdslot = PROTECT);
  bool BranchAndLinkShortHelperR6(int32_t offset, Label* L, Condition cond,
                                  Register rs, const Operand& rt);
  bool BranchAndLinkShortHelper(int16_t offset, Label* L, Condition cond,
                                Register rs, const Operand& rt,
                                BranchDelaySlot bdslot);
  bool BranchAndLinkShortCheck(int32_t offset, Label* L, Condition cond,
                               Register rs, const Operand& rt,
                               BranchDelaySlot bdslot);
  void BranchAndLinkLong(Label* L, BranchDelaySlot bdslot);

  template <typename RoundFunc>
  void RoundDouble(FPURegister dst, FPURegister src, FPURoundingMode mode,
                   RoundFunc round);

  template <typename RoundFunc>
  void RoundFloat(FPURegister dst, FPURegister src, FPURoundingMode mode,
                  RoundFunc round);

  // Push a fixed frame, consisting of ra, fp.
  void PushCommonFrame(Register marker_reg = no_reg);
};

// MacroAssembler implements a collection of frequently used macros.
class V8_EXPORT_PRIVATE MacroAssembler : public TurboAssembler {
 public:
  using TurboAssembler::TurboAssembler;

  // It assumes that the arguments are located below the stack pointer.
  // argc is the number of arguments not including the receiver.
  // TODO(victorgomes): Remove this function once we stick with the reversed
  // arguments order.
  void LoadReceiver(Register dest, Register argc) {
    Lw(dest, MemOperand(sp, 0));
  }

  void StoreReceiver(Register rec, Register argc, Register scratch) {
    Sw(rec, MemOperand(sp, 0));
  }

  // Swap two registers.  If the scratch register is omitted then a slightly
  // less efficient form using xor instead of mov is emitted.
  void Swap(Register reg1, Register reg2, Register scratch = no_reg);

  void PushRoot(RootIndex index) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    LoadRoot(scratch, index);
    Push(scratch);
  }

  // Compare the object in a register to a value and jump if they are equal.
  void JumpIfRoot(Register with, RootIndex index, Label* if_equal) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    LoadRoot(scratch, index);
    Branch(if_equal, eq, with, Operand(scratch));
  }

  // Compare the object in a register to a value and jump if they are not equal.
  void JumpIfNotRoot(Register with, RootIndex index, Label* if_not_equal) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    LoadRoot(scratch, index);
    Branch(if_not_equal, ne, with, Operand(scratch));
  }

  // Checks if value is in range [lower_limit, higher_limit] using a single
  // comparison.
  void JumpIfIsInRange(Register value, unsigned lower_limit,
                       unsigned higher_limit, Label* on_in_range);

  // ---------------------------------------------------------------------------
  // GC Support

  // Notify the garbage collector that we wrote a pointer into an object.
  // |object| is the object being stored into, |value| is the object being
  // stored.  value and scratch registers are clobbered by the operation.
  // The offset is the offset from the start of the object, not the offset from
  // the tagged HeapObject pointer.  For use with FieldOperand(reg, off).
  void RecordWriteField(
      Register object, int offset, Register value, Register scratch,
      RAStatus ra_status, SaveFPRegsMode save_fp,
      RememberedSetAction remembered_set_action = RememberedSetAction::kEmit,
      SmiCheck smi_check = SmiCheck::kInline);

  // For a given |object| notify the garbage collector that the slot |address|
  // has been written.  |value| is the object being stored. The value and
  // address registers are clobbered by the operation.
  void RecordWrite(
      Register object, Register address, Register value, RAStatus ra_status,
      SaveFPRegsMode save_fp,
      RememberedSetAction remembered_set_action = RememberedSetAction::kEmit,
      SmiCheck smi_check = SmiCheck::kInline);

  void Pref(int32_t hint, const MemOperand& rs);

  // Enter exit frame.
  // argc - argument count to be dropped by LeaveExitFrame.
  // save_doubles - saves FPU registers on stack, currently disabled.
  // stack_space - extra stack space.
  void EnterExitFrame(bool save_doubles, int stack_space = 0,
                      StackFrame::Type frame_type = StackFrame::EXIT);

  // Leave the current exit frame.
  void LeaveExitFrame(bool save_doubles, Register arg_count,
                      bool do_return = NO_EMIT_RETURN,
                      bool argument_count_is_length = false);

  // Make sure the stack is aligned. Only emits code in debug mode.
  void AssertStackIsAligned();

  // Load the global proxy from the current context.
  void LoadGlobalProxy(Register dst) {
    LoadNativeContextSlot(dst, Context::GLOBAL_PROXY_INDEX);
  }

  void LoadNativeContextSlot(Register dst, int index);

  // -------------------------------------------------------------------------
  // JavaScript invokes.

  // Invoke the JavaScript function code by either calling or jumping.
  void InvokeFunctionCode(Register function, Register new_target,
                          Register expected_parameter_count,
                          Register actual_parameter_count, InvokeType type);

  // On function call, call into the debugger if necessary.
  void CheckDebugHook(Register fun, Register new_target,
                      Register expected_parameter_count,
                      Register actual_parameter_count);

  // Invoke the JavaScript function in the given register. Changes the
  // current context to the context in the function before invoking.
  void InvokeFunctionWithNewTarget(Register function, Register new_target,
                                   Register actual_parameter_count,
                                   InvokeType type);

  void InvokeFunction(Register function, Register expected_parameter_count,
                      Register actual_parameter_count, InvokeType type);

  // Exception handling.

  // Push a new stack handler and link into stack handler chain.
  void PushStackHandler();

  // Unlink the stack handler on top of the stack from the stack handler chain.
  // Must preserve the result register.
  void PopStackHandler();

  // -------------------------------------------------------------------------
  // Support functions.

  void GetObjectType(Register function, Register map, Register type_reg);

  void GetInstanceTypeRange(Register map, Register type_reg,
                            InstanceType lower_limit, Register range);

  // -------------------------------------------------------------------------
  // Runtime calls.

  // Call a runtime routine.
  void CallRuntime(const Runtime::Function* f, int num_arguments,
                   SaveFPRegsMode save_doubles = SaveFPRegsMode::kIgnore);

  // Convenience function: Same as above, but takes the fid instead.
  void CallRuntime(Runtime::FunctionId fid,
                   SaveFPRegsMode save_doubles = SaveFPRegsMode::kIgnore) {
    const Runtime::Function* function = Runtime::FunctionForId(fid);
    CallRuntime(function, function->nargs, save_doubles);
  }

  // Convenience function: Same as above, but takes the fid instead.
  void CallRuntime(Runtime::FunctionId id, int num_arguments,
                   SaveFPRegsMode save_doubles = SaveFPRegsMode::kIgnore) {
    CallRuntime(Runtime::FunctionForId(id), num_arguments, save_doubles);
  }

  // Convenience function: tail call a runtime routine (jump).
  void TailCallRuntime(Runtime::FunctionId fid);

  // Jump to the builtin routine.
  void JumpToExternalReference(const ExternalReference& builtin,
                               BranchDelaySlot bd = PROTECT,
                               bool builtin_exit_frame = false);

  // Generates a trampoline to jump to the off-heap instruction stream.
  void JumpToInstructionStream(Address entry);

  // ---------------------------------------------------------------------------
  // In-place weak references.
  void LoadWeakValue(Register out, Register in, Label* target_if_cleared);

  // -------------------------------------------------------------------------
  // StatsCounter support.

  void IncrementCounter(StatsCounter* counter, int value, Register scratch1,
                        Register scratch2) {
    if (!FLAG_native_code_counters) return;
    EmitIncrementCounter(counter, value, scratch1, scratch2);
  }
  void EmitIncrementCounter(StatsCounter* counter, int value, Register scratch1,
                            Register scratch2);
  void DecrementCounter(StatsCounter* counter, int value, Register scratch1,
                        Register scratch2) {
    if (!FLAG_native_code_counters) return;
    EmitDecrementCounter(counter, value, scratch1, scratch2);
  }
  void EmitDecrementCounter(StatsCounter* counter, int value, Register scratch1,
                            Register scratch2);

  // -------------------------------------------------------------------------
  // Stack limit utilities

  enum StackLimitKind { kInterruptStackLimit, kRealStackLimit };
  void LoadStackLimit(Register destination, StackLimitKind kind);
  void StackOverflowCheck(Register num_args, Register scratch1,
                          Register scratch2, Label* stack_overflow);

  // ---------------------------------------------------------------------------
  // Smi utilities.

  void SmiTag(Register reg) { Addu(reg, reg, reg); }

  void SmiTag(Register dst, Register src) { Addu(dst, src, src); }

  // Test if the register contains a smi.
  inline void SmiTst(Register value, Register scratch) {
    And(scratch, value, Operand(kSmiTagMask));
  }

  // Jump if the register contains a non-smi.
  void JumpIfNotSmi(Register value, Label* not_smi_label, Register scratch = at,
                    BranchDelaySlot bd = PROTECT);

  // Abort execution if argument is a smi, enabled via --debug-code.
  void AssertNotSmi(Register object);
  void AssertSmi(Register object);

  // Abort execution if argument is not a Constructor, enabled via --debug-code.
  void AssertConstructor(Register object);

  // Abort execution if argument is not a JSFunction, enabled via --debug-code.
  void AssertFunction(Register object);

  // Abort execution if argument is not a JSBoundFunction,
  // enabled via --debug-code.
  void AssertBoundFunction(Register object);

  // Abort execution if argument is not a JSGeneratorObject (or subclass),
  // enabled via --debug-code.
  void AssertGeneratorObject(Register object);

  // Abort execution if argument is not undefined or an AllocationSite, enabled
  // via --debug-code.
  void AssertUndefinedOrAllocationSite(Register object, Register scratch);

  template <typename Field>
  void DecodeField(Register dst, Register src) {
    Ext(dst, src, Field::kShift, Field::kSize);
  }

  template <typename Field>
  void DecodeField(Register reg) {
    DecodeField<Field>(reg, reg);
  }

 private:
  // Helper functions for generating invokes.
  void InvokePrologue(Register expected_parameter_count,
                      Register actual_parameter_count, Label* done,
                      InvokeType type);

  // Compute memory operands for safepoint stack slots.
  static int SafepointRegisterStackIndex(int reg_code);

  // Needs access to SafepointRegisterStackIndex for compiled frame
  // traversal.
  friend class CommonFrame;

  DISALLOW_IMPLICIT_CONSTRUCTORS(MacroAssembler);
};

template <typename Func>
void TurboAssembler::GenerateSwitchTable(Register index, size_t case_count,
                                         Func GetLabelFunction) {
  Label here;
  BlockTrampolinePoolFor(case_count + kSwitchTablePrologueSize);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  if (kArchVariant >= kMips32r6) {
    addiupc(scratch, 5);
    Lsa(scratch, scratch, index, kPointerSizeLog2);
    lw(scratch, MemOperand(scratch));
  } else {
    push(ra);
    bal(&here);
    sll(scratch, index, kPointerSizeLog2);  // Branch delay slot.
    bind(&here);
    addu(scratch, scratch, ra);
    pop(ra);
    lw(scratch, MemOperand(scratch, 6 * v8::internal::kInstrSize));
  }
  jr(scratch);
  nop();  // Branch delay slot nop.
  for (size_t index = 0; index < case_count; ++index) {
    dd(GetLabelFunction(index));
  }
}

#define ACCESS_MASM(masm) masm->

}  // namespace internal
}  // namespace v8

#endif  // V8_CODEGEN_MIPS_MACRO_ASSEMBLER_MIPS_H_

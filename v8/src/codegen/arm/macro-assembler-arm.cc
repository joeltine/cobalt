// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>  // For LONG_MIN, LONG_MAX.

#if V8_TARGET_ARCH_ARM

#include "src/base/bits.h"
#include "src/base/division-by-constant.h"
#include "src/base/numbers/double.h"
#include "src/base/utils/random-number-generator.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/callable.h"
#include "src/codegen/code-factory.h"
#include "src/codegen/external-reference-table.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/register-configuration.h"
#include "src/codegen/register.h"
#include "src/debug/debug.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/frames-inl.h"
#include "src/heap/memory-chunk.h"
#include "src/init/bootstrapper.h"
#include "src/logging/counters.h"
#include "src/objects/objects-inl.h"
#include "src/runtime/runtime.h"
#include "src/snapshot/snapshot.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-code-manager.h"
#endif  // V8_ENABLE_WEBASSEMBLY

// Satisfy cpplint check, but don't include platform-specific header. It is
// included recursively via macro-assembler.h.
#if 0
#include "src/codegen/arm/macro-assembler-arm.h"
#endif

#define __ ACCESS_MASM(masm)

namespace v8 {
namespace internal {

int MacroAssembler::RequiredStackSizeForCallerSaved(SaveFPRegsMode fp_mode,
                                                    Register exclusion1,
                                                    Register exclusion2,
                                                    Register exclusion3) const {
  int bytes = 0;
  RegList exclusions = {exclusion1, exclusion2, exclusion3};
  RegList list = (kCallerSaved | lr) - exclusions;

  bytes += list.Count() * kPointerSize;

  if (fp_mode == SaveFPRegsMode::kSave) {
    bytes += DwVfpRegister::kNumRegisters * DwVfpRegister::kSizeInBytes;
  }

  return bytes;
}

int MacroAssembler::PushCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1,
                                    Register exclusion2, Register exclusion3) {
  ASM_CODE_COMMENT(this);
  int bytes = 0;
  RegList exclusions = {exclusion1, exclusion2, exclusion3};
  RegList list = (kCallerSaved | lr) - exclusions;
  stm(db_w, sp, list);

  bytes += list.Count() * kPointerSize;

  if (fp_mode == SaveFPRegsMode::kSave) {
    SaveFPRegs(sp, lr);
    bytes += DwVfpRegister::kNumRegisters * DwVfpRegister::kSizeInBytes;
  }

  return bytes;
}

int MacroAssembler::PopCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1,
                                   Register exclusion2, Register exclusion3) {
  ASM_CODE_COMMENT(this);
  int bytes = 0;
  if (fp_mode == SaveFPRegsMode::kSave) {
    RestoreFPRegs(sp, lr);
    bytes += DwVfpRegister::kNumRegisters * DwVfpRegister::kSizeInBytes;
  }

  RegList exclusions = {exclusion1, exclusion2, exclusion3};
  RegList list = (kCallerSaved | lr) - exclusions;
  ldm(ia_w, sp, list);

  bytes += list.Count() * kPointerSize;

  return bytes;
}

void MacroAssembler::LoadFromConstantsTable(Register destination,
                                            int constant_index) {
  DCHECK(RootsTable::IsImmortalImmovable(RootIndex::kBuiltinsConstantsTable));

  const uint32_t offset =
      FixedArray::kHeaderSize + constant_index * kPointerSize - kHeapObjectTag;

  LoadRoot(destination, RootIndex::kBuiltinsConstantsTable);
  ldr(destination, MemOperand(destination, offset));
}

void MacroAssembler::LoadRootRelative(Register destination, int32_t offset) {
  ldr(destination, MemOperand(kRootRegister, offset));
}

void MacroAssembler::LoadRootRegisterOffset(Register destination,
                                            intptr_t offset) {
  if (offset == 0) {
    Move(destination, kRootRegister);
  } else {
    add(destination, kRootRegister, Operand(offset));
  }
}

MemOperand MacroAssembler::ExternalReferenceAsOperand(
    ExternalReference reference, Register scratch) {
  if (root_array_available_ && options().enable_root_relative_access) {
    int64_t offset =
        RootRegisterOffsetForExternalReference(isolate(), reference);
    if (is_int32(offset)) {
      return MemOperand(kRootRegister, static_cast<int32_t>(offset));
    }
  }
  if (root_array_available_ && options().isolate_independent_code) {
    if (IsAddressableThroughRootRegister(isolate(), reference)) {
      // Some external references can be efficiently loaded as an offset from
      // kRootRegister.
      intptr_t offset =
          RootRegisterOffsetForExternalReference(isolate(), reference);
      CHECK(is_int32(offset));
      return MemOperand(kRootRegister, static_cast<int32_t>(offset));
    } else {
      // Otherwise, do a memory load from the external reference table.
      ldr(scratch, MemOperand(kRootRegister,
                              RootRegisterOffsetForExternalReferenceTableEntry(
                                  isolate(), reference)));
      return MemOperand(scratch, 0);
    }
  }
  Move(scratch, reference);
  return MemOperand(scratch, 0);
}

void MacroAssembler::Jump(Register target, Condition cond) { bx(target, cond); }

void MacroAssembler::Jump(intptr_t target, RelocInfo::Mode rmode,
                          Condition cond) {
  mov(pc, Operand(target, rmode), LeaveCC, cond);
}

void MacroAssembler::Jump(Address target, RelocInfo::Mode rmode,
                          Condition cond) {
  DCHECK(!RelocInfo::IsCodeTarget(rmode));
  Jump(static_cast<intptr_t>(target), rmode, cond);
}

void MacroAssembler::Jump(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond) {
  DCHECK(RelocInfo::IsCodeTarget(rmode));
  DCHECK_IMPLIES(options().isolate_independent_code,
                 Builtins::IsIsolateIndependentBuiltin(*code));

  Builtin builtin = Builtin::kNoBuiltinId;
  if (isolate()->builtins()->IsBuiltinHandle(code, &builtin)) {
    TailCallBuiltin(builtin, cond);
    return;
  }

  // 'code' is always generated ARM code, never THUMB code
  Jump(static_cast<intptr_t>(code.address()), rmode, cond);
}

void MacroAssembler::Jump(const ExternalReference& reference) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Move(scratch, reference);
  Jump(scratch);
}

void MacroAssembler::Call(Register target, Condition cond) {
  // Block constant pool for the call instruction sequence.
  BlockConstPoolScope block_const_pool(this);
  blx(target, cond);
}

void MacroAssembler::Call(Address target, RelocInfo::Mode rmode, Condition cond,
                          TargetAddressStorageMode mode,
                          bool check_constant_pool) {
  // Check if we have to emit the constant pool before we block it.
  if (check_constant_pool) MaybeCheckConstPool();
  // Block constant pool for the call instruction sequence.
  BlockConstPoolScope block_const_pool(this);

  bool old_predictable_code_size = predictable_code_size();
  if (mode == NEVER_INLINE_TARGET_ADDRESS) {
    set_predictable_code_size(true);
  }

  // Use ip directly instead of using UseScratchRegisterScope, as we do not
  // preserve scratch registers across calls.

  // Call sequence on V7 or later may be :
  //  movw  ip, #... @ call address low 16
  //  movt  ip, #... @ call address high 16
  //  blx   ip
  //                      @ return address
  // Or for pre-V7 or values that may be back-patched
  // to avoid ICache flushes:
  //  ldr   ip, [pc, #...] @ call address
  //  blx   ip
  //                      @ return address

  mov(ip, Operand(target, rmode));
  blx(ip, cond);

  if (mode == NEVER_INLINE_TARGET_ADDRESS) {
    set_predictable_code_size(old_predictable_code_size);
  }
}

void MacroAssembler::Call(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond, TargetAddressStorageMode mode,
                          bool check_constant_pool) {
  DCHECK(RelocInfo::IsCodeTarget(rmode));
  DCHECK_IMPLIES(options().isolate_independent_code,
                 Builtins::IsIsolateIndependentBuiltin(*code));

  Builtin builtin = Builtin::kNoBuiltinId;
  if (isolate()->builtins()->IsBuiltinHandle(code, &builtin)) {
    CallBuiltin(builtin);
    return;
  }

  // 'code' is always generated ARM code, never THUMB code
  Call(code.address(), rmode, cond, mode);
}

void MacroAssembler::LoadEntryFromBuiltinIndex(Register builtin_index) {
  ASM_CODE_COMMENT(this);
  static_assert(kSystemPointerSize == 4);
  static_assert(kSmiShiftSize == 0);
  static_assert(kSmiTagSize == 1);
  static_assert(kSmiTag == 0);

  // The builtin_index register contains the builtin index as a Smi.
  // Untagging is folded into the indexing operand below.
  mov(builtin_index,
      Operand(builtin_index, LSL, kSystemPointerSizeLog2 - kSmiTagSize));
  add(builtin_index, builtin_index,
      Operand(IsolateData::builtin_entry_table_offset()));
  ldr(builtin_index, MemOperand(kRootRegister, builtin_index));
}

void MacroAssembler::CallBuiltinByIndex(Register builtin_index) {
  LoadEntryFromBuiltinIndex(builtin_index);
  Call(builtin_index);
}

void MacroAssembler::LoadEntryFromBuiltin(Builtin builtin,
                                          Register destination) {
  ASM_CODE_COMMENT(this);
  ldr(destination, EntryFromBuiltinAsOperand(builtin));
}

MemOperand MacroAssembler::EntryFromBuiltinAsOperand(Builtin builtin) {
  ASM_CODE_COMMENT(this);
  DCHECK(root_array_available());
  return MemOperand(kRootRegister,
                    IsolateData::BuiltinEntrySlotOffset(builtin));
}

void MacroAssembler::CallBuiltin(Builtin builtin, Condition cond) {
  ASM_CODE_COMMENT_STRING(this, CommentForOffHeapTrampoline("call", builtin));
  // Use ip directly instead of using UseScratchRegisterScope, as we do not
  // preserve scratch registers across calls.
  switch (options().builtin_call_jump_mode) {
    case BuiltinCallJumpMode::kAbsolute: {
      mov(ip, Operand(BuiltinEntry(builtin), RelocInfo::OFF_HEAP_TARGET));
      Call(ip, cond);
      break;
    }
    case BuiltinCallJumpMode::kPCRelative:
      UNREACHABLE();
    case BuiltinCallJumpMode::kIndirect:
      ldr(ip, EntryFromBuiltinAsOperand(builtin));
      Call(ip, cond);
      break;
    case BuiltinCallJumpMode::kForMksnapshot: {
      if (options().use_pc_relative_calls_and_jumps_for_mksnapshot) {
        Handle<Code> code = isolate()->builtins()->code_handle(builtin);
        int32_t code_target_index = AddCodeTarget(code);
        bl(code_target_index * kInstrSize, cond,
           RelocInfo::RELATIVE_CODE_TARGET);
      } else {
        ldr(ip, EntryFromBuiltinAsOperand(builtin));
        Call(ip, cond);
      }
      break;
    }
  }
}

void MacroAssembler::TailCallBuiltin(Builtin builtin, Condition cond) {
  ASM_CODE_COMMENT_STRING(this,
                          CommentForOffHeapTrampoline("tail call", builtin));
  // Use ip directly instead of using UseScratchRegisterScope, as we do not
  // preserve scratch registers across calls.
  switch (options().builtin_call_jump_mode) {
    case BuiltinCallJumpMode::kAbsolute: {
      mov(ip, Operand(BuiltinEntry(builtin), RelocInfo::OFF_HEAP_TARGET));
      Jump(ip, cond);
      break;
    }
    case BuiltinCallJumpMode::kPCRelative:
      UNREACHABLE();
    case BuiltinCallJumpMode::kIndirect:
      ldr(ip, EntryFromBuiltinAsOperand(builtin));
      Jump(ip, cond);
      break;
    case BuiltinCallJumpMode::kForMksnapshot: {
      if (options().use_pc_relative_calls_and_jumps_for_mksnapshot) {
        Handle<Code> code = isolate()->builtins()->code_handle(builtin);
        int32_t code_target_index = AddCodeTarget(code);
        b(code_target_index * kInstrSize, cond,
          RelocInfo::RELATIVE_CODE_TARGET);
      } else {
        ldr(ip, EntryFromBuiltinAsOperand(builtin));
        Jump(ip, cond);
      }
      break;
    }
  }
}

void MacroAssembler::LoadCodeInstructionStart(Register destination,
                                              Register code_object) {
  ASM_CODE_COMMENT(this);
  ldr(destination, FieldMemOperand(code_object, Code::kInstructionStartOffset));
}

void MacroAssembler::CallCodeObject(Register code_object) {
  ASM_CODE_COMMENT(this);
  LoadCodeInstructionStart(code_object, code_object);
  Call(code_object);
}

void MacroAssembler::JumpCodeObject(Register code_object, JumpMode jump_mode) {
  ASM_CODE_COMMENT(this);
  DCHECK_EQ(JumpMode::kJump, jump_mode);
  LoadCodeInstructionStart(code_object, code_object);
  Jump(code_object);
}

void MacroAssembler::StoreReturnAddressAndCall(Register target) {
  ASM_CODE_COMMENT(this);
  // This generates the final instruction sequence for calls to C functions
  // once an exit frame has been constructed.
  //
  // Note that this assumes the caller code (i.e. the InstructionStream object
  // currently being generated) is immovable or that the callee function cannot
  // trigger GC, since the callee function will return to it.

  // Compute the return address in lr to return to after the jump below. The pc
  // is already at '+ 8' from the current instruction; but return is after three
  // instructions, so add another 4 to pc to get the return address.
  Assembler::BlockConstPoolScope block_const_pool(this);
  add(lr, pc, Operand(4));
  str(lr, MemOperand(sp));
  Call(target);
}

void MacroAssembler::Ret(Condition cond) { bx(lr, cond); }

void MacroAssembler::Drop(int count, Condition cond) {
  if (count > 0) {
    add(sp, sp, Operand(count * kPointerSize), LeaveCC, cond);
  }
}

void MacroAssembler::Drop(Register count, Condition cond) {
  add(sp, sp, Operand(count, LSL, kPointerSizeLog2), LeaveCC, cond);
}

void MacroAssembler::TestCodeIsMarkedForDeoptimization(Register code,
                                                       Register scratch) {
  ldr(scratch, FieldMemOperand(code, Code::kFlagsOffset));
  tst(scratch, Operand(1 << Code::kMarkedForDeoptimizationBit));
}

Operand MacroAssembler::ClearedValue() const {
  return Operand(
      static_cast<int32_t>(HeapObjectReference::ClearedValue(isolate()).ptr()));
}

void MacroAssembler::Call(Label* target) { bl(target); }

void MacroAssembler::Push(Handle<HeapObject> handle) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  mov(scratch, Operand(handle));
  push(scratch);
}

void MacroAssembler::Push(Smi smi) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  mov(scratch, Operand(smi));
  push(scratch);
}

void MacroAssembler::PushArray(Register array, Register size, Register scratch,
                               PushArrayOrder order) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  Register counter = scratch;
  Register tmp = temps.Acquire();
  DCHECK(!AreAliased(array, size, counter, tmp));
  Label loop, entry;
  if (order == PushArrayOrder::kReverse) {
    mov(counter, Operand(0));
    b(&entry);
    bind(&loop);
    ldr(tmp, MemOperand(array, counter, LSL, kSystemPointerSizeLog2));
    push(tmp);
    add(counter, counter, Operand(1));
    bind(&entry);
    cmp(counter, size);
    b(lt, &loop);
  } else {
    mov(counter, size);
    b(&entry);
    bind(&loop);
    ldr(tmp, MemOperand(array, counter, LSL, kSystemPointerSizeLog2));
    push(tmp);
    bind(&entry);
    sub(counter, counter, Operand(1), SetCC);
    b(ge, &loop);
  }
}

void MacroAssembler::Move(Register dst, Smi smi) { mov(dst, Operand(smi)); }

void MacroAssembler::Move(Register dst, Handle<HeapObject> value) {
  // TODO(jgruber,v8:8887): Also consider a root-relative load when generating
  // non-isolate-independent code. In many cases it might be cheaper than
  // embedding the relocatable value.
  if (root_array_available_ && options().isolate_independent_code) {
    IndirectLoadConstant(dst, value);
    return;
  }
  mov(dst, Operand(value));
}

void MacroAssembler::Move(Register dst, ExternalReference reference) {
  // TODO(jgruber,v8:8887): Also consider a root-relative load when generating
  // non-isolate-independent code. In many cases it might be cheaper than
  // embedding the relocatable value.
  if (root_array_available_ && options().isolate_independent_code) {
    IndirectLoadExternalReference(dst, reference);
    return;
  }
  mov(dst, Operand(reference));
}

void MacroAssembler::Move(Register dst, Register src, Condition cond) {
  if (dst != src) {
    mov(dst, src, LeaveCC, cond);
  }
}

void MacroAssembler::Move(SwVfpRegister dst, SwVfpRegister src,
                          Condition cond) {
  if (dst != src) {
    vmov(dst, src, cond);
  }
}

void MacroAssembler::Move(DwVfpRegister dst, DwVfpRegister src,
                          Condition cond) {
  if (dst != src) {
    vmov(dst, src, cond);
  }
}

void MacroAssembler::Move(QwNeonRegister dst, QwNeonRegister src) {
  if (dst != src) {
    vmov(dst, src);
  }
}

void MacroAssembler::MovePair(Register dst0, Register src0, Register dst1,
                              Register src1) {
  DCHECK_NE(dst0, dst1);
  if (dst0 != src1) {
    Move(dst0, src0);
    Move(dst1, src1);
  } else if (dst1 != src0) {
    // Swap the order of the moves to resolve the overlap.
    Move(dst1, src1);
    Move(dst0, src0);
  } else {
    // Worse case scenario, this is a swap.
    Swap(dst0, src0);
  }
}

void MacroAssembler::Swap(Register srcdst0, Register srcdst1) {
  DCHECK(srcdst0 != srcdst1);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  mov(scratch, srcdst0);
  mov(srcdst0, srcdst1);
  mov(srcdst1, scratch);
}

void MacroAssembler::Swap(DwVfpRegister srcdst0, DwVfpRegister srcdst1) {
  DCHECK(srcdst0 != srcdst1);
  DCHECK(VfpRegisterIsAvailable(srcdst0));
  DCHECK(VfpRegisterIsAvailable(srcdst1));

  if (CpuFeatures::IsSupported(NEON)) {
    vswp(srcdst0, srcdst1);
  } else {
    UseScratchRegisterScope temps(this);
    DwVfpRegister scratch = temps.AcquireD();
    vmov(scratch, srcdst0);
    vmov(srcdst0, srcdst1);
    vmov(srcdst1, scratch);
  }
}

void MacroAssembler::Swap(QwNeonRegister srcdst0, QwNeonRegister srcdst1) {
  DCHECK(srcdst0 != srcdst1);
  vswp(srcdst0, srcdst1);
}

void MacroAssembler::Mls(Register dst, Register src1, Register src2,
                         Register srcA, Condition cond) {
  if (CpuFeatures::IsSupported(ARMv7)) {
    CpuFeatureScope scope(this, ARMv7);
    mls(dst, src1, src2, srcA, cond);
  } else {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(srcA != scratch);
    mul(scratch, src1, src2, LeaveCC, cond);
    sub(dst, srcA, scratch, LeaveCC, cond);
  }
}

void MacroAssembler::And(Register dst, Register src1, const Operand& src2,
                         Condition cond) {
  if (!src2.IsRegister() && !src2.MustOutputRelocInfo(this) &&
      src2.immediate() == 0) {
    mov(dst, Operand::Zero(), LeaveCC, cond);
  } else if (!(src2.InstructionsRequired(this) == 1) &&
             !src2.MustOutputRelocInfo(this) &&
             CpuFeatures::IsSupported(ARMv7) &&
             base::bits::IsPowerOfTwo(src2.immediate() + 1)) {
    CpuFeatureScope scope(this, ARMv7);
    ubfx(dst, src1, 0,
         base::bits::WhichPowerOfTwo(static_cast<uint32_t>(src2.immediate()) +
                                     1),
         cond);
  } else {
    and_(dst, src1, src2, LeaveCC, cond);
  }
}

void MacroAssembler::Ubfx(Register dst, Register src1, int lsb, int width,
                          Condition cond) {
  DCHECK_LT(lsb, 32);
  if (!CpuFeatures::IsSupported(ARMv7) || predictable_code_size()) {
    int mask = (1u << (width + lsb)) - 1u - ((1u << lsb) - 1u);
    and_(dst, src1, Operand(mask), LeaveCC, cond);
    if (lsb != 0) {
      mov(dst, Operand(dst, LSR, lsb), LeaveCC, cond);
    }
  } else {
    CpuFeatureScope scope(this, ARMv7);
    ubfx(dst, src1, lsb, width, cond);
  }
}

void MacroAssembler::Sbfx(Register dst, Register src1, int lsb, int width,
                          Condition cond) {
  DCHECK_LT(lsb, 32);
  if (!CpuFeatures::IsSupported(ARMv7) || predictable_code_size()) {
    int mask = (1 << (width + lsb)) - 1 - ((1 << lsb) - 1);
    and_(dst, src1, Operand(mask), LeaveCC, cond);
    int shift_up = 32 - lsb - width;
    int shift_down = lsb + shift_up;
    if (shift_up != 0) {
      mov(dst, Operand(dst, LSL, shift_up), LeaveCC, cond);
    }
    if (shift_down != 0) {
      mov(dst, Operand(dst, ASR, shift_down), LeaveCC, cond);
    }
  } else {
    CpuFeatureScope scope(this, ARMv7);
    sbfx(dst, src1, lsb, width, cond);
  }
}

void MacroAssembler::Bfc(Register dst, Register src, int lsb, int width,
                         Condition cond) {
  DCHECK_LT(lsb, 32);
  if (!CpuFeatures::IsSupported(ARMv7) || predictable_code_size()) {
    int mask = (1 << (width + lsb)) - 1 - ((1 << lsb) - 1);
    bic(dst, src, Operand(mask));
  } else {
    CpuFeatureScope scope(this, ARMv7);
    Move(dst, src, cond);
    bfc(dst, lsb, width, cond);
  }
}

void MacroAssembler::LoadRoot(Register destination, RootIndex index,
                              Condition cond) {
  ldr(destination,
      MemOperand(kRootRegister, RootRegisterOffsetForRootIndex(index)), cond);
}

void MacroAssembler::RecordWriteField(Register object, int offset,
                                      Register value,
                                      LinkRegisterStatus lr_status,
                                      SaveFPRegsMode save_fp,
                                      SmiCheck smi_check) {
  ASM_CODE_COMMENT(this);
  // First, check if a write barrier is even needed. The tests below
  // catch stores of Smis.
  Label done;

  // Skip barrier if writing a smi.
  if (smi_check == SmiCheck::kInline) {
    JumpIfSmi(value, &done);
  }

  // Although the object register is tagged, the offset is relative to the start
  // of the object, so so offset must be a multiple of kPointerSize.
  DCHECK(IsAligned(offset, kPointerSize));

  if (v8_flags.debug_code) {
    ASM_CODE_COMMENT_STRING(this, "Verify slot_address");
    Label ok;
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(!AreAliased(object, value, scratch));
    add(scratch, object, Operand(offset - kHeapObjectTag));
    tst(scratch, Operand(kPointerSize - 1));
    b(eq, &ok);
    stop();
    bind(&ok);
  }

  RecordWrite(object, Operand(offset - kHeapObjectTag), value, lr_status,
              save_fp, SmiCheck::kOmit);

  bind(&done);
}

void MacroAssembler::MaybeSaveRegisters(RegList registers) {
  if (registers.is_empty()) return;
  ASM_CODE_COMMENT(this);
  stm(db_w, sp, registers);
}

void MacroAssembler::MaybeRestoreRegisters(RegList registers) {
  if (registers.is_empty()) return;
  ASM_CODE_COMMENT(this);
  ldm(ia_w, sp, registers);
}

void MacroAssembler::CallEphemeronKeyBarrier(Register object, Operand offset,
                                             SaveFPRegsMode fp_mode) {
  ASM_CODE_COMMENT(this);
  RegList registers = WriteBarrierDescriptor::ComputeSavedRegisters(object);
  MaybeSaveRegisters(registers);

  Register object_parameter = WriteBarrierDescriptor::ObjectRegister();
  Register slot_address_parameter =
      WriteBarrierDescriptor::SlotAddressRegister();
  MoveObjectAndSlot(object_parameter, slot_address_parameter, object, offset);

  Call(isolate()->builtins()->code_handle(
           Builtins::GetEphemeronKeyBarrierStub(fp_mode)),
       RelocInfo::CODE_TARGET);
  MaybeRestoreRegisters(registers);
}

void MacroAssembler::CallRecordWriteStubSaveRegisters(Register object,
                                                      Operand offset,
                                                      SaveFPRegsMode fp_mode,
                                                      StubCallMode mode) {
  ASM_CODE_COMMENT(this);
  RegList registers = WriteBarrierDescriptor::ComputeSavedRegisters(object);
  MaybeSaveRegisters(registers);

  Register object_parameter = WriteBarrierDescriptor::ObjectRegister();
  Register slot_address_parameter =
      WriteBarrierDescriptor::SlotAddressRegister();
  MoveObjectAndSlot(object_parameter, slot_address_parameter, object, offset);

  CallRecordWriteStub(object_parameter, slot_address_parameter, fp_mode, mode);

  MaybeRestoreRegisters(registers);
}

void MacroAssembler::CallRecordWriteStub(Register object, Register slot_address,
                                         SaveFPRegsMode fp_mode,
                                         StubCallMode mode) {
  ASM_CODE_COMMENT(this);
  DCHECK_EQ(WriteBarrierDescriptor::ObjectRegister(), object);
  DCHECK_EQ(WriteBarrierDescriptor::SlotAddressRegister(), slot_address);
#if V8_ENABLE_WEBASSEMBLY
  if (mode == StubCallMode::kCallWasmRuntimeStub) {
    auto wasm_target = wasm::WasmCode::GetRecordWriteStub(fp_mode);
    Call(wasm_target, RelocInfo::WASM_STUB_CALL);
#else
  if (false) {
#endif
  } else {
    Builtin builtin = Builtins::GetRecordWriteStub(fp_mode);
    CallBuiltin(builtin);
  }
}

void MacroAssembler::MoveObjectAndSlot(Register dst_object, Register dst_slot,
                                       Register object, Operand offset) {
  DCHECK_NE(dst_object, dst_slot);
  DCHECK(offset.IsRegister() || offset.IsImmediate());
  // If `offset` is a register, it cannot overlap with `object`.
  DCHECK_IMPLIES(offset.IsRegister(), offset.rm() != object);

  // If the slot register does not overlap with the object register, we can
  // overwrite it.
  if (dst_slot != object) {
    add(dst_slot, object, offset);
    Move(dst_object, object);
    return;
  }

  DCHECK_EQ(dst_slot, object);

  // If the destination object register does not overlap with the offset
  // register, we can overwrite it.
  if (!offset.IsRegister() || (offset.rm() != dst_object)) {
    Move(dst_object, dst_slot);
    add(dst_slot, dst_slot, offset);
    return;
  }

  DCHECK_EQ(dst_object, offset.rm());

  // We only have `dst_slot` and `dst_object` left as distinct registers so we
  // have to swap them. We write this as a add+sub sequence to avoid using a
  // scratch register.
  add(dst_slot, dst_slot, dst_object);
  sub(dst_object, dst_slot, dst_object);
}

// The register 'object' contains a heap object pointer. The heap object tag is
// shifted away. A scratch register also needs to be available.
void MacroAssembler::RecordWrite(Register object, Operand offset,
                                 Register value, LinkRegisterStatus lr_status,
                                 SaveFPRegsMode fp_mode, SmiCheck smi_check) {
  DCHECK(!AreAliased(object, value));
  if (v8_flags.debug_code) {
    ASM_CODE_COMMENT_STRING(this, "Verify slot_address");
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(!AreAliased(object, value, scratch));
    add(scratch, object, offset);
    ldr(scratch, MemOperand(scratch));
    cmp(scratch, value);
    Check(eq, AbortReason::kWrongAddressOrValuePassedToRecordWrite);
  }

  if (v8_flags.disable_write_barriers) {
    return;
  }

  // First, check if a write barrier is even needed. The tests below
  // catch stores of smis and stores into the young generation.
  Label done;

  if (smi_check == SmiCheck::kInline) {
    JumpIfSmi(value, &done);
  }

  CheckPageFlag(value, MemoryChunk::kPointersToHereAreInterestingMask, eq,
                &done);
  CheckPageFlag(object, MemoryChunk::kPointersFromHereAreInterestingMask, eq,
                &done);

  // Record the actual write.
  if (lr_status == kLRHasNotBeenSaved) {
    push(lr);
  }

  Register slot_address = WriteBarrierDescriptor::SlotAddressRegister();
  DCHECK(!AreAliased(object, value, slot_address));
  DCHECK(!offset.IsRegister());
  add(slot_address, object, offset);
  CallRecordWriteStub(object, slot_address, fp_mode);
  if (lr_status == kLRHasNotBeenSaved) {
    pop(lr);
  }

  if (v8_flags.debug_code) Move(slot_address, Operand(kZapValue));

  bind(&done);
}

void MacroAssembler::PushCommonFrame(Register marker_reg) {
  ASM_CODE_COMMENT(this);
  if (marker_reg.is_valid()) {
    if (marker_reg.code() > fp.code()) {
      stm(db_w, sp, {fp, lr});
      mov(fp, Operand(sp));
      Push(marker_reg);
    } else {
      stm(db_w, sp, {marker_reg, fp, lr});
      add(fp, sp, Operand(kPointerSize));
    }
  } else {
    stm(db_w, sp, {fp, lr});
    mov(fp, sp);
  }
}

void MacroAssembler::PushStandardFrame(Register function_reg) {
  ASM_CODE_COMMENT(this);
  DCHECK(!function_reg.is_valid() || function_reg.code() < cp.code());
  stm(db_w, sp, {function_reg, cp, fp, lr});
  int offset = -StandardFrameConstants::kContextOffset;
  offset += function_reg.is_valid() ? kPointerSize : 0;
  add(fp, sp, Operand(offset));
  Push(kJavaScriptCallArgCountRegister);
}

void MacroAssembler::VFPCanonicalizeNaN(const DwVfpRegister dst,
                                        const DwVfpRegister src,
                                        const Condition cond) {
  // Subtracting 0.0 preserves all inputs except for signalling NaNs, which
  // become quiet NaNs. We use vsub rather than vadd because vsub preserves -0.0
  // inputs: -0.0 + 0.0 = 0.0, but -0.0 - 0.0 = -0.0.
  vsub(dst, src, kDoubleRegZero, cond);
}

void MacroAssembler::VFPCompareAndSetFlags(const SwVfpRegister src1,
                                           const SwVfpRegister src2,
                                           const Condition cond) {
  // Compare and move FPSCR flags to the normal condition flags.
  VFPCompareAndLoadFlags(src1, src2, pc, cond);
}

void MacroAssembler::VFPCompareAndSetFlags(const SwVfpRegister src1,
                                           const float src2,
                                           const Condition cond) {
  // Compare and move FPSCR flags to the normal condition flags.
  VFPCompareAndLoadFlags(src1, src2, pc, cond);
}

void MacroAssembler::VFPCompareAndSetFlags(const DwVfpRegister src1,
                                           const DwVfpRegister src2,
                                           const Condition cond) {
  // Compare and move FPSCR flags to the normal condition flags.
  VFPCompareAndLoadFlags(src1, src2, pc, cond);
}

void MacroAssembler::VFPCompareAndSetFlags(const DwVfpRegister src1,
                                           const double src2,
                                           const Condition cond) {
  // Compare and move FPSCR flags to the normal condition flags.
  VFPCompareAndLoadFlags(src1, src2, pc, cond);
}

void MacroAssembler::VFPCompareAndLoadFlags(const SwVfpRegister src1,
                                            const SwVfpRegister src2,
                                            const Register fpscr_flags,
                                            const Condition cond) {
  // Compare and load FPSCR.
  vcmp(src1, src2, cond);
  vmrs(fpscr_flags, cond);
}

void MacroAssembler::VFPCompareAndLoadFlags(const SwVfpRegister src1,
                                            const float src2,
                                            const Register fpscr_flags,
                                            const Condition cond) {
  // Compare and load FPSCR.
  vcmp(src1, src2, cond);
  vmrs(fpscr_flags, cond);
}

void MacroAssembler::VFPCompareAndLoadFlags(const DwVfpRegister src1,
                                            const DwVfpRegister src2,
                                            const Register fpscr_flags,
                                            const Condition cond) {
  // Compare and load FPSCR.
  vcmp(src1, src2, cond);
  vmrs(fpscr_flags, cond);
}

void MacroAssembler::VFPCompareAndLoadFlags(const DwVfpRegister src1,
                                            const double src2,
                                            const Register fpscr_flags,
                                            const Condition cond) {
  // Compare and load FPSCR.
  vcmp(src1, src2, cond);
  vmrs(fpscr_flags, cond);
}

void MacroAssembler::VmovHigh(Register dst, DwVfpRegister src) {
  if (src.code() < 16) {
    const LowDwVfpRegister loc = LowDwVfpRegister::from_code(src.code());
    vmov(dst, loc.high());
  } else {
    vmov(NeonS32, dst, src, 1);
  }
}

void MacroAssembler::VmovHigh(DwVfpRegister dst, Register src) {
  if (dst.code() < 16) {
    const LowDwVfpRegister loc = LowDwVfpRegister::from_code(dst.code());
    vmov(loc.high(), src);
  } else {
    vmov(NeonS32, dst, 1, src);
  }
}

void MacroAssembler::VmovLow(Register dst, DwVfpRegister src) {
  if (src.code() < 16) {
    const LowDwVfpRegister loc = LowDwVfpRegister::from_code(src.code());
    vmov(dst, loc.low());
  } else {
    vmov(NeonS32, dst, src, 0);
  }
}

void MacroAssembler::VmovLow(DwVfpRegister dst, Register src) {
  if (dst.code() < 16) {
    const LowDwVfpRegister loc = LowDwVfpRegister::from_code(dst.code());
    vmov(loc.low(), src);
  } else {
    vmov(NeonS32, dst, 0, src);
  }
}

void MacroAssembler::VmovExtended(Register dst, int src_code) {
  DCHECK_LE(SwVfpRegister::kNumRegisters, src_code);
  DCHECK_GT(SwVfpRegister::kNumRegisters * 2, src_code);
  if (src_code & 0x1) {
    VmovHigh(dst, DwVfpRegister::from_code(src_code / 2));
  } else {
    VmovLow(dst, DwVfpRegister::from_code(src_code / 2));
  }
}

void MacroAssembler::VmovExtended(int dst_code, Register src) {
  DCHECK_LE(SwVfpRegister::kNumRegisters, dst_code);
  DCHECK_GT(SwVfpRegister::kNumRegisters * 2, dst_code);
  if (dst_code & 0x1) {
    VmovHigh(DwVfpRegister::from_code(dst_code / 2), src);
  } else {
    VmovLow(DwVfpRegister::from_code(dst_code / 2), src);
  }
}

void MacroAssembler::VmovExtended(int dst_code, int src_code) {
  if (src_code == dst_code) return;

  if (src_code < SwVfpRegister::kNumRegisters &&
      dst_code < SwVfpRegister::kNumRegisters) {
    // src and dst are both s-registers.
    vmov(SwVfpRegister::from_code(dst_code),
         SwVfpRegister::from_code(src_code));
    return;
  }
  DwVfpRegister dst_d_reg = DwVfpRegister::from_code(dst_code / 2);
  DwVfpRegister src_d_reg = DwVfpRegister::from_code(src_code / 2);
  int dst_offset = dst_code & 1;
  int src_offset = src_code & 1;
  if (CpuFeatures::IsSupported(NEON)) {
    UseScratchRegisterScope temps(this);
    DwVfpRegister scratch = temps.AcquireD();
    // On Neon we can shift and insert from d-registers.
    if (src_offset == dst_offset) {
      // Offsets are the same, use vdup to copy the source to the opposite lane.
      vdup(Neon32, scratch, src_d_reg, src_offset);
      // Here we are extending the lifetime of scratch.
      src_d_reg = scratch;
      src_offset = dst_offset ^ 1;
    }
    if (dst_offset) {
      if (dst_d_reg == src_d_reg) {
        vdup(Neon32, dst_d_reg, src_d_reg, 0);
      } else {
        vsli(Neon64, dst_d_reg, src_d_reg, 32);
      }
    } else {
      if (dst_d_reg == src_d_reg) {
        vdup(Neon32, dst_d_reg, src_d_reg, 1);
      } else {
        vsri(Neon64, dst_d_reg, src_d_reg, 32);
      }
    }
    return;
  }

  // Without Neon, use the scratch registers to move src and/or dst into
  // s-registers.
  UseScratchRegisterScope temps(this);
  LowDwVfpRegister d_scratch = temps.AcquireLowD();
  LowDwVfpRegister d_scratch2 = temps.AcquireLowD();
  int s_scratch_code = d_scratch.low().code();
  int s_scratch_code2 = d_scratch2.low().code();
  if (src_code < SwVfpRegister::kNumRegisters) {
    // src is an s-register, dst is not.
    vmov(d_scratch, dst_d_reg);
    vmov(SwVfpRegister::from_code(s_scratch_code + dst_offset),
         SwVfpRegister::from_code(src_code));
    vmov(dst_d_reg, d_scratch);
  } else if (dst_code < SwVfpRegister::kNumRegisters) {
    // dst is an s-register, src is not.
    vmov(d_scratch, src_d_reg);
    vmov(SwVfpRegister::from_code(dst_code),
         SwVfpRegister::from_code(s_scratch_code + src_offset));
  } else {
    // Neither src or dst are s-registers. Both scratch double registers are
    // available when there are 32 VFP registers.
    vmov(d_scratch, src_d_reg);
    vmov(d_scratch2, dst_d_reg);
    vmov(SwVfpRegister::from_code(s_scratch_code + dst_offset),
         SwVfpRegister::from_code(s_scratch_code2 + src_offset));
    vmov(dst_d_reg, d_scratch2);
  }
}

void MacroAssembler::VmovExtended(int dst_code, const MemOperand& src) {
  if (dst_code < SwVfpRegister::kNumRegisters) {
    vldr(SwVfpRegister::from_code(dst_code), src);
  } else {
    UseScratchRegisterScope temps(this);
    LowDwVfpRegister scratch = temps.AcquireLowD();
    // TODO(bbudge) If Neon supported, use load single lane form of vld1.
    int dst_s_code = scratch.low().code() + (dst_code & 1);
    vmov(scratch, DwVfpRegister::from_code(dst_code / 2));
    vldr(SwVfpRegister::from_code(dst_s_code), src);
    vmov(DwVfpRegister::from_code(dst_code / 2), scratch);
  }
}

void MacroAssembler::VmovExtended(const MemOperand& dst, int src_code) {
  if (src_code < SwVfpRegister::kNumRegisters) {
    vstr(SwVfpRegister::from_code(src_code), dst);
  } else {
    // TODO(bbudge) If Neon supported, use store single lane form of vst1.
    UseScratchRegisterScope temps(this);
    LowDwVfpRegister scratch = temps.AcquireLowD();
    int src_s_code = scratch.low().code() + (src_code & 1);
    vmov(scratch, DwVfpRegister::from_code(src_code / 2));
    vstr(SwVfpRegister::from_code(src_s_code), dst);
  }
}

void MacroAssembler::ExtractLane(Register dst, QwNeonRegister src,
                                 NeonDataType dt, int lane) {
  int size = NeonSz(dt);  // 0, 1, 2
  int byte = lane << size;
  int double_word = byte >> kDoubleSizeLog2;
  int double_byte = byte & (kDoubleSize - 1);
  int double_lane = double_byte >> size;
  DwVfpRegister double_source =
      DwVfpRegister::from_code(src.code() * 2 + double_word);
  vmov(dt, dst, double_source, double_lane);
}

void MacroAssembler::ExtractLane(Register dst, DwVfpRegister src,
                                 NeonDataType dt, int lane) {
  int size = NeonSz(dt);  // 0, 1, 2
  int byte = lane << size;
  int double_byte = byte & (kDoubleSize - 1);
  int double_lane = double_byte >> size;
  vmov(dt, dst, src, double_lane);
}

void MacroAssembler::ExtractLane(SwVfpRegister dst, QwNeonRegister src,
                                 int lane) {
  int s_code = src.code() * 4 + lane;
  VmovExtended(dst.code(), s_code);
}

void MacroAssembler::ExtractLane(DwVfpRegister dst, QwNeonRegister src,
                                 int lane) {
  DwVfpRegister double_dst = DwVfpRegister::from_code(src.code() * 2 + lane);
  vmov(dst, double_dst);
}

void MacroAssembler::ReplaceLane(QwNeonRegister dst, QwNeonRegister src,
                                 Register src_lane, NeonDataType dt, int lane) {
  Move(dst, src);
  int size = NeonSz(dt);  // 0, 1, 2
  int byte = lane << size;
  int double_word = byte >> kDoubleSizeLog2;
  int double_byte = byte & (kDoubleSize - 1);
  int double_lane = double_byte >> size;
  DwVfpRegister double_dst =
      DwVfpRegister::from_code(dst.code() * 2 + double_word);
  vmov(dt, double_dst, double_lane, src_lane);
}

void MacroAssembler::ReplaceLane(QwNeonRegister dst, QwNeonRegister src,
                                 SwVfpRegister src_lane, int lane) {
  Move(dst, src);
  int s_code = dst.code() * 4 + lane;
  VmovExtended(s_code, src_lane.code());
}

void MacroAssembler::ReplaceLane(QwNeonRegister dst, QwNeonRegister src,
                                 DwVfpRegister src_lane, int lane) {
  Move(dst, src);
  DwVfpRegister double_dst = DwVfpRegister::from_code(dst.code() * 2 + lane);
  vmov(double_dst, src_lane);
}

void MacroAssembler::LoadLane(NeonSize sz, NeonListOperand dst_list,
                              uint8_t lane, NeonMemOperand src) {
  if (sz == Neon64) {
    // vld1s is not valid for Neon64.
    vld1(Neon64, dst_list, src);
  } else {
    vld1s(sz, dst_list, lane, src);
  }
}

void MacroAssembler::StoreLane(NeonSize sz, NeonListOperand src_list,
                               uint8_t lane, NeonMemOperand dst) {
  if (sz == Neon64) {
    // vst1s is not valid for Neon64.
    vst1(Neon64, src_list, dst);
  } else {
    vst1s(sz, src_list, lane, dst);
  }
}

void MacroAssembler::LslPair(Register dst_low, Register dst_high,
                             Register src_low, Register src_high,
                             Register shift) {
  DCHECK(!AreAliased(dst_high, src_low));
  DCHECK(!AreAliased(dst_high, shift));
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();

  Label less_than_32;
  Label done;
  rsb(scratch, shift, Operand(32), SetCC);
  b(gt, &less_than_32);
  // If shift >= 32
  and_(scratch, shift, Operand(0x1F));
  lsl(dst_high, src_low, Operand(scratch));
  mov(dst_low, Operand(0));
  jmp(&done);
  bind(&less_than_32);
  // If shift < 32
  lsl(dst_high, src_high, Operand(shift));
  orr(dst_high, dst_high, Operand(src_low, LSR, scratch));
  lsl(dst_low, src_low, Operand(shift));
  bind(&done);
}

void MacroAssembler::LslPair(Register dst_low, Register dst_high,
                             Register src_low, Register src_high,
                             uint32_t shift) {
  DCHECK_GE(63, shift);
  DCHECK(!AreAliased(dst_high, src_low));

  if (shift == 0) {
    Move(dst_high, src_high);
    Move(dst_low, src_low);
  } else if (shift == 32) {
    Move(dst_high, src_low);
    Move(dst_low, Operand(0));
  } else if (shift >= 32) {
    shift &= 0x1F;
    lsl(dst_high, src_low, Operand(shift));
    mov(dst_low, Operand(0));
  } else {
    lsl(dst_high, src_high, Operand(shift));
    orr(dst_high, dst_high, Operand(src_low, LSR, 32 - shift));
    lsl(dst_low, src_low, Operand(shift));
  }
}

void MacroAssembler::LsrPair(Register dst_low, Register dst_high,
                             Register src_low, Register src_high,
                             Register shift) {
  DCHECK(!AreAliased(dst_low, src_high));
  DCHECK(!AreAliased(dst_low, shift));
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();

  Label less_than_32;
  Label done;
  rsb(scratch, shift, Operand(32), SetCC);
  b(gt, &less_than_32);
  // If shift >= 32
  and_(scratch, shift, Operand(0x1F));
  lsr(dst_low, src_high, Operand(scratch));
  mov(dst_high, Operand(0));
  jmp(&done);
  bind(&less_than_32);
  // If shift < 32

  lsr(dst_low, src_low, Operand(shift));
  orr(dst_low, dst_low, Operand(src_high, LSL, scratch));
  lsr(dst_high, src_high, Operand(shift));
  bind(&done);
}

void MacroAssembler::LsrPair(Register dst_low, Register dst_high,
                             Register src_low, Register src_high,
                             uint32_t shift) {
  DCHECK_GE(63, shift);
  DCHECK(!AreAliased(dst_low, src_high));

  if (shift == 32) {
    mov(dst_low, src_high);
    mov(dst_high, Operand(0));
  } else if (shift > 32) {
    shift &= 0x1F;
    lsr(dst_low, src_high, Operand(shift));
    mov(dst_high, Operand(0));
  } else if (shift == 0) {
    Move(dst_low, src_low);
    Move(dst_high, src_high);
  } else {
    lsr(dst_low, src_low, Operand(shift));
    orr(dst_low, dst_low, Operand(src_high, LSL, 32 - shift));
    lsr(dst_high, src_high, Operand(shift));
  }
}

void MacroAssembler::AsrPair(Register dst_low, Register dst_high,
                             Register src_low, Register src_high,
                             Register shift) {
  DCHECK(!AreAliased(dst_low, src_high));
  DCHECK(!AreAliased(dst_low, shift));
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();

  Label less_than_32;
  Label done;
  rsb(scratch, shift, Operand(32), SetCC);
  b(gt, &less_than_32);
  // If shift >= 32
  and_(scratch, shift, Operand(0x1F));
  asr(dst_low, src_high, Operand(scratch));
  asr(dst_high, src_high, Operand(31));
  jmp(&done);
  bind(&less_than_32);
  // If shift < 32
  lsr(dst_low, src_low, Operand(shift));
  orr(dst_low, dst_low, Operand(src_high, LSL, scratch));
  asr(dst_high, src_high, Operand(shift));
  bind(&done);
}

void MacroAssembler::AsrPair(Register dst_low, Register dst_high,
                             Register src_low, Register src_high,
                             uint32_t shift) {
  DCHECK_GE(63, shift);
  DCHECK(!AreAliased(dst_low, src_high));

  if (shift == 32) {
    mov(dst_low, src_high);
    asr(dst_high, src_high, Operand(31));
  } else if (shift > 32) {
    shift &= 0x1F;
    asr(dst_low, src_high, Operand(shift));
    asr(dst_high, src_high, Operand(31));
  } else if (shift == 0) {
    Move(dst_low, src_low);
    Move(dst_high, src_high);
  } else {
    lsr(dst_low, src_low, Operand(shift));
    orr(dst_low, dst_low, Operand(src_high, LSL, 32 - shift));
    asr(dst_high, src_high, Operand(shift));
  }
}

void MacroAssembler::StubPrologue(StackFrame::Type type) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  mov(scratch, Operand(StackFrame::TypeToMarker(type)));
  PushCommonFrame(scratch);
}

void MacroAssembler::Prologue() { PushStandardFrame(r1); }

void MacroAssembler::DropArguments(Register count, ArgumentsCountType type,
                                   ArgumentsCountMode mode) {
  int receiver_bytes = (mode == kCountExcludesReceiver) ? kPointerSize : 0;
  switch (type) {
    case kCountIsInteger: {
      add(sp, sp, Operand(count, LSL, kPointerSizeLog2), LeaveCC);
      break;
    }
    case kCountIsSmi: {
      static_assert(kSmiTagSize == 1 && kSmiTag == 0);
      add(sp, sp, Operand(count, LSL, kPointerSizeLog2 - kSmiTagSize), LeaveCC);
      break;
    }
    case kCountIsBytes: {
      add(sp, sp, count, LeaveCC);
      break;
    }
  }
  if (receiver_bytes != 0) {
    add(sp, sp, Operand(receiver_bytes), LeaveCC);
  }
}

void MacroAssembler::DropArgumentsAndPushNewReceiver(Register argc,
                                                     Register receiver,
                                                     ArgumentsCountType type,
                                                     ArgumentsCountMode mode) {
  DCHECK(!AreAliased(argc, receiver));
  if (mode == kCountExcludesReceiver) {
    // Drop arguments without receiver and override old receiver.
    DropArguments(argc, type, kCountIncludesReceiver);
    str(receiver, MemOperand(sp, 0));
  } else {
    DropArguments(argc, type, mode);
    push(receiver);
  }
}

void MacroAssembler::EnterFrame(StackFrame::Type type,
                                bool load_constant_pool_pointer_reg) {
  ASM_CODE_COMMENT(this);
  // r0-r3: preserved
  UseScratchRegisterScope temps(this);
  Register scratch = no_reg;
  if (!StackFrame::IsJavaScript(type)) {
    scratch = temps.Acquire();
    mov(scratch, Operand(StackFrame::TypeToMarker(type)));
  }
  PushCommonFrame(scratch);
#if V8_ENABLE_WEBASSEMBLY
  if (type == StackFrame::WASM) Push(kWasmInstanceRegister);
#endif  // V8_ENABLE_WEBASSEMBLY
}

int MacroAssembler::LeaveFrame(StackFrame::Type type) {
  ASM_CODE_COMMENT(this);
  // r0: preserved
  // r1: preserved
  // r2: preserved

  // Drop the execution stack down to the frame pointer and restore
  // the caller frame pointer and return address.
  mov(sp, fp);
  int frame_ends = pc_offset();
  ldm(ia_w, sp, {fp, lr});
  return frame_ends;
}

#ifdef V8_OS_WIN
void MacroAssembler::AllocateStackSpace(Register bytes_scratch) {
  // "Functions that allocate 4 KB or more on the stack must ensure that each
  // page prior to the final page is touched in order." Source:
  // https://docs.microsoft.com/en-us/cpp/build/overview-of-arm-abi-conventions?view=vs-2019#stack
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  DwVfpRegister scratch = temps.AcquireD();
  Label check_offset;
  Label touch_next_page;
  jmp(&check_offset);
  bind(&touch_next_page);
  sub(sp, sp, Operand(kStackPageSize));
  // Just to touch the page, before we increment further.
  vldr(scratch, MemOperand(sp));
  sub(bytes_scratch, bytes_scratch, Operand(kStackPageSize));

  bind(&check_offset);
  cmp(bytes_scratch, Operand(kStackPageSize));
  b(gt, &touch_next_page);

  sub(sp, sp, bytes_scratch);
}

void MacroAssembler::AllocateStackSpace(int bytes) {
  ASM_CODE_COMMENT(this);
  DCHECK_GE(bytes, 0);
  UseScratchRegisterScope temps(this);
  DwVfpRegister scratch = no_dreg;
  while (bytes > kStackPageSize) {
    if (scratch == no_dreg) {
      scratch = temps.AcquireD();
    }
    sub(sp, sp, Operand(kStackPageSize));
    vldr(scratch, MemOperand(sp));
    bytes -= kStackPageSize;
  }
  if (bytes == 0) return;
  sub(sp, sp, Operand(bytes));
}
#endif

void MacroAssembler::EnterExitFrame(int stack_space,
                                    StackFrame::Type frame_type) {
  ASM_CODE_COMMENT(this);
  DCHECK(frame_type == StackFrame::EXIT ||
         frame_type == StackFrame::BUILTIN_EXIT);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();

  // Set up the frame structure on the stack.
  DCHECK_EQ(2 * kPointerSize, ExitFrameConstants::kCallerSPDisplacement);
  DCHECK_EQ(1 * kPointerSize, ExitFrameConstants::kCallerPCOffset);
  DCHECK_EQ(0 * kPointerSize, ExitFrameConstants::kCallerFPOffset);
  mov(scratch, Operand(StackFrame::TypeToMarker(frame_type)));
  PushCommonFrame(scratch);
  // Reserve room for saved entry sp.
  sub(sp, fp, Operand(ExitFrameConstants::kFixedFrameSizeFromFp));
  if (v8_flags.debug_code) {
    mov(scratch, Operand::Zero());
    str(scratch, MemOperand(fp, ExitFrameConstants::kSPOffset));
  }

  // Save the frame pointer and the context in top.
  Move(scratch, ExternalReference::Create(IsolateAddressId::kCEntryFPAddress,
                                          isolate()));
  str(fp, MemOperand(scratch));
  Move(scratch,
       ExternalReference::Create(IsolateAddressId::kContextAddress, isolate()));
  str(cp, MemOperand(scratch));

  // Reserve place for the return address and stack space and align the frame
  // preparing for calling the runtime function.
  const int frame_alignment = MacroAssembler::ActivationFrameAlignment();
  AllocateStackSpace((stack_space + 1) * kPointerSize);
  if (frame_alignment > 0) {
    DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
    and_(sp, sp, Operand(-frame_alignment));
  }

  // Set the exit frame sp value to point just before the return address
  // location.
  add(scratch, sp, Operand(kPointerSize));
  str(scratch, MemOperand(fp, ExitFrameConstants::kSPOffset));
}

int MacroAssembler::ActivationFrameAlignment() {
#if V8_HOST_ARCH_ARM
  // Running on the real platform. Use the alignment as mandated by the local
  // environment.
  // Note: This will break if we ever start generating snapshots on one ARM
  // platform for another ARM platform with a different alignment.
  return base::OS::ActivationFrameAlignment();
#else   // V8_HOST_ARCH_ARM
  // If we are using the simulator then we should always align to the expected
  // alignment. As the simulator is used to generate snapshots we do not know
  // if the target platform will need alignment, so this is controlled from a
  // flag.
  return v8_flags.sim_stack_alignment;
#endif  // V8_HOST_ARCH_ARM
}

void MacroAssembler::LeaveExitFrame(Register argument_count,
                                    bool argument_count_is_length) {
  ASM_CODE_COMMENT(this);
  ConstantPoolUnavailableScope constant_pool_unavailable(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();

  // Clear top frame.
  mov(r3, Operand::Zero());
  Move(scratch, ExternalReference::Create(IsolateAddressId::kCEntryFPAddress,
                                          isolate()));
  str(r3, MemOperand(scratch));

  // Restore current context from top and clear it in debug mode.
  Move(scratch,
       ExternalReference::Create(IsolateAddressId::kContextAddress, isolate()));
  ldr(cp, MemOperand(scratch));
#ifdef DEBUG
  mov(r3, Operand(Context::kInvalidContext));
  Move(scratch,
       ExternalReference::Create(IsolateAddressId::kContextAddress, isolate()));
  str(r3, MemOperand(scratch));
#endif

  // Tear down the exit frame, pop the arguments, and return.
  mov(sp, Operand(fp));
  ldm(ia_w, sp, {fp, lr});
  if (argument_count.is_valid()) {
    if (argument_count_is_length) {
      add(sp, sp, argument_count);
    } else {
      add(sp, sp, Operand(argument_count, LSL, kPointerSizeLog2));
    }
  }
}

void MacroAssembler::MovFromFloatResult(const DwVfpRegister dst) {
  if (use_eabi_hardfloat()) {
    Move(dst, d0);
  } else {
    vmov(dst, r0, r1);
  }
}

// On ARM this is just a synonym to make the purpose clear.
void MacroAssembler::MovFromFloatParameter(DwVfpRegister dst) {
  MovFromFloatResult(dst);
}

void MacroAssembler::LoadStackLimit(Register destination, StackLimitKind kind) {
  ASM_CODE_COMMENT(this);
  DCHECK(root_array_available());
  Isolate* isolate = this->isolate();
  ExternalReference limit =
      kind == StackLimitKind::kRealStackLimit
          ? ExternalReference::address_of_real_jslimit(isolate)
          : ExternalReference::address_of_jslimit(isolate);
  DCHECK(MacroAssembler::IsAddressableThroughRootRegister(isolate, limit));

  intptr_t offset =
      MacroAssembler::RootRegisterOffsetForExternalReference(isolate, limit);
  CHECK(is_int32(offset));
  ldr(destination, MemOperand(kRootRegister, offset));
}

void MacroAssembler::StackOverflowCheck(Register num_args, Register scratch,
                                        Label* stack_overflow) {
  ASM_CODE_COMMENT(this);
  // Check the stack for overflow. We are not trying to catch
  // interruptions (e.g. debug break and preemption) here, so the "real stack
  // limit" is checked.
  LoadStackLimit(scratch, StackLimitKind::kRealStackLimit);
  // Make scratch the space we have left. The stack might already be overflowed
  // here which will cause scratch to become negative.
  sub(scratch, sp, scratch);
  // Check if the arguments will overflow the stack.
  cmp(scratch, Operand(num_args, LSL, kPointerSizeLog2));
  b(le, stack_overflow);  // Signed comparison.
}

void MacroAssembler::InvokePrologue(Register expected_parameter_count,
                                    Register actual_parameter_count,
                                    Label* done, InvokeType type) {
  ASM_CODE_COMMENT(this);
  Label regular_invoke;
  //  r0: actual arguments count
  //  r1: function (passed through to callee)
  //  r2: expected arguments count
  DCHECK_EQ(actual_parameter_count, r0);
  DCHECK_EQ(expected_parameter_count, r2);

  // If overapplication or if the actual argument count is equal to the
  // formal parameter count, no need to push extra undefined values.
  sub(expected_parameter_count, expected_parameter_count,
      actual_parameter_count, SetCC);
  b(le, &regular_invoke);

  Label stack_overflow;
  Register scratch = r4;
  StackOverflowCheck(expected_parameter_count, scratch, &stack_overflow);

  // Underapplication. Move the arguments already in the stack, including the
  // receiver and the return address.
  {
    Label copy, check;
    Register num = r5, src = r6, dest = r9;  // r7 and r8 are context and root.
    mov(src, sp);
    // Update stack pointer.
    lsl(scratch, expected_parameter_count, Operand(kSystemPointerSizeLog2));
    AllocateStackSpace(scratch);
    mov(dest, sp);
    mov(num, actual_parameter_count);
    b(&check);
    bind(&copy);
    ldr(scratch, MemOperand(src, kSystemPointerSize, PostIndex));
    str(scratch, MemOperand(dest, kSystemPointerSize, PostIndex));
    sub(num, num, Operand(1), SetCC);
    bind(&check);
    b(gt, &copy);
  }

  // Fill remaining expected arguments with undefined values.
  LoadRoot(scratch, RootIndex::kUndefinedValue);
  {
    Label loop;
    bind(&loop);
    str(scratch, MemOperand(r9, kSystemPointerSize, PostIndex));
    sub(expected_parameter_count, expected_parameter_count, Operand(1), SetCC);
    b(gt, &loop);
  }
  b(&regular_invoke);

  bind(&stack_overflow);
  {
    FrameScope frame(
        this, has_frame() ? StackFrame::NO_FRAME_TYPE : StackFrame::INTERNAL);
    CallRuntime(Runtime::kThrowStackOverflow);
    bkpt(0);
  }

  bind(&regular_invoke);
}

void MacroAssembler::CallDebugOnFunctionCall(Register fun, Register new_target,
                                             Register expected_parameter_count,
                                             Register actual_parameter_count) {
  ASM_CODE_COMMENT(this);
  // Load receiver to pass it later to DebugOnFunctionCall hook.
  ldr(r4, ReceiverOperand(actual_parameter_count));
  FrameScope frame(
      this, has_frame() ? StackFrame::NO_FRAME_TYPE : StackFrame::INTERNAL);

  SmiTag(expected_parameter_count);
  Push(expected_parameter_count);

  SmiTag(actual_parameter_count);
  Push(actual_parameter_count);

  if (new_target.is_valid()) {
    Push(new_target);
  }
  Push(fun);
  Push(fun);
  Push(r4);
  CallRuntime(Runtime::kDebugOnFunctionCall);
  Pop(fun);
  if (new_target.is_valid()) {
    Pop(new_target);
  }

  Pop(actual_parameter_count);
  SmiUntag(actual_parameter_count);

  Pop(expected_parameter_count);
  SmiUntag(expected_parameter_count);
}

void MacroAssembler::InvokeFunctionCode(Register function, Register new_target,
                                        Register expected_parameter_count,
                                        Register actual_parameter_count,
                                        InvokeType type) {
  ASM_CODE_COMMENT(this);
  // You can't call a function without a valid frame.
  DCHECK_IMPLIES(type == InvokeType::kCall, has_frame());
  DCHECK_EQ(function, r1);
  DCHECK_IMPLIES(new_target.is_valid(), new_target == r3);

  // On function call, call into the debugger if necessary.
  Label debug_hook, continue_after_hook;
  {
    ExternalReference debug_hook_active =
        ExternalReference::debug_hook_on_function_call_address(isolate());
    Move(r4, debug_hook_active);
    ldrsb(r4, MemOperand(r4));
    cmp(r4, Operand(0));
    b(ne, &debug_hook);
  }
  bind(&continue_after_hook);

  // Clear the new.target register if not given.
  if (!new_target.is_valid()) {
    LoadRoot(r3, RootIndex::kUndefinedValue);
  }

  Label done;
  InvokePrologue(expected_parameter_count, actual_parameter_count, &done, type);
  // We call indirectly through the code field in the function to
  // allow recompilation to take effect without changing any of the
  // call sites.
  Register code = kJavaScriptCallCodeStartRegister;
  ldr(code, FieldMemOperand(function, JSFunction::kCodeOffset));
  switch (type) {
    case InvokeType::kCall:
      CallCodeObject(code);
      break;
    case InvokeType::kJump:
      JumpCodeObject(code);
      break;
  }
  b(&done);

  // Deferred debug hook.
  bind(&debug_hook);
  CallDebugOnFunctionCall(function, new_target, expected_parameter_count,
                          actual_parameter_count);
  b(&continue_after_hook);

  // Continue here if InvokePrologue does handle the invocation due to
  // mismatched parameter counts.
  bind(&done);
}

void MacroAssembler::InvokeFunctionWithNewTarget(
    Register fun, Register new_target, Register actual_parameter_count,
    InvokeType type) {
  ASM_CODE_COMMENT(this);
  // You can't call a function without a valid frame.
  DCHECK_IMPLIES(type == InvokeType::kCall, has_frame());

  // Contract with called JS functions requires that function is passed in r1.
  DCHECK_EQ(fun, r1);

  Register expected_reg = r2;
  Register temp_reg = r4;

  ldr(temp_reg, FieldMemOperand(r1, JSFunction::kSharedFunctionInfoOffset));
  ldr(cp, FieldMemOperand(r1, JSFunction::kContextOffset));
  ldrh(expected_reg,
       FieldMemOperand(temp_reg,
                       SharedFunctionInfo::kFormalParameterCountOffset));

  InvokeFunctionCode(fun, new_target, expected_reg, actual_parameter_count,
                     type);
}

void MacroAssembler::InvokeFunction(Register function,
                                    Register expected_parameter_count,
                                    Register actual_parameter_count,
                                    InvokeType type) {
  ASM_CODE_COMMENT(this);
  // You can't call a function without a valid frame.
  DCHECK_IMPLIES(type == InvokeType::kCall, has_frame());

  // Contract with called JS functions requires that function is passed in r1.
  DCHECK_EQ(function, r1);

  // Get the function and setup the context.
  ldr(cp, FieldMemOperand(r1, JSFunction::kContextOffset));

  InvokeFunctionCode(r1, no_reg, expected_parameter_count,
                     actual_parameter_count, type);
}

void MacroAssembler::PushStackHandler() {
  ASM_CODE_COMMENT(this);
  // Adjust this code if not the case.
  static_assert(StackHandlerConstants::kSize == 2 * kPointerSize);
  static_assert(StackHandlerConstants::kNextOffset == 0 * kPointerSize);

  Push(Smi::zero());  // Padding.
  // Link the current handler as the next handler.
  Move(r6,
       ExternalReference::Create(IsolateAddressId::kHandlerAddress, isolate()));
  ldr(r5, MemOperand(r6));
  push(r5);
  // Set this new handler as the current one.
  str(sp, MemOperand(r6));
}

void MacroAssembler::PopStackHandler() {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  static_assert(StackHandlerConstants::kNextOffset == 0);
  pop(r1);
  Move(scratch,
       ExternalReference::Create(IsolateAddressId::kHandlerAddress, isolate()));
  str(r1, MemOperand(scratch));
  add(sp, sp, Operand(StackHandlerConstants::kSize - kPointerSize));
}

void MacroAssembler::CompareObjectType(Register object, Register map,
                                       Register type_reg, InstanceType type) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  const Register temp = type_reg == no_reg ? temps.Acquire() : type_reg;

  LoadMap(map, object);
  CompareInstanceType(map, temp, type);
}

void MacroAssembler::CompareInstanceType(Register map, Register type_reg,
                                         InstanceType type) {
  ldrh(type_reg, FieldMemOperand(map, Map::kInstanceTypeOffset));
  cmp(type_reg, Operand(type));
}

void MacroAssembler::CompareRange(Register value, unsigned lower_limit,
                                  unsigned higher_limit) {
  ASM_CODE_COMMENT(this);
  DCHECK_LT(lower_limit, higher_limit);
  if (lower_limit != 0) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    sub(scratch, value, Operand(lower_limit));
    cmp(scratch, Operand(higher_limit - lower_limit));
  } else {
    cmp(value, Operand(higher_limit));
  }
}
void MacroAssembler::CompareInstanceTypeRange(Register map, Register type_reg,
                                              InstanceType lower_limit,
                                              InstanceType higher_limit) {
  ASM_CODE_COMMENT(this);
  DCHECK_LT(lower_limit, higher_limit);
  ldrh(type_reg, FieldMemOperand(map, Map::kInstanceTypeOffset));
  CompareRange(type_reg, lower_limit, higher_limit);
}

void MacroAssembler::CompareRoot(Register obj, RootIndex index) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  DCHECK(obj != scratch);
  LoadRoot(scratch, index);
  cmp(obj, scratch);
}

void MacroAssembler::JumpIfIsInRange(Register value, unsigned lower_limit,
                                     unsigned higher_limit,
                                     Label* on_in_range) {
  ASM_CODE_COMMENT(this);
  CompareRange(value, lower_limit, higher_limit);
  b(ls, on_in_range);
}

void MacroAssembler::TryInlineTruncateDoubleToI(Register result,
                                                DwVfpRegister double_input,
                                                Label* done) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  SwVfpRegister single_scratch = SwVfpRegister::no_reg();
  if (temps.CanAcquireVfp<SwVfpRegister>()) {
    single_scratch = temps.AcquireS();
  } else {
    // Re-use the input as a scratch register. However, we can only do this if
    // the input register is d0-d15 as there are no s32+ registers.
    DCHECK_LT(double_input.code(), LowDwVfpRegister::kNumRegisters);
    LowDwVfpRegister double_scratch =
        LowDwVfpRegister::from_code(double_input.code());
    single_scratch = double_scratch.low();
  }
  vcvt_s32_f64(single_scratch, double_input);
  vmov(result, single_scratch);

  Register scratch = temps.Acquire();
  // If result is not saturated (0x7FFFFFFF or 0x80000000), we are done.
  sub(scratch, result, Operand(1));
  cmp(scratch, Operand(0x7FFFFFFE));
  b(lt, done);
}

void MacroAssembler::TruncateDoubleToI(Isolate* isolate, Zone* zone,
                                       Register result,
                                       DwVfpRegister double_input,
                                       StubCallMode stub_mode) {
  ASM_CODE_COMMENT(this);
  Label done;

  TryInlineTruncateDoubleToI(result, double_input, &done);

  // If we fell through then inline version didn't succeed - call stub instead.
  push(lr);
  AllocateStackSpace(kDoubleSize);  // Put input on stack.
  vstr(double_input, MemOperand(sp, 0));

#if V8_ENABLE_WEBASSEMBLY
  if (stub_mode == StubCallMode::kCallWasmRuntimeStub) {
    Call(wasm::WasmCode::kDoubleToI, RelocInfo::WASM_STUB_CALL);
#else
  // For balance.
  if (false) {
#endif  // V8_ENABLE_WEBASSEMBLY
  } else {
    CallBuiltin(Builtin::kDoubleToI);
  }
  ldr(result, MemOperand(sp, 0));

  add(sp, sp, Operand(kDoubleSize));
  pop(lr);

  bind(&done);
}

namespace {

void TailCallOptimizedCodeSlot(MacroAssembler* masm,
                               Register optimized_code_entry,
                               Register scratch) {
  // ----------- S t a t e -------------
  //  -- r0 : actual argument count
  //  -- r3 : new target (preserved for callee if needed, and caller)
  //  -- r1 : target function (preserved for callee if needed, and caller)
  // -----------------------------------
  DCHECK(!AreAliased(r1, r3, optimized_code_entry, scratch));

  Register closure = r1;
  Label heal_optimized_code_slot;

  // If the optimized code is cleared, go to runtime to update the optimization
  // marker field.
  __ LoadWeakValue(optimized_code_entry, optimized_code_entry,
                   &heal_optimized_code_slot);

  // Check if the optimized code is marked for deopt. If it is, call the
  // runtime to clear it.
  {
    UseScratchRegisterScope temps(masm);
    __ TestCodeIsMarkedForDeoptimization(optimized_code_entry, temps.Acquire());
    __ b(ne, &heal_optimized_code_slot);
  }

  // Optimized code is good, get it into the closure and link the closure
  // into the optimized functions list, then tail call the optimized code.
  __ ReplaceClosureCodeWithOptimizedCode(optimized_code_entry, closure);
  static_assert(kJavaScriptCallCodeStartRegister == r2, "ABI mismatch");
  __ LoadCodeInstructionStart(r2, optimized_code_entry);
  __ Jump(r2);

  // Optimized code slot contains deoptimized code or code is cleared and
  // optimized code marker isn't updated. Evict the code, update the marker
  // and re-enter the closure's code.
  __ bind(&heal_optimized_code_slot);
  __ GenerateTailCallToReturnedCode(Runtime::kHealOptimizedCodeSlot);
}

}  // namespace

#ifdef V8_ENABLE_DEBUG_CODE
void MacroAssembler::AssertFeedbackVector(Register object) {
  if (v8_flags.debug_code) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    CompareObjectType(object, scratch, scratch, FEEDBACK_VECTOR_TYPE);
    Assert(eq, AbortReason::kExpectedFeedbackVector);
  }
}
#endif  // V8_ENABLE_DEBUG_CODE

void MacroAssembler::ReplaceClosureCodeWithOptimizedCode(
    Register optimized_code, Register closure) {
  ASM_CODE_COMMENT(this);
  DCHECK(!AreAliased(optimized_code, closure));
  // Store code entry in the closure.
  str(optimized_code, FieldMemOperand(closure, JSFunction::kCodeOffset));
  RecordWriteField(closure, JSFunction::kCodeOffset, optimized_code,
                   kLRHasNotBeenSaved, SaveFPRegsMode::kIgnore,
                   SmiCheck::kOmit);
}

void MacroAssembler::GenerateTailCallToReturnedCode(
    Runtime::FunctionId function_id) {
  // ----------- S t a t e -------------
  //  -- r0 : actual argument count
  //  -- r1 : target function (preserved for callee)
  //  -- r3 : new target (preserved for callee)
  // -----------------------------------
  {
    FrameAndConstantPoolScope scope(this, StackFrame::INTERNAL);
    // Push a copy of the target function, the new target and the actual
    // argument count.
    // Push function as parameter to the runtime call.
    SmiTag(kJavaScriptCallArgCountRegister);
    Push(kJavaScriptCallTargetRegister, kJavaScriptCallNewTargetRegister,
         kJavaScriptCallArgCountRegister, kJavaScriptCallTargetRegister);

    CallRuntime(function_id, 1);
    mov(r2, r0);

    // Restore target function, new target and actual argument count.
    Pop(kJavaScriptCallTargetRegister, kJavaScriptCallNewTargetRegister,
        kJavaScriptCallArgCountRegister);
    SmiUntag(kJavaScriptCallArgCountRegister);
  }
  static_assert(kJavaScriptCallCodeStartRegister == r2, "ABI mismatch");
  JumpCodeObject(r2);
}

// Read off the flags in the feedback vector and check if there
// is optimized code or a tiering state that needs to be processed.
void MacroAssembler::LoadFeedbackVectorFlagsAndJumpIfNeedsProcessing(
    Register flags, Register feedback_vector, CodeKind current_code_kind,
    Label* flags_need_processing) {
  ASM_CODE_COMMENT(this);
  DCHECK(!AreAliased(flags, feedback_vector));
  DCHECK(CodeKindCanTierUp(current_code_kind));
  ldrh(flags, FieldMemOperand(feedback_vector, FeedbackVector::kFlagsOffset));
  uint32_t kFlagsMask = FeedbackVector::kFlagsTieringStateIsAnyRequested |
                        FeedbackVector::kFlagsMaybeHasTurbofanCode |
                        FeedbackVector::kFlagsLogNextExecution;
  if (current_code_kind != CodeKind::MAGLEV) {
    kFlagsMask |= FeedbackVector::kFlagsMaybeHasMaglevCode;
  }
  tst(flags, Operand(kFlagsMask));
  b(ne, flags_need_processing);
}

void MacroAssembler::OptimizeCodeOrTailCallOptimizedCodeSlot(
    Register flags, Register feedback_vector) {
  ASM_CODE_COMMENT(this);
  DCHECK(!AreAliased(flags, feedback_vector));
  Label maybe_has_optimized_code, maybe_needs_logging;
  // Check if optimized code is available.
  tst(flags, Operand(FeedbackVector::kFlagsTieringStateIsAnyRequested));
  b(eq, &maybe_needs_logging);
  GenerateTailCallToReturnedCode(Runtime::kCompileOptimized);

  bind(&maybe_needs_logging);
  tst(flags, Operand(FeedbackVector::LogNextExecutionBit::kMask));
  b(eq, &maybe_has_optimized_code);
  GenerateTailCallToReturnedCode(Runtime::kFunctionLogNextExecution);

  bind(&maybe_has_optimized_code);
  Register optimized_code_entry = flags;
  ldr(optimized_code_entry,
      FieldMemOperand(feedback_vector,
                      FeedbackVector::kMaybeOptimizedCodeOffset));
  TailCallOptimizedCodeSlot(this, optimized_code_entry, r6);
}

void MacroAssembler::CallRuntime(const Runtime::Function* f,
                                 int num_arguments) {
  ASM_CODE_COMMENT(this);
  // All parameters are on the stack.  r0 has the return value after call.

  // If the expected number of arguments of the runtime function is
  // constant, we check that the actual number of arguments match the
  // expectation.
  CHECK(f->nargs < 0 || f->nargs == num_arguments);

  // TODO(1236192): Most runtime routines don't need the number of
  // arguments passed in because it is constant. At some point we
  // should remove this need and make the runtime routine entry code
  // smarter.
  mov(r0, Operand(num_arguments));
  Move(r1, ExternalReference::Create(f));
  Handle<Code> code = CodeFactory::CEntry(isolate(), f->result_size);
  Call(code, RelocInfo::CODE_TARGET);
}

void MacroAssembler::TailCallRuntime(Runtime::FunctionId fid) {
  ASM_CODE_COMMENT(this);
  const Runtime::Function* function = Runtime::FunctionForId(fid);
  DCHECK_EQ(1, function->result_size);
  if (function->nargs >= 0) {
    // TODO(1236192): Most runtime routines don't need the number of
    // arguments passed in because it is constant. At some point we
    // should remove this need and make the runtime routine entry code
    // smarter.
    mov(r0, Operand(function->nargs));
  }
  JumpToExternalReference(ExternalReference::Create(fid));
}

void MacroAssembler::JumpToExternalReference(const ExternalReference& builtin,
                                             bool builtin_exit_frame) {
#if defined(__thumb__)
  // Thumb mode builtin.
  DCHECK_EQ(builtin.address() & 1, 1);
#endif
  Move(r1, builtin);
  Handle<Code> code =
      CodeFactory::CEntry(isolate(), 1, ArgvMode::kStack, builtin_exit_frame);
  Jump(code, RelocInfo::CODE_TARGET);
}

void MacroAssembler::LoadWeakValue(Register out, Register in,
                                   Label* target_if_cleared) {
  cmp(in, Operand(kClearedWeakHeapObjectLower32));
  b(eq, target_if_cleared);

  and_(out, in, Operand(~kWeakHeapObjectMask));
}

void MacroAssembler::EmitIncrementCounter(StatsCounter* counter, int value,
                                          Register scratch1,
                                          Register scratch2) {
  DCHECK_GT(value, 0);
  if (v8_flags.native_code_counters && counter->Enabled()) {
    ASM_CODE_COMMENT(this);
    Move(scratch2, ExternalReference::Create(counter));
    ldr(scratch1, MemOperand(scratch2));
    add(scratch1, scratch1, Operand(value));
    str(scratch1, MemOperand(scratch2));
  }
}

void MacroAssembler::EmitDecrementCounter(StatsCounter* counter, int value,
                                          Register scratch1,
                                          Register scratch2) {
  DCHECK_GT(value, 0);
  if (v8_flags.native_code_counters && counter->Enabled()) {
    ASM_CODE_COMMENT(this);
    Move(scratch2, ExternalReference::Create(counter));
    ldr(scratch1, MemOperand(scratch2));
    sub(scratch1, scratch1, Operand(value));
    str(scratch1, MemOperand(scratch2));
  }
}

#ifdef V8_ENABLE_DEBUG_CODE
void MacroAssembler::Assert(Condition cond, AbortReason reason) {
  if (v8_flags.debug_code) Check(cond, reason);
}

void MacroAssembler::AssertUnreachable(AbortReason reason) {
  if (v8_flags.debug_code) Abort(reason);
}

void MacroAssembler::AssertNotSmi(Register object) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  static_assert(kSmiTag == 0);
  tst(object, Operand(kSmiTagMask));
  Check(ne, AbortReason::kOperandIsASmi);
}

void MacroAssembler::AssertSmi(Register object) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  static_assert(kSmiTag == 0);
  tst(object, Operand(kSmiTagMask));
  Check(eq, AbortReason::kOperandIsNotASmi);
}

void MacroAssembler::AssertConstructor(Register object) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  static_assert(kSmiTag == 0);
  tst(object, Operand(kSmiTagMask));
  Check(ne, AbortReason::kOperandIsASmiAndNotAConstructor);
  push(object);
  LoadMap(object, object);
  ldrb(object, FieldMemOperand(object, Map::kBitFieldOffset));
  tst(object, Operand(Map::Bits1::IsConstructorBit::kMask));
  pop(object);
  Check(ne, AbortReason::kOperandIsNotAConstructor);
}

void MacroAssembler::AssertFunction(Register object) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  static_assert(kSmiTag == 0);
  tst(object, Operand(kSmiTagMask));
  Check(ne, AbortReason::kOperandIsASmiAndNotAFunction);
  push(object);
  LoadMap(object, object);
  CompareInstanceTypeRange(object, object, FIRST_JS_FUNCTION_TYPE,
                           LAST_JS_FUNCTION_TYPE);
  pop(object);
  Check(ls, AbortReason::kOperandIsNotAFunction);
}

void MacroAssembler::AssertCallableFunction(Register object) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  static_assert(kSmiTag == 0);
  tst(object, Operand(kSmiTagMask));
  Check(ne, AbortReason::kOperandIsASmiAndNotAFunction);
  push(object);
  LoadMap(object, object);
  CompareInstanceTypeRange(object, object, FIRST_CALLABLE_JS_FUNCTION_TYPE,
                           LAST_CALLABLE_JS_FUNCTION_TYPE);
  pop(object);
  Check(ls, AbortReason::kOperandIsNotACallableFunction);
}

void MacroAssembler::AssertBoundFunction(Register object) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  static_assert(kSmiTag == 0);
  tst(object, Operand(kSmiTagMask));
  Check(ne, AbortReason::kOperandIsASmiAndNotABoundFunction);
  push(object);
  CompareObjectType(object, object, object, JS_BOUND_FUNCTION_TYPE);
  pop(object);
  Check(eq, AbortReason::kOperandIsNotABoundFunction);
}

void MacroAssembler::AssertGeneratorObject(Register object) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  tst(object, Operand(kSmiTagMask));
  Check(ne, AbortReason::kOperandIsASmiAndNotAGeneratorObject);

  // Load map
  Register map = object;
  push(object);
  LoadMap(map, object);

  // Check if JSGeneratorObject
  Register instance_type = object;
  CompareInstanceTypeRange(map, instance_type, FIRST_JS_GENERATOR_OBJECT_TYPE,
                           LAST_JS_GENERATOR_OBJECT_TYPE);
  // Restore generator object to register and perform assertion
  pop(object);
  Check(ls, AbortReason::kOperandIsNotAGeneratorObject);
}

void MacroAssembler::AssertUndefinedOrAllocationSite(Register object,
                                                     Register scratch) {
  if (!v8_flags.debug_code) return;
  ASM_CODE_COMMENT(this);
  Label done_checking;
  AssertNotSmi(object);
  CompareRoot(object, RootIndex::kUndefinedValue);
  b(eq, &done_checking);
  LoadMap(scratch, object);
  CompareInstanceType(scratch, scratch, ALLOCATION_SITE_TYPE);
  Assert(eq, AbortReason::kExpectedUndefinedOrCell);
  bind(&done_checking);
}

void MacroAssembler::AssertJSAny(Register object, Register map_tmp,
                                 Register tmp, AbortReason abort_reason) {
  if (!v8_flags.debug_code) return;

  ASM_CODE_COMMENT(this);
  DCHECK(!AreAliased(object, map_tmp, tmp));
  Label ok;

  JumpIfSmi(object, &ok);

  LoadMap(map_tmp, object);
  CompareInstanceType(map_tmp, tmp, LAST_NAME_TYPE);
  b(kUnsignedLessThanEqual, &ok);

  CompareInstanceType(map_tmp, tmp, FIRST_JS_RECEIVER_TYPE);
  b(kUnsignedGreaterThanEqual, &ok);

  CompareRoot(map_tmp, RootIndex::kHeapNumberMap);
  b(kEqual, &ok);

  CompareRoot(map_tmp, RootIndex::kBigIntMap);
  b(kEqual, &ok);

  CompareRoot(object, RootIndex::kUndefinedValue);
  b(kEqual, &ok);

  CompareRoot(object, RootIndex::kTrueValue);
  b(kEqual, &ok);

  CompareRoot(object, RootIndex::kFalseValue);
  b(kEqual, &ok);

  CompareRoot(object, RootIndex::kNullValue);
  b(kEqual, &ok);

  Abort(abort_reason);

  bind(&ok);
}

#endif  // V8_ENABLE_DEBUG_CODE

void MacroAssembler::Check(Condition cond, AbortReason reason) {
  Label L;
  b(cond, &L);
  Abort(reason);
  // will not return here
  bind(&L);
}

void MacroAssembler::Abort(AbortReason reason) {
  ASM_CODE_COMMENT(this);
  Label abort_start;
  bind(&abort_start);
  if (v8_flags.code_comments) {
    const char* msg = GetAbortReason(reason);
    RecordComment("Abort message: ");
    RecordComment(msg);
  }

  // Avoid emitting call to builtin if requested.
  if (trap_on_abort()) {
    stop();
    return;
  }

  if (should_abort_hard()) {
    // We don't care if we constructed a frame. Just pretend we did.
    FrameScope assume_frame(this, StackFrame::NO_FRAME_TYPE);
    Move32BitImmediate(r0, Operand(static_cast<int>(reason)));
    PrepareCallCFunction(1, 0, r1);
    Move(r1, ExternalReference::abort_with_reason());
    // Use Call directly to avoid any unneeded overhead. The function won't
    // return anyway.
    Call(r1);
    return;
  }

  Move(r1, Smi::FromInt(static_cast<int>(reason)));

  {
    // We don't actually want to generate a pile of code for this, so just
    // claim there is a stack frame, without generating one.
    FrameScope scope(this, StackFrame::NO_FRAME_TYPE);
    if (root_array_available()) {
      // Generate an indirect call via builtins entry table here in order to
      // ensure that the interpreter_entry_return_pc_offset is the same for
      // InterpreterEntryTrampoline and InterpreterEntryTrampolineForProfiling
      // when v8_flags.debug_code is enabled.
      LoadEntryFromBuiltin(Builtin::kAbort, ip);
      Call(ip);
    } else {
      Call(BUILTIN_CODE(isolate(), Abort), RelocInfo::CODE_TARGET);
    }
  }
  // will not return here
}

void MacroAssembler::LoadMap(Register destination, Register object) {
  ldr(destination, FieldMemOperand(object, HeapObject::kMapOffset));
}

void MacroAssembler::LoadGlobalProxy(Register dst) {
  ASM_CODE_COMMENT(this);
  LoadNativeContextSlot(dst, Context::GLOBAL_PROXY_INDEX);
}

void MacroAssembler::LoadNativeContextSlot(Register dst, int index) {
  ASM_CODE_COMMENT(this);
  LoadMap(dst, cp);
  ldr(dst, FieldMemOperand(
               dst, Map::kConstructorOrBackPointerOrNativeContextOffset));
  ldr(dst, MemOperand(dst, Context::SlotOffset(index)));
}

void MacroAssembler::InitializeRootRegister() {
  ASM_CODE_COMMENT(this);
  ExternalReference isolate_root = ExternalReference::isolate_root(isolate());
  mov(kRootRegister, Operand(isolate_root));
}

void MacroAssembler::SmiTag(Register reg, SBit s) {
  add(reg, reg, Operand(reg), s);
}

void MacroAssembler::SmiTag(Register dst, Register src, SBit s) {
  add(dst, src, Operand(src), s);
}

void MacroAssembler::SmiTst(Register value) {
  tst(value, Operand(kSmiTagMask));
}

void MacroAssembler::JumpIfSmi(Register value, Label* smi_label) {
  tst(value, Operand(kSmiTagMask));
  b(eq, smi_label);
}

void MacroAssembler::JumpIfEqual(Register x, int32_t y, Label* dest) {
  cmp(x, Operand(y));
  b(eq, dest);
}

void MacroAssembler::JumpIfLessThan(Register x, int32_t y, Label* dest) {
  cmp(x, Operand(y));
  b(lt, dest);
}

void MacroAssembler::JumpIfNotSmi(Register value, Label* not_smi_label) {
  tst(value, Operand(kSmiTagMask));
  b(ne, not_smi_label);
}

void MacroAssembler::CheckFor32DRegs(Register scratch) {
  ASM_CODE_COMMENT(this);
  Move(scratch, ExternalReference::cpu_features());
  ldr(scratch, MemOperand(scratch));
  tst(scratch, Operand(1u << VFP32DREGS));
}

void MacroAssembler::SaveFPRegs(Register location, Register scratch) {
  ASM_CODE_COMMENT(this);
  CpuFeatureScope scope(this, VFP32DREGS, CpuFeatureScope::kDontCheckSupported);
  CheckFor32DRegs(scratch);
  vstm(db_w, location, d16, d31, ne);
  sub(location, location, Operand(16 * kDoubleSize), LeaveCC, eq);
  vstm(db_w, location, d0, d15);
}

void MacroAssembler::RestoreFPRegs(Register location, Register scratch) {
  ASM_CODE_COMMENT(this);
  CpuFeatureScope scope(this, VFP32DREGS, CpuFeatureScope::kDontCheckSupported);
  CheckFor32DRegs(scratch);
  vldm(ia_w, location, d0, d15);
  vldm(ia_w, location, d16, d31, ne);
  add(location, location, Operand(16 * kDoubleSize), LeaveCC, eq);
}

void MacroAssembler::SaveFPRegsToHeap(Register location, Register scratch) {
  ASM_CODE_COMMENT(this);
  CpuFeatureScope scope(this, VFP32DREGS, CpuFeatureScope::kDontCheckSupported);
  CheckFor32DRegs(scratch);
  vstm(ia_w, location, d0, d15);
  vstm(ia_w, location, d16, d31, ne);
  add(location, location, Operand(16 * kDoubleSize), LeaveCC, eq);
}

void MacroAssembler::RestoreFPRegsFromHeap(Register location,
                                           Register scratch) {
  ASM_CODE_COMMENT(this);
  CpuFeatureScope scope(this, VFP32DREGS, CpuFeatureScope::kDontCheckSupported);
  CheckFor32DRegs(scratch);
  vldm(ia_w, location, d0, d15);
  vldm(ia_w, location, d16, d31, ne);
  add(location, location, Operand(16 * kDoubleSize), LeaveCC, eq);
}

template <typename T>
void MacroAssembler::FloatMaxHelper(T result, T left, T right,
                                    Label* out_of_line) {
  // This trivial case is caught sooner, so that the out-of-line code can be
  // completely avoided.
  DCHECK(left != right);

  if (CpuFeatures::IsSupported(ARMv8)) {
    CpuFeatureScope scope(this, ARMv8);
    VFPCompareAndSetFlags(left, right);
    b(vs, out_of_line);
    vmaxnm(result, left, right);
  } else {
    Label done;
    VFPCompareAndSetFlags(left, right);
    b(vs, out_of_line);
    // Avoid a conditional instruction if the result register is unique.
    bool aliased_result_reg = result == left || result == right;
    Move(result, right, aliased_result_reg ? mi : al);
    Move(result, left, gt);
    b(ne, &done);
    // Left and right are equal, but check for +/-0.
    VFPCompareAndSetFlags(left, 0.0);
    b(eq, out_of_line);
    // The arguments are equal and not zero, so it doesn't matter which input we
    // pick. We have already moved one input into the result (if it didn't
    // already alias) so there's nothing more to do.
    bind(&done);
  }
}

template <typename T>
void MacroAssembler::FloatMaxOutOfLineHelper(T result, T left, T right) {
  DCHECK(left != right);

  // ARMv8: At least one of left and right is a NaN.
  // Anything else: At least one of left and right is a NaN, or both left and
  // right are zeroes with unknown sign.

  // If left and right are +/-0, select the one with the most positive sign.
  // If left or right are NaN, vadd propagates the appropriate one.
  vadd(result, left, right);
}

template <typename T>
void MacroAssembler::FloatMinHelper(T result, T left, T right,
                                    Label* out_of_line) {
  // This trivial case is caught sooner, so that the out-of-line code can be
  // completely avoided.
  DCHECK(left != right);

  if (CpuFeatures::IsSupported(ARMv8)) {
    CpuFeatureScope scope(this, ARMv8);
    VFPCompareAndSetFlags(left, right);
    b(vs, out_of_line);
    vminnm(result, left, right);
  } else {
    Label done;
    VFPCompareAndSetFlags(left, right);
    b(vs, out_of_line);
    // Avoid a conditional instruction if the result register is unique.
    bool aliased_result_reg = result == left || result == right;
    Move(result, left, aliased_result_reg ? mi : al);
    Move(result, right, gt);
    b(ne, &done);
    // Left and right are equal, but check for +/-0.
    VFPCompareAndSetFlags(left, 0.0);
    // If the arguments are equal and not zero, it doesn't matter which input we
    // pick. We have already moved one input into the result (if it didn't
    // already alias) so there's nothing more to do.
    b(ne, &done);
    // At this point, both left and right are either 0 or -0.
    // We could use a single 'vorr' instruction here if we had NEON support.
    // The algorithm used is -((-L) + (-R)), which is most efficiently expressed
    // as -((-L) - R).
    if (left == result) {
      DCHECK(right != result);
      vneg(result, left);
      vsub(result, result, right);
      vneg(result, result);
    } else {
      DCHECK(left != result);
      vneg(result, right);
      vsub(result, result, left);
      vneg(result, result);
    }
    bind(&done);
  }
}

template <typename T>
void MacroAssembler::FloatMinOutOfLineHelper(T result, T left, T right) {
  DCHECK(left != right);

  // At least one of left and right is a NaN. Use vadd to propagate the NaN
  // appropriately. +/-0 is handled inline.
  vadd(result, left, right);
}

void MacroAssembler::FloatMax(SwVfpRegister result, SwVfpRegister left,
                              SwVfpRegister right, Label* out_of_line) {
  FloatMaxHelper(result, left, right, out_of_line);
}

void MacroAssembler::FloatMin(SwVfpRegister result, SwVfpRegister left,
                              SwVfpRegister right, Label* out_of_line) {
  FloatMinHelper(result, left, right, out_of_line);
}

void MacroAssembler::FloatMax(DwVfpRegister result, DwVfpRegister left,
                              DwVfpRegister right, Label* out_of_line) {
  FloatMaxHelper(result, left, right, out_of_line);
}

void MacroAssembler::FloatMin(DwVfpRegister result, DwVfpRegister left,
                              DwVfpRegister right, Label* out_of_line) {
  FloatMinHelper(result, left, right, out_of_line);
}

void MacroAssembler::FloatMaxOutOfLine(SwVfpRegister result, SwVfpRegister left,
                                       SwVfpRegister right) {
  FloatMaxOutOfLineHelper(result, left, right);
}

void MacroAssembler::FloatMinOutOfLine(SwVfpRegister result, SwVfpRegister left,
                                       SwVfpRegister right) {
  FloatMinOutOfLineHelper(result, left, right);
}

void MacroAssembler::FloatMaxOutOfLine(DwVfpRegister result, DwVfpRegister left,
                                       DwVfpRegister right) {
  FloatMaxOutOfLineHelper(result, left, right);
}

void MacroAssembler::FloatMinOutOfLine(DwVfpRegister result, DwVfpRegister left,
                                       DwVfpRegister right) {
  FloatMinOutOfLineHelper(result, left, right);
}

static const int kRegisterPassedArguments = 4;
// The hardfloat calling convention passes double arguments in registers d0-d7.
static const int kDoubleRegisterPassedArguments = 8;

int MacroAssembler::CalculateStackPassedWords(int num_reg_arguments,
                                              int num_double_arguments) {
  int stack_passed_words = 0;
  if (use_eabi_hardfloat()) {
    // In the hard floating point calling convention, we can use the first 8
    // registers to pass doubles.
    if (num_double_arguments > kDoubleRegisterPassedArguments) {
      stack_passed_words +=
          2 * (num_double_arguments - kDoubleRegisterPassedArguments);
    }
  } else {
    // In the soft floating point calling convention, every double
    // argument is passed using two registers.
    num_reg_arguments += 2 * num_double_arguments;
  }
  // Up to four simple arguments are passed in registers r0..r3.
  if (num_reg_arguments > kRegisterPassedArguments) {
    stack_passed_words += num_reg_arguments - kRegisterPassedArguments;
  }
  return stack_passed_words;
}

void MacroAssembler::PrepareCallCFunction(int num_reg_arguments,
                                          int num_double_arguments,
                                          Register scratch) {
  ASM_CODE_COMMENT(this);
  int frame_alignment = ActivationFrameAlignment();
  int stack_passed_arguments =
      CalculateStackPassedWords(num_reg_arguments, num_double_arguments);
  if (frame_alignment > kPointerSize) {
    UseScratchRegisterScope temps(this);
    if (!scratch.is_valid()) scratch = temps.Acquire();
    // Make stack end at alignment and make room for num_arguments - 4 words
    // and the original value of sp.
    mov(scratch, sp);
    AllocateStackSpace((stack_passed_arguments + 1) * kPointerSize);
    DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
    and_(sp, sp, Operand(-frame_alignment));
    str(scratch, MemOperand(sp, stack_passed_arguments * kPointerSize));
  } else if (stack_passed_arguments > 0) {
    AllocateStackSpace(stack_passed_arguments * kPointerSize);
  }
}

void MacroAssembler::MovToFloatParameter(DwVfpRegister src) {
  DCHECK(src == d0);
  if (!use_eabi_hardfloat()) {
    vmov(r0, r1, src);
  }
}

// On ARM this is just a synonym to make the purpose clear.
void MacroAssembler::MovToFloatResult(DwVfpRegister src) {
  MovToFloatParameter(src);
}

void MacroAssembler::MovToFloatParameters(DwVfpRegister src1,
                                          DwVfpRegister src2) {
  DCHECK(src1 == d0);
  DCHECK(src2 == d1);
  if (!use_eabi_hardfloat()) {
    vmov(r0, r1, src1);
    vmov(r2, r3, src2);
  }
}

void MacroAssembler::CallCFunction(ExternalReference function,
                                   int num_reg_arguments,
                                   int num_double_arguments,
                                   SetIsolateDataSlots set_isolate_data_slots) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Move(scratch, function);
  CallCFunctionHelper(scratch, num_reg_arguments, num_double_arguments,
                      set_isolate_data_slots);
}

void MacroAssembler::CallCFunction(Register function, int num_reg_arguments,
                                   int num_double_arguments,
                                   SetIsolateDataSlots set_isolate_data_slots) {
  CallCFunctionHelper(function, num_reg_arguments, num_double_arguments,
                      set_isolate_data_slots);
}

void MacroAssembler::CallCFunction(ExternalReference function,
                                   int num_arguments,
                                   SetIsolateDataSlots set_isolate_data_slots) {
  CallCFunction(function, num_arguments, 0, set_isolate_data_slots);
}

void MacroAssembler::CallCFunction(Register function, int num_arguments,
                                   SetIsolateDataSlots set_isolate_data_slots) {
  CallCFunction(function, num_arguments, 0, set_isolate_data_slots);
}

void MacroAssembler::CallCFunctionHelper(
    Register function, int num_reg_arguments, int num_double_arguments,
    SetIsolateDataSlots set_isolate_data_slots) {
  ASM_CODE_COMMENT(this);
  DCHECK_LE(num_reg_arguments + num_double_arguments, kMaxCParameters);
  DCHECK(has_frame());
  // Make sure that the stack is aligned before calling a C function unless
  // running in the simulator. The simulator has its own alignment check which
  // provides more information.
#if V8_HOST_ARCH_ARM
  if (v8_flags.debug_code) {
    int frame_alignment = base::OS::ActivationFrameAlignment();
    int frame_alignment_mask = frame_alignment - 1;
    if (frame_alignment > kPointerSize) {
      ASM_CODE_COMMENT_STRING(this, "Check stack alignment");
      DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
      Label alignment_as_expected;
      tst(sp, Operand(frame_alignment_mask));
      b(eq, &alignment_as_expected);
      // Don't use Check here, as it will call Runtime_Abort possibly
      // re-entering here.
      stop();
      bind(&alignment_as_expected);
    }
  }
#endif

  if (set_isolate_data_slots == SetIsolateDataSlots::kYes) {
    // Save the frame pointer and PC so that the stack layout remains iterable,
    // even without an ExitFrame which normally exists between JS and C frames.
    // See x64 code for reasoning about how to address the isolate data fields.
    if (root_array_available()) {
      str(pc, MemOperand(kRootRegister,
                         IsolateData::fast_c_call_caller_pc_offset()));
      str(fp, MemOperand(kRootRegister,
                         IsolateData::fast_c_call_caller_fp_offset()));
    } else {
      DCHECK_NOT_NULL(isolate());
      Register addr_scratch = r4;
      Push(addr_scratch);

      Move(addr_scratch,
           ExternalReference::fast_c_call_caller_pc_address(isolate()));
      str(pc, MemOperand(addr_scratch));
      Move(addr_scratch,
           ExternalReference::fast_c_call_caller_fp_address(isolate()));
      str(fp, MemOperand(addr_scratch));

      Pop(addr_scratch);
    }
  }

  // Just call directly. The function called cannot cause a GC, or
  // allow preemption, so the return address in the link register
  // stays correct.
  Call(function);

  if (set_isolate_data_slots == SetIsolateDataSlots::kYes) {
    // We don't unset the PC; the FP is the source of truth.
    Register zero_scratch = r5;
    Push(zero_scratch);
    mov(zero_scratch, Operand::Zero());

    if (root_array_available()) {
      str(zero_scratch,
          MemOperand(kRootRegister,
                     IsolateData::fast_c_call_caller_fp_offset()));
    } else {
      DCHECK_NOT_NULL(isolate());
      Register addr_scratch = r4;
      Push(addr_scratch);
      Move(addr_scratch,
           ExternalReference::fast_c_call_caller_fp_address(isolate()));
      str(zero_scratch, MemOperand(addr_scratch));
      Pop(addr_scratch);
    }

    Pop(zero_scratch);
  }

  int stack_passed_arguments =
      CalculateStackPassedWords(num_reg_arguments, num_double_arguments);
  if (ActivationFrameAlignment() > kPointerSize) {
    ldr(sp, MemOperand(sp, stack_passed_arguments * kPointerSize));
  } else {
    add(sp, sp, Operand(stack_passed_arguments * kPointerSize));
  }
}

void MacroAssembler::CheckPageFlag(Register object, int mask, Condition cc,
                                   Label* condition_met) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  DCHECK(!AreAliased(object, scratch));
  DCHECK(cc == eq || cc == ne);
  Bfc(scratch, object, 0, kPageSizeBits);
  ldr(scratch, MemOperand(scratch, BasicMemoryChunk::kFlagsOffset));
  tst(scratch, Operand(mask));
  b(cc, condition_met);
}

Register GetRegisterThatIsNotOneOf(Register reg1, Register reg2, Register reg3,
                                   Register reg4, Register reg5,
                                   Register reg6) {
  RegList regs = {reg1, reg2, reg3, reg4, reg5, reg6};

  const RegisterConfiguration* config = RegisterConfiguration::Default();
  for (int i = 0; i < config->num_allocatable_general_registers(); ++i) {
    int code = config->GetAllocatableGeneralCode(i);
    Register candidate = Register::from_code(code);
    if (regs.has(candidate)) continue;
    return candidate;
  }
  UNREACHABLE();
}

void MacroAssembler::ComputeCodeStartAddress(Register dst) {
  ASM_CODE_COMMENT(this);
  // We can use the register pc - 8 for the address of the current instruction.
  sub(dst, pc, Operand(pc_offset() + Instruction::kPcLoadDelta));
}

void MacroAssembler::CallForDeoptimization(Builtin target, int, Label* exit,
                                           DeoptimizeKind kind, Label* ret,
                                           Label*) {
  ASM_CODE_COMMENT(this);

  // All constants should have been emitted prior to deoptimization exit
  // emission. See PrepareForDeoptimizationExits.
  DCHECK(!has_pending_constants());
  BlockConstPoolScope block_const_pool(this);

  CHECK_LE(target, Builtins::kLastTier0);
  ldr(ip,
      MemOperand(kRootRegister, IsolateData::BuiltinEntrySlotOffset(target)));
  Call(ip);
  DCHECK_EQ(SizeOfCodeGeneratedSince(exit),
            (kind == DeoptimizeKind::kLazy) ? Deoptimizer::kLazyDeoptExitSize
                                            : Deoptimizer::kEagerDeoptExitSize);

  // The above code must not emit constants either.
  DCHECK(!has_pending_constants());
}

void MacroAssembler::Trap() { stop(); }
void MacroAssembler::DebugBreak() { stop(); }

void MacroAssembler::I64x2BitMask(Register dst, QwNeonRegister src) {
  UseScratchRegisterScope temps(this);
  QwNeonRegister tmp1 = temps.AcquireQ();
  Register tmp = temps.Acquire();

  vshr(NeonU64, tmp1, src, 63);
  vmov(NeonU32, dst, tmp1.low(), 0);
  vmov(NeonU32, tmp, tmp1.high(), 0);
  add(dst, dst, Operand(tmp, LSL, 1));
}

void MacroAssembler::I64x2Eq(QwNeonRegister dst, QwNeonRegister src1,
                             QwNeonRegister src2) {
  UseScratchRegisterScope temps(this);
  Simd128Register scratch = temps.AcquireQ();
  vceq(Neon32, dst, src1, src2);
  vrev64(Neon32, scratch, dst);
  vand(dst, dst, scratch);
}

void MacroAssembler::I64x2Ne(QwNeonRegister dst, QwNeonRegister src1,
                             QwNeonRegister src2) {
  UseScratchRegisterScope temps(this);
  Simd128Register tmp = temps.AcquireQ();
  vceq(Neon32, dst, src1, src2);
  vrev64(Neon32, tmp, dst);
  vmvn(dst, dst);
  vorn(dst, dst, tmp);
}

void MacroAssembler::I64x2GtS(QwNeonRegister dst, QwNeonRegister src1,
                              QwNeonRegister src2) {
  ASM_CODE_COMMENT(this);
  vqsub(NeonS64, dst, src2, src1);
  vshr(NeonS64, dst, dst, 63);
}

void MacroAssembler::I64x2GeS(QwNeonRegister dst, QwNeonRegister src1,
                              QwNeonRegister src2) {
  ASM_CODE_COMMENT(this);
  vqsub(NeonS64, dst, src1, src2);
  vshr(NeonS64, dst, dst, 63);
  vmvn(dst, dst);
}

void MacroAssembler::I64x2AllTrue(Register dst, QwNeonRegister src) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  QwNeonRegister tmp = temps.AcquireQ();
  // src = | a | b | c | d |
  // tmp = | max(a,b) | max(c,d) | ...
  vpmax(NeonU32, tmp.low(), src.low(), src.high());
  // tmp = | max(a,b) == 0 | max(c,d) == 0 | ...
  vceq(Neon32, tmp, tmp, 0);
  // tmp = | max(a,b) == 0 or max(c,d) == 0 | ...
  vpmax(NeonU32, tmp.low(), tmp.low(), tmp.low());
  // dst = (max(a,b) == 0 || max(c,d) == 0)
  // dst will either be -1 or 0.
  vmov(NeonS32, dst, tmp.low(), 0);
  // dst = !dst (-1 -> 0, 0 -> 1)
  add(dst, dst, Operand(1));
  // This works because:
  // !dst
  // = !(max(a,b) == 0 || max(c,d) == 0)
  // = max(a,b) != 0 && max(c,d) != 0
  // = (a != 0 || b != 0) && (c != 0 || d != 0)
  // = defintion of i64x2.all_true.
}

void MacroAssembler::I64x2Abs(QwNeonRegister dst, QwNeonRegister src) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  Simd128Register tmp = temps.AcquireQ();
  vshr(NeonS64, tmp, src, 63);
  veor(dst, src, tmp);
  vsub(Neon64, dst, dst, tmp);
}

namespace {
using AssemblerFunc = void (Assembler::*)(DwVfpRegister, SwVfpRegister,
                                          VFPConversionMode, const Condition);
// Helper function for f64x2 convert low instructions.
// This ensures that we do not overwrite src, if dst == src.
void F64x2ConvertLowHelper(Assembler* assm, QwNeonRegister dst,
                           QwNeonRegister src, AssemblerFunc convert_fn) {
  LowDwVfpRegister src_d = LowDwVfpRegister::from_code(src.low().code());
  UseScratchRegisterScope temps(assm);
  if (dst == src) {
    LowDwVfpRegister tmp = temps.AcquireLowD();
    assm->vmov(tmp, src_d);
    src_d = tmp;
  }
  // Default arguments are not part of the function type
  (assm->*convert_fn)(dst.low(), src_d.low(), kDefaultRoundToZero, al);
  (assm->*convert_fn)(dst.high(), src_d.high(), kDefaultRoundToZero, al);
}
}  // namespace

void MacroAssembler::F64x2ConvertLowI32x4S(QwNeonRegister dst,
                                           QwNeonRegister src) {
  F64x2ConvertLowHelper(this, dst, src, &Assembler::vcvt_f64_s32);
}

void MacroAssembler::F64x2ConvertLowI32x4U(QwNeonRegister dst,
                                           QwNeonRegister src) {
  F64x2ConvertLowHelper(this, dst, src, &Assembler::vcvt_f64_u32);
}

void MacroAssembler::F64x2PromoteLowF32x4(QwNeonRegister dst,
                                          QwNeonRegister src) {
  F64x2ConvertLowHelper(this, dst, src, &Assembler::vcvt_f64_f32);
}

}  // namespace internal
}  // namespace v8

#undef __

#endif  // V8_TARGET_ARCH_ARM

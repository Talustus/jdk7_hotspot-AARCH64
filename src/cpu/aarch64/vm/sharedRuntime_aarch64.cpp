/*
 * Copyright (c) 2003, 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/debugInfoRec.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interpreter.hpp"
#include "oops/compiledICHolder.hpp"
#include "prims/jvmtiRedefineClassesTrace.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/vframeArray.hpp"
#include "vmreg_aarch64.inline.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#endif
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif

#define __ masm->

const int StackAlignmentInSlots = StackAlignmentInBytes / VMRegImpl::stack_slot_size;

class SimpleRuntimeFrame {

  public:

  // Most of the runtime stubs have this simple frame layout.
  // This class exists to make the layout shared in one place.
  // Offsets are for compiler stack slots, which are jints.
  enum layout {
    // The frame sender code expects that rbp will be in the "natural" place and
    // will override any oopMap setting for it. We must therefore force the layout
    // so that it agrees with the frame sender code.
    rbp_off = frame::arg_reg_save_area_bytes/BytesPerInt,
    rbp_off2,
    return_off, return_off2,
    framesize
  };
};

// FIXME -- this is used by C1
class RegisterSaver {
 public:
  static OopMap* save_live_registers(MacroAssembler* masm, int additional_frame_words, int* total_frame_words);
  static void restore_live_registers(MacroAssembler* masm);

  // Offsets into the register save area
  // Used by deoptimization when it is managing result register
  // values on its own

  static int r0_offset_in_bytes(void)    { return (32 + r0->encoding()) * wordSize; }
  static int reg_offset_in_bytes(Register r)    { return r0_offset_in_bytes() + r->encoding() * wordSize; }
  static int rmethod_offset_in_bytes(void)    { return reg_offset_in_bytes(rmethod); }
  static int rscratch1_offset_in_bytes(void)    { return (32 + rscratch1->encoding()) * wordSize; }
  static int v0_offset_in_bytes(void)   { return 0; }
  static int return_offset_in_bytes(void) { return (32 /* floats*/ + 32 /* gregs*/ + 1) * wordSize; }

  // During deoptimization only the result registers need to be restored,
  // all the other values have already been extracted.
  static void restore_result_registers(MacroAssembler* masm);

    // Capture info about frame layout
  enum layout {
                fpu_state_off = 0,
                fpu_state_end = fpu_state_off+FPUStateSizeInWords-1,
                // The frame sender code expects that rfp will be in
                // the "natural" place and will override any oopMap
                // setting for it. We must therefore force the layout
                // so that it agrees with the frame sender code.
		r0_off = fpu_state_off+FPUStateSizeInWords,
                rfp_off = r0_off + 30 * 2,
                return_off = rfp_off + 2,      // slot for return address
                reg_save_size = return_off + 2};

};

OopMap* RegisterSaver::save_live_registers(MacroAssembler* masm, int additional_frame_words, int* total_frame_words) {
  int frame_size_in_bytes = round_to(additional_frame_words*wordSize +
                                     reg_save_size*BytesPerInt, 16);
  // OopMap frame size is in compiler stack slots (jint's) not bytes or words
  int frame_size_in_slots = frame_size_in_bytes / BytesPerInt;
  // The caller will allocate additional_frame_words
  int additional_frame_slots = additional_frame_words*wordSize / BytesPerInt;
  // CodeBlob frame size is in words.
  int frame_size_in_words = frame_size_in_bytes / wordSize;
  *total_frame_words = frame_size_in_words;

  // Save registers, fpu state, and flags.

  __ enter();
  __ push_CPU_state();

  // Set an oopmap for the call site.  This oopmap will map all
  // oop-registers and debug-info registers as callee-saved.  This
  // will allow deoptimization at this safepoint to find all possible
  // debug-info recordings, as well as let GC find all oops.

  OopMapSet *oop_maps = new OopMapSet();
  OopMap* oop_map = new OopMap(frame_size_in_slots, 0);

  for (int i = 0; i < FrameMap::nof_cpu_regs; i++) {
    Register r = as_Register(i);
    if (r < rheapbase && r != rscratch1 && r != rscratch2) {
      int sp_offset = 2 * (i + 32); // SP offsets are in 4-byte words,
				    // register slots are 8 bytes
				    // wide, 32 floating-point
				    // registers
      oop_map->set_callee_saved(VMRegImpl::stack2reg(sp_offset),
                                r->as_VMReg());
    }
  }

  for (int i = 0; i < FrameMap::nof_fpu_regs; i++) {
    FloatRegister r = as_FloatRegister(i);
    int sp_offset = 2 * i;
    oop_map->set_callee_saved(VMRegImpl::stack2reg(sp_offset),
			      r->as_VMReg());
  }

  return oop_map;
}

void RegisterSaver::restore_live_registers(MacroAssembler* masm) {
  __ pop_CPU_state();
  __ leave();
}

void RegisterSaver::restore_result_registers(MacroAssembler* masm) {

  // Just restore result register. Only used by deoptimization. By
  // now any callee save register that needs to be restored to a c2
  // caller of the deoptee has been extracted into the vframeArray
  // and will be stuffed into the c2i adapter we create for later
  // restoration so only result registers need to be restored here.

  // Restore fp result register
  __ ldrd(v0, Address(sp, v0_offset_in_bytes()));
  // Restore integer result register
  __ ldr(r0, Address(sp, r0_offset_in_bytes()));

  // Pop all of the register save are off the stack
  __ add(sp, sp, round_to(return_offset_in_bytes(), 16));
}

// Is vector's size (in bytes) bigger than a size saved by default?
// 16 bytes XMM registers are saved by default using fxsave/fxrstor instructions.
bool SharedRuntime::is_wide_vector(int size) {
  return size > 16;
}
// The java_calling_convention describes stack locations as ideal slots on
// a frame with no abi restrictions. Since we must observe abi restrictions
// (like the placement of the register window) the slots must be biased by
// the following value.
static int reg2offset_in(VMReg r) { Unimplemented(); return 0; }

static int reg2offset_out(VMReg r) { Unimplemented(); return 0; }

// ---------------------------------------------------------------------------
// Read the array of BasicTypes from a signature, and compute where the
// arguments should go.  Values in the VMRegPair regs array refer to 4-byte
// quantities.  Values less than VMRegImpl::stack0 are registers, those above
// refer to 4-byte stack slots.  All stack slots are based off of the stack pointer
// as framesizes are fixed.
// VMRegImpl::stack0 refers to the first slot 0(sp).
// and VMRegImpl::stack0+1 refers to the memory word 4-byes higher.  Register
// up to RegisterImpl::number_of_registers) are the 64-bit
// integer registers.

// Note: the INPUTS in sig_bt are in units of Java argument words,
// which are 64-bit.  The OUTPUTS are in 32-bit units.

// The Java calling convention is a "shifted" version of the C ABI.
// By skipping the first C ABI register we can call non-static jni
// methods with small numbers of arguments without having to shuffle
// the arguments at all. Since we control the java ABI we ought to at
// least get some advantage out of it.

int SharedRuntime::java_calling_convention(const BasicType *sig_bt,
                                           VMRegPair *regs,
                                           int total_args_passed,
                                           int is_outgoing) {

  // Create the mapping between argument positions and
  // registers.
  static const Register INT_ArgReg[Argument::n_int_register_parameters_j] = {
    j_rarg0, j_rarg1, j_rarg2, j_rarg3, j_rarg4, j_rarg5, j_rarg6, j_rarg7
  };
  static const FloatRegister FP_ArgReg[Argument::n_float_register_parameters_j] = {
    j_farg0, j_farg1, j_farg2, j_farg3,
    j_farg4, j_farg5, j_farg6, j_farg7
  };


  uint int_args = 0;
  uint fp_args = 0;
  uint stk_args = 0; // inc by 2 each time

  for (int i = 0; i < total_args_passed; i++) {
    switch (sig_bt[i]) {
    case T_BOOLEAN:
    case T_CHAR:
    case T_BYTE:
    case T_SHORT:
    case T_INT:
      if (int_args < Argument::n_int_register_parameters_j) {
        regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_VOID:
      // halves of T_LONG or T_DOUBLE
      assert(i != 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "expecting half");
      regs[i].set_bad();
      break;
    case T_LONG:
      assert(sig_bt[i + 1] == T_VOID, "expecting half");
      // fall through
    case T_OBJECT:
    case T_ARRAY:
    case T_ADDRESS:
      if (int_args < Argument::n_int_register_parameters_j) {
        regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_FLOAT:
      if (fp_args < Argument::n_float_register_parameters_j) {
        regs[i].set1(FP_ArgReg[fp_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_DOUBLE:
      assert(sig_bt[i + 1] == T_VOID, "expecting half");
      if (fp_args < Argument::n_float_register_parameters_j) {
        regs[i].set2(FP_ArgReg[fp_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    default:
      ShouldNotReachHere();
      break;
    }
  }

  return round_to(stk_args, 2);
}

// Patch the callers callsite with entry to compiled code if it exists.
static void patch_callers_callsite(MacroAssembler *masm) {
  Label L;
  __ ldr(rscratch1, Address(rmethod, in_bytes(Method::code_offset())));
  __ cbz(rscratch1, L);

  // Schedule the branch target address early.
  // Call into the VM to patch the caller, then jump to compiled callee
  // rax isn't live so capture return address while we easily can
  __ mov(r0, lr);

  __ push_CPU_state();

  // VM needs caller's callsite
  // VM needs target method
  // This needs to be a long call since we will relocate this adapter to
  // the codeBuffer and it may not reach

  // Allocate argument register save area
  if (frame::arg_reg_save_area_bytes != 0) {
    __ sub(sp, sp, frame::arg_reg_save_area_bytes);
  }

  __ mov(rscratch1, c_rarg1);
  __ mov(c_rarg1, r0);
  __ mov(c_rarg0, rscratch1);
  __ mov(rscratch1, RuntimeAddress(CAST_FROM_FN_PTR(address, SharedRuntime::fixup_callers_callsite)));
  __ brx86(rscratch1, 2, 0, 0);

  // De-allocate argument register save area
  if (frame::arg_reg_save_area_bytes != 0) {
    __ add(sp, sp, frame::arg_reg_save_area_bytes);
  }

  __ pop_CPU_state();
  // restore sp
  __ bind(L);
}

static void gen_c2i_adapter(MacroAssembler *masm,
                            int total_args_passed,
                            int comp_args_on_stack,
                            const BasicType *sig_bt,
                            const VMRegPair *regs,
                            Label& skip_fixup) {
  // Before we get into the guts of the C2I adapter, see if we should be here
  // at all.  We've come from compiled code and are attempting to jump to the
  // interpreter, which means the caller made a static call to get here
  // (vcalls always get a compiled target if there is one).  Check for a
  // compiled target.  If there is one, we need to patch the caller's call.
  patch_callers_callsite(masm);

  __ bind(skip_fixup);

  int words_pushed = 0;

  // Since all args are passed on the stack, total_args_passed *
  // Interpreter::stackElementSize is the space we need.

  int extraspace = total_args_passed * Interpreter::stackElementSize;

  __ mov(r13, sp);

  // stack is aligned, keep it that way
  extraspace = round_to(extraspace, 2*wordSize);

  if (extraspace)
    __ sub(sp, sp, extraspace);

  // Now write the args into the outgoing interpreter space
  for (int i = 0; i < total_args_passed; i++) {
    if (sig_bt[i] == T_VOID) {
      assert(i > 0 && (sig_bt[i-1] == T_LONG || sig_bt[i-1] == T_DOUBLE), "missing half");
      continue; 
    }

    // offset to start parameters
    int st_off   = (total_args_passed - i - 1) * Interpreter::stackElementSize;
    int next_off = st_off - Interpreter::stackElementSize;

    // Say 4 args:
    // i   st_off
    // 0   32 T_LONG
    // 1   24 T_VOID
    // 2   16 T_OBJECT
    // 3    8 T_BOOL
    // -    0 return address
    //
    // However to make thing extra confusing. Because we can fit a long/double in
    // a single slot on a 64 bt vm and it would be silly to break them up, the interpreter
    // leaves one slot empty and only stores to a single slot. In this case the
    // slot that is occupied is the T_VOID slot. See I said it was confusing.

    VMReg r_1 = regs[i].first();
    VMReg r_2 = regs[i].second();
    if (!r_1->is_valid()) {
      assert(!r_2->is_valid(), "");
      continue;
    }
    if (r_1->is_stack()) {
      // memory to memory use r0
      int ld_off = (r_1->reg2stack() * VMRegImpl::stack_slot_size
		    + extraspace
		    + words_pushed * wordSize);
      if (!r_2->is_valid()) {
        // sign extend??
        __ ldrw(r0, Address(sp, ld_off));
        __ str(r0, Address(sp, st_off));

      } else {

        __ ldr(r0, Address(sp, ld_off));

        // Two VMREgs|OptoRegs can be T_OBJECT, T_ADDRESS, T_DOUBLE, T_LONG
        // T_DOUBLE and T_LONG use two slots in the interpreter
        if ( sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) {
          // ld_off == LSW, ld_off+wordSize == MSW
          // st_off == MSW, next_off == LSW
#ifdef ASSERT
          // Overwrite the unused slot with known junk
          __ mov(r0, 0xdeadffffdeadaaaaul);
          __ str(r0, Address(sp, st_off));
#endif /* ASSERT */
          __ str(r0, Address(sp, next_off));
        } else {
          __ str(r0, Address(sp, st_off));
        }
      }
    } else if (r_1->is_Register()) {
      Register r = r_1->as_Register();
      if (!r_2->is_valid()) {
        // must be only an int (or less ) so move only 32bits to slot
        // why not sign extend??
        __ str(r, Address(sp, st_off));
      } else {
        // Two VMREgs|OptoRegs can be T_OBJECT, T_ADDRESS, T_DOUBLE, T_LONG
        // T_DOUBLE and T_LONG use two slots in the interpreter
        if ( sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) {
          // long/double in gpr
#ifdef ASSERT
          // Overwrite the unused slot with known junk
          __ mov(r0, 0xdeadffffdeadaaabul);
          __ str(r0, Address(sp, st_off));
#endif /* ASSERT */
          __ str(r, Address(sp, next_off));
        } else {
          __ str(r, Address(sp, st_off));
        }
      }
    } else {
      assert(r_1->is_FloatRegister(), "");
      if (!r_2->is_valid()) {
        // only a float use just part of the slot
        __ strs(r_1->as_FloatRegister(), Address(sp, st_off));
      } else {
#ifdef ASSERT
        // Overwrite the unused slot with known junk
        __ mov(r0, 0xdeadffffdeadaaacul);
        __ str(r0, Address(sp, st_off));
#endif /* ASSERT */
        __ strd(r_1->as_FloatRegister(), Address(sp, next_off));
      }
    }
  }

  __ mov(esp, sp); // Interp expects args on caller's expression stack

  __ ldr(rscratch1, Address(rmethod, in_bytes(Method::interpreter_entry_offset())));
  __ br(rscratch1);
}


static void gen_i2c_adapter(MacroAssembler *masm,
                            int total_args_passed,
                            int comp_args_on_stack,
                            const BasicType *sig_bt,
                            const VMRegPair *regs) {

  // Note: r13 contains the senderSP on entry. We must preserve it since
  // we may do a i2c -> c2i transition if we lose a race where compiled
  // code goes non-entrant while we get args ready.
  // In addition we use r13 to locate all the interpreter args as
  // we must align the stack to 16 bytes on an i2c entry else we
  // lose alignment we expect in all compiled code and register
  // save code can segv when fxsave instructions find improperly
  // aligned stack pointer.

  // Adapters can be frameless because they do not require the caller
  // to perform additional cleanup work, such as correcting the stack pointer.
  // An i2c adapter is frameless because the *caller* frame, which is interpreted,
  // routinely repairs its own stack pointer (from interpreter_frame_last_sp),
  // even if a callee has modified the stack pointer.
  // A c2i adapter is frameless because the *callee* frame, which is interpreted,
  // routinely repairs its caller's stack pointer (from sender_sp, which is set
  // up via the senderSP register).
  // In other words, if *either* the caller or callee is interpreted, we can
  // get the stack pointer repaired after a call.
  // This is why c2i and i2c adapters cannot be indefinitely composed.
  // In particular, if a c2i adapter were to somehow call an i2c adapter,
  // both caller and callee would be compiled methods, and neither would
  // clean up the stack pointer changes performed by the two adapters.
  // If this happens, control eventually transfers back to the compiled
  // caller, but with an uncorrected stack, causing delayed havoc.

  if (VerifyAdapterCalls &&
      (Interpreter::code() != NULL || StubRoutines::code1() != NULL)) {
#if 0
    // So, let's test for cascading c2i/i2c adapters right now.
    //  assert(Interpreter::contains($return_addr) ||
    //         StubRoutines::contains($return_addr),
    //         "i2c adapter must return to an interpreter frame");
    __ block_comment("verify_i2c { ");
    Label L_ok;
    if (Interpreter::code() != NULL)
      range_check(masm, rax, r11,
                  Interpreter::code()->code_start(), Interpreter::code()->code_end(),
                  L_ok);
    if (StubRoutines::code1() != NULL)
      range_check(masm, rax, r11,
                  StubRoutines::code1()->code_begin(), StubRoutines::code1()->code_end(),
                  L_ok);
    if (StubRoutines::code2() != NULL)
      range_check(masm, rax, r11,
                  StubRoutines::code2()->code_begin(), StubRoutines::code2()->code_end(),
                  L_ok);
    const char* msg = "i2c adapter must return to an interpreter frame";
    __ block_comment(msg);
    __ stop(msg);
    __ bind(L_ok);
    __ block_comment("} verify_i2ce ");
#endif
  }

  // Cut-out for having no stack args.
  int comp_words_on_stack = round_to(comp_args_on_stack*VMRegImpl::stack_slot_size, wordSize)>>LogBytesPerWord;
  if (comp_args_on_stack) {
    __ sub(rscratch1, sp, comp_words_on_stack * wordSize);
    __ andr(sp, rscratch1, -16);
  }

  // Will jump to the compiled code just as if compiled code was doing it.
  // Pre-load the register-jump target early, to schedule it better.
  __ ldr(rscratch1, Address(rmethod, in_bytes(Method::from_compiled_offset())));

  // Now generate the shuffle code.
  for (int i = 0; i < total_args_passed; i++) {
    if (sig_bt[i] == T_VOID) {
      assert(i > 0 && (sig_bt[i-1] == T_LONG || sig_bt[i-1] == T_DOUBLE), "missing half");
      continue;
    }

    // Pick up 0, 1 or 2 words from SP+offset.

    assert(!regs[i].second()->is_valid() || regs[i].first()->next() == regs[i].second(),
            "scrambled load targets?");
    // Load in argument order going down.
    int ld_off = (total_args_passed - i - 1)*Interpreter::stackElementSize;
    // Point to interpreter value (vs. tag)
    int next_off = ld_off - Interpreter::stackElementSize;
    //
    //
    //
    VMReg r_1 = regs[i].first();
    VMReg r_2 = regs[i].second();
    if (!r_1->is_valid()) {
      assert(!r_2->is_valid(), "");
      continue;
    }
    if (r_1->is_stack()) {
      // Convert stack slot to an SP offset (+ wordSize to account for return address )
      int st_off = regs[i].first()->reg2stack()*VMRegImpl::stack_slot_size;
      if (!r_2->is_valid()) {
        // sign extend???
        __ ldrsw(rscratch2, Address(esp, ld_off));
        __ str(rscratch2, Address(sp, st_off));
      } else {
        //
        // We are using two optoregs. This can be either T_OBJECT,
        // T_ADDRESS, T_LONG, or T_DOUBLE the interpreter allocates
        // two slots but only uses one for thr T_LONG or T_DOUBLE case
        // So we must adjust where to pick up the data to match the
        // interpreter.
        //
        // Interpreter local[n] == MSW, local[n+1] == LSW however locals
        // are accessed as negative so LSW is at LOW address

        // ld_off is MSW so get LSW
        const int offset = (sig_bt[i]==T_LONG||sig_bt[i]==T_DOUBLE)?
                           next_off : ld_off;
        __ ldr(rscratch2, Address(esp, offset));
        // st_off is LSW (i.e. reg.first())
        __ str(rscratch2, Address(sp, st_off));
      }
    } else if (r_1->is_Register()) {  // Register argument
      Register r = r_1->as_Register();
      if (r_2->is_valid()) {
        //
        // We are using two VMRegs. This can be either T_OBJECT,
        // T_ADDRESS, T_LONG, or T_DOUBLE the interpreter allocates
        // two slots but only uses one for thr T_LONG or T_DOUBLE case
        // So we must adjust where to pick up the data to match the
        // interpreter.

        const int offset = (sig_bt[i]==T_LONG||sig_bt[i]==T_DOUBLE)?
                           next_off : ld_off;

        // this can be a misaligned move
        __ ldr(r, Address(esp, offset));
      } else {
        // sign extend and use a full word?
        __ ldrw(r, Address(esp, ld_off));
      }
    } else {
      if (!r_2->is_valid()) {
        __ ldrs(r_1->as_FloatRegister(), Address(esp, ld_off));
      } else {
        __ ldrd(r_1->as_FloatRegister(), Address(esp, next_off));
      }
    }
  }

  // 6243940 We might end up in handle_wrong_method if
  // the callee is deoptimized as we race thru here. If that
  // happens we don't want to take a safepoint because the
  // caller frame will look interpreted and arguments are now
  // "compiled" so it is much better to make this transition
  // invisible to the stack walking code. Unfortunately if
  // we try and find the callee by normal means a safepoint
  // is possible. So we stash the desired callee in the thread
  // and the vm will find there should this case occur.

  __ str(rmethod, Address(rthread, JavaThread::callee_target_offset()));

  __ br(rscratch1);
}

// ---------------------------------------------------------------
AdapterHandlerEntry* SharedRuntime::generate_i2c2i_adapters(MacroAssembler *masm,
                                                            int total_args_passed,
                                                            int comp_args_on_stack,
                                                            const BasicType *sig_bt,
                                                            const VMRegPair *regs,
                                                            AdapterFingerPrint* fingerprint) { 
  address c2i_unverified_entry = __ pc();
  Label skip_fixup;

  address i2c_entry = __ pc();
  gen_i2c_adapter(masm, total_args_passed, comp_args_on_stack, sig_bt, regs);

  __ call_Unimplemented();
  __ b(skip_fixup);

  address c2i_entry = __ pc();
  gen_c2i_adapter(masm, total_args_passed, comp_args_on_stack, sig_bt, regs, skip_fixup);

  __ flush();
  return AdapterHandlerLibrary::new_entry(fingerprint, i2c_entry, c2i_entry, c2i_unverified_entry);
}

int SharedRuntime::c_calling_convention(const BasicType *sig_bt,
                                         VMRegPair *regs,
                                         int total_args_passed) {
// We return the amount of VMRegImpl stack slots we need to reserve for all
// the arguments NOT counting out_preserve_stack_slots.

    static const Register INT_ArgReg[Argument::n_int_register_parameters_c] = {
      c_rarg0, c_rarg1, c_rarg2, c_rarg3, c_rarg4, c_rarg5,  c_rarg6,  c_rarg7
    };
    static const FloatRegister FP_ArgReg[Argument::n_float_register_parameters_c] = {
      c_farg0, c_farg1, c_farg2, c_farg3,
      c_farg4, c_farg5, c_farg6, c_farg7
    };

    uint int_args = 0;
    uint fp_args = 0;
    uint stk_args = 0; // inc by 2 each time

    for (int i = 0; i < total_args_passed; i++) {
      switch (sig_bt[i]) {
      case T_BOOLEAN:
      case T_CHAR:
      case T_BYTE:
      case T_SHORT:
      case T_INT:
        if (int_args < Argument::n_int_register_parameters_c) {
          regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
#ifdef _WIN64
          fp_args++;
          // Allocate slots for callee to stuff register args the stack.
          stk_args += 2;
#endif
        } else {
          regs[i].set1(VMRegImpl::stack2reg(stk_args));
          stk_args += 2;
        }
        break;
      case T_LONG:
        assert(sig_bt[i + 1] == T_VOID, "expecting half");
        // fall through
      case T_OBJECT:
      case T_ARRAY:
      case T_ADDRESS:
      case T_METADATA:
        if (int_args < Argument::n_int_register_parameters_c) {
          regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
#ifdef _WIN64
          fp_args++;
          stk_args += 2;
#endif
        } else {
          regs[i].set2(VMRegImpl::stack2reg(stk_args));
          stk_args += 2;
        }
        break;
      case T_FLOAT:
        if (fp_args < Argument::n_float_register_parameters_c) {
          regs[i].set1(FP_ArgReg[fp_args++]->as_VMReg());
#ifdef _WIN64
          int_args++;
          // Allocate slots for callee to stuff register args the stack.
          stk_args += 2;
#endif
        } else {
          regs[i].set1(VMRegImpl::stack2reg(stk_args));
          stk_args += 2;
        }
        break;
      case T_DOUBLE:
        assert(sig_bt[i + 1] == T_VOID, "expecting half");
        if (fp_args < Argument::n_float_register_parameters_c) {
          regs[i].set2(FP_ArgReg[fp_args++]->as_VMReg());
#ifdef _WIN64
          int_args++;
          // Allocate slots for callee to stuff register args the stack.
          stk_args += 2;
#endif
        } else {
          regs[i].set2(VMRegImpl::stack2reg(stk_args));
          stk_args += 2;
        }
        break;
      case T_VOID: // Halves of longs and doubles
        assert(i != 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "expecting half");
        regs[i].set_bad();
        break;
      default:
        ShouldNotReachHere();
        break;
      }
    }
#ifdef _WIN64
  // windows abi requires that we always allocate enough stack space
  // for 4 64bit registers to be stored down.
  if (stk_args < 8) {
    stk_args = 8;
  }
#endif // _WIN64

  return stk_args;
}

// On 64 bit we will store integer like items to the stack as
// 64 bits items (sparc abi) even though java would only store
// 32bits for a parameter. On 32bit it will simply be 32 bits
// So this routine will do 32->32 on 32bit and 32->64 on 64bit
static void move32_64(MacroAssembler* masm, VMRegPair src, VMRegPair dst) { Unimplemented(); }


static void move_ptr(MacroAssembler* masm, VMRegPair src, VMRegPair dst) { Unimplemented(); }

// An oop arg. Must pass a handle not the oop itself
static void object_move(MacroAssembler* masm,
                        OopMap* map,
                        int oop_handle_offset,
                        int framesize_in_slots,
                        VMRegPair src,
                        VMRegPair dst,
                        bool is_receiver,
                        int* receiver_offset) { Unimplemented(); }

// A float arg may have to do float reg int reg conversion
static void float_move(MacroAssembler* masm, VMRegPair src, VMRegPair dst) { Unimplemented(); }

// A long move
static void long_move(MacroAssembler* masm, VMRegPair src, VMRegPair dst) { Unimplemented(); }

// A double move
static void double_move(MacroAssembler* masm, VMRegPair src, VMRegPair dst) { Unimplemented(); }


void SharedRuntime::save_native_result(MacroAssembler *masm, BasicType ret_type, int frame_slots) { Unimplemented(); }

void SharedRuntime::restore_native_result(MacroAssembler *masm, BasicType ret_type, int frame_slots) { Unimplemented(); }

static void save_args(MacroAssembler *masm, int arg_count, int first_arg, VMRegPair *args) { Unimplemented(); }

static void restore_args(MacroAssembler *masm, int arg_count, int first_arg, VMRegPair *args) { Unimplemented(); }


static void save_or_restore_arguments(MacroAssembler* masm,
                                      const int stack_slots,
                                      const int total_in_args,
                                      const int arg_save_area,
                                      OopMap* map,
                                      VMRegPair* in_regs,
                                      BasicType* in_sig_bt) { Unimplemented(); }


// Check GC_locker::needs_gc and enter the runtime if it's true.  This
// keeps a new JNI critical region from starting until a GC has been
// forced.  Save down any oops in registers and describe them in an
// OopMap.
static void check_needs_gc_for_critical_native(MacroAssembler* masm,
                                               int stack_slots,
                                               int total_c_args,
                                               int total_in_args,
                                               int arg_save_area,
                                               OopMapSet* oop_maps,
                                               VMRegPair* in_regs,
                                               BasicType* in_sig_bt) { Unimplemented(); }

// Unpack an array argument into a pointer to the body and the length
// if the array is non-null, otherwise pass 0 for both.
static void unpack_array_argument(MacroAssembler* masm, VMRegPair reg, BasicType in_elem_type, VMRegPair body_arg, VMRegPair length_arg) { Unimplemented(); }


class ComputeMoveOrder: public StackObj {
  class MoveOperation: public ResourceObj {
    friend class ComputeMoveOrder;
   private:
    VMRegPair        _src;
    VMRegPair        _dst;
    int              _src_index;
    int              _dst_index;
    bool             _processed;
    MoveOperation*  _next;
    MoveOperation*  _prev;

    static int get_id(VMRegPair r) { Unimplemented(); return 0; }

   public:
    MoveOperation(int src_index, VMRegPair src, int dst_index, VMRegPair dst):
      _src(src)
    , _src_index(src_index)
    , _dst(dst)
    , _dst_index(dst_index)
    , _next(NULL)
    , _prev(NULL)
    , _processed(false) { Unimplemented(); }

    VMRegPair src() const              { Unimplemented(); return _src; }
    int src_id() const                 { Unimplemented(); return 0; }
    int src_index() const              { Unimplemented(); return 0; }
    VMRegPair dst() const              { Unimplemented(); return _src; }
    void set_dst(int i, VMRegPair dst) { Unimplemented(); }
    int dst_index() const              { Unimplemented(); return 0; }
    int dst_id() const                 { Unimplemented(); return 0; }
    MoveOperation* next() const        { Unimplemented(); return 0; }
    MoveOperation* prev() const        { Unimplemented(); return 0; }
    void set_processed()               { Unimplemented(); }
    bool is_processed() const          { Unimplemented(); return 0; }

    // insert
    void break_cycle(VMRegPair temp_register) { Unimplemented(); }

    void link(GrowableArray<MoveOperation*>& killer) { Unimplemented(); }
  };

 private:
  GrowableArray<MoveOperation*> edges;

 public:
  ComputeMoveOrder(int total_in_args, VMRegPair* in_regs, int total_c_args, VMRegPair* out_regs,
                    BasicType* in_sig_bt, GrowableArray<int>& arg_order, VMRegPair tmp_vmreg) { Unimplemented(); }

  // Collected all the move operations
  void add_edge(int src_index, VMRegPair src, int dst_index, VMRegPair dst) { Unimplemented(); }

  // Walk the edges breaking cycles between moves.  The result list
  // can be walked in order to produce the proper set of loads
  GrowableArray<MoveOperation*>* get_store_order(VMRegPair temp_register) { Unimplemented(); return 0; }
};


// ---------------------------------------------------------------------------
// Generate a native wrapper for a given method.  The method takes arguments
// in the Java compiled code convention, marshals them to the native
// convention (handlizes oops, etc), transitions to native, makes the call,
// returns to java state (possibly blocking), unhandlizes any result and
// returns.
//
// Critical native functions are a shorthand for the use of
// GetPrimtiveArrayCritical and disallow the use of any other JNI
// functions.  The wrapper is expected to unpack the arguments before
// passing them to the callee and perform checks before and after the
// native call to ensure that they GC_locker
// lock_critical/unlock_critical semantics are followed.  Some other
// parts of JNI setup are skipped like the tear down of the JNI handle
// block and the check for pending exceptions it's impossible for them
// to be thrown.
//
// They are roughly structured like this:
//    if (GC_locker::needs_gc())
//      SharedRuntime::block_for_jni_critical();
//    tranistion to thread_in_native
//    unpack arrray arguments and call native entry point
//    check for safepoint in progress
//    check if any thread suspend flags are set
//      call into JVM and possible unlock the JNI critical
//      if a GC was suppressed while in the critical native.
//    transition back to thread_in_Java
//    return to caller
//
nmethod* SharedRuntime::generate_native_wrapper(MacroAssembler* masm,
				 methodHandle method,
				 int compile_id,
				 BasicType* sig_bt,
				 VMRegPair* regs,
				 BasicType ret_type) {
#if 0
#ifdef METHOD_HANDLES
  if (method->is_method_handle_intrinsic()) {
    vmIntrinsics::ID iid = method->intrinsic_id();
    intptr_t start = (intptr_t)__ pc();
    int vep_offset = ((intptr_t)__ pc()) - start;
    gen_special_dispatch(masm,
                         total_in_args,
                         comp_args_on_stack,
                         method->intrinsic_id(),
                         in_sig_bt,
                         in_regs);
    int frame_complete = ((intptr_t)__ pc()) - start;  // not complete, period
    __ flush();
    int stack_slots = SharedRuntime::out_preserve_stack_slots();  // no out slots at all, actually
    return nmethod::new_native_nmethod(method,
                                       compile_id,
                                       masm->code(),
                                       vep_offset,
                                       frame_complete,
                                       stack_slots / VMRegImpl::slots_per_word,
                                       in_ByteSize(-1),
                                       in_ByteSize(-1),
                                       (OopMapSet*)NULL);
  }
#endif

  bool is_critical_native = true;
  address native_func = method->critical_native_function();
  if (native_func == NULL) {
    native_func = method->native_function();
    is_critical_native = false;
  }
  assert(native_func != NULL, "must have function");

  // An OopMap for lock (and class if static)
  OopMapSet *oop_maps = new OopMapSet();
  intptr_t start = (intptr_t)__ pc();

  // We have received a description of where all the java arg are located
  // on entry to the wrapper. We need to convert these args to where
  // the jni function will expect them. To figure out where they go
  // we convert the java signature to a C signature by inserting
  // the hidden arguments as arg[0] and possibly arg[1] (static method)

  int total_c_args = total_in_args;
  if (!is_critical_native) {
    total_c_args += 1;
    if (method->is_static()) {
      total_c_args++;
    }
  } else {
    for (int i = 0; i < total_in_args; i++) {
      if (in_sig_bt[i] == T_ARRAY) {
        total_c_args++;
      }
    }
  }

  BasicType* out_sig_bt = NEW_RESOURCE_ARRAY(BasicType, total_c_args);
  VMRegPair* out_regs   = NEW_RESOURCE_ARRAY(VMRegPair, total_c_args);
  BasicType* in_elem_bt = NULL;

  int argc = 0;
  if (!is_critical_native) {
    out_sig_bt[argc++] = T_ADDRESS;
    if (method->is_static()) {
      out_sig_bt[argc++] = T_OBJECT;
    }

    for (int i = 0; i < total_in_args ; i++ ) {
      out_sig_bt[argc++] = in_sig_bt[i];
    }
  } else {
    Thread* THREAD = Thread::current();
    in_elem_bt = NEW_RESOURCE_ARRAY(BasicType, total_in_args);
    SignatureStream ss(method->signature());
    for (int i = 0; i < total_in_args ; i++ ) {
      if (in_sig_bt[i] == T_ARRAY) {
        // Arrays are passed as int, elem* pair
        out_sig_bt[argc++] = T_INT;
        out_sig_bt[argc++] = T_ADDRESS;
        Symbol* atype = ss.as_symbol(CHECK_NULL);
        const char* at = atype->as_C_string();
        if (strlen(at) == 2) {
          assert(at[0] == '[', "must be");
          switch (at[1]) {
            case 'B': in_elem_bt[i]  = T_BYTE; break;
            case 'C': in_elem_bt[i]  = T_CHAR; break;
            case 'D': in_elem_bt[i]  = T_DOUBLE; break;
            case 'F': in_elem_bt[i]  = T_FLOAT; break;
            case 'I': in_elem_bt[i]  = T_INT; break;
            case 'J': in_elem_bt[i]  = T_LONG; break;
            case 'S': in_elem_bt[i]  = T_SHORT; break;
            case 'Z': in_elem_bt[i]  = T_BOOLEAN; break;
            default: ShouldNotReachHere();
          }
        }
      } else {
        out_sig_bt[argc++] = in_sig_bt[i];
        in_elem_bt[i] = T_VOID;
      }
      if (in_sig_bt[i] != T_VOID) {
        assert(in_sig_bt[i] == ss.type(), "must match");
        ss.next();
      }
    }
  }

  // Now figure out where the args must be stored and how much stack space
  // they require.
  int out_arg_slots;
  out_arg_slots = c_calling_convention(out_sig_bt, out_regs, total_c_args);

  // Compute framesize for the wrapper.  We need to handlize all oops in
  // incoming registers

  // Calculate the total number of stack slots we will need.

  // First count the abi requirement plus all of the outgoing args
  int stack_slots = SharedRuntime::out_preserve_stack_slots() + out_arg_slots;

  // Now the space for the inbound oop handle area
  int total_save_slots = 6 * VMRegImpl::slots_per_word;  // 6 arguments passed in registers
  if (is_critical_native) {
    // Critical natives may have to call out so they need a save area
    // for register arguments.
    int double_slots = 0;
    int single_slots = 0;
    for ( int i = 0; i < total_in_args; i++) {
      if (in_regs[i].first()->is_Register()) {
        const Register reg = in_regs[i].first()->as_Register();
        switch (in_sig_bt[i]) {
          case T_BOOLEAN:
          case T_BYTE:
          case T_SHORT:
          case T_CHAR:
          case T_INT:  single_slots++; break;
          case T_ARRAY:  // specific to LP64 (7145024)
          case T_LONG: double_slots++; break;
          default:  ShouldNotReachHere();
        }
      } else if (in_regs[i].first()->is_XMMRegister()) {
        switch (in_sig_bt[i]) {
          case T_FLOAT:  single_slots++; break;
          case T_DOUBLE: double_slots++; break;
          default:  ShouldNotReachHere();
        }
      } else if (in_regs[i].first()->is_FloatRegister()) {
        ShouldNotReachHere();
      }
    }
    total_save_slots = double_slots * 2 + single_slots;
    // align the save area
    if (double_slots != 0) {
      stack_slots = round_to(stack_slots, 2);
    }
  }

  int oop_handle_offset = stack_slots;
  stack_slots += total_save_slots;

  // Now any space we need for handlizing a klass if static method

  int klass_slot_offset = 0;
  int klass_offset = -1;
  int lock_slot_offset = 0;
  bool is_static = false;

  if (method->is_static()) {
    klass_slot_offset = stack_slots;
    stack_slots += VMRegImpl::slots_per_word;
    klass_offset = klass_slot_offset * VMRegImpl::stack_slot_size;
    is_static = true;
  }

  // Plus a lock if needed

  if (method->is_synchronized()) {
    lock_slot_offset = stack_slots;
    stack_slots += VMRegImpl::slots_per_word;
  }

  // Now a place (+2) to save return values or temp during shuffling
  // + 4 for return address (which we own) and saved rbp
  stack_slots += 6;

  // Ok The space we have allocated will look like:
  //
  //
  // FP-> |                     |
  //      |---------------------|
  //      | 2 slots for moves   |
  //      |---------------------|
  //      | lock box (if sync)  |
  //      |---------------------| <- lock_slot_offset
  //      | klass (if static)   |
  //      |---------------------| <- klass_slot_offset
  //      | oopHandle area      |
  //      |---------------------| <- oop_handle_offset (6 java arg registers)
  //      | outbound memory     |
  //      | based arguments     |
  //      |                     |
  //      |---------------------|
  //      |                     |
  // SP-> | out_preserved_slots |
  //
  //


  // Now compute actual number of stack words we need rounding to make
  // stack properly aligned.
  stack_slots = round_to(stack_slots, StackAlignmentInSlots);

  int stack_size = stack_slots * VMRegImpl::stack_slot_size;

  // First thing make an ic check to see if we should even be here

  // We are free to use all registers as temps without saving them and
  // restoring them except rbp. rbp is the only callee save register
  // as far as the interpreter and the compiler(s) are concerned.


  const Register ic_reg = rax;
  const Register receiver = j_rarg0;

  Label hit;
  Label exception_pending;

  assert_different_registers(ic_reg, receiver, rscratch1);
  __ verify_oop(receiver);
  __ load_klass(rscratch1, receiver);
  __ cmpq(ic_reg, rscratch1);
  __ jcc(Assembler::equal, hit);

  __ jump(RuntimeAddress(SharedRuntime::get_ic_miss_stub()));

  // Verified entry point must be aligned
  __ align(8);

  __ bind(hit);

  int vep_offset = ((intptr_t)__ pc()) - start;

  // The instruction at the verified entry point must be 5 bytes or longer
  // because it can be patched on the fly by make_non_entrant. The stack bang
  // instruction fits that requirement.

  // Generate stack overflow check

  if (UseStackBanging) {
    __ bang_stack_with_offset(StackShadowPages*os::vm_page_size());
  } else {
    // need a 5 byte instruction to allow MT safe patching to non-entrant
    __ fat_nop();
  }

  // Generate a new frame for the wrapper.
  __ enter();
  // -2 because return address is already present and so is saved rbp
  __ subptr(rsp, stack_size - 2*wordSize);

  // Frame is now completed as far as size and linkage.
  int frame_complete = ((intptr_t)__ pc()) - start;

#ifdef ASSERT
    {
      Label L;
      __ mov(rax, rsp);
      __ andptr(rax, -16); // must be 16 byte boundary (see amd64 ABI)
      __ cmpptr(rax, rsp);
      __ jcc(Assembler::equal, L);
      __ stop("improperly aligned stack");
      __ bind(L);
    }
#endif /* ASSERT */


  // We use r14 as the oop handle for the receiver/klass
  // It is callee save so it survives the call to native

  const Register oop_handle_reg = r14;

  if (is_critical_native) {
    check_needs_gc_for_critical_native(masm, stack_slots, total_c_args, total_in_args,
                                       oop_handle_offset, oop_maps, in_regs, in_sig_bt);
  }

  //
  // We immediately shuffle the arguments so that any vm call we have to
  // make from here on out (sync slow path, jvmti, etc.) we will have
  // captured the oops from our caller and have a valid oopMap for
  // them.

  // -----------------
  // The Grand Shuffle

  // The Java calling convention is either equal (linux) or denser (win64) than the
  // c calling convention. However the because of the jni_env argument the c calling
  // convention always has at least one more (and two for static) arguments than Java.
  // Therefore if we move the args from java -> c backwards then we will never have
  // a register->register conflict and we don't have to build a dependency graph
  // and figure out how to break any cycles.
  //

  // Record esp-based slot for receiver on stack for non-static methods
  int receiver_offset = -1;

  // This is a trick. We double the stack slots so we can claim
  // the oops in the caller's frame. Since we are sure to have
  // more args than the caller doubling is enough to make
  // sure we can capture all the incoming oop args from the
  // caller.
  //
  OopMap* map = new OopMap(stack_slots * 2, 0 /* arg_slots*/);

  // Mark location of rbp (someday)
  // map->set_callee_saved(VMRegImpl::stack2reg( stack_slots - 2), stack_slots * 2, 0, vmreg(rbp));

  // Use eax, ebx as temporaries during any memory-memory moves we have to do
  // All inbound args are referenced based on rbp and all outbound args via rsp.


#ifdef ASSERT
  bool reg_destroyed[RegisterImpl::number_of_registers];
  bool freg_destroyed[XMMRegisterImpl::number_of_registers];
  for ( int r = 0 ; r < RegisterImpl::number_of_registers ; r++ ) {
    reg_destroyed[r] = false;
  }
  for ( int f = 0 ; f < XMMRegisterImpl::number_of_registers ; f++ ) {
    freg_destroyed[f] = false;
  }

#endif /* ASSERT */

  // This may iterate in two different directions depending on the
  // kind of native it is.  The reason is that for regular JNI natives
  // the incoming and outgoing registers are offset upwards and for
  // critical natives they are offset down.
  GrowableArray<int> arg_order(2 * total_in_args);
  VMRegPair tmp_vmreg;
  tmp_vmreg.set1(rbx->as_VMReg());

  if (!is_critical_native) {
    for (int i = total_in_args - 1, c_arg = total_c_args - 1; i >= 0; i--, c_arg--) {
      arg_order.push(i);
      arg_order.push(c_arg);
    }
  } else {
    // Compute a valid move order, using tmp_vmreg to break any cycles
    ComputeMoveOrder cmo(total_in_args, in_regs, total_c_args, out_regs, in_sig_bt, arg_order, tmp_vmreg);
  }

  int temploc = -1;
  for (int ai = 0; ai < arg_order.length(); ai += 2) {
    int i = arg_order.at(ai);
    int c_arg = arg_order.at(ai + 1);
    __ block_comment(err_msg("move %d -> %d", i, c_arg));
    if (c_arg == -1) {
      assert(is_critical_native, "should only be required for critical natives");
      // This arg needs to be moved to a temporary
      __ mov(tmp_vmreg.first()->as_Register(), in_regs[i].first()->as_Register());
      in_regs[i] = tmp_vmreg;
      temploc = i;
      continue;
    } else if (i == -1) {
      assert(is_critical_native, "should only be required for critical natives");
      // Read from the temporary location
      assert(temploc != -1, "must be valid");
      i = temploc;
      temploc = -1;
    }
#ifdef ASSERT
    if (in_regs[i].first()->is_Register()) {
      assert(!reg_destroyed[in_regs[i].first()->as_Register()->encoding()], "destroyed reg!");
    } else if (in_regs[i].first()->is_XMMRegister()) {
      assert(!freg_destroyed[in_regs[i].first()->as_XMMRegister()->encoding()], "destroyed reg!");
    }
    if (out_regs[c_arg].first()->is_Register()) {
      reg_destroyed[out_regs[c_arg].first()->as_Register()->encoding()] = true;
    } else if (out_regs[c_arg].first()->is_XMMRegister()) {
      freg_destroyed[out_regs[c_arg].first()->as_XMMRegister()->encoding()] = true;
    }
#endif /* ASSERT */
    switch (in_sig_bt[i]) {
      case T_ARRAY:
        if (is_critical_native) {
          unpack_array_argument(masm, in_regs[i], in_elem_bt[i], out_regs[c_arg + 1], out_regs[c_arg]);
          c_arg++;
#ifdef ASSERT
          if (out_regs[c_arg].first()->is_Register()) {
            reg_destroyed[out_regs[c_arg].first()->as_Register()->encoding()] = true;
          } else if (out_regs[c_arg].first()->is_XMMRegister()) {
            freg_destroyed[out_regs[c_arg].first()->as_XMMRegister()->encoding()] = true;
          }
#endif
          break;
        }
      case T_OBJECT:
        assert(!is_critical_native, "no oop arguments");
        object_move(masm, map, oop_handle_offset, stack_slots, in_regs[i], out_regs[c_arg],
                    ((i == 0) && (!is_static)),
                    &receiver_offset);
        break;
      case T_VOID:
        break;

      case T_FLOAT:
        float_move(masm, in_regs[i], out_regs[c_arg]);
          break;

      case T_DOUBLE:
        assert( i + 1 < total_in_args &&
                in_sig_bt[i + 1] == T_VOID &&
                out_sig_bt[c_arg+1] == T_VOID, "bad arg list");
        double_move(masm, in_regs[i], out_regs[c_arg]);
        break;

      case T_LONG :
        long_move(masm, in_regs[i], out_regs[c_arg]);
        break;

      case T_ADDRESS: assert(false, "found T_ADDRESS in java args");

      default:
        move32_64(masm, in_regs[i], out_regs[c_arg]);
    }
  }

  // point c_arg at the first arg that is already loaded in case we
  // need to spill before we call out
  int c_arg = total_c_args - total_in_args;

  // Pre-load a static method's oop into r14.  Used both by locking code and
  // the normal JNI call code.
  if (method->is_static() && !is_critical_native) {

    //  load oop into a register
    __ movoop(oop_handle_reg, JNIHandles::make_local(Klass::cast(method->method_holder())->java_mirror()));

    // Now handlize the static class mirror it's known not-null.
    __ movptr(Address(rsp, klass_offset), oop_handle_reg);
    map->set_oop(VMRegImpl::stack2reg(klass_slot_offset));

    // Now get the handle
    __ lea(oop_handle_reg, Address(rsp, klass_offset));
    // store the klass handle as second argument
    __ movptr(c_rarg1, oop_handle_reg);
    // and protect the arg if we must spill
    c_arg--;
  }

  // Change state to native (we save the return address in the thread, since it might not
  // be pushed on the stack when we do a a stack traversal). It is enough that the pc()
  // points into the right code segment. It does not have to be the correct return pc.
  // We use the same pc/oopMap repeatedly when we call out

  intptr_t the_pc = (intptr_t) __ pc();
  oop_maps->add_gc_map(the_pc - start, map);

  __ set_last_Java_frame(rsp, noreg, (address)the_pc);


  // We have all of the arguments setup at this point. We must not touch any register
  // argument registers at this point (what if we save/restore them there are no oop?

  {
    SkipIfEqual skip(masm, &DTraceMethodProbes, false);
    // protect the args we've loaded
    save_args(masm, total_c_args, c_arg, out_regs);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
      CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_entry),
      r15_thread, c_rarg1);
    restore_args(masm, total_c_args, c_arg, out_regs);
  }

  // RedefineClasses() tracing support for obsolete method entry
  if (RC_TRACE_IN_RANGE(0x00001000, 0x00002000)) {
    // protect the args we've loaded
    save_args(masm, total_c_args, c_arg, out_regs);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
      CAST_FROM_FN_PTR(address, SharedRuntime::rc_trace_method_entry),
      r15_thread, c_rarg1);
    restore_args(masm, total_c_args, c_arg, out_regs);
  }

  // Lock a synchronized method

  // Register definitions used by locking and unlocking

  const Register swap_reg = rax;  // Must use rax for cmpxchg instruction
  const Register obj_reg  = rbx;  // Will contain the oop
  const Register lock_reg = r13;  // Address of compiler lock object (BasicLock)
  const Register old_hdr  = r13;  // value of old header at unlock time

  Label slow_path_lock;
  Label lock_done;

  if (method->is_synchronized()) {
    assert(!is_critical_native, "unhandled");


    const int mark_word_offset = BasicLock::displaced_header_offset_in_bytes();

    // Get the handle (the 2nd argument)
    __ mov(oop_handle_reg, c_rarg1);

    // Get address of the box

    __ lea(lock_reg, Address(rsp, lock_slot_offset * VMRegImpl::stack_slot_size));

    // Load the oop from the handle
    __ movptr(obj_reg, Address(oop_handle_reg, 0));

    if (UseBiasedLocking) {
      __ biased_locking_enter(lock_reg, obj_reg, swap_reg, rscratch1, false, lock_done, &slow_path_lock);
    }

    // Load immediate 1 into swap_reg %rax
    __ movl(swap_reg, 1);

    // Load (object->mark() | 1) into swap_reg %rax
    __ orptr(swap_reg, Address(obj_reg, 0));

    // Save (object->mark() | 1) into BasicLock's displaced header
    __ movptr(Address(lock_reg, mark_word_offset), swap_reg);

    if (os::is_MP()) {
      __ lock();
    }

    // src -> dest iff dest == rax else rax <- dest
    __ cmpxchgptr(lock_reg, Address(obj_reg, 0));
    __ jcc(Assembler::equal, lock_done);

    // Hmm should this move to the slow path code area???

    // Test if the oopMark is an obvious stack pointer, i.e.,
    //  1) (mark & 3) == 0, and
    //  2) rsp <= mark < mark + os::pagesize()
    // These 3 tests can be done by evaluating the following
    // expression: ((mark - rsp) & (3 - os::vm_page_size())),
    // assuming both stack pointer and pagesize have their
    // least significant 2 bits clear.
    // NOTE: the oopMark is in swap_reg %rax as the result of cmpxchg

    __ subptr(swap_reg, rsp);
    __ andptr(swap_reg, 3 - os::vm_page_size());

    // Save the test result, for recursive case, the result is zero
    __ movptr(Address(lock_reg, mark_word_offset), swap_reg);
    __ jcc(Assembler::notEqual, slow_path_lock);

    // Slow path will re-enter here

    __ bind(lock_done);
  }


  // Finally just about ready to make the JNI call


  // get JNIEnv* which is first argument to native
  if (!is_critical_native) {
    __ lea(c_rarg0, Address(r15_thread, in_bytes(JavaThread::jni_environment_offset())));
  }

  // Now set thread in native
  __ movl(Address(r15_thread, JavaThread::thread_state_offset()), _thread_in_native);

  __ call(RuntimeAddress(native_func));

    // Either restore the MXCSR register after returning from the JNI Call
    // or verify that it wasn't changed.
    if (RestoreMXCSROnJNICalls) {
      __ ldmxcsr(ExternalAddress(StubRoutines::x86::mxcsr_std()));

    }
    else if (CheckJNICalls ) {
      __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, StubRoutines::x86::verify_mxcsr_entry())));
    }


  // Unpack native results.
  switch (ret_type) {
  case T_BOOLEAN: __ c2bool(rax);            break;
  case T_CHAR   : __ movzwl(rax, rax);      break;
  case T_BYTE   : __ sign_extend_byte (rax); break;
  case T_SHORT  : __ sign_extend_short(rax); break;
  case T_INT    : /* nothing to do */        break;
  case T_DOUBLE :
  case T_FLOAT  :
    // Result is in xmm0 we'll save as needed
    break;
  case T_ARRAY:                 // Really a handle
  case T_OBJECT:                // Really a handle
      break; // can't de-handlize until after safepoint check
  case T_VOID: break;
  case T_LONG: break;
  default       : ShouldNotReachHere();
  }

  // Switch thread to "native transition" state before reading the synchronization state.
  // This additional state is necessary because reading and testing the synchronization
  // state is not atomic w.r.t. GC, as this scenario demonstrates:
  //     Java thread A, in _thread_in_native state, loads _not_synchronized and is preempted.
  //     VM thread changes sync state to synchronizing and suspends threads for GC.
  //     Thread A is resumed to finish this native method, but doesn't block here since it
  //     didn't see any synchronization is progress, and escapes.
  __ movl(Address(r15_thread, JavaThread::thread_state_offset()), _thread_in_native_trans);

  if(os::is_MP()) {
    if (UseMembar) {
      // Force this write out before the read below
      __ membar(Assembler::Membar_mask_bits(
           Assembler::LoadLoad | Assembler::LoadStore |
           Assembler::StoreLoad | Assembler::StoreStore));
    } else {
      // Write serialization page so VM thread can do a pseudo remote membar.
      // We use the current thread pointer to calculate a thread specific
      // offset to write to within the page. This minimizes bus traffic
      // due to cache line collision.
      __ serialize_memory(r15_thread, rcx);
    }
  }

  Label after_transition;

  // check for safepoint operation in progress and/or pending suspend requests
  {
    Label Continue;

    __ cmp32(ExternalAddress((address)SafepointSynchronize::address_of_state()),
             SafepointSynchronize::_not_synchronized);

    Label L;
    __ jcc(Assembler::notEqual, L);
    __ cmpl(Address(r15_thread, JavaThread::suspend_flags_offset()), 0);
    __ jcc(Assembler::equal, Continue);
    __ bind(L);

    // Don't use call_VM as it will see a possible pending exception and forward it
    // and never return here preventing us from clearing _last_native_pc down below.
    // Also can't use call_VM_leaf either as it will check to see if rsi & rdi are
    // preserved and correspond to the bcp/locals pointers. So we do a runtime call
    // by hand.
    //
    save_native_result(masm, ret_type, stack_slots);
    __ mov(c_rarg0, r15_thread);
    __ mov(r12, rsp); // remember sp
    __ subptr(rsp, frame::arg_reg_save_area_bytes); // windows
    __ andptr(rsp, -16); // align stack as required by ABI
    if (!is_critical_native) {
      __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans)));
    } else {
      __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans_and_transition)));
    }
    __ mov(rsp, r12); // restore sp
    __ reinit_heapbase();
    // Restore any method result value
    restore_native_result(masm, ret_type, stack_slots);

    if (is_critical_native) {
      // The call above performed the transition to thread_in_Java so
      // skip the transition logic below.
      __ jmpb(after_transition);
    }

    __ bind(Continue);
  }

  // change thread state
  __ movl(Address(r15_thread, JavaThread::thread_state_offset()), _thread_in_Java);
  __ bind(after_transition);

  Label reguard;
  Label reguard_done;
  __ cmpl(Address(r15_thread, JavaThread::stack_guard_state_offset()), JavaThread::stack_guard_yellow_disabled);
  __ jcc(Assembler::equal, reguard);
  __ bind(reguard_done);

  // native result if any is live

  // Unlock
  Label unlock_done;
  Label slow_path_unlock;
  if (method->is_synchronized()) {

    // Get locked oop from the handle we passed to jni
    __ movptr(obj_reg, Address(oop_handle_reg, 0));

    Label done;

    if (UseBiasedLocking) {
      __ biased_locking_exit(obj_reg, old_hdr, done);
    }

    // Simple recursive lock?

    __ cmpptr(Address(rsp, lock_slot_offset * VMRegImpl::stack_slot_size), (int32_t)NULL_WORD);
    __ jcc(Assembler::equal, done);

    // Must save rax if if it is live now because cmpxchg must use it
    if (ret_type != T_FLOAT && ret_type != T_DOUBLE && ret_type != T_VOID) {
      save_native_result(masm, ret_type, stack_slots);
    }


    // get address of the stack lock
    __ lea(rax, Address(rsp, lock_slot_offset * VMRegImpl::stack_slot_size));
    //  get old displaced header
    __ movptr(old_hdr, Address(rax, 0));

    // Atomic swap old header if oop still contains the stack lock
    if (os::is_MP()) {
      __ lock();
    }
    __ cmpxchgptr(old_hdr, Address(obj_reg, 0));
    __ jcc(Assembler::notEqual, slow_path_unlock);

    // slow path re-enters here
    __ bind(unlock_done);
    if (ret_type != T_FLOAT && ret_type != T_DOUBLE && ret_type != T_VOID) {
      restore_native_result(masm, ret_type, stack_slots);
    }

    __ bind(done);

  }
  {
    SkipIfEqual skip(masm, &DTraceMethodProbes, false);
    save_native_result(masm, ret_type, stack_slots);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
         CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_exit),
         r15_thread, c_rarg1);
    restore_native_result(masm, ret_type, stack_slots);
  }

  __ reset_last_Java_frame(false, true);

  // Unpack oop result
  if (ret_type == T_OBJECT || ret_type == T_ARRAY) {
      Label L;
      __ testptr(rax, rax);
      __ jcc(Assembler::zero, L);
      __ movptr(rax, Address(rax, 0));
      __ bind(L);
      __ verify_oop(rax);
  }

  if (!is_critical_native) {
    // reset handle block
    __ movptr(rcx, Address(r15_thread, JavaThread::active_handles_offset()));
    __ movptr(Address(rcx, JNIHandleBlock::top_offset_in_bytes()), (int32_t)NULL_WORD);
  }

  // pop our frame

  __ leave();

  if (!is_critical_native) {
    // Any exception pending?
    __ cmpptr(Address(r15_thread, in_bytes(Thread::pending_exception_offset())), (int32_t)NULL_WORD);
    __ jcc(Assembler::notEqual, exception_pending);
  }

  // Return

  __ ret(0);

  // Unexpected paths are out of line and go here

  if (!is_critical_native) {
    // forward the exception
    __ bind(exception_pending);

    // and forward the exception
    __ jump(RuntimeAddress(StubRoutines::forward_exception_entry()));
  }

  // Slow path locking & unlocking
  if (method->is_synchronized()) {

    // BEGIN Slow path lock
    __ bind(slow_path_lock);

    // has last_Java_frame setup. No exceptions so do vanilla call not call_VM
    // args are (oop obj, BasicLock* lock, JavaThread* thread)

    // protect the args we've loaded
    save_args(masm, total_c_args, c_arg, out_regs);

    __ mov(c_rarg0, obj_reg);
    __ mov(c_rarg1, lock_reg);
    __ mov(c_rarg2, r15_thread);

    // Not a leaf but we have last_Java_frame setup as we want
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::complete_monitor_locking_C), 3);
    restore_args(masm, total_c_args, c_arg, out_regs);

#ifdef ASSERT
    { Label L;
    __ cmpptr(Address(r15_thread, in_bytes(Thread::pending_exception_offset())), (int32_t)NULL_WORD);
    __ jcc(Assembler::equal, L);
    __ stop("no pending exception allowed on exit from monitorenter");
    __ bind(L);
    }
#endif
    __ jmp(lock_done);

    // END Slow path lock

    // BEGIN Slow path unlock
    __ bind(slow_path_unlock);

    // If we haven't already saved the native result we must save it now as xmm registers
    // are still exposed.

    if (ret_type == T_FLOAT || ret_type == T_DOUBLE ) {
      save_native_result(masm, ret_type, stack_slots);
    }

    __ lea(c_rarg1, Address(rsp, lock_slot_offset * VMRegImpl::stack_slot_size));

    __ mov(c_rarg0, obj_reg);
    __ mov(r12, rsp); // remember sp
    __ subptr(rsp, frame::arg_reg_save_area_bytes); // windows
    __ andptr(rsp, -16); // align stack as required by ABI

    // Save pending exception around call to VM (which contains an EXCEPTION_MARK)
    // NOTE that obj_reg == rbx currently
    __ movptr(rbx, Address(r15_thread, in_bytes(Thread::pending_exception_offset())));
    __ movptr(Address(r15_thread, in_bytes(Thread::pending_exception_offset())), (int32_t)NULL_WORD);

    __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, SharedRuntime::complete_monitor_unlocking_C)));
    __ mov(rsp, r12); // restore sp
    __ reinit_heapbase();
#ifdef ASSERT
    {
      Label L;
      __ cmpptr(Address(r15_thread, in_bytes(Thread::pending_exception_offset())), (int)NULL_WORD);
      __ jcc(Assembler::equal, L);
      __ stop("no pending exception allowed on exit complete_monitor_unlocking_C");
      __ bind(L);
    }
#endif /* ASSERT */

    __ movptr(Address(r15_thread, in_bytes(Thread::pending_exception_offset())), rbx);

    if (ret_type == T_FLOAT || ret_type == T_DOUBLE ) {
      restore_native_result(masm, ret_type, stack_slots);
    }
    __ jmp(unlock_done);

    // END Slow path unlock

  } // synchronized

  // SLOW PATH Reguard the stack if needed

  __ bind(reguard);
  save_native_result(masm, ret_type, stack_slots);
  __ mov(r12, rsp); // remember sp
  __ subptr(rsp, frame::arg_reg_save_area_bytes); // windows
  __ andptr(rsp, -16); // align stack as required by ABI
  __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, SharedRuntime::reguard_yellow_pages)));
  __ mov(rsp, r12); // restore sp
  __ reinit_heapbase();
  restore_native_result(masm, ret_type, stack_slots);
  // and continue
  __ jmp(reguard_done);



  __ flush();

  nmethod *nm = nmethod::new_native_nmethod(method,
                                            compile_id,
                                            masm->code(),
                                            vep_offset,
                                            frame_complete,
                                            stack_slots / VMRegImpl::slots_per_word,
                                            (is_static ? in_ByteSize(klass_offset) : in_ByteSize(receiver_offset)),
                                            in_ByteSize(lock_slot_offset*VMRegImpl::stack_slot_size),
                                            oop_maps);

  if (is_critical_native) {
    nm->set_lazy_critical_native(true);
  }

#endif // 0
  return NULL;

}

#ifdef HAVE_DTRACE_H
// ---------------------------------------------------------------------------
// Generate a dtrace nmethod for a given signature.  The method takes arguments
// in the Java compiled code convention, marshals them to the native
// abi and then leaves nops at the position you would expect to call a native
// function. When the probe is enabled the nops are replaced with a trap
// instruction that dtrace inserts and the trace will cause a notification
// to dtrace.
//
// The probes are only able to take primitive types and java/lang/String as
// arguments.  No other java types are allowed. Strings are converted to utf8
// strings so that from dtrace point of view java strings are converted to C
// strings. There is an arbitrary fixed limit on the total space that a method
// can use for converting the strings. (256 chars per string in the signature).
// So any java string larger then this is truncated.

static int  fp_offset[ConcreteRegisterImpl::number_of_registers] = { 0 };
static bool offsets_initialized = false;


nmethod *SharedRuntime::generate_dtrace_nmethod(MacroAssembler *masm,
                                                methodHandle method) { Unimplemented(); return 0; }

#endif // HAVE_DTRACE_H

// this function returns the adjust size (in number of words) to a c2i adapter
// activation for use during deoptimization
int Deoptimization::last_frame_adjust(int callee_parameters, int callee_locals ) {
  // FIXME: This is a guess
  return (callee_locals - callee_parameters) * Interpreter::stackElementWords;
}


//------------------------------generate_deopt_blob----------------------------
void SharedRuntime::generate_deopt_blob() {
  // Allocate space for the code
  ResourceMark rm;
  // Setup code generation tools
  CodeBuffer buffer("deopt_blob", 2048, 1024);
  MacroAssembler* masm = new MacroAssembler(&buffer);
  int frame_size_in_words;
  OopMap* map = NULL;
  OopMapSet *oop_maps = new OopMapSet();

  // -------------
  // This code enters when returning to a de-optimized nmethod.  A return
  // address has been pushed on the the stack, and return values are in
  // registers.
  // If we are doing a normal deopt then we were called from the patched
  // nmethod from the point we returned to the nmethod. So the return
  // address on the stack is wrong by NativeCall::instruction_size
  // We will adjust the value so it looks like we have the original return
  // address on the stack (like when we eagerly deoptimized).
  // In the case of an exception pending when deoptimizing, we enter
  // with a return address on the stack that points after the call we patched
  // into the exception handler. We have the following register state from,
  // e.g., the forward exception stub (see stubGenerator_x86_64.cpp).
  //    r0: exception oop
  //    r19: exception handler
  //    r3: throwing pc
  // So in this case we simply jam r3 into the useless return address and
  // the stack looks just like we want.
  //
  // At this point we need to de-opt.  We save the argument return
  // registers.  We call the first C routine, fetch_unroll_info().  This
  // routine captures the return values and returns a structure which
  // describes the current frame size and the sizes of all replacement frames.
  // The current frame is compiled code and may contain many inlined
  // functions, each with their own JVM state.  We pop the current frame, then
  // push all the new frames.  Then we call the C routine unpack_frames() to
  // populate these frames.  Finally unpack_frames() returns us the new target
  // address.  Notice that callee-save registers are BLOWN here; they have
  // already been captured in the vframeArray at the time the return PC was
  // patched.
  address start = __ pc();
  Label cont;

  // Prolog for non exception case!

  // Save everything in sight.
  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  // Normal deoptimization.  Save exec mode for unpack_frames.
  __ movw(rcpool, Deoptimization::Unpack_deopt); // callee-saved
  __ b(cont);

  int reexecute_offset = __ pc() - start;

  // Reexecute case
  // return address is the pc describes what bci to do re-execute at

  // No need to update map as each call to save_live_registers will produce identical oopmap
  (void) RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  __ movw(rcpool, Deoptimization::Unpack_reexecute); // callee-saved
  __ b(cont);

  int exception_offset = __ pc() - start;

  // Prolog for exception case

  // all registers are dead at this entry point, except for r0, and
  // r3 which contain the exception oop and exception pc
  // respectively.  Set them in TLS and fall thru to the
  // unpack_with_exception_in_tls entry point.

  __ str(r3, Address(rthread, JavaThread::exception_pc_offset()));
  __ str(r0, Address(rthread, JavaThread::exception_oop_offset()));

  int exception_in_tls_offset = __ pc() - start;

  // new implementation because exception oop is now passed in JavaThread

  // Prolog for exception case
  // All registers must be preserved because they might be used by LinearScan
  // Exceptiop oop and throwing PC are passed in JavaThread
  // tos: stack at point of call to method that threw the exception (i.e. only
  // args are on the stack, no return address)

  // The return address pushed by save_live_registers will be patched
  // later with the throwing pc. The correct value is not available
  // now because loading it from memory would destroy registers.

  // NB: The SP at this point must be the SP of the method that is
  // being deoptimized.  Deoptimization assumes that the frame created
  // here by save_live_registers is immediately below the method's SP.
  // This is a somewhat fragile mechanism.

  // Save everything in sight.
  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  // Now it is safe to overwrite any register

  // Deopt during an exception.  Save exec mode for unpack_frames.
  __ mov(rcpool, Deoptimization::Unpack_exception); // callee-saved

  // load throwing pc from JavaThread and patch it as the return address
  // of the current frame. Then clear the field in JavaThread

  __ ldr(r3, Address(rthread, JavaThread::exception_pc_offset()));
  __ str(r3, Address(rfp, wordSize));
  __ str(zr, Address(rthread, JavaThread::exception_pc_offset()));

#ifdef ASSERT
  // verify that there is really an exception oop in JavaThread
  __ ldr(r0, Address(rthread, JavaThread::exception_oop_offset()));
  __ verify_oop(r0);

  // verify that there is no pending exception
  Label no_pending_exception;
  __ ldr(rscratch1, Address(rthread, Thread::pending_exception_offset()));
  __ cbz(rscratch1, no_pending_exception);
  __ stop("must not have pending exception here");
  __ bind(no_pending_exception);
#endif

  __ bind(cont);

  // Call C code.  Need thread and this frame, but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.
  //
  // UnrollBlock* fetch_unroll_info(JavaThread* thread)

  // fetch_unroll_info needs to call last_java_frame().

  Label retaddr;
  __ set_last_Java_frame(sp, rfp, retaddr, rscratch1);
#ifdef ASSERT0
  { Label L;
    __ ldr(rscratch1, Address(rthread,
			      JavaThread::last_Java_fp_offset()));
    __ cbz(rscratch1, L);
    __ stop("SharedRuntime::generate_deopt_blob: last_Java_fp not cleared");
    __ bind(L);
  }
#endif // ASSERT
  __ mov(c_rarg0, rthread);
  __ mov(rscratch1, RuntimeAddress(CAST_FROM_FN_PTR(address, Deoptimization::fetch_unroll_info)));
  __ brx86(rscratch1, 1, 0, 1);
  __ bind(retaddr);

  // Need to have an oopmap that tells fetch_unroll_info where to
  // find any register it might need.
  oop_maps->add_gc_map(__ pc() - start, map);

  __ reset_last_Java_frame(true, false);

  // Load UnrollBlock* into rdi
  __ mov(r5, r0);

   Label noException;
  __ cmpw(rcpool, Deoptimization::Unpack_exception);   // Was exception pending?
  __ br(Assembler::NE, noException);
  __ ldr(r0, Address(rthread, JavaThread::exception_oop_offset()));
  // QQQ this is useless it was NULL above
  __ ldr(r3, Address(rthread, JavaThread::exception_pc_offset()));
  __ str(zr, Address(rthread, JavaThread::exception_oop_offset()));
  __ str(zr, Address(rthread, JavaThread::exception_pc_offset()));

  __ verify_oop(r0);

  // Overwrite the result registers with the exception results.
  __ str(r0, Address(sp, RegisterSaver::r0_offset_in_bytes()));
  // I think this is useless
  // __ str(r3, Address(sp, RegisterSaver::r3_offset_in_bytes()));

  __ bind(noException);

  // Only register save data is on the stack.
  // Now restore the result registers.  Everything else is either dead
  // or captured in the vframeArray.
  RegisterSaver::restore_result_registers(masm);

  // All of the register save area has been popped of the stack. Only the
  // return address remains.

  // Pop all the frames we must move/replace.
  //
  // Frame picture (youngest to oldest)
  // 1: self-frame (no frame link)
  // 2: deopting frame  (no frame link)
  // 3: caller of deopting frame (could be compiled/interpreted).
  //
  // Note: by leaving the return address of self-frame on the stack
  // and using the size of frame 2 to adjust the stack
  // when we are done the return to frame 3 will still be on the stack.

  // Pop deoptimized frame
  __ ldrw(r2, Address(r5, Deoptimization::UnrollBlock::size_of_deoptimized_frame_offset_in_bytes()));
  __ add(sp, sp, r2);

  // sp should be pointing at the return address to the caller (3)

  // Stack bang to make sure there's enough room for these interpreter frames.
  if (UseStackBanging) {
    __ ldrw(r19, Address(r5, Deoptimization::UnrollBlock::total_frame_sizes_offset_in_bytes()));
    __ bang_stack_size(r19, r2);
  }

  // Load address of array of frame pcs into r2
  __ ldr(r2, Address(r5, Deoptimization::UnrollBlock::frame_pcs_offset_in_bytes()));

  // Trash the old pc
  // __ addptr(rsp, wordSize);  FIXME ????

  // Load address of array of frame sizes into r4
  __ ldr(r4, Address(r5, Deoptimization::UnrollBlock::frame_sizes_offset_in_bytes()));

  // Load counter into r3
  __ ldrw(r3, Address(r5, Deoptimization::UnrollBlock::number_of_frames_offset_in_bytes()));

  // Pick up the initial fp we should save
  __ ldr(rfp, Address(r5, Deoptimization::UnrollBlock::initial_info_offset_in_bytes()));

  // Now adjust the caller's stack to make up for the extra locals
  // but record the original sp so that we can save it in the skeletal interpreter
  // frame and the stack walking of interpreter_sender will get the unextended sp
  // value and not the "real" sp value.

  const Register sender_sp = r6;

  __ mov(sender_sp, sp);
  __ ldrw(r19, Address(r5,
                       Deoptimization::UnrollBlock::
                       caller_adjustment_offset_in_bytes()));
  __ sub(sp, sp, r19);

  // Push interpreter frames in a loop
  __ mov(rscratch1, (address)0xDEADDEAD);        // Make a recognizable pattern
  __ mov(rscratch2, rscratch1);
  Label loop;
  __ bind(loop);
  __ ldr(r19, Address(__ post(r4, wordSize)));          // Load frame size
#ifdef CC_INTERP
  __ subptr(rbx, 4*wordSize);           // we'll push pc and ebp by hand and
#ifdef ASSERT
  __ push(0xDEADDEAD);                  // Make a recognizable pattern
  __ push(0xDEADDEAD);
#else /* ASSERT */
  __ subptr(rsp, 2*wordSize);           // skip the "static long no_param"
#endif /* ASSERT */
#else
  __ sub(r19, r19, 2*wordSize);           // We'll push pc and fp by hand
#endif // CC_INTERP
  __ ldr(lr, Address(__ post(r2, wordSize)));  // Load pc
  __ enter();                           // Save old & set new fp
  __ sub(sp, sp, r19);                  // Prolog
#ifdef CC_INTERP
  __ movptr(Address(rfp,
                  -(sizeof(BytecodeInterpreter)) + in_bytes(byte_offset_of(BytecodeInterpreter, _sender_sp))),
            sender_sp); // Make it walkable
#else /* CC_INTERP */
  // This value is corrected by layout_activation_impl
  __ str(zr, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
  __ str(sender_sp, Address(rfp, frame::interpreter_frame_sender_sp_offset * wordSize)); // Make it walkable
#endif /* CC_INTERP */
  __ mov(sender_sp, sp);               // Pass sender_sp to next frame
  __ sub(r3, r3, 1);                   // Decrement counter
  __ cbnz(r3, loop);

    // Re-push self-frame
  __ ldr(lr, Address(r2));
  __ enter();

  // Allocate a full sized register save area.
  __ sub(sp, sp, frame_size_in_words * wordSize);

  // Restore frame locals after moving the frame
  __ strd(v0, Address(sp, RegisterSaver::v0_offset_in_bytes()));
  __ str(r0, Address(sp, RegisterSaver::r0_offset_in_bytes()));

  // Call C code.  Need thread but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.  Call should
  // restore return values to their stack-slots with the new SP.
  //
  // void Deoptimization::unpack_frames(JavaThread* thread, int exec_mode)

  // Use rfp because the frames look interpreted now
  // Don't need the precise return PC here, just precise enough to point into this code blob.
  address the_pc = __ pc();
  __ set_last_Java_frame(sp, rfp, address(NULL), rscratch1);

  __ mov(c_rarg0, rthread);
  __ movw(c_rarg1, rcpool); // second arg: exec_mode
  __ mov(rscratch1, RuntimeAddress(CAST_FROM_FN_PTR(address, Deoptimization::unpack_frames)));
  __ brx86(rscratch1, 2, 0, 0);

  // Set an oopmap for the call site
  // Use the same PC we used for the last java frame
  oop_maps->add_gc_map(the_pc - start,
                       new OopMap( frame_size_in_words, 0 ));

  // Clear fp AND pc
  __ reset_last_Java_frame(true, true);

  // Collect return values
  __ ldrd(v0, Address(sp, RegisterSaver::v0_offset_in_bytes()));
  __ ldr(r0, Address(sp, RegisterSaver::r0_offset_in_bytes()));
  // I think this is useless (throwing pc?)
  // __ ldr(r3, Address(sp, RegisterSaver::r3_offset_in_bytes()));

  // Pop self-frame.
  __ leave();                           // Epilog

  // Jump to interpreter
  __ ret(lr);

  // Make sure all code is generated
  masm->flush();

  _deopt_blob = DeoptimizationBlob::create(&buffer, oop_maps, 0, exception_offset, reexecute_offset, frame_size_in_words);
  _deopt_blob->set_unpack_with_exception_in_tls_offset(exception_in_tls_offset);
}

uint SharedRuntime::out_preserve_stack_slots() {
  return 0;
}

#ifdef COMPILER2
//------------------------------generate_uncommon_trap_blob--------------------
void SharedRuntime::generate_uncommon_trap_blob() { Unimplemented(); }
#endif // COMPILER2


//------------------------------generate_handler_blob------
//
// Generate a special Compile2Runtime blob that saves all registers,
// and setup oopmap.
//
SafepointBlob* SharedRuntime::generate_handler_blob(address call_ptr, int poll_type) {
    ResourceMark rm;
  OopMapSet *oop_maps = new OopMapSet();
  OopMap* map;

  // Allocate space for the code.  Setup code generation tools.
  CodeBuffer buffer("handler_blob", 2048, 1024);
  MacroAssembler* masm = new MacroAssembler(&buffer);

  address start   = __ pc();
  address call_pc = NULL;
  int frame_size_in_words;
  bool cause_return = (poll_type == POLL_AT_RETURN);
  bool save_vectors = (poll_type == POLL_AT_VECTOR_LOOP);

  // Save registers, fpu state, and flags
  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  // The following is basically a call_VM.  However, we need the precise
  // address of the call in order to generate an oopmap. Hence, we do all the
  // work outselves.

  Label retaddr;
  __ set_last_Java_frame(sp, rfp, retaddr, rscratch1);

  // The return address must always be correct so that frame constructor never
  // sees an invalid pc.

  if (!cause_return) {
    // overwrite the return address pushed by save_live_registers
    __ ldr(c_rarg0, Address(rthread, JavaThread::saved_exception_pc_offset()));
    __ str(c_rarg0, Address(rfp, wordSize));
  }

  // Do the call
  __ mov(c_rarg0, rthread);
  __ lea(rscratch1, RuntimeAddress(call_ptr));
  __ brx86(rscratch1, 1, 0, 1);
  __ bind(retaddr);

  // Set an oopmap for the call site.  This oopmap will map all
  // oop-registers and debug-info registers as callee-saved.  This
  // will allow deoptimization at this safepoint to find all possible
  // debug-info recordings, as well as let GC find all oops.

  oop_maps->add_gc_map( __ pc() - start, map);

  Label noException;

  __ reset_last_Java_frame(false, false);

  __ ldr(rscratch1, Address(rthread, Thread::pending_exception_offset()));
  __ cbz(rscratch1, noException);

  // Exception pending

  RegisterSaver::restore_live_registers(masm);

  __ b(RuntimeAddress(StubRoutines::forward_exception_entry()));

  // No exception case
  __ bind(noException);

  // Normal exit, restore registers and exit.
  RegisterSaver::restore_live_registers(masm);

  __ ret(lr);

  // Make sure all code is generated
  masm->flush();

  // Fill-out other meta info
  return SafepointBlob::create(&buffer, oop_maps, frame_size_in_words);
}

//
// generate_resolve_blob - call resolution (static/virtual/opt-virtual/ic-miss
//
// Generate a stub that calls into vm to find out the proper destination
// of a java call. All the argument registers are live at this point
// but since this is generic code we don't know what they are and the caller
// must do any gc of the args.
//
RuntimeStub* SharedRuntime::generate_resolve_blob(address destination, const char* name) {
  assert (StubRoutines::forward_exception_entry() != NULL, "must be generated before");

  // allocate space for the code
  ResourceMark rm;

  CodeBuffer buffer(name, 1000, 512);
  MacroAssembler* masm                = new MacroAssembler(&buffer);

  int frame_size_in_words;

  OopMapSet *oop_maps = new OopMapSet();
  OopMap* map = NULL;

  int start = __ offset();

  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  int frame_complete = __ offset();

  {
    Label retaddr;
    __ set_last_Java_frame(sp, rfp, retaddr, rscratch1);

    __ mov(c_rarg0, rthread);
    __ mov(rscratch1, RuntimeAddress(destination));

    __ brx86(rscratch1, 1, 0, 1);
    __ bind(retaddr);
  }

  // Set an oopmap for the call site.
  // We need this not only for callee-saved registers, but also for volatile
  // registers that the compiler might be keeping live across a safepoint.

  oop_maps->add_gc_map( __ offset() - start, map);

  // rax contains the address we are going to jump to assuming no exception got installed

  // clear last_Java_sp
  __ reset_last_Java_frame(false, false);
  // check for pending exceptions
  Label pending;
  __ ldr(rscratch1, Address(rthread, Thread::pending_exception_offset()));
  __ cbnz(rscratch1, pending);

  // get the returned Method*
  __ get_vm_result_2(rmethod, rthread);
  __ str(rmethod, Address(sp, RegisterSaver::reg_offset_in_bytes(rmethod)));

  // r0 is where we want to jump, overwrite rscratch1 which is saved and scratch
  __ str(r0, Address(sp, RegisterSaver::rscratch1_offset_in_bytes()));
  RegisterSaver::restore_live_registers(masm);

  // We are back the the original state on entry and ready to go.

  __ br(rscratch1);

  // Pending exception after the safepoint

  __ bind(pending);

  RegisterSaver::restore_live_registers(masm);

  // exception pending => remove activation and forward to exception handler

  __ str(zr, Address(rthread, JavaThread::vm_result_offset()));

  __ ldr(r0, Address(rthread, Thread::pending_exception_offset()));
  __ b(RuntimeAddress(StubRoutines::forward_exception_entry()));

  // -------------
  // make sure all code is generated
  masm->flush();

  // return the  blob
  // frame_size_words or bytes??
  return RuntimeStub::new_runtime_stub(name, &buffer, frame_complete, frame_size_in_words, oop_maps, true);
}


#ifdef COMPILER2
// This is here instead of runtime_x86_64.cpp because it uses SimpleRuntimeFrame
//
//------------------------------generate_exception_blob---------------------------
// creates exception blob at the end
// Using exception blob, this code is jumped from a compiled method.
// (see emit_exception_handler in x86_64.ad file)
//
// Given an exception pc at a call we call into the runtime for the
// handler in this method. This handler might merely restore state
// (i.e. callee save registers) unwind the frame and jump to the
// exception handler for the nmethod if there is no Java level handler
// for the nmethod.
//
// This code is entered with a jmp.
//
// Arguments:
//   rax: exception oop
//   rdx: exception pc
//
// Results:
//   rax: exception oop
//   rdx: exception pc in caller or ???
//   destination: exception handler of caller
//
// Note: the exception pc MUST be at a call (precise debug information)
//       Registers rax, rdx, rcx, rsi, rdi, r8-r11 are not callee saved.
//

void OptoRuntime::generate_exception_blob() { Unimplemented(); }
#endif // COMPILER2

/*
 * Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.
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
#include "asm/assembler.hpp"
#include "c1/c1_Compilation.hpp"
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "c1/c1_Runtime1.hpp"
#include "c1/c1_ValueStack.hpp"
#include "ci/ciArrayKlass.hpp"
#include "ci/ciInstance.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "memory/barrierSet.hpp"
#include "memory/cardTableModRefBS.hpp"
#include "nativeInst_aarch64.hpp"
#include "oops/objArrayKlass.hpp"
#include "runtime/sharedRuntime.hpp"



NEEDS_CLEANUP // remove this definitions ?
const Register IC_Klass    = r0;   // where the IC klass is cached
const Register SYNC_header = r0;   // synchronization header
const Register SHIFT_count = r0;   // where count for shift operations must be

#define __ _masm->


static void select_different_registers(Register preserve,
                                       Register extra,
                                       Register &tmp1,
                                       Register &tmp2) { Unimplemented(); }



static void select_different_registers(Register preserve,
                                       Register extra,
                                       Register &tmp1,
                                       Register &tmp2,
                                       Register &tmp3) { Unimplemented(); }



bool LIR_Assembler::is_small_constant(LIR_Opr opr) { Unimplemented(); return false; }


LIR_Opr LIR_Assembler::receiverOpr() { Unimplemented(); return 0; }

LIR_Opr LIR_Assembler::osrBufferPointer() { Unimplemented(); return 0; }

//--------------fpu register translations-----------------------


address LIR_Assembler::float_constant(float f) {
  address const_addr = __ float_constant(f);
  if (const_addr == NULL) {
    bailout("const section overflow");
    return __ code()->consts()->start();
  } else {
    return const_addr;
  }
}


address LIR_Assembler::double_constant(double d) {
  address const_addr = __ double_constant(d);
  if (const_addr == NULL) {
    bailout("const section overflow");
    return __ code()->consts()->start();
  } else {
    return const_addr;
  }
}

void LIR_Assembler::set_24bit_FPU() { Unimplemented(); }

void LIR_Assembler::reset_FPU() { Unimplemented(); }

void LIR_Assembler::fpop() { Unimplemented(); }

void LIR_Assembler::fxch(int i) { Unimplemented(); }

void LIR_Assembler::fld(int i) { Unimplemented(); }

void LIR_Assembler::ffree(int i) { Unimplemented(); }

void LIR_Assembler::breakpoint() { Unimplemented(); }

void LIR_Assembler::push(LIR_Opr opr) { Unimplemented(); }

void LIR_Assembler::pop(LIR_Opr opr) { Unimplemented(); }

bool LIR_Assembler::is_literal_address(LIR_Address* addr) { Unimplemented(); return false; }
//-------------------------------------------

Address LIR_Assembler::as_Address(LIR_Address* addr) { Unimplemented(); return Address(sp, 0); }

Address LIR_Assembler::as_Address(LIR_Address* addr, Register tmp) { Unimplemented(); return Address(sp, 0); }


Address LIR_Assembler::as_Address_hi(LIR_Address* addr) { Unimplemented(); return Address(sp, 0); }


Address LIR_Assembler::as_Address_lo(LIR_Address* addr) { Unimplemented(); return Address(sp, 0); }


void LIR_Assembler::osr_entry() { Unimplemented(); }


// inline cache check; done before the frame is built.
int LIR_Assembler::check_icache() { Unimplemented(); return 0; }


void LIR_Assembler::jobject2reg_with_patching(Register reg, CodeEmitInfo* info) { Unimplemented(); }


// This specifies the rsp decrement needed to build the frame
int LIR_Assembler::initial_frame_size_in_bytes() {
  // if rounding, must let FrameMap know!

  // The frame_map records size in slots (32bit word)

  // subtract two words to account for return address and link
  return (frame_map()->framesize() - (2*VMRegImpl::slots_per_word))  * VMRegImpl::stack_slot_size;
}


int LIR_Assembler::emit_exception_handler() { __ call_Unimplemented(); return 0; }


// Emit the code to remove the frame from the stack in the exception
// unwind path.
int LIR_Assembler::emit_unwind_handler() { __ call_Unimplemented(); return 0; }


int LIR_Assembler::emit_deopt_handler() { __ call_Unimplemented(); return 0; }


// This is the fast version of java.lang.String.compare; it has not
// OSR-entry and therefore, we generate a slow version for OSR's
void LIR_Assembler::emit_string_compare(LIR_Opr arg0, LIR_Opr arg1, LIR_Opr dst, CodeEmitInfo* info) { __ call_Unimplemented(); }



void LIR_Assembler::return_op(LIR_Opr result) {
  assert(result->is_illegal() || !result->is_single_cpu() || result->as_register() == r0, "word returns are in r0,");
  // Pop the stack before the safepoint code
  __ remove_frame(initial_frame_size_in_bytes());

  bool result_is_oop = result->is_valid() ? result->is_oop() : false;

  // Note: we do not need to round double result; float result has the right precision
  // the poll sets the condition code, but no data registers
  Address polling_page(os::get_polling_page() + (SafepointPollOffset % os::vm_page_size()),
		       relocInfo::poll_return_type);

  unsigned long byte_offset;
  __ adrp(rscratch1, polling_page, byte_offset);
  __ ldr(zr, Address(rscratch1, byte_offset));

  __ ret(lr);
}


int LIR_Assembler::safepoint_poll(LIR_Opr tmp, CodeEmitInfo* info) { __ call_Unimplemented(); return 0; }


void LIR_Assembler::move_regs(Register from_reg, Register to_reg) { __ call_Unimplemented(); }

void LIR_Assembler::swap_reg(Register a, Register b) { __ call_Unimplemented(); }


void LIR_Assembler::const2reg(LIR_Opr src, LIR_Opr dest, LIR_PatchCode patch_code, CodeEmitInfo* info) {
  assert(src->is_constant(), "should not call otherwise");
  assert(dest->is_register(), "should not call otherwise");
  LIR_Const* c = src->as_constant_ptr();

  switch (c->type()) {
    case T_INT: {
      assert(patch_code == lir_patch_none, "no patching handled here");
      __ mov(dest->as_register(), c->as_jint());
      break;
    }

    case T_ADDRESS: {
      assert(patch_code == lir_patch_none, "no patching handled here");
      __ mov(dest->as_register(), c->as_jint());
      break;
    }

    case T_LONG: {
      assert(patch_code == lir_patch_none, "no patching handled here");
      __ mov(dest->as_register_lo(), (intptr_t)c->as_jlong());
      break;
    }

    case T_OBJECT: {
      if (patch_code != lir_patch_none) {
        jobject2reg_with_patching(dest->as_register(), info);
      } else {
        Unimplemented();
      }
      break;
    }

    case T_METADATA: {
      Unimplemented();
      break;
    }

    case T_FLOAT: {
      if (__ operand_valid_for_float_immediate(c->as_jfloat())) {
	__ fmovs(dest->as_float_reg(), (c->as_jfloat()));
      } else {
	__ ldrs(dest->as_float_reg(),
		InternalAddress(float_constant(c->as_jfloat())));
      }
      break;
    }

    case T_DOUBLE: {
      if (__ operand_valid_for_float_immediate(c->as_jdouble())) {
	__ fmovd(dest->as_double_reg(), (c->as_jdouble()));
      } else {
	__ adr(rscratch1, InternalAddress(double_constant(c->as_jdouble())));
	__ ldrd(dest->as_double_reg(), Address(rscratch1));
      }
      break;
    }

    default:
      ShouldNotReachHere();
  }
}

void LIR_Assembler::const2stack(LIR_Opr src, LIR_Opr dest) { Unimplemented(); }

void LIR_Assembler::const2mem(LIR_Opr src, LIR_Opr dest, BasicType type, CodeEmitInfo* info, bool wide) { Unimplemented(); }


void LIR_Assembler::reg2reg(LIR_Opr src, LIR_Opr dest) {
  assert(src->is_register(), "should not call otherwise");
  assert(dest->is_register(), "should not call otherwise");

  // move between cpu-registers
  if (dest->is_single_cpu()) {
    if (src->type() == T_LONG) {
      // Can do LONG -> OBJECT
      move_regs(src->as_register_lo(), dest->as_register());
      return;
    }
    assert(src->is_single_cpu(), "must match");
    if (src->type() == T_OBJECT) {
      __ verify_oop(src->as_register());
    }
    move_regs(src->as_register(), dest->as_register());

  } else if (dest->is_double_cpu()) {
    if (src->type() == T_OBJECT || src->type() == T_ARRAY) {
      // Surprising to me but we can see move of a long to t_object
      __ verify_oop(src->as_register());
      move_regs(src->as_register(), dest->as_register_lo());
      return;
    }
    assert(src->is_double_cpu(), "must match");
    Register f_lo = src->as_register_lo();
    Register f_hi = src->as_register_hi();
    Register t_lo = dest->as_register_lo();
    Register t_hi = dest->as_register_hi();
    assert(f_hi == f_lo, "must be same");
    assert(t_hi == t_lo, "must be same");
    move_regs(f_lo, t_lo);

  } else if (dest->is_single_fpu()) {
    __ fmovs(dest->as_float_reg(), src->as_float_reg());

  } else if (dest->is_double_fpu()) {
    __ fmovd(src->as_double_reg(), src->as_double_reg());

  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::reg2stack(LIR_Opr src, LIR_Opr dest, BasicType type, bool pop_fpu_stack) {
    if (src->is_single_cpu()) {
    if (type == T_ARRAY || type == T_OBJECT) {
      __ str(src->as_register(), frame_map()->address_for_slot(dest->single_stack_ix()));
      __ verify_oop(src->as_register());
    } else if (type == T_METADATA) {
      __ str(src->as_register(), frame_map()->address_for_slot(dest->single_stack_ix()));
    } else {
      __ strw(src->as_register(), frame_map()->address_for_slot(dest->single_stack_ix()));
    }

  } else if (src->is_double_cpu()) {
    Address dest_addr_LO = frame_map()->address_for_slot(dest->double_stack_ix(), lo_word_offset_in_bytes);
    __ str(src->as_register_lo(), dest_addr_LO);

  } else if (src->is_single_fpu()) {
    Address dest_addr = frame_map()->address_for_slot(dest->single_stack_ix());
    __ strs(src->as_float_reg(), dest_addr);

  } else if (src->is_double_fpu()) {
    Address dest_addr = frame_map()->address_for_slot(dest->double_stack_ix());
    __ strd(src->as_double_reg(), dest_addr);

  } else {
    ShouldNotReachHere();
  }

}


void LIR_Assembler::reg2mem(LIR_Opr src, LIR_Opr dest, BasicType type, LIR_PatchCode patch_code, CodeEmitInfo* info, bool pop_fpu_stack, bool wide, bool /* unaligned */) { Unimplemented(); }


void LIR_Assembler::stack2reg(LIR_Opr src, LIR_Opr dest, BasicType type) {
  assert(src->is_stack(), "should not call otherwise");
  assert(dest->is_register(), "should not call otherwise");

  if (dest->is_single_cpu()) {
    if (type == T_ARRAY || type == T_OBJECT) {
      __ ldr(dest->as_register(), frame_map()->address_for_slot(src->single_stack_ix()));
      __ verify_oop(dest->as_register());
    } else if (type == T_METADATA) {
      __ ldr(dest->as_register(), frame_map()->address_for_slot(src->single_stack_ix()));
    } else {
      __ ldrw(dest->as_register(), frame_map()->address_for_slot(src->single_stack_ix()));
    }

  } else if (dest->is_double_cpu()) {
    Address src_addr_LO = frame_map()->address_for_slot(src->double_stack_ix(), lo_word_offset_in_bytes);
    __ ldr(dest->as_register_lo(), src_addr_LO);

  } else if (dest->is_single_fpu()) {
    Address src_addr = frame_map()->address_for_slot(src->single_stack_ix());
    __ ldrs(dest->as_float_reg(), src_addr);

  } else if (dest->is_double_fpu()) {
    Address src_addr = frame_map()->address_for_slot(src->double_stack_ix());
    __ ldrd(dest->as_double_reg(), src_addr);

  } else {
    ShouldNotReachHere();
  }
}


void LIR_Assembler::stack2stack(LIR_Opr src, LIR_Opr dest, BasicType type) { Unimplemented(); }


void LIR_Assembler::mem2reg(LIR_Opr src, LIR_Opr dest, BasicType type, LIR_PatchCode patch_code, CodeEmitInfo* info, bool wide, bool /* unaligned */) { Unimplemented(); }


void LIR_Assembler::prefetchr(LIR_Opr src) { Unimplemented(); }


void LIR_Assembler::prefetchw(LIR_Opr src) { Unimplemented(); }


NEEDS_CLEANUP; // This could be static?
Address::ScaleFactor LIR_Assembler::array_element_size(BasicType type) const { Unimplemented(); return Address::times_4; }


void LIR_Assembler::emit_op3(LIR_Op3* op) { Unimplemented(); }

void LIR_Assembler::emit_opBranch(LIR_OpBranch* op) { Unimplemented(); }

void LIR_Assembler::emit_opConvert(LIR_OpConvert* op) {
  LIR_Opr src  = op->in_opr();
  LIR_Opr dest = op->result_opr();
  Register tmp = op->tmp1()->as_register();

  switch (op->bytecode()) {
    case Bytecodes::_d2i:
      {
	Label L_Okay;
	__ clear_fpsr();
	__ fcvtzdw(dest->as_register(), src->as_double_reg());
	__ get_fpsr(tmp);
	__ cbnzw(tmp, *(op->stub()->entry()));
	__ bind(*op->stub()->continuation());
	break;
      }
    case Bytecodes::_i2f:
      {
	__ scvtfws(dest->as_float_reg(), src->as_register());
	break;
      }
    case Bytecodes::_i2d:
      {
	__ scvtfwd(dest->as_double_reg(), src->as_register());
	break;
      }
    case Bytecodes::_l2d:
      {
	__ scvtfd(dest->as_double_reg(), src->as_register_lo());
	break;
      }
    case Bytecodes::_f2d:
      {
	__ fcvts(dest->as_double_reg(), src->as_float_reg());
	break;
      }
    case Bytecodes::_i2l:
      {
	__ sxtw(dest->as_register_lo(), src->as_register());
	break;
      }

    case Bytecodes::_l2i:
      {
	// FIXME: This could be a no-op
	__ uxtw(dest->as_register(), src->as_register_lo());
	break;
      }

    default: ShouldNotReachHere();
  }
}

void LIR_Assembler::emit_alloc_obj(LIR_OpAllocObj* op) { Unimplemented(); }

void LIR_Assembler::emit_alloc_array(LIR_OpAllocArray* op) { Unimplemented(); }

void LIR_Assembler::type_profile_helper(Register mdo,
                                        ciMethodData *md, ciProfileData *data,
                                        Register recv, Label* update_done) { Unimplemented(); }

void LIR_Assembler::emit_typecheck_helper(LIR_OpTypeCheck *op, Label* success, Label* failure, Label* obj_is_null) { Unimplemented(); }


void LIR_Assembler::emit_opTypeCheck(LIR_OpTypeCheck* op) { Unimplemented(); }


void LIR_Assembler::emit_compare_and_swap(LIR_OpCompareAndSwap* op) { Unimplemented(); }

void LIR_Assembler::cmove(LIR_Condition condition, LIR_Opr opr1, LIR_Opr opr2, LIR_Opr result, BasicType type) { Unimplemented(); }


void LIR_Assembler::arith_op(LIR_Code code, LIR_Opr left, LIR_Opr right, LIR_Opr dest, CodeEmitInfo* info, bool pop_fpu_stack) {
  assert(info == NULL, "should never be used, idiv/irem and ldiv/lrem not handled by this method");

  if (left->is_single_cpu()) {
    assert(left == dest, "left and dest must be equal");
    Register lreg = left->as_register();

    if (right->is_single_cpu()) {
      // cpu register - cpu register
      Register rreg = right->as_register();
      switch (code) {
      case lir_add: __ addw (dest->as_register(), lreg, rreg); break;
      case lir_sub:
      case lir_mul:
      default:      ShouldNotReachHere();
      }

    } else if (right->is_stack()) {
      // cpu register - stack
      Address raddr = frame_map()->address_for_slot(right->single_stack_ix());
      switch (code) {
        case lir_add:
        case lir_sub:
        default:      ShouldNotReachHere();
      }

    } else if (right->is_constant()) {
      // cpu register - constant
      jint c = right->as_constant_ptr()->as_jint();
      switch (code) {
        case lir_add: {
        }
        case lir_sub: {
        }
        default: ShouldNotReachHere();
      }

    } else {
      ShouldNotReachHere();
    }

  } else if (left->is_double_cpu()) {
    Register lreg_lo = left->as_register_lo();
    Register lreg_hi = left->as_register_hi();

    if (right->is_double_cpu()) {
      // cpu register - cpu register
      Register rreg_lo = right->as_register_lo();
      Register rreg_hi = right->as_register_hi();
      assert_different_registers(lreg_lo, rreg_lo);
      switch (code) {
      case lir_add: __ add (dest->as_register_lo(), lreg_lo, rreg_lo); break;
      case lir_sub:
      case lir_mul: __ mul (dest->as_register_lo(), lreg_lo, rreg_lo); break;
      default:
	ShouldNotReachHere();
      }

    } else if (right->is_constant()) {
      jlong c = right->as_constant_ptr()->as_jlong_bits();
      switch (code) {
        case lir_add:
	  if (__ operand_valid_for_add_sub_immediate(c))
	    __ add(dest->as_register_lo(), lreg_lo, c);
	  else {
	    __ mov(rscratch1, c);
	    __ add(dest->as_register_lo(), lreg_lo, rscratch1);
	  }
	  break;

        case lir_sub:
        default:
          ShouldNotReachHere();
      }
    } else {
      ShouldNotReachHere();
    }
  } else if (left->is_single_fpu()) {
      switch (code) {
      case lir_add: __ fadds (dest->as_float_reg(), left->as_float_reg(), right->as_float_reg()); break;
      case lir_sub:
      case lir_mul:
      default:
	ShouldNotReachHere();
      }
  } else if (left->is_double_fpu()) {
    if (right->is_double_fpu()) {
      // cpu register - cpu register
      switch (code) {
      case lir_add: __ faddd (dest->as_double_reg(), left->as_double_reg(), right->as_double_reg()); break;
      case lir_sub:
      case lir_mul: __ fmuld (dest->as_double_reg(), left->as_double_reg(), right->as_double_reg()); break;
      default:
	ShouldNotReachHere();
      }
    } else {
      if (right->is_constant()) {
	ShouldNotReachHere();
      }
      ShouldNotReachHere();
    }
  } else if (left->is_single_stack() || left->is_address()) {
    assert(left == dest, "left and dest must be equal");
    ShouldNotReachHere();
  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::arith_fpu_implementation(LIR_Code code, int left_index, int right_index, int dest_index, bool pop_fpu_stack) { Unimplemented(); }


void LIR_Assembler::intrinsic_op(LIR_Code code, LIR_Opr value, LIR_Opr unused, LIR_Opr dest, LIR_Op* op) { Unimplemented(); }

void LIR_Assembler::logic_op(LIR_Code code, LIR_Opr left, LIR_Opr right, LIR_Opr dst) { Unimplemented(); }


// we assume that rax, and rdx can be overwritten
void LIR_Assembler::arithmetic_idiv(LIR_Code code, LIR_Opr left, LIR_Opr right, LIR_Opr temp, LIR_Opr result, CodeEmitInfo* info) { Unimplemented(); }


void LIR_Assembler::comp_op(LIR_Condition condition, LIR_Opr opr1, LIR_Opr opr2, LIR_Op2* op) { Unimplemented(); }

void LIR_Assembler::comp_fl2i(LIR_Code code, LIR_Opr left, LIR_Opr right, LIR_Opr dst, LIR_Op2* op) { Unimplemented(); }


void LIR_Assembler::align_call(LIR_Code code) { Unimplemented(); }


void LIR_Assembler::call(LIR_OpJavaCall* op, relocInfo::relocType rtype) { Unimplemented(); }



void LIR_Assembler::ic_call(LIR_OpJavaCall* op) { Unimplemented(); }


/* Currently, vtable-dispatch is only enabled for sparc platforms */
void LIR_Assembler::vtable_call(LIR_OpJavaCall* op) {
  ShouldNotReachHere();
}


void LIR_Assembler::emit_static_call_stub() { Unimplemented(); }


void LIR_Assembler::throw_op(LIR_Opr exceptionPC, LIR_Opr exceptionOop, CodeEmitInfo* info) { Unimplemented(); }


void LIR_Assembler::unwind_op(LIR_Opr exceptionOop) { Unimplemented(); }


void LIR_Assembler::shift_op(LIR_Code code, LIR_Opr left, LIR_Opr count, LIR_Opr dest, LIR_Opr tmp) { Unimplemented(); }


void LIR_Assembler::shift_op(LIR_Code code, LIR_Opr left, jint count, LIR_Opr dest) { Unimplemented(); }


void LIR_Assembler::store_parameter(Register r, int offset_from_rsp_in_words) { Unimplemented(); }


void LIR_Assembler::store_parameter(jint c,     int offset_from_rsp_in_words) { Unimplemented(); }


void LIR_Assembler::store_parameter(jobject o,  int offset_from_rsp_in_words) { Unimplemented(); }


// This code replaces a call to arraycopy; no exception may
// be thrown in this code, they must be thrown in the System.arraycopy
// activation frame; we could save some checks if this would not be the case
void LIR_Assembler::emit_arraycopy(LIR_OpArrayCopy* op) { Unimplemented(); }


void LIR_Assembler::emit_lock(LIR_OpLock* op) { Unimplemented(); }


void LIR_Assembler::emit_profile_call(LIR_OpProfileCall* op) { Unimplemented(); }

void LIR_Assembler::emit_delay(LIR_OpDelay*) {
  Unimplemented();
}


void LIR_Assembler::monitor_address(int monitor_no, LIR_Opr dst) { Unimplemented(); }


void LIR_Assembler::align_backward_branch_target() { Unimplemented(); }


void LIR_Assembler::negate(LIR_Opr left, LIR_Opr dest) { Unimplemented(); }


void LIR_Assembler::leal(LIR_Opr addr, LIR_Opr dest) { Unimplemented(); }


void LIR_Assembler::rt_call(LIR_Opr result, address dest, const LIR_OprList* args, LIR_Opr tmp, CodeEmitInfo* info) { Unimplemented(); }


void LIR_Assembler::volatile_move_op(LIR_Opr src, LIR_Opr dest, BasicType type, CodeEmitInfo* info) { Unimplemented(); }


void LIR_Assembler::membar() { Unimplemented(); }

void LIR_Assembler::membar_acquire() { Unimplemented(); }

void LIR_Assembler::membar_release() { Unimplemented(); }

void LIR_Assembler::membar_loadload() { Unimplemented(); }

void LIR_Assembler::membar_storestore() { Unimplemented(); }

void LIR_Assembler::membar_loadstore() { Unimplemented(); }

void LIR_Assembler::membar_storeload() { Unimplemented(); }

void LIR_Assembler::get_thread(LIR_Opr result_reg) { Unimplemented(); }


void LIR_Assembler::peephole(LIR_List*) {
}

void LIR_Assembler::atomic_op(LIR_Code code, LIR_Opr src, LIR_Opr data, LIR_Opr dest, LIR_Opr tmp) { Unimplemented(); }

#undef __
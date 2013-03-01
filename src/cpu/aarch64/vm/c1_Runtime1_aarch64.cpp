/*
 * Copyright (c) 1999, 2011, Oracle and/or its affiliates. All rights reserved.
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
#include "c1/c1_Defs.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "c1/c1_Runtime1.hpp"
#include "interpreter/interpreter.hpp"
#include "nativeInst_aarch64.hpp"
#include "oops/compiledICHolder.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "register_aarch64.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/signature.hpp"
#include "runtime/vframeArray.hpp"
#include "vmreg_aarch64.inline.hpp"


// Implementation of StubAssembler

int StubAssembler::call_RT(Register oop_result1, Register metadata_result, address entry, int args_size) {
  // setup registers
  assert(!(oop_result1->is_valid() || metadata_result->is_valid()) || oop_result1 != metadata_result, "registers must be different");
  assert(oop_result1 != rthread && metadata_result != rthread, "registers must be different");
  assert(args_size >= 0, "illegal args_size");
  bool align_stack = false;

  mov(c_rarg0, rthread);
  set_num_rt_args(0); // Nothing on stack

  Label retaddr;
  set_last_Java_frame(sp, rfp, retaddr, rscratch1);

  // do the call
  mov(rscratch1, RuntimeAddress(entry));
  brx86(rscratch1, args_size + 1, 8, 1);
  bind(retaddr);
  int call_offset = offset();
  // verify callee-saved register
#ifdef ASSERT
  push(0b1, sp); // r0
  { Label L;
    get_thread(r0);
    cmp(rthread, r0);
    br(Assembler::EQ, L);
    stop("StubAssembler::call_RT: rthread not callee saved?");
    bind(L);
  }
  pop(0b1, sp);
#endif
  reset_last_Java_frame(true, true);

  // check for pending exceptions
  { Label L;
    // check for pending exceptions (java_thread is set upon return)
    ldr(rscratch1, Address(rthread, in_bytes(Thread::pending_exception_offset())));
    cbz(rscratch1, L);
    // exception pending => remove activation and forward to exception handler
    ldr(r0, Address(rthread, Thread::pending_exception_offset()));
    // make sure that the vm_results are cleared
    if (oop_result1->is_valid()) {
      str(zr, Address(rthread, JavaThread::vm_result_offset()));
    }
    if (metadata_result->is_valid()) {
      str(zr, Address(rthread, JavaThread::vm_result_2_offset()));
    }
    if (frame_size() == no_frame_size) {
      leave();
      b(RuntimeAddress(StubRoutines::forward_exception_entry()));
    } else if (_stub_id == Runtime1::forward_exception_id) {
      should_not_reach_here();
    } else {
      b(RuntimeAddress(Runtime1::entry_for(Runtime1::forward_exception_id)));
    }
    bind(L);
  }
  // get oop results if there are any and reset the values in the thread
  if (oop_result1->is_valid()) {
    get_vm_result(oop_result1, rthread);
  }
  if (metadata_result->is_valid()) {
    get_vm_result_2(metadata_result, rthread);
  }
  return call_offset;
}


int StubAssembler::call_RT(Register oop_result1, Register metadata_result, address entry, Register arg1) {
  mov(c_rarg1, arg1);
  return call_RT(oop_result1, metadata_result, entry, 1);
}


int StubAssembler::call_RT(Register oop_result1, Register metadata_result, address entry, Register arg1, Register arg2) {
  if (c_rarg1 == arg2) {
    if (c_rarg2 == arg1) {
      mov(rscratch1, arg1);
      mov(arg1, arg2);
      mov(arg2, rscratch1);
    } else {
      mov(c_rarg2, arg2);
      mov(c_rarg1, arg1);
    }
  } else {
    mov(c_rarg1, arg1);
    mov(c_rarg2, arg2);
  }
  return call_RT(oop_result1, metadata_result, entry, 2);
}


int StubAssembler::call_RT(Register oop_result1, Register metadata_result, address entry, Register arg1, Register arg2, Register arg3) {
  // if there is any conflict use the stack
  if (arg1 == c_rarg2 || arg1 == c_rarg3 ||
      arg2 == c_rarg1 || arg1 == c_rarg3 ||
      arg3 == c_rarg1 || arg1 == c_rarg2) {
    stp(arg3, arg2, Address(pre(sp, 2 * wordSize)));
    stp(arg1, zr, Address(pre(sp, -2 * wordSize)));
    ldp(c_rarg1, zr, Address(post(sp, 2 * wordSize)));
    ldp(c_rarg3, c_rarg2, Address(post(sp, 2 * wordSize)));
  } else {
    mov(c_rarg1, arg1);
    mov(c_rarg2, arg2);
    mov(c_rarg3, arg3);
  }
  return call_RT(oop_result1, metadata_result, entry, 3);
}

// Implementation of StubFrame

class StubFrame: public StackObj {
 private:
  StubAssembler* _sasm;

 public:
  StubFrame(StubAssembler* sasm, const char* name, bool must_gc_arguments);
  void load_argument(int offset_in_words, Register reg);

  ~StubFrame();
};;


#define __ _sasm->

StubFrame::StubFrame(StubAssembler* sasm, const char* name, bool must_gc_arguments) {
  _sasm = sasm;
  __ set_info(name, must_gc_arguments);
  __ enter();
}

// load parameters that were stored with LIR_Assembler::store_parameter
// Note: offsets for store_parameter and load_argument must match
void StubFrame::load_argument(int offset_in_words, Register reg) { Unimplemented(); }

StubFrame::~StubFrame() {
  __ leave();
  __ ret(lr);
}

#undef __


// Implementation of Runtime1

#define __ sasm->

const int float_regs_as_doubles_size_in_slots = pd_nof_fpu_regs_frame_map * 2;

// Stack layout for saving/restoring  all the registers needed during a runtime
// call (this includes deoptimization)
// Note: note that users of this frame may well have arguments to some runtime
// while these values are on the stack. These positions neglect those arguments
// but the code in save_live_registers will take the argument count into
// account.
//

enum reg_save_layout {
  reg_save_frame_size = 32 /* float */ + 32 /* integer */
};

// Save off registers which might be killed by calls into the runtime.
// Tries to smart of about FP registers.  In particular we separate
// saving and describing the FPU registers for deoptimization since we
// have to save the FPU registers twice if we describe them and on P4
// saving FPU registers which don't contain anything appears
// expensive.  The deopt blob is the only thing which needs to
// describe FPU registers.  In all other cases it should be sufficient
// to simply save their current value.

static int cpu_reg_save_offsets[FrameMap::nof_cpu_regs];
static int fpu_reg_save_offsets[FrameMap::nof_fpu_regs];
static int reg_save_size_in_words;
static int frame_size_in_bytes = -1;

static OopMap* generate_oop_map(StubAssembler* sasm, bool save_fpu_registers) {
  int frame_size_in_bytes = reg_save_frame_size * BytesPerWord;
  sasm->set_frame_size(frame_size_in_bytes / BytesPerWord);
  int frame_size_in_slots = frame_size_in_bytes / sizeof(jint);
  OopMap* oop_map = new OopMap(frame_size_in_slots, 0);

  int i;
  for (i = 0; i < FrameMap::nof_cpu_regs; i++) {
    Register r = as_Register(i);
    if (i >= 19 && i <= 28) {
      int sp_offset = cpu_reg_save_offsets[i];
      oop_map->set_callee_saved(VMRegImpl::stack2reg(sp_offset),
                                r->as_VMReg());
    }
  }

  if (save_fpu_registers) {
    for (i = 0; i < FrameMap::nof_fpu_regs; i++) {
      FloatRegister r = as_FloatRegister(i);
      if (i >= 8 && i <= 15) {
	int sp_offset = fpu_reg_save_offsets[i];
	oop_map->set_callee_saved(VMRegImpl::stack2reg(sp_offset),
				  r->as_VMReg());
      }
    }
  }
  return oop_map;
}

static OopMap* save_live_registers(StubAssembler* sasm,
                                   bool save_fpu_registers = true) {
  __ block_comment("save_live_registers");

  __ push(0x3fffffff, sp);         // integer registers except lr & sp

  if (save_fpu_registers) {
    for (int i = 30; i >= 0; i -= 2)
      __ stpd(as_FloatRegister(i), as_FloatRegister(i+1),
	      Address(__ pre(sp, -2 * wordSize)));
  }

  return generate_oop_map(sasm, save_fpu_registers);
}

static void restore_fpu(StubAssembler* sasm, bool restore_fpu_registers = true) { Unimplemented(); }


static void restore_live_registers(StubAssembler* sasm, bool restore_fpu_registers = true) {
  if (restore_fpu_registers) {
    for (int i = 0; i < 32; i += 2)
      __ ldpd(as_FloatRegister(i), as_FloatRegister(i+1),
	      Address(__ post(sp, 2 * wordSize)));
  }
  __ pop(0x3fffffff, sp);
}

static void restore_live_registers_except_rax(StubAssembler* sasm, bool restore_fpu_registers = true)  {

  if (restore_fpu_registers) {
    for (int i = 0; i < 32; i += 2)
      __ ldpd(as_FloatRegister(i), as_FloatRegister(i+1),
	      Address(__ post(sp, 2 * wordSize)));
  }
  __ ldp(zr, r1, Address(__ post(sp, 16)));
  __ pop(0x7ffffffc, sp);
}



void Runtime1::initialize_pd() {
  int i;
  int sp_offset = 0; //  start doubleword aligned

  for (i = 0; i < FrameMap::nof_cpu_regs; i++) {
    Register r = as_Register(i);
    cpu_reg_save_offsets[i] = sp_offset;
    sp_offset++;
  }

  // all float registers are saved explicitly
  assert(FrameMap::nof_fpu_regs == 32, "double registers not handled here");
  for (i = 0; i < FrameMap::nof_fpu_regs; i++) {
    fpu_reg_save_offsets[i] = sp_offset;
    sp_offset++;
  }
}


// target: the entry point of the method that creates and posts the exception oop
// has_argument: true if the exception needs an argument (passed on stack because registers must be preserved)

OopMapSet* Runtime1::generate_exception_throw(StubAssembler* sasm, address target, bool has_argument) { Unimplemented(); return 0; }


OopMapSet* Runtime1::generate_handle_exception(StubID id, StubAssembler *sasm) { Unimplemented(); return 0; }


void Runtime1::generate_unwind_exception(StubAssembler *sasm) { Unimplemented(); }


OopMapSet* Runtime1::generate_patching(StubAssembler* sasm, address target) { Unimplemented(); return 0; }


OopMapSet* Runtime1::generate_code_for(StubID id, StubAssembler* sasm) {

  // for better readability
  const bool must_gc_arguments = true;
  const bool dont_gc_arguments = false;

  // default value; overwritten for some optimized stubs that are called from methods that do not use the fpu
  bool save_fpu_registers = true;

  // stub code & info for the different stubs
  OopMapSet* oop_maps = NULL;
  switch (id) {
    case forward_exception_id:
      {
        __ call_Unimplemented();
      }
      break;

    case new_instance_id:
    case fast_new_instance_id:
    case fast_new_instance_init_check_id:
      {
	__ call_Unimplemented();
      }
      break;

    case new_type_array_id:
    case new_object_array_id:
      {
        Register length   = r19; // Incoming
        Register klass    = r3; // Incoming
        Register obj      = r0; // Result

        if (id == new_type_array_id) {
          __ set_info("new_type_array", dont_gc_arguments);
        } else {
          __ set_info("new_object_array", dont_gc_arguments);
        }

#ifdef ASSERT
        // assert object type is really an array of the proper kind
        {
          Label ok;
          Register t0 = obj;
          __ ldrw(t0, Address(klass, Klass::layout_helper_offset()));
          __ asrw(t0, t0, Klass::_lh_array_tag_shift);
          int tag = ((id == new_type_array_id)
                     ? Klass::_lh_array_tag_type_value
                     : Klass::_lh_array_tag_obj_value);
	  __ mov(rscratch1, tag);
          __ cmpw(t0, rscratch1);
          __ br(Assembler::EQ, ok);
          __ stop("assert(is an array klass)");
          __ should_not_reach_here();
          __ bind(ok);
        }
#endif // ASSERT

        if (UseTLAB && FastTLABRefill) {
          Register arr_size = r4;
          Register t1       = r1;
          Register t2       = r5;
          Label slow_path;
          assert_different_registers(length, klass, obj, arr_size, t1, t2);

          // check that array length is small enough for fast path.
	  __ mov(rscratch1, C1_MacroAssembler::max_array_allocation_length);
          __ cmpw(length, rscratch1);
          __ br(Assembler::HI, slow_path);

          // if we got here then the TLAB allocation failed, so try
          // refilling the TLAB or allocating directly from eden.
          Label retry_tlab, try_eden;
          const Register thread =
            __ tlab_refill(retry_tlab, try_eden, slow_path); // preserves rbx & rdx, returns rdi

          __ bind(retry_tlab);

          // get the allocation size: round_up(hdr + length << (layout_helper & 0x1F))
          // since size is positive ldrw does right thing on 64bit
          __ ldrw(t1, Address(klass, Klass::layout_helper_offset()));
          __ lslvw(arr_size, length, t1);
	  __ ubfx(t1, t1, Klass::_lh_header_size_shift,
		  exact_log2(Klass::_lh_header_size_mask + 1));
          __ add(arr_size, arr_size, t1);
          __ add(arr_size, arr_size, MinObjAlignmentInBytesMask); // align up
          __ andr(arr_size, arr_size, ~MinObjAlignmentInBytesMask);

          __ tlab_allocate(obj, arr_size, 0, t1, t2, slow_path);  // preserves arr_size

          __ initialize_header(obj, klass, length, t1, t2);
          __ ldrb(t1, Address(klass, in_bytes(Klass::layout_helper_offset()) + (Klass::_lh_header_size_shift / BitsPerByte)));
          assert(Klass::_lh_header_size_shift % BitsPerByte == 0, "bytewise");
          assert(Klass::_lh_header_size_mask <= 0xFF, "bytewise");
          __ andr(t1, t1, Klass::_lh_header_size_mask);
          __ sub(arr_size, arr_size, t1);  // body length
          __ add(t1, t1, obj);       // body start
          __ initialize_body(t1, arr_size, 0, t2);
          __ verify_oop(obj);
          __ ret(lr);

          __ bind(try_eden);
          // get the allocation size: round_up(hdr + length << (layout_helper & 0x1F))
          // since size is positive ldrw does right thing on 64bit
          __ ldrw(t1, Address(klass, Klass::layout_helper_offset()));
          // since size is postive movw does right thing on 64bit
          __ movw(arr_size, length);
          __ lslvw(arr_size, length, t1);
	  __ ubfx(t1, t1, Klass::_lh_header_size_shift,
		  exact_log2(Klass::_lh_header_size_mask + 1));
          __ add(arr_size, arr_size, t1);
          __ add(arr_size, arr_size, MinObjAlignmentInBytesMask); // align up
          __ andr(arr_size, arr_size, ~MinObjAlignmentInBytesMask);

          __ eden_allocate(obj, arr_size, 0, t1, slow_path);  // preserves arr_size
          __ incr_allocated_bytes(thread, arr_size, 0, rscratch1);

          __ initialize_header(obj, klass, length, t1, t2);
          __ ldrb(t1, Address(klass, in_bytes(Klass::layout_helper_offset()) + (Klass::_lh_header_size_shift / BitsPerByte)));
          assert(Klass::_lh_header_size_shift % BitsPerByte == 0, "bytewise");
          assert(Klass::_lh_header_size_mask <= 0xFF, "bytewise");
          __ andr(t1, t1, Klass::_lh_header_size_mask);
          __ sub(arr_size, arr_size, t1);  // body length
          __ add(t1, t1, obj);       // body start
          __ initialize_body(t1, arr_size, 0, t2);
          __ verify_oop(obj);
          __ ret(lr);

          __ bind(slow_path);
        }

        __ enter();
        OopMap* map = save_live_registers(sasm, 3);
        int call_offset;
	if (id == new_type_array_id) {
          call_offset = __ call_RT(obj, noreg, CAST_FROM_FN_PTR(address, new_type_array), klass, length);
        } else {
          call_offset = __ call_RT(obj, noreg, CAST_FROM_FN_PTR(address, new_object_array), klass, length);
        }

        oop_maps = new OopMapSet();
        oop_maps->add_gc_map(call_offset, map);
        restore_live_registers_except_rax(sasm);

        __ verify_oop(obj);
        __ leave();
        __ ret(lr);

        // r0: new array
      }
      break;

    default:
      { StubFrame f(sasm, "unimplemented entry", dont_gc_arguments);
        __ mov(r0, (int)id);
        __ call_RT(noreg, noreg, CAST_FROM_FN_PTR(address, unimplemented_entry), r0);
        __ should_not_reach_here();
      }
      break;
  }
  return oop_maps;
}

#undef __

const char *Runtime1::pd_name_for_address(address entry) { Unimplemented(); return 0; }
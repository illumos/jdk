/*
 * Copyright (c) 1999, 2020, Oracle and/or its affiliates. All rights reserved.
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

#include "jvm.h"
#include "asm/macroAssembler.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interpreter.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "os_solaris.hpp"
#include "os_posix.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/osThread.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/timer.hpp"
#include "signals_posix.hpp"
#include "utilities/align.hpp"
#include "utilities/events.hpp"
#include "utilities/vmError.hpp"

// put OS-includes here
# include <sys/types.h>
# include <sys/mman.h>
# include <pthread.h>
# include <signal.h>
# include <setjmp.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <thread.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/filio.h>
# include <sys/utsname.h>
# include <sys/systeminfo.h>
# include <sys/socket.h>
# include <sys/trap.h>
# include <sys/lwp.h>
# include <poll.h>
# include <sys/lwp.h>
# include <procfs.h>


#define MAX_PATH (2 * K)

// Minimum usable stack sizes required to get to user code. Space for
// HotSpot guard pages is added later.
#ifdef _LP64
// The adlc generated method 'State::MachNodeGenerator(int)' used by the C2 compiler
// threads requires a large stack with the Solaris Studio C++ compiler version 5.13
// and product VM builds (debug builds require significantly less stack space).
size_t os::_compiler_thread_min_stack_allowed = 325 * K;
size_t os::_java_thread_min_stack_allowed = 48 * K;
size_t os::_vm_internal_thread_min_stack_allowed = 224 * K;
#else
size_t os::_compiler_thread_min_stack_allowed = 32 * K;
size_t os::_java_thread_min_stack_allowed = 32 * K;
size_t os::_vm_internal_thread_min_stack_allowed = 64 * K;
#endif // _LP64

#define REG_SP REG_RSP
#define REG_PC REG_RIP
#define REG_FP REG_RBP
#define REG_BCP REG_R13

char* os::non_memory_address_word() {
  // Must never look like an address returned by reserve_memory,
  // even in its subfields (as defined by the CPU immediate fields,
  // if the CPU splits constants across multiple instructions).
  return (char*) -1;
}

//
// Validate a ucontext retrieved from walking a uc_link of a ucontext.
// There are issues with libthread giving out uc_links for different threads
// on the same uc_link chain and bad or circular links.
//
bool os::Solaris::valid_ucontext(Thread* thread, const ucontext_t* valid, const ucontext_t* suspect) {
  if (valid >= suspect ||
      valid->uc_stack.ss_flags != suspect->uc_stack.ss_flags ||
      valid->uc_stack.ss_sp    != suspect->uc_stack.ss_sp    ||
      valid->uc_stack.ss_size  != suspect->uc_stack.ss_size) {
    DEBUG_ONLY(tty->print_cr("valid_ucontext: failed test 1");)
    return false;
  }

  if (thread->is_Java_thread()) {
    if (!thread->is_in_full_stack_checked((address)suspect)) {
      DEBUG_ONLY(tty->print_cr("valid_ucontext: uc_link not in thread stack");)
      return false;
    }
    if (!thread->is_in_full_stack_checked((address) suspect->uc_mcontext.gregs[REG_SP])) {
      DEBUG_ONLY(tty->print_cr("valid_ucontext: stackpointer not in thread stack");)
      return false;
    }
  }
  return true;
}

// We will only follow one level of uc_link since there are libthread
// issues with ucontext linking and it is better to be safe and just
// let caller retry later.
const ucontext_t* os::Solaris::get_valid_uc_in_signal_handler(Thread *thread,
  const ucontext_t *uc) {

  const ucontext_t *retuc = nullptr;

  if (uc != nullptr) {
    if (uc->uc_link == nullptr) {
      // cannot validate without uc_link so accept current ucontext
      retuc = uc;
    } else if (os::Solaris::valid_ucontext(thread, uc, uc->uc_link)) {
      // first ucontext is valid so try the next one
      uc = uc->uc_link;
      if (uc->uc_link == nullptr) {
        // cannot validate without uc_link so accept current ucontext
        retuc = uc;
      } else if (os::Solaris::valid_ucontext(thread, uc, uc->uc_link)) {
        // the ucontext one level down is also valid so return it
        retuc = uc;
      }
    }
  }
  return retuc;
}

void os::Posix::ucontext_set_pc(ucontext_t* uc, address pc) {
  uc->uc_mcontext.gregs [REG_PC]  = (greg_t) pc;
}

// Assumes ucontext is valid
intptr_t* os::Solaris::ucontext_get_sp(const ucontext_t *uc) {
  return (intptr_t*)uc->uc_mcontext.gregs[REG_SP];
}

// Assumes ucontext is valid
intptr_t* os::Solaris::ucontext_get_fp(const ucontext_t *uc) {
  return (intptr_t*)uc->uc_mcontext.gregs[REG_FP];
}

address os::Posix::ucontext_get_pc(const ucontext_t *uc) {
  return (address) uc->uc_mcontext.gregs[REG_PC];
}

address os::fetch_frame_from_context(const void* ucVoid,
                    intptr_t** ret_sp, intptr_t** ret_fp) {

  address  epc;
  const ucontext_t *uc = (const ucontext_t*)ucVoid;

  if (uc != nullptr) {
    epc = os::Posix::ucontext_get_pc(uc);
    if (ret_sp) *ret_sp = os::Solaris::ucontext_get_sp(uc);
    if (ret_fp) *ret_fp = os::Solaris::ucontext_get_fp(uc);
  } else {
    epc = nullptr;
    if (ret_sp) *ret_sp = (intptr_t *)nullptr;
    if (ret_fp) *ret_fp = (intptr_t *)nullptr;
  }

  return epc;
}

frame os::fetch_frame_from_context(const void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  address epc = fetch_frame_from_context(ucVoid, &sp, &fp);
  return frame(sp, fp, epc);
}

frame os::fetch_compiled_frame_from_context(const void* ucVoid) {
  const ucontext_t* uc = (const ucontext_t*)ucVoid;
  frame fr = os::fetch_frame_from_context(uc);
  // in compiled code, the stack banging is performed just after the return pc
  // has been pushed on the stack
  return frame(fr.sp() + 1, fr.fp(), (address)*(fr.sp()));
}

intptr_t* os::fetch_bcp_from_context(const void* ucVoid) {
  assert(ucVoid != nullptr, "invariant");
  const ucontext_t* uc = (const ucontext_t*)ucVoid;
  assert(os::Posix::ucontext_is_interpreter(uc), "invariant");
  return reinterpret_cast<intptr_t*>(uc->uc_mcontext.gregs[REG_BCP]);
}

frame os::get_sender_for_C_frame(frame* fr) {
  return frame(fr->sender_sp(), fr->link(), fr->sender_pc());
}

extern "C" intptr_t *_get_current_sp() {
  register intptr_t *rsp __asm__ ("rsp");
  return rsp;
}

address os::current_stack_pointer() {
  return (address)_get_current_sp();
}

extern "C" intptr_t *_get_current_fp() {
  register intptr_t **rbp __asm__ ("rbp");
  return (intptr_t*) *rbp;
}

frame os::current_frame() {
  intptr_t* fp = _get_current_fp();  // it's inlined so want current fp
  // fp is for os::current_frame. We want the fp for our caller.
  frame myframe((intptr_t*)os::current_stack_pointer(),
                (intptr_t*)fp,
                CAST_FROM_FN_PTR(address, os::current_frame));
  frame caller_frame = os::get_sender_for_C_frame(&myframe);

  if (os::is_first_C_frame(&caller_frame)) {
    // stack is not walkable
    frame ret; // This will be a null useless frame
    return ret;
  } else {
    // return frame for our caller's caller
    return os::get_sender_for_C_frame(&caller_frame);
  }
}


juint os::cpu_microcode_revision() {
  juint result = 0;
  // to implement this, look at the source for ucodeadm -v
  return result;
}

bool PosixSignals::pd_hotspot_signal_handler(int sig, siginfo_t* info,
                                             ucontext_t* uc, JavaThread* thread) {

  if (info == nullptr || info->si_code <= 0 || info->si_code == SI_NOINFO) {
    // can't decode this kind of signal
    info = nullptr;
  } else {
    assert(sig == info->si_signo, "bad siginfo");
  }

  // decide if this trap can be handled by a stub
  address stub = nullptr;

  address pc          = nullptr;

  //%note os_trap_1
  if (info != nullptr && uc != nullptr && thread != nullptr) {
    // factor me: getPCfromContext
    pc = (address) uc->uc_mcontext.gregs[REG_PC];

    // Handle ALL stack overflow variations here
    if (sig == SIGSEGV && info->si_code == SEGV_ACCERR) {
      address addr = (address) info->si_addr;
      if (thread->is_in_full_stack(addr)) {
        // stack overflow
        if (os::Posix::handle_stack_overflow(thread, addr, pc, uc, &stub)) {
          return true; // continue
        }
      }
    }

    if ((sig == SIGSEGV) && VM_Version::is_cpuinfo_segv_addr(pc)) {
      // Verify that OS save/restore AVX registers.
      stub = VM_Version::cpuinfo_cont_addr();
    }

    if (thread->thread_state() == _thread_in_vm ||
         thread->thread_state() == _thread_in_native) {
      if (sig == SIGBUS && info->si_code == BUS_OBJERR && thread->doing_unsafe_access()) {
        address next_pc = Assembler::locate_next_instruction(pc);
        if (UnsafeMemoryAccess::contains_pc(pc)) {
          next_pc = UnsafeMemoryAccess::page_error_continue_pc(pc);
        }
        stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
      }
    }

    if (thread->thread_state() == _thread_in_Java) {
      // Support Safepoint Polling
      if ( sig == SIGSEGV && SafepointMechanism::is_poll_address((address)info->si_addr)) {
        stub = SharedRuntime::get_poll_stub(pc);
      }
      else if (sig == SIGBUS && info->si_code == BUS_OBJERR) {
        // BugId 4454115: A read from a MappedByteBuffer can fault
        // here if the underlying file has been truncated.
        // Do not crash the VM in such a case.
        CodeBlob* cb = CodeCache::find_blob(pc);
        if (cb != nullptr) {
          nmethod* nm = cb->as_nmethod_or_null();
          bool is_unsafe_memory_access = thread->doing_unsafe_access() && UnsafeMemoryAccess::contains_pc(pc);
          if ((nm != nullptr && nm->has_unsafe_access()) || is_unsafe_memory_access) {
            address next_pc = Assembler::locate_next_instruction(pc);
            if (is_unsafe_memory_access) {
              next_pc = UnsafeMemoryAccess::page_error_continue_pc(pc);
            }
            stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
          }
        }
      } else
      if (sig == SIGFPE && info->si_code == FPE_INTDIV) {
        // integer divide by zero
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_DIVIDE_BY_ZERO);
      }

        // QQQ It doesn't seem that we need to do this on x86 because we should be able
        // to return properly from the handler without this extra stuff on the back side.

      else if (sig == SIGSEGV && info->si_code > 0 &&
               MacroAssembler::uses_implicit_null_check(info->si_addr)) {
        // Determination of interpreter/vtable stub/compiled code null exception
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
      }
    }

    // jni_fast_Get<Primitive>Field can trap at certain pc's if a GC kicks in
    // and the heap gets shrunk before the field access.
    if ((sig == SIGSEGV) || (sig == SIGBUS)) {
      address addr = JNI_FastGetField::find_slowcase_pc(pc);
      if (addr != (address)-1) {
        stub = addr;
      }
    }
  }

  if (stub != nullptr) {
    // save all thread context in case we need to restore it
    if (thread != nullptr) thread->set_saved_exception_pc(pc);

    os::Posix::ucontext_set_pc(uc, stub);
    return true;
  }

  return false;
}

void os::print_context(outputStream *st, const void *context) {
  if (context == nullptr) return;

  const ucontext_t *uc = (const ucontext_t*)context;
  st->print_cr("Registers:");
  st->print(  "RAX=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RAX]);
  st->print(", RBX=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RBX]);
  st->print(", RCX=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RCX]);
  st->print(", RDX=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RDX]);
  st->cr();
  st->print(  "RSP=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RSP]);
  st->print(", RBP=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RBP]);
  st->print(", RSI=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RSI]);
  st->print(", RDI=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RDI]);
  st->cr();
  st->print(  "R8 =" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R8]);
  st->print(", R9 =" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R9]);
  st->print(", R10=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R10]);
  st->print(", R11=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R11]);
  st->cr();
  st->print(  "R12=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R12]);
  st->print(", R13=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R13]);
  st->print(", R14=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R14]);
  st->print(", R15=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_R15]);
  st->cr();
  st->print(  "RIP=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RIP]);
  st->print(", RFLAGS=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_RFL]);
  st->cr();
  st->cr();
}

void os::print_register_info(outputStream *st, const void *context, int& continuation) {
  const int register_count = 16;
  int n = continuation;
  assert(n >= 0 && n <= register_count, "Invalid continuation value");
  if (context == nullptr || n == register_count) {
    return;
  }

  const ucontext_t *uc = (const ucontext_t*)context;
  while (n < register_count) {
    // Update continuation with next index before printing location
    continuation = n + 1;
# define CASE_PRINT_REG(n, str, id) case n: st->print(str); print_location(st, uc->uc_mcontext.gregs[REG_##id]);
    switch (n) {
    CASE_PRINT_REG( 0, "RAX=", RAX); break;
    CASE_PRINT_REG( 1, "RBX=", RBX); break;
    CASE_PRINT_REG( 2, "RCX=", RCX); break;
    CASE_PRINT_REG( 3, "RDX=", RDX); break;
    CASE_PRINT_REG( 4, "RSP=", RSP); break;
    CASE_PRINT_REG( 5, "RBP=", RBP); break;
    CASE_PRINT_REG( 6, "RSI=", RSI); break;
    CASE_PRINT_REG( 7, "RDI=", RDI); break;
    CASE_PRINT_REG( 8, "R8 =", R8); break;
    CASE_PRINT_REG( 9, "R9 =", R9); break;
    CASE_PRINT_REG(10, "R10=", R10); break;
    CASE_PRINT_REG(11, "R11=", R11); break;
    CASE_PRINT_REG(12, "R12=", R12); break;
    CASE_PRINT_REG(13, "R13=", R13); break;
    CASE_PRINT_REG(14, "R14=", R14); break;
    CASE_PRINT_REG(15, "R15=", R15); break;
    }
# undef CASE_PRINT_REG
    ++n;
  }
}


void os::Solaris::init_thread_fpu_state(void) {
  // Nothing to do
}
void os::setup_fpu() {}

#ifndef PRODUCT
void os::verify_stack_alignment() {
  assert(((intptr_t)os::current_stack_pointer() & (StackAlignmentInBytes-1)) == 0, "incorrect stack alignment");
}
#endif

int os::extra_bang_size_in_bytes() {
  // JDK-8050147 requires the full cache line bang for x86.
  return VM_Version::L1_line_size();
}

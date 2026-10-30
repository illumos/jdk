/*
 * Copyright (c) 1997, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef OS_SOLARIS_OSTHREAD_SOLARIS_HPP
#define OS_SOLARIS_OSTHREAD_SOLARIS_HPP

#include "runtime/osThreadBase.hpp"
#include "suspendResume_posix.hpp"
#include "utilities/globalDefinitions.hpp"

class OSThread : public OSThreadBase {
  friend class VMStructs;

  typedef thread_t thread_id_t;
  thread_id_t _thread_id;

  uint     _lwp_id;            // lwp ID, only used with bound threads
  int      _native_priority;   // Saved native priority when starting
                               // a bound thread
  sigset_t _caller_sigmask;    // Caller's signal mask
  bool     _vm_created_thread; // true if the VM created this thread,
                               // false if primary thread or attached thread
 public:
  OSThread();
  ~OSThread();

  uint     lwp_id() const          { return _lwp_id; }
  int      native_priority() const { return _native_priority; }

  // Set and get state of _vm_created_thread flag
  void set_vm_created()           { _vm_created_thread = true; }
  bool is_vm_created()            { return _vm_created_thread; }

  // Methods to save/restore caller's signal mask
  sigset_t  caller_sigmask() const       { return _caller_sigmask; }
  void    set_caller_sigmask(sigset_t sigmask)  { _caller_sigmask = sigmask; }

  void set_lwp_id(uint id)           { _lwp_id = id; }
  void set_native_priority(int prio) { _native_priority = prio; }

  thread_id_t thread_id() const {
    return _thread_id;
  }
  void set_thread_id(thread_id_t id) {
    _thread_id = id;
  }

  pthread_t pthread_id() const {
    // Here: same as OSThread::thread_id()
    return _thread_id;
  }
  SuspendResume sr;

private:
  void* _siginfo;
  ucontext_t* _ucontext;

public:
  void set_siginfo(void* ptr)             { _siginfo = ptr;   }
  ucontext_t* ucontext() const            { return _ucontext; }
  void set_ucontext(ucontext_t* ptr)      { _ucontext = ptr;  }

private:
  Monitor* _startThread_lock;     // sync parent and child in thread creation

public:

  Monitor* startThread_lock() const {
    return _startThread_lock;
  }

  // Printing
  uintx thread_id_for_printing() const override {
    return (uintx)_thread_id;
  }
};

#endif // OS_SOLARIS_OSTHREAD_SOLARIS_HPP

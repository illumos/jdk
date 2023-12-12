/*
 * Copyright (c) 2006, 2019, Oracle and/or its affiliates. All rights reserved.
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
#include "code/codeCache.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "runtime/vmThread.hpp"
#include "runtime/vmOperations.hpp"
#include "services/dtraceAttacher.hpp"
#include "runtime/flags/jvmFlagAccess.hpp"

#ifdef SOLARIS

static void set_bool_flag(const char* name, bool value) {
  JVMFlag* flag = JVMFlag::find_flag(name);
  JVMFlagAccess::set_bool(flag, &value, JVMFlagOrigin::ATTACH_ON_DEMAND);
}

// Enable the "fine grained" flags.
void DTrace::enable_dprobes(int probes) {
  bool changed = false;
  if (!DTraceAllocProbes && (probes & DTRACE_ALLOC_PROBES)) {
    set_bool_flag("DTraceAllocProbes", true);
    changed = true;
  }
  if (!DTraceMethodProbes && (probes & DTRACE_METHOD_PROBES)) {
    set_bool_flag("DTraceMethodProbes", true);
    changed = true;
  }
  if (!DTraceMonitorProbes && (probes & DTRACE_MONITOR_PROBES)) {
    set_bool_flag("DTraceMonitorProbes", true);
    changed = true;
  }

  if (changed) {
    // one or more flags changed, need to deoptimize
    DeoptimizationScope deopt_scope;
    CodeCache::mark_all_nmethods_for_deoptimization(&deopt_scope);
    deopt_scope.deoptimize_marked();
  }
}

// Disable the "fine grained" flags.
void DTrace::disable_dprobes(int probes) {
  bool changed = false;
  if (DTraceAllocProbes && (probes & DTRACE_ALLOC_PROBES)) {
    set_bool_flag("DTraceAllocProbes", false);
    changed = true;
  }
  if (DTraceMethodProbes && (probes & DTRACE_METHOD_PROBES)) {
    set_bool_flag("DTraceMethodProbes", false);
    changed = true;
  }
  if (DTraceMonitorProbes && (probes & DTRACE_MONITOR_PROBES)) {
    set_bool_flag("DTraceMonitorProbes", false);
    changed = true;
  }
  if (changed) {
    // one or more flags changed, need to deoptimize
    DeoptimizationScope deopt_scope;
    CodeCache::mark_all_nmethods_for_deoptimization(&deopt_scope);
    deopt_scope.deoptimize_marked();
  }
}

// Do clean-up on "all door clients detached" event.
void DTrace::detach_all_clients() {
  disable_dprobes(DTRACE_ALL_PROBES);
}

void DTrace::set_monitor_dprobes(bool flag) {
  // explicit setting of DTraceMonitorProbes flag
  set_bool_flag("DTraceMonitorProbes", flag);
}

#endif /* SOLARIS */

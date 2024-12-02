/*
 * Copyright (c) 1997, 2020, Oracle and/or its affiliates. All rights reserved.
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

// no precompiled headers
#include "jvm.h"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "interpreter/interpreter.hpp"
#include "jvmtifiles/jvmti.h"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "os_solaris.inline.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/osInfo.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/osThread.hpp"
#include "runtime/park.hpp"
#include "runtime/perfMemory.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/statSampler.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/threadCritical.hpp"
#include "runtime/threads.hpp"
#include "runtime/timer.hpp"
#include "runtime/vm_version.hpp"
#include "semaphore_posix.hpp"
#include "services/attachListener.hpp"
#include "services/memTracker.hpp"
#include "services/runtimeService.hpp"
#include "signals_posix.hpp"
#include "utilities/align.hpp"
#include "utilities/decoder.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/events.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/vmError.hpp"

// put OS-includes here
# include <dlfcn.h>
# include <errno.h>
# include <exception>
# include <link.h>
# include <poll.h>
# include <pthread.h>
# include <setjmp.h>
# include <signal.h>
# include <stdio.h>
# include <alloca.h>
# include <sys/filio.h>
# include <sys/ipc.h>
# include <sys/lwp.h>
# include <sys/machelf.h>     // for elf Sym structure used by dladdr1
# include <sys/mman.h>
# include <sys/processor.h>
# include <sys/procset.h>
# include <sys/pset.h>
# include <sys/resource.h>
# include <sys/shm.h>
# include <sys/socket.h>
# include <sys/stat.h>
# include <sys/systeminfo.h>
# include <sys/time.h>
# include <sys/times.h>
# include <sys/types.h>
# include <sys/wait.h>
# include <sys/utsname.h>
# include <thread.h>
# include <unistd.h>
# include <sys/priocntl.h>
# include <sys/rtpriocntl.h>
# include <sys/tspriocntl.h>
# include <sys/iapriocntl.h>
# include <sys/fxpriocntl.h>
# include <sys/loadavg.h>
# include <string.h>
# include <stdio.h>

# include <procfs.h>

#define MAX_PATH (2 * K)

// for timer info max values which include all bits
#define ALL_64_BITS CONST64(0xFFFFFFFFFFFFFFFF)

// Here are some liblgrp types from sys/lgrp_user.h to be able to
// compile on older systems without this header file.

#ifndef MADV_ACCESS_LWP
  #define  MADV_ACCESS_LWP   7       /* next LWP to access heavily */
#endif
#ifndef MADV_ACCESS_MANY
  #define  MADV_ACCESS_MANY  8       /* many processes to access heavily */
#endif

#ifndef LGRP_RSRC_CPU
  #define LGRP_RSRC_CPU      0       /* CPU resources */
#endif
#ifndef LGRP_RSRC_MEM
  #define LGRP_RSRC_MEM      1       /* memory resources */
#endif

// guarded in sys/mman.h
extern "C" {
extern int getpagesizes(size_t[], int);
}

// Values for ThreadPriorityPolicy == 1
int prio_policy1[CriticalPriority+1] = {
  -99999,  0, 16,  32,  48,  64,
          80, 96, 112, 124, 127, 127 };

// System parameters used internally
static clock_t clock_tics_per_sec = 100;

// For diagnostics to print a message once. see run_periodic_checks
static bool check_addr0_done = false;

address os::Solaris::handler_start;  // start pc of thr_sighndlrinfo
address os::Solaris::handler_end;    // end pc of thr_sighndlrinfo

address os::Solaris::_main_stack_base = NULL;  // 4352906 workaround

os::Solaris::pthread_setname_np_func_t os::Solaris::_pthread_setname_np = NULL;

// "default" initializers for missing libc APIs
extern "C" {
  int memcntl(void *, size_t, int, void *, int, int);
  int meminfo(const uint64_t *, int, const uint_t *, int, uint64_t *, uint_t *);
}

static inline size_t adjust_stack_size(address base, size_t size) {
  if ((ssize_t)size < 0) {
    // 4759953: Compensate for ridiculous stack size.
    size = max_intx;
  }
  if (size > (size_t)base) {
    // 4812466: Make sure size doesn't allow the stack to wrap the address space.
    size = (size_t)base;
  }
  return size;
}

static inline stack_t get_stack_info() {
  stack_t st;
  int retval = thr_stksegment(&st);
  st.ss_size = adjust_stack_size((address)st.ss_sp, st.ss_size);
  assert(retval == 0, "incorrect return value from thr_stksegment");
  assert((address)&st < (address)st.ss_sp, "Invalid stack base returned");
  assert((address)&st > (address)st.ss_sp-st.ss_size, "Invalid stack size returned");
  return st;
}

bool os::is_primordial_thread(void) {
  int r = thr_main();
  guarantee(r == 0 || r == 1, "CR6501650 or CR6493689");
  return r == 1;
}

address os::current_stack_base() {
  bool _is_primordial_thread = is_primordial_thread();

  // Workaround 4352906, avoid calls to thr_stksegment by
  // thr_main after the first one (it looks like we trash
  // some data, causing the value for ss_sp to be incorrect).
  if (!_is_primordial_thread || os::Solaris::_main_stack_base == NULL) {
    stack_t st = get_stack_info();
    if (_is_primordial_thread) {
      // cache initial value of stack base
      os::Solaris::_main_stack_base = (address)st.ss_sp;
    }
    return (address)st.ss_sp;
  } else {
    guarantee(os::Solaris::_main_stack_base != NULL, "Attempt to use null cached stack base");
    return os::Solaris::_main_stack_base;
  }
}

size_t os::current_stack_size() {
  size_t size;

  if (!is_primordial_thread()) {
    size = get_stack_info().ss_size;
  } else {
    struct rlimit limits;
    getrlimit(RLIMIT_STACK, &limits);
    size = adjust_stack_size(os::Solaris::_main_stack_base, (size_t)limits.rlim_cur);
  }
  // base may not be page aligned
  address base = current_stack_base();
  address bottom = align_up(base - size, os::vm_page_size());;
  return (size_t)(base - bottom);
}

jint os::Solaris::_os_thread_limit = 0;
volatile jint os::Solaris::_os_thread_count = 0;

julong os::available_memory() {
  return Solaris::available_memory();
}

julong os::free_memory() {
  return Solaris::available_memory();
}

julong os::Solaris::available_memory() {
  return (julong)sysconf(_SC_AVPHYS_PAGES) * os::vm_page_size();
}

julong os::Solaris::_physical_memory = 0;

julong os::physical_memory() {
  return Solaris::physical_memory();
}

static hrtime_t first_hrtime = 0;
static const hrtime_t hrtime_hz = 1000*1000*1000;
static volatile hrtime_t max_hrtime = 0;


void os::Solaris::initialize_system_info() {
  set_processor_count(sysconf(_SC_NPROCESSORS_CONF));
  _physical_memory = (julong)sysconf(_SC_PHYS_PAGES) *
                                     (julong)sysconf(_SC_PAGESIZE);
}

uint os::processor_id() {
  const processorid_t id = ::getcpuid();
  assert(id >= 0 && id < _processor_count, "Invalid processor id");
  return (uint)id;
}

int os::active_processor_count() {
  // User has overridden the number of active processors
  if (ActiveProcessorCount > 0) {
    log_trace(os)("active_processor_count: "
                  "active processor count set by user : %d",
                  ActiveProcessorCount);
    return ActiveProcessorCount;
  }

  int online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  pid_t pid = getpid();
  psetid_t pset = PS_NONE;
  // Are we running in a processor set or is there any processor set around?
  if (pset_bind(PS_QUERY, P_PID, pid, &pset) == 0) {
    uint_t pset_cpus;
    // Query the number of cpus available to us.
    if (pset_info(pset, NULL, &pset_cpus, NULL) == 0) {
      assert(pset_cpus > 0 && pset_cpus <= online_cpus, "sanity check");
      return pset_cpus;
    }
  }
  // Otherwise return number of online cpus
  return online_cpus;
}

void os::set_native_thread_name(const char *name) {
  if (Solaris::_pthread_setname_np != NULL) {
    // Only the first 31 bytes of 'name' are processed by pthread_setname_np
    // but we explicitly copy into a size-limited buffer to avoid any
    // possible overflow.
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", name);
    buf[sizeof(buf) - 1] = '\0';
    Solaris::_pthread_setname_np(pthread_self(), buf);
  }
}

void os::init_system_properties_values() {
  // The next steps are taken in the product version:
  //
  // Obtain the JAVA_HOME value from the location of libjvm.so.
  // This library should be located at:
  // <JAVA_HOME>/jre/lib/<arch>/{client|server}/libjvm.so.
  //
  // If "/jre/lib/" appears at the right place in the path, then we
  // assume libjvm.so is installed in a JDK and we use this path.
  //
  // Otherwise exit with message: "Could not create the Java virtual machine."
  //
  // The following extra steps are taken in the debugging version:
  //
  // If "/jre/lib/" does NOT appear at the right place in the path
  // instead of exit check for $JAVA_HOME environment variable.
  //
  // If it is defined and we are able to locate $JAVA_HOME/jre/lib/<arch>,
  // then we append a fake suffix "hotspot/libjvm.so" to this path so
  // it looks like libjvm.so is installed there
  // <JAVA_HOME>/jre/lib/<arch>/hotspot/libjvm.so.
  //
  // Otherwise exit.
  //
  // Important note: if the location of libjvm.so changes this
  // code needs to be changed accordingly.

// Base path of extensions installed on the system.
#define SYS_EXT_DIR     "/usr/jdk/packages"
#define EXTENSIONS_DIR  "/lib/ext"

  // Buffer that fits several sprintfs.
  // Note that the space for the colon and the trailing null are provided
  // by the nulls included by the sizeof operator.
  const size_t bufsize =
    MAX3((size_t)MAXPATHLEN,  // For dll_dir & friends.
         sizeof(SYS_EXT_DIR) + sizeof("/lib/"), // invariant ld_library_path
         (size_t)MAXPATHLEN + sizeof(EXTENSIONS_DIR) + sizeof(SYS_EXT_DIR) + sizeof(EXTENSIONS_DIR)); // extensions dir
  char *buf = NEW_C_HEAP_ARRAY(char, bufsize, mtInternal);

  // sysclasspath, java_home, dll_dir
  {
    char *pslash;
    os::jvm_path(buf, bufsize);

    // Found the full path to libjvm.so.
    // Now cut the path to <java_home>/jre if we can.
    *(strrchr(buf, '/')) = '\0'; // Get rid of /libjvm.so.
    pslash = strrchr(buf, '/');
    if (pslash != NULL) {
      *pslash = '\0';            // Get rid of /{client|server|hotspot}.
    }
    Arguments::set_dll_dir(buf);

    if (pslash != NULL) {
      pslash = strrchr(buf, '/');
      if (pslash != NULL) {
        *pslash = '\0';        // Get rid of /lib.
      }
    }
    Arguments::set_java_home(buf);
    if (!set_boot_path('/', ':')) {
      vm_exit_during_initialization("Failed setting boot class path.", NULL);
    }
  }

  // Where to look for native libraries.
  {
    // Use dlinfo() to determine the correct java.library.path.
    //
    // If we're launched by the Java launcher, and the user
    // does not set java.library.path explicitly on the commandline,
    // the Java launcher sets LD_LIBRARY_PATH for us and unsets
    // LD_LIBRARY_PATH_32 and LD_LIBRARY_PATH_64.  In this case
    // dlinfo returns LD_LIBRARY_PATH + crle settings (including
    // /usr/lib), which is exactly what we want.
    //
    // If the user does set java.library.path, it completely
    // overwrites this setting, and always has.
    //
    // If we're not launched by the Java launcher, we may
    // get here with any/all of the LD_LIBRARY_PATH[_32|64]
    // settings.  Again, dlinfo does exactly what we want.

    Dl_serinfo     info_sz, *info = &info_sz;
    Dl_serpath     *path;
    char           *library_path;
    char           *common_path = buf;

    // Determine search path count and required buffer size.
    if (dlinfo(RTLD_SELF, RTLD_DI_SERINFOSIZE, (void *)info) == -1) {
      FREE_C_HEAP_ARRAY(char, buf);
      vm_exit_during_initialization("dlinfo SERINFOSIZE request", dlerror());
    }

    // Allocate new buffer and initialize.
    info = (Dl_serinfo*)NEW_C_HEAP_ARRAY(char, info_sz.dls_size, mtInternal);
    info->dls_size = info_sz.dls_size;
    info->dls_cnt = info_sz.dls_cnt;

    // Obtain search path information.
    if (dlinfo(RTLD_SELF, RTLD_DI_SERINFO, (void *)info) == -1) {
      FREE_C_HEAP_ARRAY(char, buf);
      FREE_C_HEAP_ARRAY(char, info);
      vm_exit_during_initialization("dlinfo SERINFO request", dlerror());
    }

    path = &info->dls_serpath[0];

    // Note: Due to a legacy implementation, most of the library path
    // is set in the launcher. This was to accommodate linking restrictions
    // on legacy Solaris implementations (which are no longer supported).
    // Eventually, all the library path setting will be done here.
    //
    // However, to prevent the proliferation of improperly built native
    // libraries, the new path component /usr/jdk/packages is added here.

    // Construct the invariant part of ld_library_path.
    sprintf(common_path, SYS_EXT_DIR "/lib");

    // Struct size is more than sufficient for the path components obtained
    // through the dlinfo() call, so only add additional space for the path
    // components explicitly added here.
    size_t library_path_size = info->dls_size + strlen(common_path);
    library_path = NEW_C_HEAP_ARRAY(char, library_path_size, mtInternal);
    library_path[0] = '\0';

    // Construct the desired Java library path from the linker's library
    // search path.
    //
    // For compatibility, it is optimal that we insert the additional path
    // components specific to the Java VM after those components specified
    // in LD_LIBRARY_PATH (if any) but before those added by the ld.so
    // infrastructure.
    if (info->dls_cnt == 0) { // Not sure this can happen, but allow for it.
      strcpy(library_path, common_path);
    } else {
      int inserted = 0;
      uint_t i;
      for (i = 0; i < info->dls_cnt; i++, path++) {
        uint_t flags = path->dls_flags & LA_SER_MASK;
        if (((flags & LA_SER_LIBPATH) == 0) && !inserted) {
          strcat(library_path, common_path);
          strcat(library_path, os::path_separator());
          inserted = 1;
        }
        strcat(library_path, path->dls_name);
        strcat(library_path, os::path_separator());
      }
      // Eliminate trailing path separator.
      library_path[strlen(library_path)-1] = '\0';
    }

    // happens before argument parsing - can't use a trace flag
    // tty->print_raw("init_system_properties_values: native lib path: ");
    // tty->print_raw_cr(library_path);

    // Callee copies into its own buffer.
    Arguments::set_library_path(library_path);

    FREE_C_HEAP_ARRAY(char, library_path);
    FREE_C_HEAP_ARRAY(char, info);
  }

  // Extensions directories.
  sprintf(buf, "%s" EXTENSIONS_DIR ":" SYS_EXT_DIR EXTENSIONS_DIR, Arguments::get_java_home());
  Arguments::set_ext_dirs(buf);

  FREE_C_HEAP_ARRAY(char, buf);

#undef SYS_EXT_DIR
#undef EXTENSIONS_DIR
}

static thread_t main_thread;

// Thread start routine for all newly created threads
extern "C" void* thread_native_entry(void* thread_addr) {

  Thread* thread = (Thread*)thread_addr;

  thread->record_stack_base_and_size();

  // Try to randomize the cache line index of hot stack frames.
  // This helps when threads of the same stack traces evict each other's
  // cache lines. The threads can be either from the same JVM instance, or
  // from different JVM instances. The benefit is especially true for
  // processors with hyperthreading technology.
  static int counter = 0;
  int pid = os::current_process_id();
  alloca(((pid ^ counter++) & 7) * 128);

  int prio;

  thread->initialize_thread_current();

  OSThread* osthr = thread->osthread();

  osthr->set_lwp_id(_lwp_self());  // Store lwp in case we are bound

  log_info(os, thread)("Thread is alive (tid: " UINTX_FORMAT ").",
    os::current_thread_id());

  if (UseNUMA) {
    int lgrp_id = os::numa_get_group_id();
    if (lgrp_id != -1) {
      thread->set_lgrp_id(lgrp_id);
    }
  }

  // Our priority was set when we were created, and stored in the
  // osthread, but couldn't be passed through to our LWP until now.
  // So read back the priority and set it again.

  if (osthr->thread_id() != -1) {
    if (UseThreadPriorities) {
      int prio = osthr->native_priority();
      os::set_native_priority(thread, prio);
    }
  }

  assert(osthr->get_state() == RUNNABLE, "invalid os thread state");

  // initialize signal mask for this thread
  PosixSignals::hotspot_sigmask(thread);

  os::Solaris::init_thread_fpu_state();

  thread->call_run();

  // Note: at this point the thread object may already have deleted itself.
  // Do not dereference it from here on out.

  // One less thread is executing
  // When the VMThread gets here, the main thread may have already exited
  // which frees the CodeHeap containing the Atomic::dec code
  if (thread != VMThread::vm_thread() && VMThread::vm_thread() != NULL) {
    Atomic::dec(&os::Solaris::_os_thread_count);
  }

  log_info(os, thread)("Thread finished (tid: " UINTX_FORMAT ").", os::current_thread_id());

  thr_exit(NULL);
  ShouldNotReachHere();

  return NULL;
}

static OSThread* create_os_thread(Thread* thread, thread_t thread_id) {
  // Allocate the OSThread object
  OSThread* osthread = new OSThread();
  if (osthread == NULL) return NULL;

  // Store info on the Solaris thread into the OSThread
  osthread->set_thread_id(thread_id);
  osthread->set_lwp_id(_lwp_self());

  if (UseNUMA) {
    int lgrp_id = os::numa_get_group_id();
    if (lgrp_id != -1) {
      thread->set_lgrp_id(lgrp_id);
    }
  }

  // Initial thread state is INITIALIZED, not SUSPENDED
  osthread->set_state(INITIALIZED);

  return osthread;
}

bool os::create_attached_thread(JavaThread* thread) {
#ifdef ASSERT
  thread->verify_not_published();
#endif
  OSThread* osthread = create_os_thread(thread, thr_self());
  if (osthread == NULL) {
    return false;
  }

  // Initial thread state is RUNNABLE
  osthread->set_state(RUNNABLE);
  thread->set_osthread(osthread);

  if (os::is_primordial_thread()) {
    os::Solaris::correct_stack_boundaries_for_primordial_thread(thread);
  }

  // initialize signal mask for this thread
  // and save the caller's signal mask
  PosixSignals::hotspot_sigmask(thread);

  log_info(os, thread)("Thread attached (tid: " UINTX_FORMAT ").",
    os::current_thread_id());

  return true;
}

bool os::create_main_thread(JavaThread* thread) {
#ifdef ASSERT
  thread->verify_not_published();
#endif
  if (_starting_thread == NULL) {
    _starting_thread = create_os_thread(thread, main_thread);
    if (_starting_thread == NULL) {
      return false;
    }
  }

  // The primodial thread is runnable from the start
  _starting_thread->set_state(RUNNABLE);

  thread->set_osthread(_starting_thread);

  // initialize signal mask for this thread
  // and save the caller's signal mask
  PosixSignals::hotspot_sigmask(thread);

  return true;
}

// Helper function to trace thread attributes, similar to os::Posix::describe_pthread_attr()
static char* describe_thr_create_attributes(char* buf, size_t buflen,
                                            size_t stacksize, long flags) {
  stringStream ss(buf, buflen);
  ss.print("stacksize: " SIZE_FORMAT "k, ", stacksize / 1024);
  ss.print("flags: ");
  #define PRINT_FLAG(f) if (flags & f) ss.print( #f " ");
  #define ALL(X) \
    X(THR_SUSPENDED) \
    X(THR_DETACHED) \
    X(THR_BOUND) \
    X(THR_NEW_LWP) \
    X(THR_DAEMON)
  ALL(PRINT_FLAG)
  #undef ALL
  #undef PRINT_FLAG
  return buf;
}

// return default stack size for thr_type
size_t os::Posix::default_stack_size(os::ThreadType thr_type) {
  // default stack size when not specified by caller is 1M (2M for LP64)
  size_t s = (BytesPerWord >> 2) * K * K;
  return s;
}

bool os::create_thread(Thread* thread, ThreadType thr_type,
                       size_t req_stack_size) {
  // Allocate the OSThread object
  OSThread* osthread = new OSThread();
  if (osthread == NULL) {
    return false;
  }

  // calculate stack size if it's not specified by caller
  size_t stack_size = os::Posix::get_initial_stack_size(thr_type, req_stack_size);

  // Initial state is ALLOCATED but not INITIALIZED
  osthread->set_state(ALLOCATED);

  if (os::Solaris::_os_thread_count > os::Solaris::_os_thread_limit) {
    // We got lots of threads. Check if we still have some address space left.
    // Need to be at least 5Mb of unreserved address space. We do check by
    // trying to reserve some.
    const size_t VirtualMemoryBangSize = 20*K*K;
    char* mem = os::reserve_memory(VirtualMemoryBangSize);
    if (mem == NULL) {
      delete osthread;
      return false;
    } else {
      // Release the memory again
      os::release_memory(mem, VirtualMemoryBangSize);
    }
  }

  // Setup osthread because the child thread may need it.
  thread->set_osthread(osthread);

  // Create the Solaris thread
  thread_t tid = 0;
  long     flags = THR_DETACHED | THR_SUSPENDED;
  int      status;

  // Mark that we don't have an lwp or thread id yet.
  // In case we attempt to set the priority before the thread starts.
  osthread->set_lwp_id(-1);
  osthread->set_thread_id(-1);

  status = thr_create(NULL, stack_size, thread_native_entry, thread, flags, &tid);

  char buf[64];
  if (status == 0) {
    log_info(os, thread)("Thread \"%s\" started (tid: " UINTX_FORMAT ", attributes: %s). ",
      thread->name(), (uintx) tid, describe_thr_create_attributes(buf, sizeof(buf), stack_size, flags));
  } else {
    log_warning(os, thread)("Failed to start thread \"%s\" - thr_create failed (%s) for attributes: %s.",
      thread->name(), os::errno_name(status), describe_thr_create_attributes(buf, sizeof(buf), stack_size, flags));
    // Log some OS information which might explain why creating the thread failed.
    log_info(os, thread)("Number of threads approx. running in the VM: %d", Threads::number_of_threads());
    LogStream st(Log(os, thread)::info());
    os::Posix::print_rlimit_info(&st);
    os::print_memory_info(&st);
  }

  if (status != 0) {
    thread->set_osthread(NULL);
    // Need to clean up stuff we've allocated so far
    delete osthread;
    return false;
  }

  Atomic::inc(&os::Solaris::_os_thread_count);

  // Store info on the Solaris thread into the OSThread
  osthread->set_thread_id(tid);

  // Remember that we created this thread so we can set priority on it
  osthread->set_vm_created();

  // Most thread types will set an explicit priority before starting the thread,
  // but for those that don't we need a valid value to read back in thread_native_entry.
  osthread->set_native_priority(NormPriority);

  // Initial thread state is INITIALIZED, not SUSPENDED
  osthread->set_state(INITIALIZED);

  // The thread is returned suspended (in state INITIALIZED), and is started higher up in the call chain
  return true;
}

// CR 7190089: on Solaris, primordial thread's stack needs adjusting.
// Without the adjustment, stack size is incorrect if stack is set to unlimited (ulimit -s unlimited).
void os::Solaris::correct_stack_boundaries_for_primordial_thread(Thread* thr) {
  assert(is_primordial_thread(), "Call only for primordial thread");

  JavaThread* jt = (JavaThread *)thr;
  assert(jt != NULL, "Sanity check");
  size_t stack_size;
  address base = jt->stack_base();
  if (Arguments::created_by_java_launcher()) {
    // Use 2MB to allow for Solaris 7 64 bit mode.
    stack_size = JavaThread::stack_size_at_create() == 0
      ? 2048*K : JavaThread::stack_size_at_create();

    // There are rare cases when we may have already used more than
    // the basic stack size allotment before this method is invoked.
    // Attempt to allow for a normally sized java_stack.
    size_t current_stack_offset = (size_t)(base - (address)&stack_size);
    stack_size += ReservedSpace::page_align_size_down(current_stack_offset);
  } else {
    // 6269555: If we were not created by a Java launcher, i.e. if we are
    // running embedded in a native application, treat the primordial thread
    // as much like a native attached thread as possible.  This means using
    // the current stack size from thr_stksegment(), unless it is too large
    // to reliably setup guard pages.  A reasonable max size is 8MB.
    size_t current_size = os::current_stack_size();
    // This should never happen, but just in case....
    if (current_size == 0) current_size = 2 * K * K;
    stack_size = current_size > (8 * K * K) ? (8 * K * K) : current_size;
  }
  address bottom = align_up(base - stack_size, os::vm_page_size());;
  stack_size = (size_t)(base - bottom);

  assert(stack_size > 0, "Stack size calculation problem");

  if (stack_size > jt->stack_size()) {
#ifndef PRODUCT
    struct rlimit limits;
    getrlimit(RLIMIT_STACK, &limits);
    size_t size = adjust_stack_size(base, (size_t)limits.rlim_cur);
    assert(size >= jt->stack_size(), "Stack size problem in main thread");
#endif
    tty->print_cr("Stack size of " SIZE_FORMAT " Kb exceeds current limit of " SIZE_FORMAT " Kb.\n"
                  "(Stack sizes are rounded up to a multiple of the system page size.)\n"
                  "See limit(1) to increase the stack size limit.",
                  stack_size / K, jt->stack_size() / K);
    vm_exit(1);
  }
  assert(jt->stack_size() >= stack_size,
         "Attempt to map more stack than was allocated");
  jt->set_stack_size(stack_size);

}



// Free Solaris resources related to the OSThread
void os::free_thread(OSThread* osthread) {
  assert(osthread != NULL, "os::free_thread but osthread not set");

  // We are told to free resources of the argument thread,
  // but we can only really operate on the current thread.
  assert(Thread::current()->osthread() == osthread,
         "os::free_thread but not current thread");

  // Restore caller's signal mask
  sigset_t sigmask = osthread->caller_sigmask();
  pthread_sigmask(SIG_SETMASK, &sigmask, NULL);

  delete osthread;
}

void os::pd_start_thread(Thread* thread) {
  int status = thr_continue(thread->osthread()->thread_id());
  assert_status(status == 0, status, "thr_continue failed");
}


intx os::current_thread_id() {
  return (intx)thr_self();
}

static pid_t _initial_pid = 0;

int os::current_process_id() {
  return (int)(_initial_pid ? _initial_pid : getpid());
}

// gethrtime() should be monotonic according to the documentation,
// but some virtualized platforms are known to break this guarantee.
// getTimeNanos() must be guaranteed not to move backwards, so we
// are forced to add a check here.
inline hrtime_t getTimeNanos() {
  const hrtime_t now = gethrtime();
  const hrtime_t prev = max_hrtime;
  if (now <= prev) {
    return prev;   // same or retrograde time;
  }
  const hrtime_t obsv = Atomic::cmpxchg(&max_hrtime, prev, now);
  assert(obsv >= prev, "invariant");   // Monotonicity
  // If the CAS succeeded then we're done and return "now".
  // If the CAS failed and the observed value "obsv" is >= now then
  // we should return "obsv".  If the CAS failed and now > obsv > prv then
  // some other thread raced this thread and installed a new value, in which case
  // we could either (a) retry the entire operation, (b) retry trying to install now
  // or (c) just return obsv.  We use (c).   No loop is required although in some cases
  // we might discard a higher "now" value in deference to a slightly lower but freshly
  // installed obsv value.   That's entirely benign -- it admits no new orderings compared
  // to (a) or (b) -- and greatly reduces coherence traffic.
  // We might also condition (c) on the magnitude of the delta between obsv and now.
  // Avoiding excessive CAS operations to hot RW locations is critical.
  // See https://blogs.oracle.com/dave/entry/cas_and_cache_trivia_invalidate
  return (prev == obsv) ? now : obsv;
}

double os::elapsedVTime() {
  return (double)gethrvtime() / (double)hrtime_hz;
}

// DLL functions

// This must be hard coded because it's the system's temporary
// directory not the java application's temp directory, ala java.io.tmpdir.
const char* os::get_temp_directory() { return "/tmp"; }

// check if addr is inside libjvm.so
bool os::address_is_in_vm(address addr) {
  static address libjvm_base_addr;
  Dl_info dlinfo;

  if (libjvm_base_addr == NULL) {
    if (dladdr(CAST_FROM_FN_PTR(void *, os::address_is_in_vm), &dlinfo) != 0) {
      libjvm_base_addr = (address)dlinfo.dli_fbase;
    }
    assert(libjvm_base_addr !=NULL, "Cannot obtain base address for libjvm");
  }

  if (dladdr((void *)addr, &dlinfo) != 0) {
    if (libjvm_base_addr == (address)dlinfo.dli_fbase) return true;
  }

  return false;
}

void os::prepare_native_symbols() {
}

typedef int (*dladdr1_func_type)(void *, Dl_info *, void **, int);
static dladdr1_func_type dladdr1_func = NULL;

bool os::dll_address_to_function_name(address addr, char *buf,
                                      int buflen, int * offset,
                                      bool demangle) {
  // buf is not optional, but offset is optional
  assert(buf != NULL, "sanity check");

  Dl_info dlinfo;

  // dladdr1_func was initialized in os::init()
  if (dladdr1_func != NULL) {
    // yes, we have dladdr1

    // Support for dladdr1 is checked at runtime; it may be
    // available even if the vm is built on a machine that does
    // not have dladdr1 support.  Make sure there is a value for
    // RTLD_DL_SYMENT.
#ifndef RTLD_DL_SYMENT
  #define RTLD_DL_SYMENT 1
#endif
#ifdef _LP64
    Elf64_Sym * info;
#else
    Elf32_Sym * info;
#endif
    if (dladdr1_func((void *)addr, &dlinfo, (void **)&info,
                     RTLD_DL_SYMENT) != 0) {
      // see if we have a matching symbol that covers our address
      if (dlinfo.dli_saddr != NULL &&
          (char *)dlinfo.dli_saddr + info->st_size > (char *)addr) {
        if (dlinfo.dli_sname != NULL) {
          if (!(demangle && Decoder::demangle(dlinfo.dli_sname, buf, buflen))) {
            jio_snprintf(buf, buflen, "%s", dlinfo.dli_sname);
          }
          if (offset != NULL) *offset = addr - (address)dlinfo.dli_saddr;
          return true;
        }
      }
      // no matching symbol so try for just file info
      if (dlinfo.dli_fname != NULL && dlinfo.dli_fbase != NULL) {
        if (Decoder::decode((address)(addr - (address)dlinfo.dli_fbase),
                            buf, buflen, offset, dlinfo.dli_fname, demangle)) {
          return true;
        }
      }
    }
    buf[0] = '\0';
    if (offset != NULL) *offset  = -1;
    return false;
  }

  // no, only dladdr is available
  if (dladdr((void *)addr, &dlinfo) != 0) {
    // see if we have a matching symbol
    if (dlinfo.dli_saddr != NULL && dlinfo.dli_sname != NULL) {
      if (!(demangle && Decoder::demangle(dlinfo.dli_sname, buf, buflen))) {
        jio_snprintf(buf, buflen, dlinfo.dli_sname);
      }
      if (offset != NULL) *offset = addr - (address)dlinfo.dli_saddr;
      return true;
    }
    // no matching symbol so try for just file info
    if (dlinfo.dli_fname != NULL && dlinfo.dli_fbase != NULL) {
      if (Decoder::decode((address)(addr - (address)dlinfo.dli_fbase),
                          buf, buflen, offset, dlinfo.dli_fname, demangle)) {
        return true;
      }
    }
  }
  buf[0] = '\0';
  if (offset != NULL) *offset  = -1;
  return false;
}

bool os::dll_address_to_library_name(address addr, char* buf,
                                     int buflen, int* offset) {
  // buf is not optional, but offset is optional
  assert(buf != NULL, "sanity check");

  Dl_info dlinfo;

  if (dladdr((void*)addr, &dlinfo) != 0) {
    if (dlinfo.dli_fname != NULL) {
      jio_snprintf(buf, buflen, "%s", dlinfo.dli_fname);
    }
    if (dlinfo.dli_fbase != NULL && offset != NULL) {
      *offset = addr - (address)dlinfo.dli_fbase;
    }
    return true;
  }

  buf[0] = '\0';
  if (offset) *offset = -1;
  return false;
}

int os::get_loaded_modules_info(os::LoadedModulesCallbackFunc callback, void *param) {
  Dl_info dli;
  // Sanity check?
  if (dladdr(CAST_FROM_FN_PTR(void *, os::get_loaded_modules_info), &dli) == 0 ||
      dli.dli_fname == NULL) {
    return 1;
  }

  void * handle = dlopen(dli.dli_fname, RTLD_LAZY);
  if (handle == NULL) {
    return 1;
  }

  Link_map *map;
  dlinfo(handle, RTLD_DI_LINKMAP, &map);
  if (map == NULL) {
    dlclose(handle);
    return 1;
  }

  while (map->l_prev != NULL) {
    map = map->l_prev;
  }

  while (map != NULL) {
    // Iterate through all map entries and call callback with fields of interest
    if(callback(map->l_name, (address)map->l_addr, (address)0, param)) {
      dlclose(handle);
      return 1;
    }
    map = map->l_next;
  }

  dlclose(handle);
  return 0;
}

int _print_dll_info_cb(const char * name, address base_address, address top_address, void * param) {
  outputStream * out = (outputStream *) param;
  out->print_cr(INTPTR_FORMAT " \t%s", (intptr_t)base_address, name);
  return 0;
}

void os::print_dll_info(outputStream * st) {
  st->print_cr("Dynamic libraries:"); st->flush();
  if (get_loaded_modules_info(_print_dll_info_cb, (void *)st)) {
    st->print_cr("Error: Cannot print dynamic libraries.");
  }
}

static void change_endianness(Elf32_Half& val) {
  unsigned char *ptr = (unsigned char *)&val;
  unsigned char swp = ptr[0];
  ptr[0] = ptr[1];
  ptr[1] = swp;
}

// Loads .dll/.so and
// in case of error it checks if .dll/.so was built for the
// same architecture as Hotspot is running on

void * os::dll_load(const char *filename, char *ebuf, int ebuflen) {
  log_info(os)("attempting shared library load of %s", filename);

  void * result= ::dlopen(filename, RTLD_LAZY);
  if (result != NULL) {
    // Successful loading
    Events::log(NULL, "Loaded shared library %s", filename);
    log_info(os)("shared library load of %s was successful", filename);
    return result;
  }

  Elf32_Ehdr elf_head;
  const char* error_report = ::dlerror();
  if (error_report == NULL) {
    error_report = "dlerror returned no error description";
  }
  if (ebuf != NULL && ebuflen > 0) {
    ::strncpy(ebuf, error_report, ebuflen-1);
    ebuf[ebuflen-1]='\0';
  }

  Events::log(NULL, "Loading shared library %s failed, %s", filename, error_report);
  log_info(os)("shared library load of %s failed, %s", filename, error_report);

  int diag_msg_max_length=ebuflen-strlen(ebuf);
  char* diag_msg_buf=ebuf+strlen(ebuf);

  if (diag_msg_max_length==0) {
    // No more space in ebuf for additional diagnostics message
    return NULL;
  }


  int file_descriptor= ::open(filename, O_RDONLY | O_NONBLOCK);

  if (file_descriptor < 0) {
    // Can't open library, report dlerror() message
    return NULL;
  }

  bool failed_to_read_elf_head=
    (sizeof(elf_head)!=
     (::read(file_descriptor, &elf_head,sizeof(elf_head))));

  ::close(file_descriptor);
  if (failed_to_read_elf_head) {
    // file i/o error - report dlerror() msg
    return NULL;
  }

  if (elf_head.e_ident[EI_DATA] != LITTLE_ENDIAN_ONLY(ELFDATA2LSB) BIG_ENDIAN_ONLY(ELFDATA2MSB)) {
    // handle invalid/out of range endianness values
    if (elf_head.e_ident[EI_DATA] == 0 || elf_head.e_ident[EI_DATA] > 2) {
      return NULL;
    }
    change_endianness(elf_head.e_machine);
  }

  typedef struct {
    Elf32_Half    code;         // Actual value as defined in elf.h
    Elf32_Half    compat_class; // Compatibility of archs at VM's sense
    unsigned char elf_class;    // 32 or 64 bit
    unsigned char endianess;    // MSB or LSB
    char*         name;         // String representation
  } arch_t;

  static const arch_t arch_array[]={
    {EM_386,         EM_386,     ELFCLASS32, ELFDATA2LSB, (char*)"IA 32"},
    {EM_486,         EM_386,     ELFCLASS32, ELFDATA2LSB, (char*)"IA 32"},
    {EM_IA_64,       EM_IA_64,   ELFCLASS64, ELFDATA2LSB, (char*)"IA 64"},
    {EM_X86_64,      EM_X86_64,  ELFCLASS64, ELFDATA2LSB, (char*)"AMD 64"},
    {EM_SPARC,       EM_SPARC,   ELFCLASS32, ELFDATA2MSB, (char*)"Sparc 32"},
    {EM_SPARC32PLUS, EM_SPARC,   ELFCLASS32, ELFDATA2MSB, (char*)"Sparc 32"},
    {EM_SPARCV9,     EM_SPARCV9, ELFCLASS64, ELFDATA2MSB, (char*)"Sparc v9 64"},
    {EM_PPC,         EM_PPC,     ELFCLASS32, ELFDATA2MSB, (char*)"Power PC 32"},
    {EM_PPC64,       EM_PPC64,   ELFCLASS64, ELFDATA2MSB, (char*)"Power PC 64"},
    {EM_ARM,         EM_ARM,     ELFCLASS32, ELFDATA2LSB, (char*)"ARM"},
    // we only support 64 bit z architecture
    {EM_S390,        EM_S390,    ELFCLASS64, ELFDATA2MSB, (char*)"IBM System/390"},
    {EM_AARCH64,     EM_AARCH64, ELFCLASS64, ELFDATA2LSB, (char*)"AARCH64"}
  };

#if  (defined IA32)
  static  Elf32_Half running_arch_code=EM_386;
#elif   (defined AMD64)
  static  Elf32_Half running_arch_code=EM_X86_64;
#elif  (defined IA64)
  static  Elf32_Half running_arch_code=EM_IA_64;
#elif  (defined __sparc) && (defined _LP64)
  static  Elf32_Half running_arch_code=EM_SPARCV9;
#elif  (defined __sparc) && (!defined _LP64)
  static  Elf32_Half running_arch_code=EM_SPARC;
#elif  (defined __powerpc64__)
  static  Elf32_Half running_arch_code=EM_PPC64;
#elif  (defined __powerpc__)
  static  Elf32_Half running_arch_code=EM_PPC;
#elif (defined ARM)
  static  Elf32_Half running_arch_code=EM_ARM;
#else
  #error Method os::dll_load requires that one of following is defined:\
       IA32, AMD64, IA64, __sparc, __powerpc__, ARM, ARM
#endif

  // Identify compatibility class for VM's architecture and library's architecture
  // Obtain string descriptions for architectures

  arch_t lib_arch={elf_head.e_machine,0,elf_head.e_ident[EI_CLASS], elf_head.e_ident[EI_DATA], NULL};
  int running_arch_index=-1;

  for (unsigned int i=0; i < ARRAY_SIZE(arch_array); i++) {
    if (running_arch_code == arch_array[i].code) {
      running_arch_index    = i;
    }
    if (lib_arch.code == arch_array[i].code) {
      lib_arch.compat_class = arch_array[i].compat_class;
      lib_arch.name         = arch_array[i].name;
    }
  }

  assert(running_arch_index != -1,
         "Didn't find running architecture code (running_arch_code) in arch_array");
  if (running_arch_index == -1) {
    // Even though running architecture detection failed
    // we may still continue with reporting dlerror() message
    return NULL;
  }

  if (lib_arch.compat_class != arch_array[running_arch_index].compat_class) {
    if (lib_arch.name != NULL) {
      ::snprintf(diag_msg_buf, diag_msg_max_length-1,
                 " (Possible cause: can't load %s .so on a %s platform)",
                 lib_arch.name, arch_array[running_arch_index].name);
    } else {
      ::snprintf(diag_msg_buf, diag_msg_max_length-1,
                 " (Possible cause: can't load this .so (machine code=0x%x) on a %s platform)",
                 lib_arch.code, arch_array[running_arch_index].name);
    }
    return NULL;
  }

  if (lib_arch.endianess != arch_array[running_arch_index].endianess) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1," (Possible cause: endianness mismatch)");
    return NULL;
  }

  // ELF file class/capacity : 0 - invalid, 1 - 32bit, 2 - 64bit
  if (lib_arch.elf_class > 2 || lib_arch.elf_class < 1) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1, " (Possible cause: invalid ELF file class)");
    return NULL;
  }

  if (lib_arch.elf_class != arch_array[running_arch_index].elf_class) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1,
               " (Possible cause: architecture word width mismatch, can't load %d-bit .so on a %d-bit platform)",
               (int) lib_arch.elf_class * 32, arch_array[running_arch_index].elf_class * 32);
    return NULL;
  }

  return NULL;
}

static inline time_t get_mtime(const char* filename) {
  struct stat st;
  int ret = os::stat(filename, &st);
  assert(ret == 0, "failed to stat() file '%s': %s", filename, os::strerror(errno));
  return st.st_mtime;
}

int os::compare_file_modified_times(const char* file1, const char* file2) {
  time_t t1 = get_mtime(file1);
  time_t t2 = get_mtime(file2);
  return t1 - t2;
}

static bool _print_ascii_file(const char* filename, outputStream* st) {
  int fd = ::open(filename, O_RDONLY);
  if (fd == -1) {
    return false;
  }

  char buf[32];
  int bytes;
  while ((bytes = ::read(fd, buf, sizeof(buf))) > 0) {
    st->print_raw(buf, bytes);
  }

  ::close(fd);

  return true;
}

void os::print_os_info_brief(outputStream* st) {
  os::Solaris::print_distro_info(st);

  os::Posix::print_uname_info(st);

  os::Solaris::print_libversion_info(st);
}

void os::print_os_info(outputStream* st) {
  st->print("OS:");

  os::Solaris::print_distro_info(st);

  os::Posix::print_uname_info(st);

  os::Posix::print_uptime_info(st);

  os::Solaris::print_libversion_info(st);

  os::Posix::print_rlimit_info(st);

  os::Posix::print_load_average(st);
}

void os::Solaris::print_distro_info(outputStream* st) {
  if (!_print_ascii_file("/etc/release", st)) {
    st->print("Solaris");
  }
  st->cr();
}

void os::get_summary_os_info(char* buf, size_t buflen) {
  strncpy(buf, "Solaris", buflen);  // default to plain solaris
  FILE* fp = fopen("/etc/release", "r");
  if (fp != NULL) {
    char tmp[256];
    // Only get the first line and chop out everything but the os name.
    if (fgets(tmp, sizeof(tmp), fp)) {
      char* ptr = tmp;
      // skip past whitespace characters
      while (*ptr != '\0' && (*ptr == ' ' || *ptr == '\t' || *ptr == '\n')) ptr++;
      if (*ptr != '\0') {
        char* nl = strchr(ptr, '\n');
        if (nl != NULL) *nl = '\0';
        strncpy(buf, ptr, buflen);
      }
    }
    fclose(fp);
  }
}

void os::Solaris::print_libversion_info(outputStream* st) {
  st->print("  (T2 libthread)");
  st->cr();
}

static bool check_addr0(outputStream* st) {
  jboolean status = false;
  const int read_chunk = 200;
  int ret = 0;
  int nmap = 0;
  int fd = ::open("/proc/self/map",O_RDONLY);
  if (fd >= 0) {
    prmap_t *p = NULL;
    char *mbuff = (char *) calloc(read_chunk, sizeof(prmap_t));
    if (NULL == mbuff) {
      ::close(fd);
      return status;
    }
    while ((ret = ::read(fd, mbuff, read_chunk*sizeof(prmap_t))) > 0) {
      //check if read() has not read partial data
      if( 0 != ret % sizeof(prmap_t)){
        break;
      }
      nmap = ret / sizeof(prmap_t);
      p = (prmap_t *)mbuff;
      for(int i = 0; i < nmap; i++){
        if (p->pr_vaddr == 0x0) {
          st->print("Warning: Address: " PTR_FORMAT ", Size: " SIZE_FORMAT "K, ",p->pr_vaddr, p->pr_size/1024);
          st->print("Mapped file: %s, ", p->pr_mapname[0] == '\0' ? "None" : p->pr_mapname);
          st->print("Access: ");
          st->print("%s",(p->pr_mflags & MA_READ)  ? "r" : "-");
          st->print("%s",(p->pr_mflags & MA_WRITE) ? "w" : "-");
          st->print("%s",(p->pr_mflags & MA_EXEC)  ? "x" : "-");
          st->cr();
          status = true;
        }
        p++;
      }
    }
    free(mbuff);
    ::close(fd);
  }
  return status;
}

void os::get_summary_cpu_info(char* buf, size_t buflen) {
  // Get MHz with system call. We don't seem to already have this.
  processor_info_t stats;
  processorid_t id = getcpuid();
  int clock = 0;
  if (processor_info(id, &stats) != -1) {
    clock = stats.pi_clock;  // pi_processor_type isn't more informative than below
  }
  snprintf(buf, buflen, "64 bit %d MHz", clock);
}

void os::pd_print_cpu_info(outputStream* st, char* buf, size_t buflen) {
  // Nothing to do for now.
}

void os::print_memory_info(outputStream* st) {
  st->print("Memory:");
  st->print(" " SIZE_FORMAT "k page", os::vm_page_size()>>10);
  st->print(", physical " UINT64_FORMAT "k", os::physical_memory()>>10);
  st->print("(" UINT64_FORMAT "k free)", os::available_memory() >> 10);
  st->cr();
  (void) check_addr0(st);
}

static int Maxsignum = 0;

static char saved_jvm_path[MAXPATHLEN] = { 0 };

// Find the full path to the current module, libjvm.so
void os::jvm_path(char *buf, jint buflen) {
  // Error checking.
  if (buflen < MAXPATHLEN) {
    assert(false, "must use a large-enough buffer");
    buf[0] = '\0';
    return;
  }
  // Lazy resolve the path to current module.
  if (saved_jvm_path[0] != 0) {
    strcpy(buf, saved_jvm_path);
    return;
  }

  Dl_info dlinfo;
  int ret = dladdr(CAST_FROM_FN_PTR(void *, os::jvm_path), &dlinfo);
  assert(ret != 0, "cannot locate libjvm");
  if (ret != 0 && dlinfo.dli_fname != NULL) {
    if (os::Posix::realpath((char *)dlinfo.dli_fname, buf, buflen) == NULL) {
      return;
    }
  } else {
    buf[0] = '\0';
    return;
  }

  if (Arguments::sun_java_launcher_is_altjvm()) {
    // Support for the java launcher's '-XXaltjvm=<path>' option. Typical
    // value for buf is "<JAVA_HOME>/jre/lib/<arch>/<vmtype>/libjvm.so".
    // If "/jre/lib/" appears at the right place in the string, then
    // assume we are installed in a JDK and we're done.  Otherwise, check
    // for a JAVA_HOME environment variable and fix up the path so it
    // looks like libjvm.so is installed there (append a fake suffix
    // hotspot/libjvm.so).
    const char *p = buf + strlen(buf) - 1;
    for (int count = 0; p > buf && count < 5; ++count) {
      for (--p; p > buf && *p != '/'; --p)
        /* empty */ ;
    }

    if (strncmp(p, "/jre/lib/", 9) != 0) {
      // Look for JAVA_HOME in the environment.
      char* java_home_var = ::getenv("JAVA_HOME");
      if (java_home_var != NULL && java_home_var[0] != 0) {
        char* jrelib_p;
        int   len;

        // Check the current module name "libjvm.so".
        p = strrchr(buf, '/');
        assert(strstr(p, "/libjvm") == p, "invalid library name");

        if (os::Posix::realpath(java_home_var, buf, buflen) == NULL) {
          return;
        }
        // determine if this is a legacy image or modules image
        // modules image doesn't have "jre" subdirectory
        len = strlen(buf);
        assert(len < buflen, "Ran out of buffer space");
        jrelib_p = buf + len;
        snprintf(jrelib_p, buflen-len, "/jre/lib");
        if (0 != access(buf, F_OK)) {
          snprintf(jrelib_p, buflen-len, "/lib");
        }

        if (0 == access(buf, F_OK)) {
          // Use current module name "libjvm.so"
          len = strlen(buf);
          snprintf(buf + len, buflen-len, "/hotspot/libjvm.so");
        } else {
          // Go back to path of .so
          if (os::Posix::realpath((char *)dlinfo.dli_fname, buf, buflen) == NULL) {
            return;
          }
        }
      }
    }
  }

  strncpy(saved_jvm_path, buf, MAXPATHLEN);
  saved_jvm_path[MAXPATHLEN - 1] = '\0';
}

////////////////////////////////////////////////////////////////////////////////
// Virtual Memory

static bool recoverable_mmap_error(int err) {
  // See if the error is one we can let the caller handle. This
  // list of errno values comes from the Solaris mmap(2) man page.
  switch (err) {
  case EBADF:
  case EINVAL:
  case ENOTSUP:
    // let the caller deal with these errors
    return true;

  default:
    // Any remaining errors on this OS can cause our reserved mapping
    // to be lost. That can cause confusion where different data
    // structures think they have the same memory mapped. The worst
    // scenario is if both the VM and a library think they have the
    // same memory mapped.
    return false;
  }
}

static void warn_fail_commit_memory(char* addr, size_t bytes, bool exec,
                                    int err) {
  warning("INFO: os::commit_memory(" PTR_FORMAT ", " SIZE_FORMAT
          ", %d) failed; error='%s' (errno=%d)", p2i(addr), bytes, exec,
          os::strerror(err), err);
}

static void warn_fail_commit_memory(char* addr, size_t bytes,
                                    size_t alignment_hint, bool exec,
                                    int err) {
  warning("INFO: os::commit_memory(" PTR_FORMAT ", " SIZE_FORMAT
          ", " SIZE_FORMAT ", %d) failed; error='%s' (errno=%d)", p2i(addr),
          bytes, alignment_hint, exec, os::strerror(err), err);
}

int os::Solaris::commit_memory_impl(char* addr, size_t bytes, bool exec) {
  int prot = exec ? PROT_READ|PROT_WRITE|PROT_EXEC : PROT_READ|PROT_WRITE;
  size_t size = bytes;
  char *res = Solaris::mmap_chunk(addr, size, MAP_PRIVATE|MAP_FIXED, prot);
  if (res != NULL) {
    if (UseNUMAInterleaving) {
        numa_make_global(addr, bytes);
    }
    return 0;
  }

  int err = errno;  // save errno from mmap() call in mmap_chunk()

  if (!recoverable_mmap_error(err)) {
    warn_fail_commit_memory(addr, bytes, exec, err);
    vm_exit_out_of_memory(bytes, OOM_MMAP_ERROR, "committing reserved memory.");
  }

  return err;
}

bool os::pd_commit_memory(char* addr, size_t bytes, bool exec) {
  return Solaris::commit_memory_impl(addr, bytes, exec) == 0;
}

void os::pd_commit_memory_or_exit(char* addr, size_t bytes, bool exec,
                                  const char* mesg) {
  assert(mesg != NULL, "mesg must be specified");
  int err = os::Solaris::commit_memory_impl(addr, bytes, exec);
  if (err != 0) {
    // the caller wants all commit errors to exit with the specified mesg:
    warn_fail_commit_memory(addr, bytes, exec, err);
    vm_exit_out_of_memory(bytes, OOM_MMAP_ERROR, "%s", mesg);
  }
}

size_t os::Solaris::page_size_for_alignment(size_t alignment) {
  assert(is_aligned(alignment, (size_t) os::vm_page_size()),
         SIZE_FORMAT " is not aligned to " SIZE_FORMAT,
         alignment, (size_t) os::vm_page_size());

  const int page_sizes_max = 9;
  size_t _illumos_page_sizes[page_sizes_max];
  int n = getpagesizes(_illumos_page_sizes, page_sizes_max);
  for (int i = 0; _illumos_page_sizes[i] != 0; i++) {
    if (is_aligned(alignment, _illumos_page_sizes[i])) {
      return _illumos_page_sizes[i];
    }
  }

  return (size_t) os::vm_page_size();
}

int os::Solaris::commit_memory_impl(char* addr, size_t bytes,
                                    size_t alignment_hint, bool exec) {
  int err = Solaris::commit_memory_impl(addr, bytes, exec);
  if (err == 0 && UseLargePages && alignment_hint > 0) {
    assert(is_aligned(bytes, alignment_hint),
           SIZE_FORMAT " is not aligned to " SIZE_FORMAT, bytes, alignment_hint);

    // The syscall memcntl requires an exact page size (see man memcntl for details).
    size_t page_size = page_size_for_alignment(alignment_hint);
    if (page_size > (size_t) os::vm_page_size()) {
      (void)Solaris::setup_large_pages(addr, bytes, page_size);
    }
  }
  return err;
}

bool os::pd_commit_memory(char* addr, size_t bytes, size_t alignment_hint,
                          bool exec) {
  return Solaris::commit_memory_impl(addr, bytes, alignment_hint, exec) == 0;
}

void os::pd_commit_memory_or_exit(char* addr, size_t bytes,
                                  size_t alignment_hint, bool exec,
                                  const char* mesg) {
  assert(mesg != NULL, "mesg must be specified");
  int err = os::Solaris::commit_memory_impl(addr, bytes, alignment_hint, exec);
  if (err != 0) {
    // the caller wants all commit errors to exit with the specified mesg:
    warn_fail_commit_memory(addr, bytes, alignment_hint, exec, err);
    vm_exit_out_of_memory(bytes, OOM_MMAP_ERROR, "%s", mesg);
  }
}

// Uncommit the pages in a specified region.
void os::pd_free_memory(char* addr, size_t bytes, size_t alignment_hint) {
  if (posix_madvise(addr, bytes, MADV_FREE) < 0) {
    debug_only(warning("MADV_FREE failed."));
    return;
  }
}

size_t os::pd_pretouch_memory(void* first, void* last, size_t page_size) {
  return page_size;
}

bool os::pd_create_stack_guard_pages(char* addr, size_t size) {
  return os::commit_memory(addr, size, !ExecMem);
}

bool os::remove_stack_guard_pages(char* addr, size_t size) {
  return os::uncommit_memory(addr, size);
}

// Change the page size in a given range.
void os::pd_realign_memory(char *addr, size_t bytes, size_t alignment_hint) {
  assert((intptr_t)addr % alignment_hint == 0, "Address should be aligned.");
  assert((intptr_t)(addr + bytes) % alignment_hint == 0, "End should be aligned.");
  if (UseLargePages) {
    size_t page_size = Solaris::page_size_for_alignment(alignment_hint);
    if (page_size > (size_t) os::vm_page_size()) {
      Solaris::setup_large_pages(addr, bytes, page_size);
    }
  }
}

// Tell the OS to make the range local to the first-touching LWP
void os::numa_make_local(char *addr, size_t bytes, int lgrp_hint) {
  assert((intptr_t)addr % os::vm_page_size() == 0, "Address should be page-aligned.");
  if (posix_madvise(addr, bytes, MADV_ACCESS_LWP) < 0) {
    debug_only(warning("MADV_ACCESS_LWP failed."));
  }
}

// Tell the OS that this range would be accessed from different LWPs.
void os::numa_make_global(char *addr, size_t bytes) {
  assert((intptr_t)addr % os::vm_page_size() == 0, "Address should be page-aligned.");
  if (posix_madvise(addr, bytes, MADV_ACCESS_MANY) < 0) {
    debug_only(warning("MADV_ACCESS_MANY failed."));
  }
}

// Get the number of the locality groups.
size_t os::numa_get_groups_num() {
  size_t n = Solaris::lgrp_nlgrps(Solaris::lgrp_cookie());
  return n != -1 ? n : 1;
}

// Get a list of leaf locality groups. A leaf lgroup is group that
// doesn't have any children. Typical leaf group is a CPU or a CPU/memory
// board. An LWP is assigned to one of these groups upon creation.
size_t os::numa_get_leaf_groups(int *ids, size_t size) {
  if ((ids[0] = Solaris::lgrp_root(Solaris::lgrp_cookie())) == -1) {
    ids[0] = 0;
    return 1;
  }
  int result_size = 0, top = 1, bottom = 0, cur = 0;
  for (unsigned int k = 0; k < size; k++) {
    int r = Solaris::lgrp_children(Solaris::lgrp_cookie(), ids[cur],
                                   (Solaris::lgrp_id_t*)&ids[top], size - top);
    if (r == -1) {
      ids[0] = 0;
      return 1;
    }
    if (!r) {
      // That's a leaf node.
      assert(bottom <= cur, "Sanity check");
      // Check if the node has memory
      if (Solaris::lgrp_resources(Solaris::lgrp_cookie(), ids[cur],
                                  NULL, 0, LGRP_RSRC_MEM) > 0) {
        ids[bottom++] = ids[cur];
      }
    }
    top += r;
    cur++;
  }
  if (bottom == 0) {
    // Handle a situation, when the OS reports no memory available.
    // Assume UMA architecture.
    ids[0] = 0;
    return 1;
  }
  return bottom;
}

// Detect the topology change. Typically happens during CPU plugging-unplugging.
bool os::numa_topology_changed() {
  int is_stale = Solaris::lgrp_cookie_stale(Solaris::lgrp_cookie());
  if (is_stale != -1 && is_stale) {
    Solaris::lgrp_fini(Solaris::lgrp_cookie());
    Solaris::lgrp_cookie_t c = Solaris::lgrp_init(Solaris::LGRP_VIEW_CALLER);
    assert(c != 0, "Failure to initialize LGRP API");
    Solaris::set_lgrp_cookie(c);
    return true;
  }
  return false;
}

// Get the group id of the current LWP.
int os::numa_get_group_id() {
  int lgrp_id = Solaris::lgrp_home(P_LWPID, P_MYID);
  if (lgrp_id == -1) {
    return 0;
  }
  const int size = os::numa_get_groups_num();
  int *ids = (int*)alloca(size * sizeof(int));

  // Get the ids of all lgroups with memory; r is the count.
  int r = Solaris::lgrp_resources(Solaris::lgrp_cookie(), lgrp_id,
                                  (Solaris::lgrp_id_t*)ids, size, LGRP_RSRC_MEM);
  if (r <= 0) {
    return 0;
  }
  return ids[os::random() % r];
}

int os::numa_get_group_id_for_address(const void* address) {
  return 0;
}

bool os::numa_get_group_ids_for_range(const void** addresses, int* lgrp_ids, size_t count) {
  return false;
}

// Scan the pages from start to end until a page different than
// the one described in the info parameter is encountered.
char *os::scan_pages(char *start, char* end, page_info* page_expected,
                     page_info* page_found) {
  const uint_t info_types[] = { MEMINFO_VLGRP, MEMINFO_VPAGESIZE };
  const size_t types = sizeof(info_types) / sizeof(info_types[0]);
  uint64_t addrs[MAX_MEMINFO_CNT], outdata[types * MAX_MEMINFO_CNT + 1];
  uint_t validity[MAX_MEMINFO_CNT];

  size_t page_size = MAX2((size_t)os::vm_page_size(), page_expected->size);
  uint64_t p = (uint64_t)start;
  while (p < (uint64_t)end) {
    addrs[0] = p;
    size_t addrs_count = 1;
    while (addrs_count < MAX_MEMINFO_CNT && addrs[addrs_count - 1] + page_size < (uint64_t)end) {
      addrs[addrs_count] = addrs[addrs_count - 1] + page_size;
      addrs_count++;
    }

    if (meminfo(addrs, addrs_count, info_types, types, outdata, validity) < 0) {
      return NULL;
    }

    size_t i = 0;
    for (; i < addrs_count; i++) {
      if ((validity[i] & 1) != 0) {
        if ((validity[i] & 4) != 0) {
          if (outdata[types * i + 1] != page_expected->size) {
            break;
          }
        } else if (page_expected->size != 0) {
          break;
        }

        if ((validity[i] & 2) != 0 && page_expected->lgrp_id > 0) {
          if (outdata[types * i] != page_expected->lgrp_id) {
            break;
          }
        }
      } else {
        return NULL;
      }
    }

    if (i < addrs_count) {
      if ((validity[i] & 2) != 0) {
        page_found->lgrp_id = outdata[types * i];
      } else {
        page_found->lgrp_id = -1;
      }
      if ((validity[i] & 4) != 0) {
        page_found->size = outdata[types * i + 1];
      } else {
        page_found->size = 0;
      }
      return (char*)addrs[i];
    }

    p = addrs[addrs_count - 1] + page_size;
  }
  return end;
}

bool os::pd_uncommit_memory(char* addr, size_t bytes, bool exec) {
  size_t size = bytes;
  // Map uncommitted pages PROT_NONE so we fail early if we touch an
  // uncommitted page. Otherwise, the read/write might succeed if we
  // have enough swap space to back the physical page.
  return
    NULL != Solaris::mmap_chunk(addr, size,
                                MAP_PRIVATE|MAP_FIXED|MAP_NORESERVE,
                                PROT_NONE);
}

char* os::Solaris::mmap_chunk(char *addr, size_t size, int flags, int prot) {
  char *b = (char *)mmap(addr, size, prot, flags, os::Solaris::_dev_zero_fd, 0);

  if (b == MAP_FAILED) {
    return NULL;
  }
  return b;
}

char* os::Solaris::anon_mmap(char* requested_addr, size_t bytes) {
  char* addr = requested_addr;
  int flags = MAP_PRIVATE | MAP_NORESERVE;

  // Map uncommitted pages PROT_NONE so we fail early if we touch an
  // uncommitted page. Otherwise, the read/write might succeed if we
  // have enough swap space to back the physical page.
  return mmap_chunk(addr, bytes, flags, PROT_NONE);
}

char* os::pd_reserve_memory(size_t bytes, bool exec) {
  char* addr = Solaris::anon_mmap(NULL, bytes);

  return addr;
}

char* os::pd_attempt_map_memory_to_file_at(char* requested_addr, size_t bytes, int file_desc) {
  assert(file_desc >= 0, "file_desc is not valid");
  char* result = pd_attempt_reserve_memory_at(requested_addr, bytes, !ExecMem);
  if (result != NULL) {
    if (replace_existing_mapping_with_file_mapping(result, bytes, file_desc) == NULL) {
      vm_exit_during_initialization(err_msg("Error in mapping Java heap at the given filesystem directory"));
    }
  }
  return result;
}

// Reserve memory at an arbitrary address, only if that area is
// available (and not reserved for something else).

char* os::pd_attempt_reserve_memory_at(char* requested_addr, size_t bytes, bool exec) {
  // Assert only that the size is a multiple of the page size, since
  // that's all that mmap requires, and since that's all we really know
  // about at this low abstraction level.  If we need higher alignment,
  // we can either pass an alignment to this method or verify alignment
  // in one of the methods further up the call chain.  See bug 5044738.
  assert(bytes % os::vm_page_size() == 0, "reserving unexpected size block");

  // Since snv_84, Solaris attempts to honor the address hint - see 5003415.
  char* addr = Solaris::anon_mmap(requested_addr, bytes);

  volatile int err = errno;
  if (addr == requested_addr) {
    return addr;
  }

  if (addr != NULL) {
    pd_unmap_memory(addr, bytes);
  }

  return NULL;
}

bool os::pd_release_memory(char* addr, size_t bytes) {
  size_t size = bytes;
  return munmap(addr, size) == 0;
}

static bool solaris_mprotect(char* addr, size_t bytes, int prot) {
  assert(addr == (char*)align_down((uintptr_t)addr, os::vm_page_size()),
         "addr must be page aligned");
  Events::log(NULL, "Protecting memory [" INTPTR_FORMAT "," INTPTR_FORMAT "] with protection modes %x", p2i(addr), p2i(addr+bytes), prot);
  int retVal = mprotect(addr, bytes, prot);
  return retVal == 0;
}

// Protect memory (Used to pass readonly pages through
// JNI GetArray<type>Elements with empty arrays.)
// Also, used for serialization page and for compressed oops null pointer
// checking.
bool os::protect_memory(char* addr, size_t bytes, ProtType prot,
                        bool is_committed) {
  unsigned int p = 0;
  switch (prot) {
  case MEM_PROT_NONE: p = PROT_NONE; break;
  case MEM_PROT_READ: p = PROT_READ; break;
  case MEM_PROT_RW:   p = PROT_READ|PROT_WRITE; break;
  case MEM_PROT_RWX:  p = PROT_READ|PROT_WRITE|PROT_EXEC; break;
  default:
    ShouldNotReachHere();
  }
  // is_committed is unused.
  return solaris_mprotect(addr, bytes, p);
}

// guard_memory and unguard_memory only happens within stack guard pages.
// Since ISM pertains only to the heap, guard and unguard memory should not
/// happen with an ISM region.
bool os::guard_memory(char* addr, size_t bytes) {
  return solaris_mprotect(addr, bytes, PROT_NONE);
}

bool os::unguard_memory(char* addr, size_t bytes) {
  return solaris_mprotect(addr, bytes, PROT_READ|PROT_WRITE);
}

// Large page support
static size_t _large_page_size = 0;

bool os::Solaris::mpss_sanity_check(bool warn, size_t* page_size) {
  // Find the page sizes supported by the system
  const int page_sizes_max = 9;
  size_t _illumos_page_sizes[page_sizes_max];
  int n = getpagesizes(_illumos_page_sizes, page_sizes_max);
  assert(n > 0, "illumos bug?");

  if (n == 1) return false;     // Only one page size available.

  // Skip sizes larger than 4M (or LargePageSizeInBytes if it was set)
  const size_t size_limit =
    FLAG_IS_DEFAULT(LargePageSizeInBytes) ? 4 * M : LargePageSizeInBytes;
  int beg;
  for (beg = 0; beg < n; ++beg) {
    if (_illumos_page_sizes[beg] <= size_limit) {
      _page_sizes.add(_illumos_page_sizes[beg]);
      if (_illumos_page_sizes[beg] > *page_size) {
        *page_size = _illumos_page_sizes[beg];
      }
    }
  }
  // make sure we add the default
  _page_sizes.add(os::vm_page_size());
  return true;
}

void os::large_page_init() {
  if (UseLargePages) {
    // print a warning if any large page related flag is specified on command line
    bool warn_on_failure = !FLAG_IS_DEFAULT(UseLargePages)        ||
                           !FLAG_IS_DEFAULT(LargePageSizeInBytes);

    UseLargePages = Solaris::mpss_sanity_check(warn_on_failure, &_large_page_size);
  }
}

bool os::Solaris::is_valid_page_size(size_t bytes) {
  return _page_sizes.contains(bytes);
}

bool os::Solaris::setup_large_pages(caddr_t start, size_t bytes, size_t align) {
  assert(is_valid_page_size(align), SIZE_FORMAT " is not a valid page size", align);
  assert(is_aligned((void*) start, align),
         PTR_FORMAT " is not aligned to " SIZE_FORMAT, p2i((void*) start), align);
  assert(is_aligned(bytes, align),
         SIZE_FORMAT " is not aligned to " SIZE_FORMAT, bytes, align);

  // Signal to OS that we want large pages for addresses
  // from addr, addr + bytes
  struct memcntl_mha mpss_struct;
  mpss_struct.mha_cmd = MHA_MAPSIZE_VA;
  mpss_struct.mha_pagesize = align;
  mpss_struct.mha_flags = 0;
  // Upon successful completion, memcntl() returns 0
  if (memcntl(start, bytes, MC_HAT_ADVISE, (caddr_t) &mpss_struct, 0, 0)) {
    debug_only(warning("Attempt to use MPSS failed."));
    return false;
  }
  return true;
}

char* os::pd_reserve_memory_special(size_t size, size_t alignment, size_t page_size, char* addr, bool exec) {
  fatal("os::reserve_memory_special should not be called on Solaris.");
  return NULL;
}

bool os::pd_release_memory_special(char* base, size_t bytes) {
  fatal("os::release_memory_special should not be called on Solaris.");
  return false;
}

size_t os::large_page_size() {
  return _large_page_size;
}

// MPSS allows application to commit large page memory on demand; with ISM
// the entire memory region must be allocated as shared memory.
bool os::can_commit_large_page_memory() {
  return true;
}

bool os::can_execute_large_page_memory() {
  return true;
}

// Interface for setting lwp priorities.  We are using T2 libthread,
// which forces the use of bound threads, so all of our threads will
// be assigned to real lwp's.  Using the thr_setprio function is
// meaningless in this mode so we must adjust the real lwp's priority.
// The routines below implement the getting and setting of lwp priorities.
//
// Note: There are three priority scales used on Solaris.  Java priorities
//       which range from 1 to 10, libthread "thr_setprio" scale which range
//       from 0 to 127, and the current scheduling class of the process we
//       are running in.  This is typically from -60 to +60.
//       The setting of the lwp priorities is done after a call to thr_setprio
//       so Java priorities are mapped to libthread priorities and we map from
//       the latter to lwp priorities.  We don't keep priorities stored in
//       Java priorities since some of our worker threads want to set priorities
//       higher than all Java threads.
//
// For related information:
// (1)  man -s 2 priocntl
// (2)  man -s 4 priocntl
// (3)  man dispadmin
// =    librt.so
// =    libthread/common/rtsched.c - thrp_setlwpprio().
// =    ps -cL <pid> ... to validate priority.
// =    sched_get_priority_min and _max
//              pthread_create
//              sched_setparam
//              pthread_setschedparam
//
// Assumptions:
// +    We assume that all threads in the process belong to the same
//              scheduling class.   IE. a homogeneous process.
// +    Must be root or in IA group to change change "interactive" attribute.
//              Priocntl() will fail silently.  The only indication of failure is when
//              we read-back the value and notice that it hasn't changed.
// +    Interactive threads enter the runq at the head, non-interactive at the tail.
// +    For RT, change timeslice as well.  Invariant:
//              constant "priority integral"
//              Konst == TimeSlice * (60-Priority)
//              Given a priority, compute appropriate timeslice.
// +    Higher numerical values have higher priority.

// sched class attributes
typedef struct {
  int   schedPolicy;              // classID
  int   maxPrio;
  int   minPrio;
} SchedInfo;


static SchedInfo tsLimits, iaLimits, rtLimits, fxLimits;

#ifdef ASSERT
static int  ReadBackValidate = 1;
#endif
static int  myClass     = 0;
static int  myMin       = 0;
static int  myMax       = 0;
static int  myCur       = 0;
static bool priocntl_enable = false;

static const int criticalPrio = FXCriticalPriority;
static int java_MaxPriority_to_os_priority = 0; // Saved mapping


// lwp_priocntl_init
//
// Try to determine the priority scale for our process.
//
// Return errno or 0 if OK.
//
static int lwp_priocntl_init() {
  int rslt;
  pcinfo_t ClassInfo;
  pcparms_t ParmInfo;
  int i;

  if (!UseThreadPriorities) return 0;

  // If ThreadPriorityPolicy is 1, switch tables
  if (ThreadPriorityPolicy == 1) {
    for (i = 0; i < CriticalPriority+1; i++)
      os::java_to_os_priority[i] = prio_policy1[i];
  }
  if (UseCriticalJavaThreadPriority) {
    // MaxPriority always maps to the FX scheduling class and criticalPrio.
    // See set_native_priority() and set_lwp_class_and_priority().
    // Save original MaxPriority mapping in case attempt to
    // use critical priority fails.
    java_MaxPriority_to_os_priority = os::java_to_os_priority[MaxPriority];
    // Set negative to distinguish from other priorities
    os::java_to_os_priority[MaxPriority] = -criticalPrio;
  }

  // Get IDs for a set of well-known scheduling classes.
  // TODO-FIXME: GETCLINFO returns the current # of classes in the
  // the system.  We should have a loop that iterates over the
  // classID values, which are known to be "small" integers.

  strcpy(ClassInfo.pc_clname, "TS");
  ClassInfo.pc_cid = -1;
  rslt = priocntl(P_ALL, 0, PC_GETCID, (caddr_t)&ClassInfo);
  if (rslt < 0) return errno;
  assert(ClassInfo.pc_cid != -1, "cid for TS class is -1");
  tsLimits.schedPolicy = ClassInfo.pc_cid;
  tsLimits.maxPrio = ((tsinfo_t*)ClassInfo.pc_clinfo)->ts_maxupri;
  tsLimits.minPrio = -tsLimits.maxPrio;

  strcpy(ClassInfo.pc_clname, "IA");
  ClassInfo.pc_cid = -1;
  rslt = priocntl(P_ALL, 0, PC_GETCID, (caddr_t)&ClassInfo);
  if (rslt < 0) return errno;
  assert(ClassInfo.pc_cid != -1, "cid for IA class is -1");
  iaLimits.schedPolicy = ClassInfo.pc_cid;
  iaLimits.maxPrio = ((iainfo_t*)ClassInfo.pc_clinfo)->ia_maxupri;
  iaLimits.minPrio = -iaLimits.maxPrio;

  strcpy(ClassInfo.pc_clname, "RT");
  ClassInfo.pc_cid = -1;
  rslt = priocntl(P_ALL, 0, PC_GETCID, (caddr_t)&ClassInfo);
  if (rslt < 0) return errno;
  assert(ClassInfo.pc_cid != -1, "cid for RT class is -1");
  rtLimits.schedPolicy = ClassInfo.pc_cid;
  rtLimits.maxPrio = ((rtinfo_t*)ClassInfo.pc_clinfo)->rt_maxpri;
  rtLimits.minPrio = 0;

  strcpy(ClassInfo.pc_clname, "FX");
  ClassInfo.pc_cid = -1;
  rslt = priocntl(P_ALL, 0, PC_GETCID, (caddr_t)&ClassInfo);
  if (rslt < 0) return errno;
  assert(ClassInfo.pc_cid != -1, "cid for FX class is -1");
  fxLimits.schedPolicy = ClassInfo.pc_cid;
  fxLimits.maxPrio = ((fxinfo_t*)ClassInfo.pc_clinfo)->fx_maxupri;
  fxLimits.minPrio = 0;

  // Query our "current" scheduling class.
  // This will normally be IA, TS or, rarely, FX or RT.
  memset(&ParmInfo, 0, sizeof(ParmInfo));
  ParmInfo.pc_cid = PC_CLNULL;
  rslt = priocntl(P_PID, P_MYID, PC_GETPARMS, (caddr_t)&ParmInfo);
  if (rslt < 0) return errno;
  myClass = ParmInfo.pc_cid;

  // We now know our scheduling classId, get specific information
  // about the class.
  ClassInfo.pc_cid = myClass;
  ClassInfo.pc_clname[0] = 0;
  rslt = priocntl((idtype)0, 0, PC_GETCLINFO, (caddr_t)&ClassInfo);
  if (rslt < 0) return errno;

  memset(&ParmInfo, 0, sizeof(pcparms_t));
  ParmInfo.pc_cid = PC_CLNULL;
  rslt = priocntl(P_PID, P_MYID, PC_GETPARMS, (caddr_t)&ParmInfo);
  if (rslt < 0) return errno;

  if (ParmInfo.pc_cid == rtLimits.schedPolicy) {
    myMin = rtLimits.minPrio;
    myMax = rtLimits.maxPrio;
  } else if (ParmInfo.pc_cid == iaLimits.schedPolicy) {
    iaparms_t *iaInfo  = (iaparms_t*)ParmInfo.pc_clparms;
    myMin = iaLimits.minPrio;
    myMax = iaLimits.maxPrio;
    myMax = MIN2(myMax, (int)iaInfo->ia_uprilim);       // clamp - restrict
  } else if (ParmInfo.pc_cid == tsLimits.schedPolicy) {
    tsparms_t *tsInfo  = (tsparms_t*)ParmInfo.pc_clparms;
    myMin = tsLimits.minPrio;
    myMax = tsLimits.maxPrio;
    myMax = MIN2(myMax, (int)tsInfo->ts_uprilim);       // clamp - restrict
  } else if (ParmInfo.pc_cid == fxLimits.schedPolicy) {
    fxparms_t *fxInfo = (fxparms_t*)ParmInfo.pc_clparms;
    myMin = fxLimits.minPrio;
    myMax = fxLimits.maxPrio;
    myMax = MIN2(myMax, (int)fxInfo->fx_uprilim);       // clamp - restrict
  } else {
    return EINVAL;      // no clue, punt
  }

  priocntl_enable = true;  // Enable changing priorities
  return 0;
}

#define IAPRI(x)        ((iaparms_t *)((x).pc_clparms))
#define RTPRI(x)        ((rtparms_t *)((x).pc_clparms))
#define TSPRI(x)        ((tsparms_t *)((x).pc_clparms))
#define FXPRI(x)        ((fxparms_t *)((x).pc_clparms))


// scale_to_lwp_priority
//
// Convert from the libthread "thr_setprio" scale to our current
// lwp scheduling class scale.
//
static int scale_to_lwp_priority(int rMin, int rMax, int x) {
  int v;

  if (x == 127) return rMax;            // avoid round-down
  v = (((x*(rMax-rMin)))/128)+rMin;
  return v;
}


// set_lwp_class_and_priority
int set_lwp_class_and_priority(int ThreadID, int lwpid,
                               int newPrio, int new_class, bool scale) {
  int rslt;
  int Actual, Expected, prv;
  pcparms_t ParmInfo;                   // for GET-SET
#ifdef ASSERT
  pcparms_t ReadBack;                   // for readback
#endif

  // Set priority via PC_GETPARMS, update, PC_SETPARMS
  // Query current values.
  // TODO: accelerate this by eliminating the PC_GETPARMS call.
  // Cache "pcparms_t" in global ParmCache.
  // TODO: elide set-to-same-value

  // If something went wrong on init, don't change priorities.
  if (!priocntl_enable) {
    return EINVAL;
  }

  // If lwp hasn't started yet, just return
  // the _start routine will call us again.
  if (lwpid <= 0) {
    return 0;
  }

  memset(&ParmInfo, 0, sizeof(pcparms_t));
  ParmInfo.pc_cid = PC_CLNULL;
  rslt = priocntl(P_LWPID, lwpid, PC_GETPARMS, (caddr_t)&ParmInfo);
  if (rslt < 0) return errno;

  int cur_class = ParmInfo.pc_cid;
  ParmInfo.pc_cid = (id_t)new_class;

  if (new_class == rtLimits.schedPolicy) {
    rtparms_t *rtInfo  = (rtparms_t*)ParmInfo.pc_clparms;
    rtInfo->rt_pri     = scale ? scale_to_lwp_priority(rtLimits.minPrio,
                                                       rtLimits.maxPrio, newPrio)
                               : newPrio;
    rtInfo->rt_tqsecs  = RT_NOCHANGE;
    rtInfo->rt_tqnsecs = RT_NOCHANGE;
  } else if (new_class == iaLimits.schedPolicy) {
    iaparms_t* iaInfo  = (iaparms_t*)ParmInfo.pc_clparms;
    int maxClamped     = MIN2(iaLimits.maxPrio,
                              cur_class == new_class
                              ? (int)iaInfo->ia_uprilim : iaLimits.maxPrio);
    iaInfo->ia_upri    = scale ? scale_to_lwp_priority(iaLimits.minPrio,
                                                       maxClamped, newPrio)
                               : newPrio;
    iaInfo->ia_uprilim = cur_class == new_class
                           ? IA_NOCHANGE : (pri_t)iaLimits.maxPrio;
    iaInfo->ia_mode    = IA_NOCHANGE;
  } else if (new_class == tsLimits.schedPolicy) {
    tsparms_t* tsInfo  = (tsparms_t*)ParmInfo.pc_clparms;
    int maxClamped     = MIN2(tsLimits.maxPrio,
                              cur_class == new_class
                              ? (int)tsInfo->ts_uprilim : tsLimits.maxPrio);
    tsInfo->ts_upri    = scale ? scale_to_lwp_priority(tsLimits.minPrio,
                                                       maxClamped, newPrio)
                               : newPrio;
    tsInfo->ts_uprilim = cur_class == new_class
                           ? TS_NOCHANGE : (pri_t)tsLimits.maxPrio;
  } else if (new_class == fxLimits.schedPolicy) {
    fxparms_t* fxInfo  = (fxparms_t*)ParmInfo.pc_clparms;
    int maxClamped     = MIN2(fxLimits.maxPrio,
                              cur_class == new_class
                              ? (int)fxInfo->fx_uprilim : fxLimits.maxPrio);
    fxInfo->fx_upri    = scale ? scale_to_lwp_priority(fxLimits.minPrio,
                                                       maxClamped, newPrio)
                               : newPrio;
    fxInfo->fx_uprilim = cur_class == new_class
                           ? FX_NOCHANGE : (pri_t)fxLimits.maxPrio;
    fxInfo->fx_tqsecs  = FX_NOCHANGE;
    fxInfo->fx_tqnsecs = FX_NOCHANGE;
  } else {
    return EINVAL;    // no clue, punt
  }

  rslt = priocntl(P_LWPID, lwpid, PC_SETPARMS, (caddr_t)&ParmInfo);
  if (rslt < 0) return errno;

#ifdef ASSERT
  // Sanity check: read back what we just attempted to set.
  // In theory it could have changed in the interim ...
  //
  // The priocntl system call is tricky.
  // Sometimes it'll validate the priority value argument and
  // return EINVAL if unhappy.  At other times it fails silently.
  // Readbacks are prudent.

  if (!ReadBackValidate) return 0;

  memset(&ReadBack, 0, sizeof(pcparms_t));
  ReadBack.pc_cid = PC_CLNULL;
  rslt = priocntl(P_LWPID, lwpid, PC_GETPARMS, (caddr_t)&ReadBack);
  assert(rslt >= 0, "priocntl failed");
  Actual = Expected = 0xBAD;
  assert(ParmInfo.pc_cid == ReadBack.pc_cid, "cid's don't match");
  if (ParmInfo.pc_cid == rtLimits.schedPolicy) {
    Actual   = RTPRI(ReadBack)->rt_pri;
    Expected = RTPRI(ParmInfo)->rt_pri;
  } else if (ParmInfo.pc_cid == iaLimits.schedPolicy) {
    Actual   = IAPRI(ReadBack)->ia_upri;
    Expected = IAPRI(ParmInfo)->ia_upri;
  } else if (ParmInfo.pc_cid == tsLimits.schedPolicy) {
    Actual   = TSPRI(ReadBack)->ts_upri;
    Expected = TSPRI(ParmInfo)->ts_upri;
  } else if (ParmInfo.pc_cid == fxLimits.schedPolicy) {
    Actual   = FXPRI(ReadBack)->fx_upri;
    Expected = FXPRI(ParmInfo)->fx_upri;
  }
#endif

  return 0;
}

// Solaris only gives access to 128 real priorities at a time,
// so we expand Java's ten to fill this range.  This would be better
// if we dynamically adjusted relative priorities.
//
// The ThreadPriorityPolicy option allows us to select 2 different
// priority scales.
//
// ThreadPriorityPolicy=0
// Since the Solaris' default priority is MaximumPriority, we do not
// set a priority lower than Max unless a priority lower than
// NormPriority is requested.
//
// ThreadPriorityPolicy=1
// This mode causes the priority table to get filled with
// linear values.  NormPriority gets mapped to 50% of the
// Maximum priority and so on.  This will cause VM threads
// to get unfair treatment against other Solaris processes
// which do not explicitly alter their thread priorities.

int os::java_to_os_priority[CriticalPriority + 1] = {
  -99999,         // 0 Entry should never be used

  0,              // 1 MinPriority
  32,             // 2
  64,             // 3

  96,             // 4
  127,            // 5 NormPriority
  127,            // 6

  127,            // 7
  127,            // 8
  127,            // 9 NearMaxPriority

  127,            // 10 MaxPriority

  -criticalPrio   // 11 CriticalPriority
};

OSReturn os::set_native_priority(Thread* thread, int newpri) {
  OSThread* osthread = thread->osthread();

  // Save requested priority in case the thread hasn't been started
  osthread->set_native_priority(newpri);

  // Check for critical priority request
  bool fxcritical = false;
  if (newpri == -criticalPrio) {
    fxcritical = true;
    newpri = criticalPrio;
  }

  assert(newpri >= MinimumPriority && newpri <= MaximumPriority, "bad priority mapping");
  if (!UseThreadPriorities) return OS_OK;

  int status = 0;

  if (!fxcritical) {
    // Use thr_setprio only if we have a priority that thr_setprio understands
    status = thr_setprio(thread->osthread()->thread_id(), newpri);
  }

  int lwp_status =
          set_lwp_class_and_priority(osthread->thread_id(),
                                     osthread->lwp_id(),
                                     newpri,
                                     fxcritical ? fxLimits.schedPolicy : myClass,
                                     !fxcritical);
  if (lwp_status != 0 && fxcritical) {
    // Try again, this time without changing the scheduling class
    newpri = java_MaxPriority_to_os_priority;
    lwp_status = set_lwp_class_and_priority(osthread->thread_id(),
                                            osthread->lwp_id(),
                                            newpri, myClass, false);
  }
  status |= lwp_status;
  return (status == 0) ? OS_OK : OS_ERR;
}


OSReturn os::get_native_priority(const Thread* const thread,
                                 int *priority_ptr) {
  int p;
  if (!UseThreadPriorities) {
    *priority_ptr = NormalPriority;
    return OS_OK;
  }
  int status = thr_getprio(thread->osthread()->thread_id(), &p);
  if (status != 0) {
    return OS_ERR;
  }
  *priority_ptr = p;
  return OS_OK;
}

////////////////////////////////////////////////////////////////////////////////

// This does not do anything on Solaris. This is basically a hook for being
// able to use structured exception handling (thread-local exception filters) on, e.g., Win32.
void os::os_exception_wrapper(java_call_t f, JavaValue* value,
                              const methodHandle& method, JavaCallArguments* args,
                              JavaThread* thread) {
  f(value, method, args, thread);
}

void report_error(const char* file_name, int line_no, const char* title,
                  const char* format, ...);

// (Static) wrappers for the liblgrp API
os::Solaris::lgrp_home_func_t os::Solaris::_lgrp_home;
os::Solaris::lgrp_init_func_t os::Solaris::_lgrp_init;
os::Solaris::lgrp_fini_func_t os::Solaris::_lgrp_fini;
os::Solaris::lgrp_root_func_t os::Solaris::_lgrp_root;
os::Solaris::lgrp_children_func_t os::Solaris::_lgrp_children;
os::Solaris::lgrp_resources_func_t os::Solaris::_lgrp_resources;
os::Solaris::lgrp_nlgrps_func_t os::Solaris::_lgrp_nlgrps;
os::Solaris::lgrp_cookie_stale_func_t os::Solaris::_lgrp_cookie_stale;
os::Solaris::lgrp_cookie_t os::Solaris::_lgrp_cookie = 0;

static address resolve_symbol_lazy(const char* name) {
  address addr = (address) dlsym(RTLD_DEFAULT, name);
  if (addr == NULL) {
    // RTLD_DEFAULT was not defined on some early versions of 2.5.1
    addr = (address) dlsym(RTLD_NEXT, name);
  }
  return addr;
}

static address resolve_symbol(const char* name) {
  address addr = resolve_symbol_lazy(name);
  if (addr == NULL) {
    fatal("resolve_symbol failed (%s)", dlerror());
  }
  return addr;
}

void os::Solaris::libthread_init() {
  address func = (address)dlsym(RTLD_DEFAULT, "_thr_suspend_allmutators");

  lwp_priocntl_init();

  // RTLD_DEFAULT was not defined on some early versions of 5.5.1
  if (func == NULL) {
    func = (address) dlsym(RTLD_NEXT, "_thr_suspend_allmutators");
    // Guarantee that this VM is running on an new enough OS (5.6 or
    // later) that it will have a new enough libthread.so.
    guarantee(func != NULL, "libthread.so is too old.");
  }

  int size;
  void (*handler_info_func)(address *, int *);
  handler_info_func = CAST_TO_FN_PTR(void (*)(address *, int *), resolve_symbol("thr_sighndlrinfo"));
  handler_info_func(&handler_start, &size);
  handler_end = handler_start + size;
}


bool os::Solaris::_synchronization_initialized;

void os::Solaris::synchronization_init() {
  _synchronization_initialized = true;
}

bool os::Solaris::liblgrp_init() {
  void *handle = dlopen("liblgrp.so.1", RTLD_LAZY);
  if (handle != NULL) {
    os::Solaris::set_lgrp_home(CAST_TO_FN_PTR(lgrp_home_func_t, dlsym(handle, "lgrp_home")));
    os::Solaris::set_lgrp_init(CAST_TO_FN_PTR(lgrp_init_func_t, dlsym(handle, "lgrp_init")));
    os::Solaris::set_lgrp_fini(CAST_TO_FN_PTR(lgrp_fini_func_t, dlsym(handle, "lgrp_fini")));
    os::Solaris::set_lgrp_root(CAST_TO_FN_PTR(lgrp_root_func_t, dlsym(handle, "lgrp_root")));
    os::Solaris::set_lgrp_children(CAST_TO_FN_PTR(lgrp_children_func_t, dlsym(handle, "lgrp_children")));
    os::Solaris::set_lgrp_resources(CAST_TO_FN_PTR(lgrp_resources_func_t, dlsym(handle, "lgrp_resources")));
    os::Solaris::set_lgrp_nlgrps(CAST_TO_FN_PTR(lgrp_nlgrps_func_t, dlsym(handle, "lgrp_nlgrps")));
    os::Solaris::set_lgrp_cookie_stale(CAST_TO_FN_PTR(lgrp_cookie_stale_func_t,
                                                      dlsym(handle, "lgrp_cookie_stale")));

    lgrp_cookie_t c = lgrp_init(LGRP_VIEW_CALLER);
    set_lgrp_cookie(c);
    return true;
  }
  return false;
}

int os::Solaris::_dev_zero_fd = -1;

// this is called _before_ the global arguments have been parsed
void os::init(void) {
  _initial_pid = getpid();

  max_hrtime = first_hrtime = gethrtime();

  init_random(1234567);

  int page_size = sysconf(_SC_PAGESIZE);
  OSInfo::set_vm_page_size(page_size);
  OSInfo::set_vm_allocation_granularity(page_size);
  if (os::vm_page_size() <= 0) {
    fatal("os_solaris.cpp: os::init: sysconf failed (%s)", os::strerror(errno));
  }
  _page_sizes.add(os::vm_page_size());

  Solaris::initialize_system_info();

  int fd = ::open("/dev/zero", O_RDWR);
  if (fd < 0) {
    fatal("os::init: cannot open /dev/zero (%s)", os::strerror(errno));
  } else {
    Solaris::set_dev_zero_fd(fd);

    // Close on exec, child won't inherit.
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }

  clock_tics_per_sec = CLK_TCK;

  // check if dladdr1() exists; dladdr1 can provide more information than
  // dladdr for os::dll_address_to_function_name. It comes with SunOS 5.9
  // and is available on linker patches for 5.7 and 5.8.
  // libdl.so must have been loaded, this call is just an entry lookup
  void * hdl = dlopen("libdl.so", RTLD_NOW);
  if (hdl) {
    dladdr1_func = CAST_TO_FN_PTR(dladdr1_func_type, dlsym(hdl, "dladdr1"));
  }

  // main_thread points to the thread that created/loaded the JVM.
  main_thread = thr_self();

  // dynamic lookup of functions that may not be available in our lowest
  // supported Solaris release
  void * handle = dlopen("libc.so.1", RTLD_LAZY);
  if (handle != NULL) {
    Solaris::_pthread_setname_np =  // from 11.3
        (Solaris::pthread_setname_np_func_t)dlsym(handle, "pthread_setname_np");
  }

  // Shared Posix initialization
  os::Posix::init();
}

// To install functions for atexit system call
extern "C" {
  static void perfMemory_exit_helper() {
    perfMemory_exit();
  }
}

// this is called _after_ the global arguments have been parsed
jint os::init_2(void) {
  Solaris::libthread_init();

  if (UseNUMA) {
    if (!Solaris::liblgrp_init()) {
      FLAG_SET_ERGO(UseNUMA, false);
    } else {
      size_t lgrp_limit = os::numa_get_groups_num();
      int *lgrp_ids = NEW_C_HEAP_ARRAY(int, lgrp_limit, mtInternal);
      size_t lgrp_num = os::numa_get_leaf_groups(lgrp_ids, lgrp_limit);
      FREE_C_HEAP_ARRAY(int, lgrp_ids);
      if (lgrp_num < 2) {
        // There's only one locality group, disable NUMA
        UseNUMA = false;
      }
    }
  }

  // When NUMA requested, not-NUMA-aware allocations default to interleaving.
  if (UseNUMA && !UseNUMAInterleaving) {
    FLAG_SET_ERGO_IF_DEFAULT(UseNUMAInterleaving, true);
  }

  if (PosixSignals::init() == JNI_ERR) {
    return JNI_ERR;
  }

  // initialize synchronization primitives
  Solaris::synchronization_init();
  DEBUG_ONLY(os::set_mutex_init_done();)

  if (MaxFDLimit) {
    // set the number of file descriptors to max. print out error
    // if getrlimit/setrlimit fails but continue regardless.
    struct rlimit nbr_files;
    int status = getrlimit(RLIMIT_NOFILE, &nbr_files);
    if (status != 0) {
      log_info(os)("os::init_2 getrlimit failed: %s", os::strerror(errno));
    } else {
      nbr_files.rlim_cur = nbr_files.rlim_max;
      status = setrlimit(RLIMIT_NOFILE, &nbr_files);
      if (status != 0) {
        log_info(os)("os::init_2 setrlimit failed: %s", os::strerror(errno));
      }
    }
  }

  // Calculate theoretical max. size of Threads to guard gainst
  // artificial out-of-memory situations, where all available address-
  // space has been reserved by thread stacks. Default stack size is 1Mb.
  size_t pre_thread_stack_size = (JavaThread::stack_size_at_create()) ?
    JavaThread::stack_size_at_create() : (1*K*K);
  assert(pre_thread_stack_size != 0, "Must have a stack");
  // Solaris has a maximum of 4Gb of user programs. Calculate the thread limit when
  // we should start doing Virtual Memory banging. Currently when the threads will
  // have used all but 200Mb of space.
  size_t max_address_space = ((unsigned int)4 * K * K * K) - (200 * K * K);
  Solaris::_os_thread_limit = max_address_space / pre_thread_stack_size;

  // at-exit methods are called in the reverse order of their registration.
  // In Solaris 7 and earlier, atexit functions are called on return from
  // main or as a result of a call to exit(3C). There can be only 32 of
  // these functions registered and atexit() does not set errno. In Solaris
  // 8 and later, there is no limit to the number of functions registered
  // and atexit() sets errno. In addition, in Solaris 8 and later, atexit
  // functions are called upon dlclose(3DL) in addition to return from main
  // and exit(3C).

  if (PerfAllowAtExitRegistration) {
    // only register atexit functions if PerfAllowAtExitRegistration is set.
    // atexit functions can be delayed until process exit time, which
    // can be problematic for embedded VM situations. Embedded VMs should
    // call DestroyJavaVM() to assure that VM resources are released.

    // note: perfMemory_exit_helper atexit function may be removed in
    // the future if the appropriate cleanup code can be added to the
    // VM_Exit VMOperation's doit method.
    if (atexit(perfMemory_exit_helper) != 0) {
      warning("os::init2 atexit(perfMemory_exit_helper) failed");
    }
  }

  // Shared Posix initialization
  os::Posix::init_2();

  return JNI_OK;
}

// This code originates from JDK's sysOpen and open64_w
// from src/solaris/hpi/src/system_md.c

int os::open(const char *path, int oflag, int mode) {
  if (strlen(path) > MAX_PATH - 1) {
    errno = ENAMETOOLONG;
    return -1;
  }
  int fd;

  fd = ::open64(path, oflag, mode);
  if (fd == -1) return -1;

  // If the open succeeded, the file might still be a directory
  {
    struct stat64 buf64;
    int ret = ::fstat64(fd, &buf64);
    int st_mode = buf64.st_mode;

    if (ret != -1) {
      if ((st_mode & S_IFMT) == S_IFDIR) {
        errno = EISDIR;
        ::close(fd);
        return -1;
      }
    } else {
      ::close(fd);
      return -1;
    }
  }

  // All file descriptors that are opened in the JVM and not
  // specifically destined for a subprocess should have the
  // close-on-exec flag set.  If we don't set it, then careless 3rd
  // party native code might fork and exec without closing all
  // appropriate file descriptors (e.g. as we do in closeDescriptors in
  // UNIXProcess.c), and this in turn might:
  //
  // - cause end-of-file to fail to be detected on some file
  //   descriptors, resulting in mysterious hangs, or
  //
  // - might cause an fopen in the subprocess to fail on a system
  //   suffering from bug 1085341.
  //
  // (Yes, the default setting of the close-on-exec flag is a Unix
  // design flaw)
  //
  // See:
  // 1085341: 32-bit stdio routines should support file descriptors >255
  // 4843136: (process) pipe file descriptor from Runtime.exec not being closed
  // 6339493: (process) Runtime.exec does not close all file descriptors on Solaris 9
  //
#ifdef FD_CLOEXEC
  {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags != -1) {
      ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }
#endif

  return fd;
}

// create binary file, rewriting existing file if required
int os::create_binary_file(const char* path, bool rewrite_existing) {
  int oflags = O_WRONLY | O_CREAT;
  if (!rewrite_existing) {
    oflags |= O_EXCL;
  }
  return ::open64(path, oflags, S_IREAD | S_IWRITE);
}

// return current position of file pointer
jlong os::current_file_offset(int fd) {
  return (jlong)::lseek64(fd, (off64_t)0, SEEK_CUR);
}

// move file pointer to the specified offset
jlong os::seek_to_file_offset(int fd, jlong offset) {
  return (jlong)::lseek64(fd, (off64_t)offset, SEEK_SET);
}

// Map a block of memory.
char* os::pd_map_memory(int fd, const char* file_name, size_t file_offset,
                        char *addr, size_t bytes, bool read_only,
                        bool allow_exec) {
  int prot;
  int flags;

  if (read_only) {
    prot = PROT_READ;
    flags = MAP_SHARED;
  } else {
    prot = PROT_READ | PROT_WRITE;
    flags = MAP_PRIVATE;
  }

  if (allow_exec) {
    prot |= PROT_EXEC;
  }

  if (addr != NULL) {
    flags |= MAP_FIXED;
  }

  char* mapped_address = (char*)mmap(addr, (size_t)bytes, prot, flags,
                                     fd, file_offset);
  if (mapped_address == MAP_FAILED) {
    return NULL;
  }
  return mapped_address;
}


// Remap a block of memory.
char* os::pd_remap_memory(int fd, const char* file_name, size_t file_offset,
                          char *addr, size_t bytes, bool read_only,
                          bool allow_exec) {
  // same as map_memory() on this OS
  return os::map_memory(fd, file_name, file_offset, addr, bytes, read_only,
                        allow_exec);
}


// Unmap a block of memory.
bool os::pd_unmap_memory(char* addr, size_t bytes) {
  return munmap(addr, bytes) == 0;
}

const intptr_t thr_time_off  = (intptr_t)(&((prusage_t *)(NULL))->pr_utime);
const intptr_t thr_time_size = (intptr_t)(&((prusage_t *)(NULL))->pr_ttime) -
                               (intptr_t)(&((prusage_t *)(NULL))->pr_utime);


// JVMTI & JVM monitoring and management support
// The thread_cpu_time() and current_thread_cpu_time() are only
// supported if is_thread_cpu_time_supported() returns true.
// They are not supported on Solaris T1.

// current_thread_cpu_time(bool) and thread_cpu_time(Thread*, bool)
// are used by JVM M&M and JVMTI to get user+sys or user CPU time
// of a thread.
//
// current_thread_cpu_time() and thread_cpu_time(Thread *)
// returns the fast estimate available on the platform.

// hrtime_t gethrvtime() return value includes
// user time but does not include system time
jlong os::current_thread_cpu_time() {
  return (jlong) gethrvtime();
}

jlong os::thread_cpu_time(Thread *thread) {
  // return user level CPU time only to be consistent with
  // what current_thread_cpu_time returns.
  // thread_cpu_time_info() must be changed if this changes
  return os::thread_cpu_time(thread, false /* user time only */);
}

jlong os::current_thread_cpu_time(bool user_sys_cpu_time) {
  if (user_sys_cpu_time) {
    return os::thread_cpu_time(Thread::current(), user_sys_cpu_time);
  } else {
    return os::current_thread_cpu_time();
  }
}

jlong os::thread_cpu_time(Thread *thread, bool user_sys_cpu_time) {
  char proc_name[64];
  int count;
  prusage_t prusage;
  jlong lwp_time;
  int fd;

  sprintf(proc_name, "/proc/%d/lwp/%d/lwpusage",
          getpid(),
          thread->osthread()->lwp_id());
  fd = ::open(proc_name, O_RDONLY);
  if (fd == -1) return -1;

  do {
    count = ::pread(fd,
                    (void *)&prusage.pr_utime,
                    thr_time_size,
                    thr_time_off);
  } while (count < 0 && errno == EINTR);
  ::close(fd);
  if (count < 0) return -1;

  if (user_sys_cpu_time) {
    // user + system CPU time
    lwp_time = (((jlong)prusage.pr_stime.tv_sec +
                 (jlong)prusage.pr_utime.tv_sec) * (jlong)1000000000) +
                 (jlong)prusage.pr_stime.tv_nsec +
                 (jlong)prusage.pr_utime.tv_nsec;
  } else {
    // user level CPU time only
    lwp_time = ((jlong)prusage.pr_utime.tv_sec * (jlong)1000000000) +
                (jlong)prusage.pr_utime.tv_nsec;
  }

  return (lwp_time);
}

void os::current_thread_cpu_time_info(jvmtiTimerInfo *info_ptr) {
  info_ptr->max_value = ALL_64_BITS;      // will not wrap in less than 64 bits
  info_ptr->may_skip_backward = false;    // elapsed time not wall time
  info_ptr->may_skip_forward = false;     // elapsed time not wall time
  info_ptr->kind = JVMTI_TIMER_USER_CPU;  // only user time is returned
}

void os::thread_cpu_time_info(jvmtiTimerInfo *info_ptr) {
  info_ptr->max_value = ALL_64_BITS;      // will not wrap in less than 64 bits
  info_ptr->may_skip_backward = false;    // elapsed time not wall time
  info_ptr->may_skip_forward = false;     // elapsed time not wall time
  info_ptr->kind = JVMTI_TIMER_USER_CPU;  // only user time is returned
}

bool os::is_thread_cpu_time_supported() {
  return true;
}

// System loadavg support.  Returns -1 if load average cannot be obtained.
// Return the load average for our processor set.
int os::loadavg(double loadavg[], int nelem) {
  return pset_getloadavg(PS_MYID, loadavg, nelem);
}

//---------------------------------------------------------------------------------

bool os::find(address addr, outputStream* st) {
  Dl_info dlinfo;
  memset(&dlinfo, 0, sizeof(dlinfo));
  if (dladdr(addr, &dlinfo) != 0) {
    st->print(PTR_FORMAT ": ", p2i(addr));
    if (dlinfo.dli_sname != NULL && dlinfo.dli_saddr != NULL) {
      st->print("%s+" PTR_FORMAT, dlinfo.dli_sname,
                p2i(addr) - p2i(dlinfo.dli_saddr));
    } else if (dlinfo.dli_fbase != NULL) {
      st->print("<offset " PTR_FORMAT ">", p2i(addr) - p2i(dlinfo.dli_fbase));
    } else {
      st->print("<absolute address>");
    }
    if (dlinfo.dli_fname != NULL) {
      st->print(" in %s", dlinfo.dli_fname);
    }
    if (dlinfo.dli_fbase != NULL) {
      st->print(" at " PTR_FORMAT, p2i(dlinfo.dli_fbase));
    }
    st->cr();

    if (Verbose) {
      // decode some bytes around the PC
      address begin = clamp_address_in_page(addr-40, addr, os::vm_page_size());
      address end   = clamp_address_in_page(addr+40, addr, os::vm_page_size());
      address       lowest = (address) dlinfo.dli_sname;
      if (!lowest)  lowest = (address) dlinfo.dli_fbase;
      if (begin < lowest)  begin = lowest;
      Dl_info dlinfo2;
      if (dladdr(end, &dlinfo2) != 0 && dlinfo2.dli_saddr != dlinfo.dli_saddr
          && end > dlinfo2.dli_saddr && dlinfo2.dli_saddr > begin) {
        end = (address) dlinfo2.dli_saddr;
      }
      Disassembler::decode(begin, end, st);
    }
    return true;
  }
  return false;
}

// Following function has been added to support HotSparc's libjvm.so running
// under Solaris production JDK 1.2.2 / 1.3.0.  These came from
// src/solaris/hpi/native_threads in the EVM codebase.
//
// NOTE: This is no longer needed in the 1.3.1 and 1.4 production release
// libraries and should thus be removed. We will leave it behind for a while
// until we no longer want to able to run on top of 1.3.0 Solaris production
// JDK. See 4341971.

#define STACK_SLACK 0x800

extern "C" {
  intptr_t sysThreadAvailableStackWithSlack() {
    stack_t st;
    intptr_t retval, stack_top;
    retval = thr_stksegment(&st);
    assert(retval == 0, "incorrect return value from thr_stksegment");
    assert((address)&st < (address)st.ss_sp, "Invalid stack base returned");
    assert((address)&st > (address)st.ss_sp-st.ss_size, "Invalid stack size returned");
    stack_top=(intptr_t)st.ss_sp-st.ss_size;
    return ((intptr_t)&stack_top - stack_top - STACK_SLACK);
  }
}

// ObjectMonitor park-unpark infrastructure ...
//
// We implement Solaris and Linux PlatformEvents with the
// obvious condvar-mutex-flag triple.
// Another alternative that works quite well is pipes:
// Each PlatformEvent consists of a pipe-pair.
// The thread associated with the PlatformEvent
// calls park(), which reads from the input end of the pipe.
// Unpark() writes into the other end of the pipe.
// The write-side of the pipe must be set NDELAY.
// Unfortunately pipes consume a large # of handles.
// Native solaris lwp_park() and lwp_unpark() work nicely, too.
// Using pipes for the 1st few threads might be workable, however.
//
// park() is permitted to return spuriously.
// Callers of park() should wrap the call to park() in
// an appropriate loop.  A litmus test for the correct
// usage of park is the following: if park() were modified
// to immediately return 0 your code should still work,
// albeit degenerating to a spin loop.
//
// In a sense, park()-unpark() just provides more polite spinning
// and polling with the key difference over naive spinning being
// that a parked thread needs to be explicitly unparked() in order
// to wake up and to poll the underlying condition.
//
// Assumption:
//    Only one parker can exist on an event, which is why we allocate
//    them per-thread. Multiple unparkers can coexist.
//
// _event transitions in park()
//   -1 => -1 : illegal
//    1 =>  0 : pass - return immediately
//    0 => -1 : block; then set _event to 0 before returning
//
// _event transitions in unpark()
//    0 => 1 : just return
//    1 => 1 : just return
//   -1 => either 0 or 1; must signal target thread
//         That is, we can safely transition _event from -1 to either
//         0 or 1.
//
// _event serves as a restricted-range semaphore.
//   -1 : thread is blocked, i.e. there is a waiter
//    0 : neutral: thread is running or ready,
//        could have been signaled after a wait started
//    1 : signaled - thread is running or ready
//
// Another possible encoding of _event would be with
// explicit "PARKED" == 01b and "SIGNALED" == 10b bits.
//
// TODO-FIXME: add DTRACE probes for:
// 1.   Tx parks
// 2.   Ty unparks Tx
// 3.   Tx resumes from park

// JSR166
// -------------------------------------------------------

// The solaris and linux implementations of park/unpark are fairly
// conservative for now, but can be improved. They currently use a
// mutex/condvar pair, plus _counter.
// Park decrements _counter if > 0, else does a condvar wait.  Unpark
// sets count to 1 and signals condvar.  Only one thread ever waits
// on the condvar. Contention seen when trying to park implies that someone
// is unparking you, so don't wait. And spurious returns are fine, so there
// is no need to track notifications.

// Get the default path to the core file
// Returns the length of the string
int os::get_core_path(char* buffer, size_t bufferSize) {
  const char* p = get_current_directory(buffer, bufferSize);

  if (p == NULL) {
    assert(p != NULL, "failed to get current directory");
    return 0;
  }

  jio_snprintf(buffer, bufferSize, "%s/core or core.%d",
                                              p, current_process_id());

  return strlen(buffer);
}

bool os::supports_map_sync() {
  return false;
}

#ifndef PRODUCT
void TestReserveMemorySpecial_test() {
  // No tests available for this platform
}
#endif

bool os::start_debugging(char *buf, int buflen) {
  int len = (int)strlen(buf);
  char *p = &buf[len];

  jio_snprintf(p, buflen-len,
               "\n\n"
               "Do you want to debug the problem?\n\n"
               "To debug, run 'dbx - %d'; then switch to thread " INTX_FORMAT "\n"
               "Enter 'yes' to launch dbx automatically (PATH must include dbx)\n"
               "Otherwise, press RETURN to abort...",
               os::current_process_id(), os::current_thread_id());

  bool yes = os::message_box("Unexpected Error", buf);

  if (yes) {
    // yes, user asked VM to launch debugger
    jio_snprintf(buf, sizeof(buf), "dbx - %d", os::current_process_id());

    os::fork_and_exec(buf);
    yes = false;
  }
  return yes;
}

void os::print_memory_mappings(char* addr, size_t bytes, outputStream* st) {}

#if INCLUDE_JFR

void os::jfr_report_memory_info() {}

#endif // INCLUDE_JFR

bool os::pd_dll_unload(void* libhandle, char* ebuf, int ebuflen) {

  if (ebuf && ebuflen > 0) {
    ebuf[0] = '\0';
    ebuf[ebuflen - 1] = '\0';
  }

  bool res = (0 == ::dlclose(libhandle));
  if (!res) {
    // error analysis when dlopen fails
    const char* error_report = ::dlerror();
    if (error_report == nullptr) {
      error_report = "dlerror returned no error description";
    }
    if (ebuf != nullptr && ebuflen > 0) {
      snprintf(ebuf, ebuflen - 1, "%s", error_report);
    }
  }

  return res;
} // end: os::pd_dll_unload()

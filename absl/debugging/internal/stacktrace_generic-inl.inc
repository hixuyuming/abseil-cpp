// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Portable implementation - just use glibc
//
// Note:  The glibc implementation may cause a call to malloc.
// This can cause a deadlock in HeapProfiler.

#ifndef ABSL_DEBUGGING_INTERNAL_STACKTRACE_GENERIC_INL_H_
#define ABSL_DEBUGGING_INTERNAL_STACKTRACE_GENERIC_INL_H_

#include <execinfo.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "absl/base/attributes.h"
#include "absl/debugging/stacktrace.h"

// When the macro __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ is defined, Android
// NDK supports a mode where all symbols added later than the target API level
// are weak. In this mode, __builtin_available can be used to check the actual
// API level available at runtime. There is also a compiler warning,
// -Wunguarded-availability, that detects unchecked uses of APIs above the level
// specified by __ANDROID_API__ that are not guarded by __builtin_available.
// Details: https://developer.android.com/ndk/guides/using-newer-apis
//
// We take advantage of this mode to weakly link to backtrace() even when the
// target API level is below 33, which is the first version of Android that
// provides backtrace() in its libc.
//
// Unfortunately, -Wunguarded-availability does not do any control flow analysis
// and will trigger if we do anything other than putting all calls to the target
// function in an if() statement that calls the builtin. We can't return early
// or even negate the condition.
#if defined(__ANDROID__) && defined(__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
// In weak symbol mode, check the compiler builtin.
#define ABSL_INTERNAL_BACKTRACE_AVAILABLE() __builtin_available(android 33, *)
#else
// Otherwise, assume that backtrace() is available, because otherwise this file
// wouldn't be included.
#define ABSL_INTERNAL_BACKTRACE_AVAILABLE() true
#endif

// Sometimes, we can try to get a stack trace from within a stack
// trace, because we don't block signals inside this code (which would be too
// expensive: the two extra system calls per stack trace do matter here).
// That can cause a self-deadlock.
// Protect against such reentrant call by failing to get a stack trace.
//
// We use __thread here because the code here is extremely low level -- it is
// called while collecting stack traces from within malloc and mmap, and thus
// can not call anything which might call malloc or mmap itself.
static __thread int recursive = 0;

// The stack trace function might be invoked very early in the program's
// execution (e.g. from the very first malloc if using tcmalloc). Also, the
// glibc implementation itself will trigger malloc the first time it is called.
// As such, we suppress usage of backtrace during this early stage of execution.
static std::atomic<bool> disable_stacktraces(true);  // Disabled until healthy.
// Waiting until static initializers run seems to be late enough.
// This file is included into stacktrace.cc so this will only run once.
ABSL_ATTRIBUTE_UNUSED static int stacktraces_enabler = []() {
  if (ABSL_INTERNAL_BACKTRACE_AVAILABLE()) {
    void* unused_stack[1];
    // Force the first backtrace to happen early to get the one-time shared lib
    // loading (allocation) out of the way. After the first call it is much
    // safer to use backtrace from a signal handler if we crash somewhere later.
    backtrace(unused_stack, 1);
    disable_stacktraces.store(false, std::memory_order_relaxed);
  }
  return 0;
}();

template <bool IS_STACK_FRAMES, bool IS_WITH_CONTEXT>
static int UnwindImpl(void** result, uintptr_t* frames, int* sizes,
                      int max_depth, int skip_count, const void* ucp,
                      int* min_dropped_frames) {
  if (recursive || disable_stacktraces.load(std::memory_order_relaxed)) {
    return 0;
  }
  if (ABSL_INTERNAL_BACKTRACE_AVAILABLE()) {
    ++recursive;

    static_cast<void>(ucp);  // Unused.
    static const int kStackLength = 64;
    void * stack[kStackLength];
    int size;

    size = backtrace(stack, kStackLength);
    skip_count++;  // we want to skip the current frame as well
    int result_count = size - skip_count;
    if (result_count < 0)
      result_count = 0;
    if (result_count > max_depth)
      result_count = max_depth;
    for (int i = 0; i < result_count; i++)
      result[i] = stack[i + skip_count];

    if (IS_STACK_FRAMES) {
      // No implementation for finding out the stack frames yet.
      if (frames != nullptr) {
        memset(frames, 0, sizeof(*frames) * static_cast<size_t>(result_count));
      }
      if (sizes != nullptr) {
        memset(sizes, 0, sizeof(*sizes) * static_cast<size_t>(result_count));
      }
    }
    if (min_dropped_frames != nullptr) {
      if (size - skip_count - max_depth > 0) {
        *min_dropped_frames = size - skip_count - max_depth;
      } else {
        *min_dropped_frames = 0;
      }
    }

    --recursive;

    return result_count;
  }
  return 0;
}

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {
bool StackTraceWorksForTest() {
  return true;
}
}  // namespace debugging_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_DEBUGGING_INTERNAL_STACKTRACE_GENERIC_INL_H_

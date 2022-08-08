/*
 * from backward.hpp
 * Copyright 2013 Google Inc. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#if defined(__linux) || defined(__linux__)
#define BACKWARD_SYSTEM_LINUX
#elif defined(__APPLE__)
#define BACKWARD_SYSTEM_DARWIN
#elif defined(_WIN32)
#define BACKWARD_SYSTEM_WINDOWS
#else
#define BACKWARD_SYSTEM_UNKNOWN
#endif

#define NOINLINE __attribute__((noinline))

#if defined(BACKWARD_SYSTEM_LINUX)

// On linux, backtrace can back-trace or "walk" the stack using the following
// libraries:
//
// #define BACKWARD_HAS_UNWIND 1
//  - unwind comes from libgcc, but I saw an equivalent inside clang itself.
//  - with unwind, the stacktrace is as accurate as it can possibly be, since
//  this is used by the C++ runtine in gcc/clang for stack unwinding on
//  exception.
//  - normally libgcc is already linked to your program by default.
//
// #define BACKWARD_HAS_LIBUNWIND 1
//  - libunwind provides, in some cases, a more accurate stacktrace as it knows
//  to decode signal handler frames and lets us edit the context registers when
//  unwinding, allowing stack traces over bad function references.
//
// #define BACKWARD_HAS_BACKTRACE == 1
//  - backtrace seems to be a little bit more portable than libunwind, but on
//  linux, it uses unwind anyway, but abstract away a tiny information that is
//  sadly really important in order to get perfectly accurate stack traces.
//  - backtrace is part of the (e)glib library.
//
// The default is:
// #define BACKWARD_HAS_UNWIND == 1
//
// Note that only one of the define should be set to 1 at a time.
//
#if BACKWARD_HAS_UNWIND == 1
#elif BACKWARD_HAS_LIBUNWIND == 1
#elif BACKWARD_HAS_BACKTRACE == 1
#else
#undef BACKWARD_HAS_UNWIND
#define BACKWARD_HAS_UNWIND 1
#undef BACKWARD_HAS_LIBUNWIND
#define BACKWARD_HAS_LIBUNWIND 0
#undef BACKWARD_HAS_BACKTRACE
#define BACKWARD_HAS_BACKTRACE 0
#endif

// On linux, backward can extract detailed information about a stack trace
// using one of the following libraries:
//
// #define BACKWARD_HAS_DW 1
//  - libdw gives you the most juicy details out of your stack traces:
//    - object filename
//    - function name
//    - source filename
//    - line and column numbers
//    - source code snippet (assuming the file is accessible)
//    - variable names (if not optimized out)
//    - variable values (not supported by backward-cpp)
//  - You need to link with the lib "dw":
//    - apt-get install libdw-dev
//    - g++/clang++ -ldw ...
//
// #define BACKWARD_HAS_BFD 1
//  - With libbfd, you get a fair amount of details:
//    - object filename
//    - function name
//    - source filename
//    - line numbers
//    - source code snippet (assuming the file is accessible)
//  - You need to link with the lib "bfd":
//    - apt-get install binutils-dev
//    - g++/clang++ -lbfd ...
//
// #define BACKWARD_HAS_DWARF 1
//  - libdwarf gives you the most juicy details out of your stack traces:
//    - object filename
//    - function name
//    - source filename
//    - line and column numbers
//    - source code snippet (assuming the file is accessible)
//    - variable names (if not optimized out)
//    - variable values (not supported by backward-cpp)
//  - You need to link with the lib "dwarf":
//    - apt-get install libdwarf-dev
//    - g++/clang++ -ldwarf ...
//
// #define BACKWARD_HAS_BACKTRACE_SYMBOL 1
//  - backtrace provides minimal details for a stack trace:
//    - object filename
//    - function name
//  - backtrace is part of the (e)glib library.
//
// The default is:
// #define BACKWARD_HAS_BACKTRACE_SYMBOL == 1
//
// Note that only one of the define should be set to 1 at a time.
//
#if BACKWARD_HAS_DW == 1
#elif BACKWARD_HAS_BFD == 1
#elif BACKWARD_HAS_DWARF == 1
#elif BACKWARD_HAS_BACKTRACE_SYMBOL == 1
#else
#undef BACKWARD_HAS_DW
#define BACKWARD_HAS_DW 0
#undef BACKWARD_HAS_BFD
#define BACKWARD_HAS_BFD 0
#undef BACKWARD_HAS_DWARF
#define BACKWARD_HAS_DWARF 0
#undef BACKWARD_HAS_BACKTRACE_SYMBOL
#define BACKWARD_HAS_BACKTRACE_SYMBOL 1
#endif

#include <cxxabi.h>
#include <fcntl.h>
#ifdef __ANDROID__
//		Old Android API levels define _Unwind_Ptr in both link.h and
// unwind.h 		Rename the one in link.h as we are not going to be using
// it
#define _Unwind_Ptr _Unwind_Ptr_Custom
#include <link.h>
#undef _Unwind_Ptr
#else
#include <link.h>
#endif
#include <signal.h>
#include <sys/stat.h>
#include <syscall.h>
#include <unistd.h>

#if BACKWARD_HAS_BFD == 1
//              NOTE: defining PACKAGE{,_VERSION} is required before including
//                    bfd.h on some platforms, see also:
//                    https://sourceware.org/bugzilla/show_bug.cgi?id=14243
#ifndef PACKAGE
#define PACKAGE
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION
#endif
#include <bfd.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <dlfcn.h>
#undef _GNU_SOURCE
#else
#include <dlfcn.h>
#endif
#endif

#if BACKWARD_HAS_DW == 1
#include <dwarf.h>
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#endif

#if BACKWARD_HAS_DWARF == 1
#include <algorithm>
#include <dwarf.h>
#include <libdwarf.h>
#include <libelf.h>
#include <map>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <dlfcn.h>
#undef _GNU_SOURCE
#else
#include <dlfcn.h>
#endif
#endif

#if (BACKWARD_HAS_BACKTRACE == 1) || (BACKWARD_HAS_BACKTRACE_SYMBOL == 1)
// then we shall rely on backtrace
#include <execinfo.h>
#endif

#endif // defined(BACKWARD_SYSTEM_LINUX)

#if defined(BACKWARD_SYSTEM_DARWIN)
// On Darwin, backtrace can back-trace or "walk" the stack using the following
// libraries:
//
// #define BACKWARD_HAS_UNWIND 1
//  - unwind comes from libgcc, but I saw an equivalent inside clang itself.
//  - with unwind, the stacktrace is as accurate as it can possibly be, since
//  this is used by the C++ runtine in gcc/clang for stack unwinding on
//  exception.
//  - normally libgcc is already linked to your program by default.
//
// #define BACKWARD_HAS_LIBUNWIND 1
//  - libunwind comes from clang, which implements an API compatible version.
//  - libunwind provides, in some cases, a more accurate stacktrace as it knows
//  to decode signal handler frames and lets us edit the context registers when
//  unwinding, allowing stack traces over bad function references.
//
// #define BACKWARD_HAS_BACKTRACE == 1
//  - backtrace is available by default, though it does not produce as much
//  information as another library might.
//
// The default is:
// #define BACKWARD_HAS_UNWIND == 1
//
// Note that only one of the define should be set to 1 at a time.
//
#if BACKWARD_HAS_UNWIND == 1
#elif BACKWARD_HAS_BACKTRACE == 1
#elif BACKWARD_HAS_LIBUNWIND == 1
#else
#undef BACKWARD_HAS_UNWIND
#define BACKWARD_HAS_UNWIND 1
#undef BACKWARD_HAS_BACKTRACE
#define BACKWARD_HAS_BACKTRACE 0
#undef BACKWARD_HAS_LIBUNWIND
#define BACKWARD_HAS_LIBUNWIND 0
#endif

// On Darwin, backward can extract detailed information about a stack trace
// using one of the following libraries:
//
// #define BACKWARD_HAS_BACKTRACE_SYMBOL 1
//  - backtrace provides minimal details for a stack trace:
//    - object filename
//    - function name
//
// The default is:
// #define BACKWARD_HAS_BACKTRACE_SYMBOL == 1
//
#if BACKWARD_HAS_BACKTRACE_SYMBOL == 1
#else
#undef BACKWARD_HAS_BACKTRACE_SYMBOL
#define BACKWARD_HAS_BACKTRACE_SYMBOL 1
#endif

#include <cxxabi.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#if (BACKWARD_HAS_BACKTRACE == 1) || (BACKWARD_HAS_BACKTRACE_SYMBOL == 1)
#include <execinfo.h>
#endif
#endif // defined(BACKWARD_SYSTEM_DARWIN)

#if defined(BACKWARD_SYSTEM_WINDOWS)

#include <condition_variable>
#include <mutex>
#include <thread>

#include <basetsd.h>
typedef SSIZE_T ssize_t;

#define NOMINMAX
#include <windows.h>
#include <winnt.h>

#include <psapi.h>
#include <signal.h>

#ifndef __clang__
#undef NOINLINE
#define NOINLINE __declspec(noinline)
#endif

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dbghelp.lib")

// Comment / packing is from stackoverflow:
// https://stackoverflow.com/questions/6205981/windows-c-stack-trace-from-a-running-app/28276227#28276227
// Some versions of imagehlp.dll lack the proper packing directives themselves
// so we need to do it.
#pragma pack(push, before_imagehlp, 8)
#include <imagehlp.h>
#pragma pack(pop, before_imagehlp)

// TODO maybe these should be undefined somewhere else?
#undef BACKWARD_HAS_UNWIND
#undef BACKWARD_HAS_BACKTRACE
#if BACKWARD_HAS_PDB_SYMBOL == 1
#else
#undef BACKWARD_HAS_PDB_SYMBOL
#define BACKWARD_HAS_PDB_SYMBOL 1
#endif

#endif

#if BACKWARD_HAS_UNWIND == 1

#include <unwind.h>
// while gcc's unwind.h defines something like that:
//  extern _Unwind_Ptr _Unwind_GetIP (struct _Unwind_Context *);
//  extern _Unwind_Ptr _Unwind_GetIPInfo (struct _Unwind_Context *, int *);
//
// clang's unwind.h defines something like this:
//  uintptr_t _Unwind_GetIP(struct _Unwind_Context* __context);
//
// Even if the _Unwind_GetIPInfo can be linked to, it is not declared, worse we
// cannot just redeclare it because clang's unwind.h doesn't define _Unwind_Ptr
// anyway.
//
// Luckily we can play on the fact that the guard macros have a different name:
#ifdef __CLANG_UNWIND_H
// In fact, this function still comes from libgcc (on my different linux boxes,
// clang links against libgcc).
#include <inttypes.h>
extern "C" uintptr_t _Unwind_GetIPInfo(_Unwind_Context *, int *);
#endif

#endif // BACKWARD_HAS_UNWIND == 1

#if BACKWARD_HAS_LIBUNWIND == 1
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif // BACKWARD_HAS_LIBUNWIND == 1


namespace backward {
namespace system_tag {
struct linux_tag; // seems that I cannot call that "linux" because the name
// is already defined... so I am adding _tag everywhere.
struct darwin_tag;
struct windows_tag;
struct unknown_tag;

#if defined(BACKWARD_SYSTEM_LINUX)
typedef linux_tag current_tag;
#elif defined(BACKWARD_SYSTEM_DARWIN)
typedef darwin_tag current_tag;
#elif defined(BACKWARD_SYSTEM_WINDOWS)
typedef windows_tag current_tag;
#elif defined(BACKWARD_SYSTEM_UNKNOWN)
typedef unknown_tag current_tag;
#else
#error "May I please get my system defines?"
#endif
} // namespace system_tag

namespace trace_resolver_tag {
#if defined(BACKWARD_SYSTEM_LINUX)
struct libdw;
struct libbfd;
struct libdwarf;
struct backtrace_symbol;

#if BACKWARD_HAS_DW == 1
typedef libdw current;
#elif BACKWARD_HAS_BFD == 1
typedef libbfd current;
#elif BACKWARD_HAS_DWARF == 1
typedef libdwarf current;
#elif BACKWARD_HAS_BACKTRACE_SYMBOL == 1
typedef backtrace_symbol current;
#else
#error "You shall not pass, until you know what you want."
#endif
#elif defined(BACKWARD_SYSTEM_DARWIN)
struct backtrace_symbol;

#if BACKWARD_HAS_BACKTRACE_SYMBOL == 1
typedef backtrace_symbol current;
#else
#error "You shall not pass, until you know what you want."
#endif
#elif defined(BACKWARD_SYSTEM_WINDOWS)
struct pdb_symbol;
#if BACKWARD_HAS_PDB_SYMBOL == 1
typedef pdb_symbol current;
#else
#error "You shall not pass, until you know what you want."
#endif
#endif
} // namespace trace_resolver_tag
} // namespace backward

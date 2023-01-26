/*
 * backward.hpp
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

#include <backward/common.hpp>
#include <backward/trace.hpp>

namespace backward {

using vil::span;

#if BACKWARD_HAS_UNWIND == 1

namespace details {

template <typename F> class Unwinder {
public:
  size_t operator()(F &f, size_t depth) {
    _f = &f;
    _index = -1;
    _depth = depth;
    _Unwind_Backtrace(&this->backtrace_trampoline, this);
    return static_cast<size_t>(_index);
  }

private:
  F *_f;
  ssize_t _index;
  size_t _depth;

  static _Unwind_Reason_Code backtrace_trampoline(_Unwind_Context *ctx,
                                                  void *self) {
    return (static_cast<Unwinder *>(self))->backtrace(ctx);
  }

  _Unwind_Reason_Code backtrace(_Unwind_Context *ctx) {
    if (_index >= 0 && static_cast<size_t>(_index) >= _depth)
      return _URC_END_OF_STACK;

    int ip_before_instruction = 0;
    uintptr_t ip = _Unwind_GetIPInfo(ctx, &ip_before_instruction);

    if (!ip_before_instruction) {
      // calculating 0-1 for unsigned, looks like a possible bug to sanitiziers,
      // so let's do it explicitly:
      if (ip == 0) {
        ip = std::numeric_limits<uintptr_t>::max(); // set it to 0xffff... (as
                                                    // from casting 0-1)
      } else {
        ip -= 1; // else just normally decrement it (no overflow/underflow will
                 // happen)
      }
    }

    if (_index >= 0) { // ignore first frame.
      (*_f)(static_cast<size_t>(_index), reinterpret_cast<void *>(ip));
    }
    _index += 1;
    return _URC_NO_REASON;
  }
};

template <typename F> size_t unwind(F f, size_t depth) {
  Unwinder<F> unwinder;
  return unwinder(f, depth);
}

} // namespace details

void do_load(span<void*>& out) {
  auto cb = [&](size_t idx, void* addr) { out[idx] = addr; };
  auto count = details::unwind(cb, out.size());
  out = out.first(count);
}

#elif BACKWARD_HAS_LIBUNWIND == 1

void do_load(span<void*>& out) {
    int result = 0;

    unw_context_t ctx;
    size_t index = 0;

    unw_cursor_t cursor;
    unw_word_t ip = 0;

    while (index < depth && unw_step(&cursor) > 0) {
      result = unw_get_reg(&cursor, UNW_REG_IP, &ip);
      if (result == 0) {
        out[index] = reinterpret_cast<void *>(--ip);
        ++index;
      }
    }

	out = out.first(count);
}

#elif defined(BACKWARD_HAS_BACKTRACE)

void do_load(span<void*>& out) {
	auto count = backtrace(out.data(), out.size());
	// skip the first one
	out = out.subspan(1).first(count);
}

#elif defined(BACKWARD_SYSTEM_WINDOWS)

// TODO PERF: use CaptureStackBackTrace instead
// https://gist.github.com/Melonpi/1197e7d999c1d1192f42e62cdb65a224
// https://blog.aaronballman.com/2011/04/generating-a-stack-crawl/
void do_load(span<void*>& out) {
    CONTEXT localCtx; // used when no context is provided
	CONTEXT* ctx_ = &localCtx;
    RtlCaptureContext(ctx_);
    auto thd_ = GetCurrentThread();
    HANDLE process = GetCurrentProcess();

    STACKFRAME64 s;
    memset(&s, 0, sizeof(STACKFRAME64));

    // TODO: 32 bit context capture
    s.AddrStack.Mode = AddrModeFlat;
    s.AddrFrame.Mode = AddrModeFlat;
    s.AddrPC.Mode = AddrModeFlat;
#ifdef _M_X64
    s.AddrPC.Offset = ctx_->Rip;
    s.AddrStack.Offset = ctx_->Rsp;
    s.AddrFrame.Offset = ctx_->Rbp;
#else
    s.AddrPC.Offset = ctx_->Eip;
    s.AddrStack.Offset = ctx_->Esp;
    s.AddrFrame.Offset = ctx_->Ebp;
#endif

  	DWORD machine_type_ = 0;
#ifdef _M_X64
    machine_type_ = IMAGE_FILE_MACHINE_AMD64;
#else
    machine_type_ = IMAGE_FILE_MACHINE_I386;
#endif

	auto id = 0u;
    for (;;) {
      // NOTE: this only works if PDBs are already loaded!
      SetLastError(0);
      if (!StackWalk64(machine_type_, process, thd_, &s, ctx_, NULL,
                       SymFunctionTableAccess64, SymGetModuleBase64, NULL))
        break;

      if (s.AddrReturn.Offset == 0)
        break;

	  out[id] = reinterpret_cast<void*>(s.AddrPC.Offset);
	  ++id;

      if (id >= out.size())
        break;
    }

	out = out.first(id);
}

#endif

vil::span<void*> load_here(vil::LinAllocator& alloc, size_t depth) {
	auto ret = alloc.alloc<void*>(depth);
	do_load(ret);
	return ret;
}

} // namespace backward

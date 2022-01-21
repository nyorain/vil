#include "../bugged.hpp"
#include <util/export.hpp>
#include <util/profiling.hpp>
#include <dlg/dlg.hpp>

// TODO: ugly, find proper solution. Init functions in profiling.hpp?
#ifdef TRACY_MANUAL_LIFETIME
namespace vil {

extern std::atomic<unsigned> tracyRefCount;

} // namespace vil
#endif // TRACY_MANUAL_LIFETIME

// Exported entry point for executing the tests.
// See unit/main.cpp for the executor
extern "C" VIL_EXPORT int vil_runUnitTests(const char* pattern) {
#ifdef TRACY_MANUAL_LIFETIME
	auto r = vil::tracyRefCount.fetch_add(1u);
	dlg_assert(r == 0u);
	dlg_trace("Starting tracy...");
	tracy::StartupProfiler();
	dlg_trace(">> done");
#endif // TRACY_MANUAL_LIFETIME

	int ret = vil::bugged::Testing::get().run(pattern);

#ifdef TRACY_MANUAL_LIFETIME
	r = vil::tracyRefCount.fetch_sub(1u);
	dlg_assert(r == 1u);
	dlg_trace("Shutting down tracy...");
	tracy::ShutdownProfiler();
	dlg_trace(">> done");
#endif // TRACY_MANUAL_LIFETIME

	return ret;
}

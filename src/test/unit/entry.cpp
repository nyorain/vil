#include "../bugged.hpp"
#include <util/export.hpp>
#include <util/profiling.hpp>

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
	if(vil::tracyRefCount.fetch_add(1u) == 0u) {
		dlg_trace("Starting tracy...");
		tracy::StartupProfiler();
		dlg_trace(">> done");
	}
#endif // TRACY_MANUAL_LIFETIME

	return vil::bugged::Testing::get().run(pattern);
}

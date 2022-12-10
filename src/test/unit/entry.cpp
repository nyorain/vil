#include "../bugged.hpp"
#include <util/export.hpp>
#include <util/profiling.hpp>
#include <util/dlg.hpp>
#include <chrono>
#include <thread>
#include <csignal>

#ifdef VIL_WITH_WIN32
  #include <win32.hpp>
#endif // VIL_WITH_WIN32

static auto dlgWarnErrorCount = 0u;
static auto breakOnError = false;

void dlgHandler(const struct dlg_origin* origin, const char* string, void* data) {
#ifndef DLG_DISABLE
	dlg_default_output(origin, string, data);
	// (void) string;
	// (void) data;

	if(origin->level >= dlg_level_warn) {
		++dlgWarnErrorCount;
	}

	if(origin->level >= dlg_level_error && breakOnError) {
		// break
		// TODO: should be disabled in non-debug modes (but all of dlg probably should be?)
		#ifdef _MSC_VER
			DebugBreak();
		#else
			// std::raise(SIGABRT);
			std::raise(SIGTRAP);
		#endif
	}
#endif // DLG_DISABLE
}

// Exported entry point for executing the tests.
// See unit/main.cpp for the executor
extern "C" VIL_EXPORT int vil_runUnitTests(const char* pattern) {
	dlg_set_handler(dlgHandler, nullptr);

	vil::initTracy();

	// useful to get insights with tracy
	constexpr auto waitForProfiling = false;

	if(waitForProfiling) {
		std::this_thread::sleep_for(std::chrono::seconds(10));
	}

	int ret = vil::bugged::Testing::get().run(pattern);
	ret += dlgWarnErrorCount;

	if(waitForProfiling) {
		std::this_thread::sleep_for(std::chrono::seconds(10));
	}

	vil::shutdownTracy();

	return ret;
}

#include "../bugged.hpp"
#include <util/export.hpp>
#include <util/profiling.hpp>
#include <util/dlg.hpp>
#include <chrono>
#include <thread>

// Exported entry point for executing the tests.
// See unit/main.cpp for the executor
extern "C" VIL_EXPORT int vil_runUnitTests(const char* pattern) {
	// TODO: set dlg handler so we know about asserts

	vil::initTracy();

	// useful to get insights with tracy
	constexpr auto waitForProfiling = false;

	if(waitForProfiling) {
		std::this_thread::sleep_for(std::chrono::seconds(10));
	}

	int ret = vil::bugged::Testing::get().run(pattern);

	if(waitForProfiling) {
		std::this_thread::sleep_for(std::chrono::seconds(10));
	}

	vil::shutdownTracy();

	return ret;
}

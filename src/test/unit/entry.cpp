#include "../bugged.hpp"
#include <util/export.hpp>
#include <util/profiling.hpp>
#include <util/dlg.hpp>

// Exported entry point for executing the tests.
// See unit/main.cpp for the executor
extern "C" VIL_EXPORT int vil_runUnitTests(const char* pattern) {
	// TODO: set dlg handler so we know about asserts

	vil::initTracy();
	int ret = vil::bugged::Testing::get().run(pattern);
	vil::shutdownTracy();

	return ret;
}

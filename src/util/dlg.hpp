#pragma once

#define DLG_FAILED_ASSERTION_TEXT(x) vil_dlgFailedAssertion(x)

[[noreturn]] inline const char* vil_dlgFailedAssertion(const char* msg) {
	throw std::runtime_error(msg);
}

#include <dlg/dlg.hpp>

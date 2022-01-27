#pragma once

#ifdef VIL_THROW_ON_ASSERT

#include <stdexcept>
#include <string>
#define DLG_FAILED_ASSERTION_TEXT(x) vil_dlgFailedAssertion(x)

[[noreturn]] inline const char* vil_dlgFailedAssertion(const char* msg) {
	std::string str = "vil: Failed assertion: ";
	str += msg;
	throw std::runtime_error(str);
}

#endif // VIL_THROW_ON_ASSERT

#include <dlg/dlg.hpp>

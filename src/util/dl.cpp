#include <util/dl.hpp>
#include <util/dlg.hpp>

#ifdef _WIN32 // Windows
	#include <windows.h>
#else // Unix
	#include <dlfcn.h>
#endif

namespace vil {

#ifdef _WIN32 // Windows

void* dlopen(const char* name) {
	auto ret = ::LoadLibraryA(name);

	if(!ret) {
		char buffer[512] = {};
		auto code = ::GetLastError();
		::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, code,
			0, buffer, sizeof(buffer), nullptr);

		dlg_error("Library: failed to open {}: {}", name, buffer);
	}

	return ret;
}

void dlclose(void* handle) {
	::FreeLibrary(static_cast<HMODULE>(handle));
}

void* dlsym(void* handle, const char* name) {
	return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
}

#else // Unix

void* dlopen(const char* name) {
	::dlerror(); // reset any error
	auto ret = ::dlopen(name, RTLD_NOW);

	if(!ret) {
		dlg_error("Failed to open '{}': {}", name, ::dlerror());
	}

	return ret;
}

void dlclose(void* handle) {
	::dlclose(handle);
}

void* dlsym(void* handle, const char* name) {
	return ::dlsym(handle, name);
}

#endif

} // namespace vil

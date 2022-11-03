#pragma once

// NOTE: currently only included when we the standalone application
//   is built. Needed then to open the vulkan lib and load its
//   function pointers.

namespace vil {

struct DynamicLibrary;

void* dlopen(const char* name);
void dlclose(void* handle);
void* dlsym(void*, const char* name);

} // namespace vil

#pragma once

namespace vil {

struct DynamicLibrary;

void* dlopen(const char* name);
void dlclose(void* handle);
void* dlsym(void*, const char* name);

} // namespace vil

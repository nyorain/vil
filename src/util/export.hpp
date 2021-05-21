#if defined(_WIN32) || defined(__CYGWIN__)
	#define VIL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
	#define VIL_EXPORT __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
	#define VIL_EXPORT __attribute__((visibility("default")))
#else
	#error "unsupported platform"
	// could just pray that it works
	#define VIL_EXPORT
#endif


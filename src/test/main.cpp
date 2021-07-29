#if defined(_WIN32) || defined(__CYGWIN__)
	#define VIL_IMPORT __declspec(dllimport)
#else
	#define VIL_IMPORT
#endif

extern "C" VIL_IMPORT int vil_runUnitTests(const char* pattern);

int main(int argc, const char** argv) {
	const char* pattern = nullptr;
	if(argc > 1) {
		pattern = argv[1];
	}

	return vil_runUnitTests(pattern);
}


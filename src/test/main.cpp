#if defined(_WIN32) || defined(__CYGWIN__)
	#define VIL_IMPORT __declspec(dllimport)
#else
	#define VIL_IMPORT
#endif

extern "C" VIL_IMPORT int vil_runUnitTests(const char* pattern);

int main() {
	// TODO: allow passing a valid pattern via command line args
	return vil_runUnitTests(nullptr);
}


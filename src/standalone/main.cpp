#if defined(_WIN32) || defined(__CYGWIN__)
	#define VIL_IMPORT __declspec(dllimport)
#else
	#define VIL_IMPORT
#endif

// entry.cpp
extern "C" VIL_IMPORT int vil_showImageViewer(int argc, const char** argv);

int main(int argc, const char** argv) {
	return vil_showImageViewer(argc, argv);
}


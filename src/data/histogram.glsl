// const uint maxNumBuckets = 256;

struct HistMetadata {
	uvec4 texMin;
	uvec4 texMax;
	uint flags;
	float begin;
	float end;
	uint maxHist;
	// uvec4 hist[maxNumBuckets];
};

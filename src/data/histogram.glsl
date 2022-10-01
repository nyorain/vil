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

// val >  0 => most significant bit of returned uint will be 1
// val <= 0 => most significant bit of returned uint will be 0
uint floatToUintOrdered(float val) {
	uint r = floatBitsToUint(val);
	if((r & (1u << 31)) == 0) {
		// val is positive
		return r ^ 0x80000000u;
	} else {
		// val is negative
		return r ^ 0xFFFFFFFFu;
	}
}

float uintOrderedToFloat(uint val) {
	if((val & (1u << 31)) == 0) {
		// original float must have been negative
		return uintBitsToFloat(val ^ 0xFFFFFFFFu);
	} else {
		// original float must have been positive
		return uintBitsToFloat(val ^ 0x80000000u);
	}
}

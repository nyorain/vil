// constants for Metadata|::copyType
const uint copyVertices = 1u;
const uint resolveIndices = 2u;

const uint indicesPerInvoc = 8u;

struct Metadata {
	// Indirect command for vertex-rate buffers.
	uint dispatchPerVertexX;
	uint dispatchPerVertexY;
	uint dispatchPerVertexZ;
	uint firstVertex;

	// Indirect command for instance-rate buffers
	uint dispatchPerInstanceX;
	uint dispatchPerInstanceY;
	uint dispatchPerInstanceZ;
	uint firstInstance;

	uint firstIndex;
	uint indexCount;
	uint minIndex;
	uint maxIndex;

	uvec2 padding;
	uint indexBufOffset; // divided by indexSize
	uint copyType;
};

uint ceilDivide(uint num, uint denom) {
	return (num + denom - 1) / denom;
}

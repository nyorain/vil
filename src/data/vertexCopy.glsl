const uint copyVertices = 1u;
const uint resolveIndices = 2u;

const uint indicesPerInvoc = 8u;

uint ceilDivide(uint num, uint denom) {
	return (num + denom - 1) / num;
}

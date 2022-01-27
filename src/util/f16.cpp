#include "f16.hpp"
#include "fwd.hpp"

#include <cstring>
#include <cmath>

namespace vil {

f16::f16(float val) {
	u32 uval;
	std::memcpy(&uval, &val, sizeof(val));

	// endianess independent
	u16 sign = (uval >> 31) & 0b1u;
	u8 fexp = (uval >> 23) & 0b11111111u;
	u32 fmantissa = (uval >> 0) & 0b11111111111111111111111u;

	u16 exp, mantissa;
	if(!fexp) { // 0 (or f32 denorm, f16 can't represent that)
		mantissa = 0;
		exp = 0;
	} else if(fexp == 0xFF) { // inf or nan
		mantissa = (fmantissa == 0) ? 0 : 1;
		exp = 31;
	} else {
		int iexp = int(fexp) - 127; // normalized [-127, 127]
		if(iexp < -24) { // can't represent as f16 in any way => 0
			mantissa = 0;
			exp = 0;
		} else if(iexp < -14) { // can represent as denorm
			exp = 0;
			mantissa = std::exp2(24 + iexp) + (fmantissa >> (-iexp - 1));
		} else if(iexp <= 15) { // normal f16
			exp = iexp + 15;
			mantissa = fmantissa >> 13;
		} else { // can't represent that high exponent; inifinity
			exp = 31;
			mantissa = 0;
		}
	}

	*this = f16(sign, exp, mantissa);
}

f16::f16(u16 sign, u16 exp, u16 mantissa) {
	bits_ =
		((sign & 0b1u) << (bitsExponent + bitsMantissa)) |
		((exp & 0b11111u) << bitsMantissa) |
		(mantissa & 0b1111111111u);
}

f16::operator float() const {
	u32 sign = this->sign();
	auto mantissa = this->mantissa();
	auto exp = this->exp();

	if(!exp && !mantissa) { // zero
		return sign ? -0.f : 0.f;
	} else if(!exp && mantissa) { // denorm, not a denorm as float though
		// easier here to directly return the floating point value
		constexpr auto denorm = (1.0f / 16384.0f);
		float fsign = sign ? -1.f : 1.f;
		return fsign * (mantissa / 1024.f) * denorm;
	}

	u32 fmantissa;
	u32 fexp;
	if(exp < 31) { // normal number
		fexp = exp + 112; // (2^7 - 1) - (2^5 - 1) = 112
		fmantissa = mantissa << 13; // 23 - 10 = 13
	} else { // c.exp == 31: infinity or nan
		fexp = 0xFFu;
		fmantissa = (mantissa == 0) ? 0 : 1;
	}

	float ret;
	u32 ufloat = (sign << 31) | (fexp << 23) | (fmantissa);
	std::memcpy(&ret, &ufloat, sizeof(ret));
	return ret;
}

unsigned f16::sign() const {
	return bits_ >> (bitsExponent + bitsMantissa);
}

unsigned f16::exp() const {
	return (bits_ >> bitsMantissa) & 0b11111u;
}

unsigned f16::mantissa() const {
	return bits_ & 0b1111111111u;
}

bool isnan(f16 val) {
	return val.exp() == 31 && (val.mantissa() != 0);
}

bool isinf(f16 val) {
	return val.exp() == 31 && (val.mantissa() == 0);
}

bool operator==(f16 a, f16 b) {
	// first check: -0 and 0 are equal
	// when either number is nan, comparison always false
	u16 sa = (a.bits() << 1);
	u16 sb = (b.bits() << 1);
	return (!sa && !sb) || (!isnan(a) && !isnan(b) && a.bits() == b.bits());
}

// The direct comparison by just using a i16 works since the bits in
// the floating point value have decreasing significance (even over
// component boundaries, i.e. sign has the most signifance)
// Assumes two's complement int representation (in use by all compilers
// and required of implementations for C++20 so face to use)
static_assert(i32(u32(0xFFFFFFFFu)) == i16(-1),
	"Compiler doesn't use two complement, assumed by comparison operators");
bool operator<=(f16 a, f16 b) {
	return !isnan(a) && !isnan(b) && (i16(a.bits()) <= i16(b.bits()));
}

bool operator<(f16 a, f16 b) {
	return !isnan(a) && !isnan(b) && (i16(a.bits()) < i16(b.bits()));
}

} // namespace tkn

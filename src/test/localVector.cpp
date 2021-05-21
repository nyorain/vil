#include "bugged.hpp"
#include <threadContext.hpp>

using namespace vil;

TEST(localVector_basic) {
	LocalVector<int> a(7);

	for(auto i = 0; i < 7; ++i) {
		a[i] = i;
		EXPECT(a[i], i);
	}

	LocalVector<int> b(3);
	for(auto i = 0; i < 3; ++i) {
		b[i] = 100 + i;
		EXPECT(b[i], 100 + i);
	}
}


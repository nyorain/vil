#include "bugged.hpp"
#include <pv.hpp>
#include <list>
#include <vector>

TEST(basic) {
	fuen::PageVector<int> vec;
	vec.push_back(3);
	vec.emplace_back(4);
	vec.emplace_back(5);

	EXPECT(vec.size(), 3u);
	EXPECT(vec.front(), 3);
	EXPECT(vec.back(), 5);

	EXPECT(vec[0], 3);
	EXPECT(vec[1], 4);
	EXPECT(vec[2], 5);

	auto it = vec.begin();
	EXPECT(it != vec.end(), true);
	EXPECT(*it, 3);

	++it;
	EXPECT(it != vec.end(), true);
	EXPECT(*it, 4);

	++it;
	EXPECT(it != vec.end(), true);
	EXPECT(*it, 5);

	++it;
	EXPECT(it == vec.end(), true);
}

unsigned gValue = 1u;
TEST(destructor) {
	struct Des {
		unsigned set;
		Des(unsigned x) : set(x) {}
		~Des() {
			gValue = set;
		}
	};

	fuen::PageVector<Des> vec;
	vec.emplace_back(7);
	EXPECT(gValue, 1u);

	vec.clear();
	EXPECT(gValue, 7u);
	EXPECT(vec.size(), 0u);

	vec.clear();
	EXPECT(gValue, 7u);
	EXPECT(vec.size(), 0u);

	gValue = 1;
	vec.emplace_back(3);
	vec.emplace_back(4);
	vec.emplace_back(5);
	EXPECT(gValue, 1u);
	EXPECT(vec.size(), 3u);

	auto tmp = vec.pop_back();
	EXPECT(gValue, 5u);
	EXPECT(vec.size(), 2u);
	vec.release();
	EXPECT(gValue, 4u);
	EXPECT(vec.size(), 0u);
	EXPECT(vec.capacity(), 0u);
}

TEST(nested) {
	struct Complex {
		std::vector<int> vals;
		std::vector<Complex*> pointers;
		std::list<std::vector<int>> uff;
	};

	struct Nest {
		unsigned value;
		fuen::PageVector<Complex> vec;
	};

	fuen::PageVector<Nest> nest;

	auto& a = nest.emplace_back();
	a.value = 1;
	auto& a1 = a.vec.emplace_back();
	a1.vals = {-1, -2, -3};
	a1.pointers = {&a1, &a1, nullptr, &a1};
	a1.uff = {{0, 1, 2}, {-1, -3, -5}, {6, 7, 8}};

	auto& b = nest.emplace_back();
	b.value = 2;
	auto& b1 = a.vec.emplace_back();
	b1.vals = {-3, -4, -5};
	b1.pointers = {nullptr, &a1, &b1};
	b1.uff = {{0, 1, 8}, {-1, -3, -5}, {6, 7, 1}};

	auto& c = nest.emplace_back();
	c.value = 3;

	auto& d = nest.emplace_back();
	d.value = 4;

	nest.release();
	EXPECT(nest.size(), 0u);
	EXPECT(nest.capacity(), 0u);

	auto& e = nest.emplace_back();
	e.value = 5;
	auto& e1 = e.vec.emplace_back();
	e1.vals = {2, 5, 2};
	e1.pointers = {&e1};
	e1.uff = {{3, 54, 1}};

	auto& f = nest.emplace_back();
	f.value = 6;

	nest.clear();
	EXPECT(nest.size(), 0u);
}

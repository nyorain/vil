#include "bugged.hpp"
#include <device.hpp>
#include <buffer.hpp>
#include <util/span.hpp>

using namespace vil;

TEST(cmp) {
	auto cmp = Device::BufferAddressCmp {};

	Buffer a {};
	a.deviceAddress = VkDeviceAddress(100);
	a.ci.size = 10;

	auto t1 = VkDeviceAddress(100);
	EXPECT(cmp(&a, t1), false);
	EXPECT(cmp(t1, &a), false);

	auto t2 = VkDeviceAddress(99);
	EXPECT(cmp(&a, t2), false);
	EXPECT(cmp(t2, &a), true);

	auto t3 = VkDeviceAddress(110);
	EXPECT(cmp(&a, t3), true);
	EXPECT(cmp(t3, &a), false);
}

TEST(set) {
	decltype(Device::bufferAddresses) set;

	Buffer a {};
	a.deviceAddress = VkDeviceAddress(100);
	a.ci.size = 10;
	set.insert(&a);

	Buffer b {};
	b.deviceAddress = VkDeviceAddress(200);
	b.ci.size = 100;
	set.insert(&b);

	auto it = set.find(105);
	EXPECT(it != set.end(), true);
	EXPECT(*it, &a);

	it = set.find(110);
	EXPECT(it, set.end());

	it = set.find(0);
	EXPECT(it, set.end());

	it = set.find(500);
	EXPECT(it, set.end());

	it = set.find(99);
	EXPECT(it, set.end());

	it = set.find(200);
	EXPECT(it != set.end(), true);
	EXPECT(*it, &b);

	it = set.find(299);
	EXPECT(it != set.end(), true);
	EXPECT(*it, &b);
}


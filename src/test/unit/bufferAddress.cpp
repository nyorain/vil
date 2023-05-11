#include "../bugged.hpp"
#include <device.hpp>
#include <buffer.hpp>
#include <nytl/span.hpp>

namespace vil {

// buffer.cpp. Internal since we don't want to pull device.hpp in buffer.hpp
Buffer* bufferAtInternal(const decltype(Device::bufferAddresses)& bufferAddresses,
		VkDeviceAddress address);
} // namespace

using namespace vil;

TEST(unit_cmp) {
	auto cmp = Device::BufferAddressCmp {};

	Buffer a;
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

TEST(unit_set) {
	decltype(Device::bufferAddresses) set;

	Buffer a;
	a.deviceAddress = VkDeviceAddress(100);
	a.ci.size = 10;
	set.insert(&a);

	Buffer b;
	b.deviceAddress = VkDeviceAddress(200);
	b.ci.size = 100;
	set.insert(&b);

	auto buf = bufferAtInternal(set, 105);
	EXPECT(buf != nullptr, true);
	EXPECT(buf, &a);

	buf = bufferAtInternal(set, 110);
	EXPECT(buf, nullptr);

	buf = bufferAtInternal(set, 0);
	EXPECT(buf, nullptr);

	buf = bufferAtInternal(set, 500);
	EXPECT(buf, nullptr);

	buf = bufferAtInternal(set, 99);
	EXPECT(buf, nullptr);

	buf = bufferAtInternal(set, 200);
	EXPECT(buf != nullptr, true);
	EXPECT(buf, &b);

	buf = bufferAtInternal(set, 299);
	EXPECT(buf != nullptr, true);
	EXPECT(buf, &b);
}

// problematic case that exposed an issue with the old handling
TEST(unit_alias) {
	decltype(Device::bufferAddresses) set;

	Buffer a;
	a.deviceAddress = VkDeviceAddress(1);
	a.ci.size = 80;
	set.insert(&a);

	Buffer b;
	b.deviceAddress = VkDeviceAddress(200);
	b.ci.size = 80;
	set.insert(&b);

	Buffer c;
	c.deviceAddress = VkDeviceAddress(300);
	c.ci.size = 80;
	set.insert(&c);

	Buffer d;
	d.deviceAddress = VkDeviceAddress(400);
	d.ci.size = 80;
	set.insert(&d);

	Buffer e;
	e.deviceAddress = VkDeviceAddress(500);
	e.ci.size = 80;
	set.insert(&e);

	Buffer f;
	f.deviceAddress = VkDeviceAddress(110);
	f.ci.size = 1000;
	set.insert(&f);

	auto buf = bufferAtInternal(set, VkDeviceAddress(490));
	EXPECT(buf, &f);

	buf = bufferAtInternal(set, VkDeviceAddress(105));
	EXPECT(buf, nullptr);
}


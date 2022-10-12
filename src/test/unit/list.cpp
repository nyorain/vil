#include "../bugged.hpp"
#include <util/list.hpp>
#include <threadContext.hpp>

// TODO: add actual tests instead of just this dummy playground

using namespace vil;

// wip: tests
struct Test {
	std::string str;

	struct AccessorA {
		Test& operator()(ListLink<Test, AccessorA>& link) {
			constexpr auto offset = offsetof(Test, a);
			auto ptr = reinterpret_cast<std::byte*>(&link) - offset;
			return *std::launder(reinterpret_cast<Test*>(ptr));

		}
	};

	ListLink<Test, AccessorA> a;

	struct AccessorB {
		Test& operator()(ListLink<Test, AccessorB>& link) {
			constexpr auto offset = offsetof(Test, b);
			auto ptr = reinterpret_cast<std::byte*>(&link) - offset;
			return *std::launder(reinterpret_cast<Test*>(ptr));

		}
	};

	ListLink<Test, AccessorB> b;
};

TEST(unit_list_grid) {
	struct Test2 {
		std::string str;
		INTRUSIVE_LIST_LINK(Test2, a) {};
		INTRUSIVE_LIST_LINK(Test2, b) {};
	};

	decltype(Test2::a) anchor1(anchor);
	decltype(Test2::b) anchor2(anchor);

	Test2 x1{"x1"};
	Test2 x2{"x2"};
	Test2 x3{"x3"};

	x1.a.insertBefore(anchor1);
	x2.a.insertBefore(anchor1);
	x3.a.insertBefore(anchor1);

	// x1 x2 x3
	for(auto it = anchor1.next_; it != &anchor1; it = it->next_) {
		auto& elem = it->get();
		dlg_trace(elem.str);
	};

	x1.b.insertAfter(anchor2);
	x3.b.insertAfter(anchor2);

	// x3 x1
	for(auto it = anchor2.next_; it != &anchor2; it = it->next_) {
		auto& elem = it->get();
		dlg_trace(elem.str);
	};
}

struct Test3 : IntrusiveListNode<Test3> {
};

TEST(unit_list_derived) {
	IntrusiveListNode<Test3> anchor3(anchor);
	Test3 a;
	Test3 b;
	a.insertAfter(anchor3);
	b.insertAfter(anchor3);
	a.unlink();
	b.unlink();
}

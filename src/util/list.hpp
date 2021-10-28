#pragma once

namespace vil {

struct DefaultC {
	template<typename T>
	auto& operator()(T& obj) const {
		return obj.link;
	}
};

template<typename T, typename C = DefaultC>
struct ListLink {
	T* next {};
	T* prev {};

	static ListLink<T, C>& get(T& obj) {
		return C{}(obj);
	}

	static const ListLink<T, C>& get(const T& obj) {
		return C{}(obj);
	}
};

template<typename T, typename C>
void unlink(ListLink<T, C>& link) {
	using L = ListLink<T, C>;
	if(link.next) L::get(*link.next).prev = link.prev;
	if(link.prev) L::get(*link.prev).next = link.next;
}

template<typename T, typename C>
void link(T& list, ListLink<T, C>& listLink, T& item) {
	using L = ListLink<T, C>;
	if(listLink.next) L::get(*listLink.next).prev = &item;
	L::get(item).next = listLink.next;
	L::get(item).prev = &list;
	listLink.next = &item;
}

template<typename T, typename C = DefaultC>
void link(T& list, T& item) {
	link(list, C{}(list), item);
}

/*
void foo() {
	struct Test {
		ListLink<Test> link;
	};

	Test a;
	Test b;

	link(a, b);
	unlink(b.link);
}
*/

} // namespace vil

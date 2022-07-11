#pragma once

// Easy interface for building intrusive doubly linked list.
// Inspired by the linux kernel implementation, but with added typesafety.

#include <util/dlg.hpp>
#include <type_traits>

namespace vil {

// Returns the ListLink
struct ListMemberLink { // embedding ListLink as specific member
	template<typename T>
	auto& operator()(T& obj) const {
		return obj.link;
	}
};

struct ListSelfLink { // e.g. via deriving from ListLink
	template<typename T>
	auto& operator()(T& obj) const {
		return obj;
	}
};

template<typename T, typename Link = ListSelfLink>
struct ListLink {
	T* next {};
	T* prev {};
};

// unlink
template<typename L, typename Link>
void unlinkLink(L& link) {
	dlg_assertm(link.next && link.prev,
		"Unlinking list node that wasn't properly linked");
	dlg_assertm(link.next != &link,
		"Unlinking list anchor");

	Link{}(*link.next).prev = link.prev;
	Link{}(*link.prev).next = link.next;
	link.next = nullptr;
	link.prev = nullptr;
}

template<typename T, typename Link = ListSelfLink>
void unlink(T& bLink) {
	unlinkLink<T, Link>(Link{}(bLink));
}

template<typename T, typename Link>
void unlink(ListLink<T, Link>& link) {
	unlinkLink<T, Link>(link);
}

// insertAfter
template<typename L, typename Link>
void insertAfterLink(L& anchor, L& toInsert) {
	dlg_assertm(anchor.next && anchor.prev, "Invalid anchor");
	dlg_assertm(!toInsert.next && !toInsert.prev, "Already linked");

	toInsert.next = anchor.next;
	toInsert.prev = &anchor;
	Link{}(*anchor.next).prev = &toInsert;
	anchor.next = &toInsert;
}

template<typename T, typename Link = ListSelfLink>
void insertAfter(T& bAnchor, T& bToInsert) {
	insertAfterLink<T, Link>(Link{}(bAnchor), Link{}(bToInsert));
}

template<typename T, typename Link>
void insertAfter(ListLink<T, Link>& bAnchor, ListLink<T, Link>& bToInsert) {
	insertAfterLink<T, Link>(bAnchor, bToInsert);
}

// insertBefore
template<typename L, typename Link>
void insertBeforeLink(L& anchor, L& toInsert) {
	dlg_assertm(anchor.next && anchor.prev, "Invalid anchor");
	dlg_assertm(!toInsert.next && !toInsert.prev, "Already linked");

	toInsert.next = &anchor;
	toInsert.prev = anchor.prev;
	Link{}(*anchor.prev).next = &toInsert;
	anchor.prev = &toInsert;
}

template<typename T, typename Link = ListSelfLink>
void insertBefore(T& bAnchor, T& bToInsert) {
	insertBeforeLink<T, Link>(Link{}(bAnchor), Link{}(bToInsert));
}

template<typename T, typename Link>
void insertBefore(ListLink<T, Link>& bAnchor, ListLink<T, Link>& bToInsert) {
	insertBeforeLink<T, Link>(bAnchor, bToInsert);
}

// initAnchor
template<typename T>
void initListAnchor(T& anchor) {
	anchor.next = &anchor;
	anchor.prev = &anchor;
}

} // namespace vil

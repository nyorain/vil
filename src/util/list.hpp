#pragma once

#include <util/dlg.hpp>
#include <memory>
#include <cstdlib>
#include <new>

namespace vil {

template<typename T, typename Accessor> struct ListLink;

template<typename T, unsigned offset>
struct OffsetAccessor {
	T& operator()(ListLink<T, OffsetAccessor<T, offset>>& link) {
		return *std::launder<T*>(
			reinterpret_cast<std::byte*>(&link) - offset);
	}

	const T& operator()(const ListLink<T, OffsetAccessor<T, offset>>& link) {
		return *std::launder<const T*>(
			reinterpret_cast<const std::byte*>(&link) - offset);
	}
};

template<typename T>
struct DerivedAccessor {
	T& operator()(ListLink<T, DerivedAccessor>& link) {
		return static_cast<T&>(link);
	}

	const T& operator()(const ListLink<T, DerivedAccessor>& link) {
		return static_cast<const T&>(link);
	}
};

constexpr struct AnchorTag {} anchor;

// T: type of elements in the linked list
// Accessor: given a link, returns the associated element
template<typename T, typename Accessor>
struct ListLink {
	ListLink* next_ {};
	ListLink* prev_ {};

	ListLink() = default;
	ListLink(AnchorTag) : next_(this), prev_(this) {}

	T& get() { return Accessor{}(*this); }
	const T& get() const { return Accessor{}(*this); }

	void unlink() {
		dlg_assertm(next_ && prev_,
			"Unlinking list node that wasn't properly linked");
		dlg_assertm(next_ != this,
			"Unlinking last list element (anchor)");

		next_->prev_ = prev_;
		prev_->next_ = next_;
		next_ = nullptr;
		prev_ = nullptr;
	}

	void insertAfter(ListLink& prev) {
		dlg_assertm(prev.next_ && prev.prev_, "Invalid anchor");
		dlg_assertm(!next_ && !prev_, "Already linked");

		next_ = prev.next_;
		prev_ = &prev;
		next_->prev_ = this;
		prev_->next_ = this;
	}

	void insertBefore(ListLink& next) {
		dlg_assertm(next.next_ && next.prev_, "Invalid anchor");
		dlg_assertm(!next_ && !prev_, "Already linked");

		next_ = &next;
		prev_ = next.prev_;
		next_->prev_ = this;
		prev_->next_ = this;
	}

	// util
	// TODO: only works for anchors, they should be separate type
	template<bool IsConst>
	struct Iterator {
		std::conditional_t<IsConst, const ListLink*, ListLink*> link;

		auto operator*() const { return link->get(); }
		auto operator->() const { return &link->get(); }
		Iterator& operator++() { link = link->next_; return *this; }
		Iterator operator++(int) { return {link->next_}; }
		Iterator& operator--() { link = link->prev_; return *this; }
		Iterator operator--(int) { return {link->prev_}; }
		bool operator==(Iterator rhs) { return rhs.link == link; }
		bool operator!=(Iterator rhs) { return rhs.link != link; }
	};

	template<bool IsConst>
	struct Range {
		std::conditional_t<IsConst, const ListLink*, ListLink*> anchor;
		Iterator<IsConst> begin() const { return Iterator{anchor->next}; }
		Iterator<IsConst> end() const { return Iterator{anchor}; }
	};

	Range<false> range() { return Range<false>{this}; }
	Range<true> range() const { return Range<true>{this}; }
};

template<typename T>
using IntrusiveListNode = ListLink<T, DerivedAccessor<T>>;

#define INTRUSIVE_LIST_LINK(Container, name) \
	struct Accessor##name { \
		Container& operator()(ListLink<Container, Accessor##name>& link) { \
			constexpr auto offset = offsetof(Container, name); \
			auto ptr = reinterpret_cast<std::byte*>(&link) - offset; \
			return *std::launder(reinterpret_cast<Container*>(ptr)); \
		} \
		const Container& operator()(const ListLink<Container, Accessor##name>& link) { \
			constexpr auto offset = offsetof(Container, name); \
			auto ptr = reinterpret_cast<const std::byte*>(&link) - offset; \
			return *std::launder(reinterpret_cast<const Container*>(ptr)); \
		} \
	}; \
	ListLink<Container, Accessor##name> name
} // namespace vil


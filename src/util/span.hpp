// Copyright (c) 2015 Microsoft Corporation. All rights reserved.
//
// This code is licensed under the MIT License (MIT).
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// Taken (and modified) from https://github.com/Microsoft/GSL
// Main changes: stripped lots of stuff, older compiler support, msvc
// workarounds. More lightweight like this but won't give you as much
// debug messages (or support shitty compilers).
// Changed to match C++20 standard more closely (especially initialization).

#pragma once

#ifndef NYTL_INCLUDE_SPAN
#define NYTL_INCLUDE_SPAN

#include <fwd.hpp>
#include <algorithm> // for lexicographical_compare
#include <array>     // for array
#include <cstddef>   // for ptrdiff_t, size_t, nullptr_t
#include <iterator>  // for reverse_iterator, distance, random_access_...
#include <limits>
#include <stdexcept>
#include <type_traits> // for enable_if_t, declval, is_convertible, inte...
#include <utility>
#include <cassert>

namespace nytl {

// implementation details
namespace details {

template <class T> struct is_span_oracle : std::false_type {};

template <class ElementType, std::size_t Extent>
struct is_span_oracle<span<ElementType, Extent>> : std::true_type {};

template <class T>
struct is_span : public is_span_oracle<std::remove_cv_t<T>> {};

template <class T>
struct is_std_array_oracle : std::false_type {};

template <class ElementType, std::size_t Extent>
struct is_std_array_oracle<std::array<ElementType, Extent>> : std::true_type {};

template <class T>
struct is_std_array : public is_std_array_oracle<std::remove_cv_t<T>> {};

template <std::size_t From, std::size_t To>
struct is_allowed_extent_conversion
	: public std::integral_constant<bool,
		From == To || From == dynamic_extent || To == dynamic_extent> {};

template <class From, class To>
struct is_allowed_element_type_conversion
	: public std::integral_constant<bool,
		std::is_convertible<From (*)[], To (*)[]>::value> {};


template <std::size_t Ext>
class extent_type {
public:
	using index_type = std::size_t;
	static_assert(Ext >= 0, "A fixed-size span must be >= 0 in size.");

	constexpr extent_type() noexcept {}

	template <index_type Other>
	constexpr extent_type(extent_type<Other>) {
		static_assert(Other == Ext || Other == dynamic_extent,
			"Mismatch between fixed-size extent and size of initializing data.");
	}

	constexpr extent_type(index_type) { }
	constexpr index_type size() const noexcept { return Ext; }
};

template <>
class extent_type<dynamic_extent> {
public:
	using index_type = std::size_t;

	template <index_type Other>
	explicit constexpr extent_type(extent_type<Other> ext) : size_(ext.size()) {}

	explicit constexpr extent_type(index_type size) : size_(size) {}
	constexpr index_type size() const noexcept { return size_; }

private:
	index_type size_;
};

template <class ElementType, std::size_t Extent, std::size_t Offset, std::size_t Count>
struct calculate_subspan_type {
	using type = span<ElementType, Count != dynamic_extent
	   ? Count
	   : (Extent != dynamic_extent ? Extent - Offset : Extent)>;
};

} // namespace details

template <class ElementType, std::size_t Extent>
class span {
public:
    // constants and types
    using element_type = ElementType;
    using value_type = std::remove_cv_t<ElementType>;
    using index_type = std::size_t;
	using difference_type = std::ptrdiff_t;
    using pointer = element_type*;
    using reference = element_type&;

    using iterator = ElementType*;
    using const_iterator = const ElementType*;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    using size_type = index_type;

    static constexpr index_type extent{Extent};

    template <bool Dependent = false,
        class = std::enable_if_t<(Dependent || Extent == dynamic_extent)>>
    constexpr span() noexcept : storage_(nullptr, details::extent_type<0>()) {
    }

    constexpr span(pointer ptr, index_type count) : storage_(ptr, count) {}

    constexpr span(pointer firstElem, pointer lastElem)
        : storage_(firstElem, std::distance(firstElem, lastElem)) {
    }

    template <std::size_t N>
    constexpr span(element_type (&arr)[N]) noexcept
        : storage_(KnownNotNull{&arr[0]}, details::extent_type<N>()) {}

    template <std::size_t N, class ArrayElementType = std::remove_const_t<element_type>>
    constexpr span(std::array<ArrayElementType, N>& arr) noexcept
        : storage_(&arr[0], details::extent_type<N>()) {}

    template <std::size_t N>
    constexpr span(const std::array<std::remove_const_t<element_type>, N>& arr) noexcept
        : storage_(&arr[0], details::extent_type<N>()) {}

	// Allows direct initialization as in `f({1, 2, 3})`
	// Otherwise `f({{1, 2, 3}})` is needed (at least with clang?)
	// With this enabled, the constructor below should probably be disabled
	// for initializer_list containers
	// Note that c++20 doesn't have this constructor (sadly) so it's not
	// enabled here either
	// constexpr span(const std::initializer_list<ElementType>& l) : span(std::data(l), std::size(l)) {}

	template <class Container,
		class = std::enable_if_t<
			!details::is_span<Container>::value && !details::is_std_array<Container>::value &&
			std::is_convertible_v<
				std::remove_pointer_t<decltype(std::data(std::declval<Container&>()))>(*)[],
	  			element_type(*)[]> &&
		  	std::is_convertible_v<
				decltype(std::size(std::declval<Container&>())),
				std::size_t>>>
    constexpr span(Container& cont) : span(std::data(cont), static_cast<index_type>(std::size(cont))) {}

    template <class Container,
	  class = std::enable_if_t<
			!details::is_span<Container>::value && !details::is_std_array<Container>::value &&
			std::is_convertible_v<
				std::remove_pointer_t<decltype(std::data(std::declval<Container&>()))>(*)[],
	  			element_type(*)[]> &&
		  	std::is_convertible_v<
				decltype(std::size(std::declval<Container&>())),
				std::size_t>>>
	constexpr span(const Container& cont) : span(std::data(cont), static_cast<index_type>(std::size(cont))) {}
    constexpr span(const span& other) noexcept = default;

    template <
        class OtherElementType, std::size_t OtherExtent,
        class = std::enable_if_t<
            details::is_allowed_extent_conversion<OtherExtent, Extent>::value &&
            details::is_allowed_element_type_conversion<OtherElementType, element_type>::value>>
    constexpr span(const span<OtherElementType, OtherExtent>& other)
        : storage_(other.data(), details::extent_type<OtherExtent>(other.size())) {}

    ~span() noexcept = default;
    constexpr span& operator=(const span& other) noexcept = default;

    // [span.sub], span subviews
    template <std::size_t Count>
    constexpr span<element_type, Count> first() const {
		assert(Count <= size());
        return {data(), Count};
    }

    template <std::size_t Count>
    constexpr span<element_type, Count> last() const {
		assert(Count <= size());
        return {data() + (size() - Count), Count};
    }

    template <std::size_t Offset, std::size_t Count = dynamic_extent>
    constexpr auto subspan() const -> typename details::calculate_subspan_type<ElementType, Extent, Offset, Count>::type {
		assert(Offset <= size());
        return {data() + Offset, Count == dynamic_extent ? size() - Offset : Count};
    }

    constexpr span<element_type, dynamic_extent> first(index_type count) const {
		assert(count <= size());
        return {data(), count};
    }

    constexpr span<element_type, dynamic_extent> last(index_type count) const {
		assert(count <= size());
        return make_subspan(size() - count, dynamic_extent, subspan_selector<Extent>{});
    }

    constexpr span<element_type, dynamic_extent> subspan(index_type offset,
            index_type count = dynamic_extent) const {
		assert(offset <= size());
        return make_subspan(offset, count, subspan_selector<Extent>{});
    }

    // [span.obs], span observers
    constexpr index_type size() const noexcept { return storage_.size(); }
    constexpr index_type size_bytes() const noexcept {
        return size() * static_cast<index_type>(sizeof(element_type));
    }
    constexpr bool empty() const noexcept { return size() == 0; }
    constexpr reference operator[](index_type idx) const {
		assert(idx < size());
        return data()[idx];
    }

    constexpr reference operator()(index_type idx) const { return this->operator[](idx); }
    constexpr pointer data() const noexcept { return storage_.data(); }

    // [span.iter], span iterator support
    constexpr iterator begin() const noexcept { return data(); }
    constexpr iterator end() const noexcept { return data() + size(); }

    constexpr const_iterator cbegin() const noexcept { return data(); }
    constexpr const_iterator cend() const noexcept { return data() + size(); }

    constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator{end()}; }
    constexpr reverse_iterator rend() const noexcept { return reverse_iterator{begin()}; }

    constexpr const_reverse_iterator crbegin() const noexcept {
        return const_reverse_iterator{cend()};
    }
    constexpr const_reverse_iterator crend() const noexcept {
        return const_reverse_iterator{cbegin()};
    }

	constexpr reference front() const noexcept { return *begin(); }
	constexpr reference back() const noexcept { return *(end() - 1); }

private:
    // Needed to remove unnecessary null check in subspans
    struct KnownNotNull {
        pointer p;
    };

    // this implementation detail class lets us take advantage of the
    // empty base class optimization to pay for only storage of a single
    // pointer in the case of fixed-size spans
    template <class ExtentType>
    class storage_type : public ExtentType {
    public:
        // KnownNotNull parameter is needed to remove unnecessary null check
        // in subspans and constructors from arrays
        template <class OtherExtentType>
        constexpr storage_type(KnownNotNull data, OtherExtentType ext)
            	: ExtentType(ext), data_(data.p) {
        }

        template <class OtherExtentType>
        constexpr storage_type(pointer data, OtherExtentType ext)
				: ExtentType(ext), data_(data) {
        }

        constexpr pointer data() const noexcept { return data_; }

    private:
        pointer data_;
    };

    storage_type<details::extent_type<Extent>> storage_;

    // The rest is needed to remove unnecessary null check
    // in subspans and constructors from arrays
    constexpr span(KnownNotNull ptr, index_type count) : storage_(ptr, count) {}

    template <std::size_t CallerExtent>
    class subspan_selector { };

    template <std::size_t CallerExtent>
    span<element_type, dynamic_extent> make_subspan(index_type offset, index_type count,
            subspan_selector<CallerExtent>) const {
        const span<element_type, dynamic_extent> tmp(*this);
        return tmp.subspan(offset, count);
    }

    span<element_type, dynamic_extent> make_subspan(index_type offset, index_type count,
            subspan_selector<dynamic_extent>) const {
        if (count == dynamic_extent) { return {KnownNotNull{data() + offset}, size() - offset}; }
        return {KnownNotNull{data() + offset}, count};
    }
};

template <class ElementType, std::size_t FirstExtent, std::size_t SecondExtent>
constexpr bool operator==(span<ElementType, FirstExtent> l, span<ElementType, SecondExtent> r) {
    return std::equal(l.begin(), l.end(), r.begin(), r.end());
}

template <class ElementType, std::size_t Extent>
constexpr bool operator!=(span<ElementType, Extent> l, span<ElementType, Extent> r) {
    return !(l == r);
}

template <class ElementType, std::size_t Extent>
constexpr bool operator<(span<ElementType, Extent> l, span<ElementType, Extent> r) {
    return std::lexicographical_compare(l.begin(), l.end(), r.begin(), r.end());
}

template <class ElementType, std::size_t Extent>
constexpr bool operator<=(span<ElementType, Extent> l, span<ElementType, Extent> r) {
    return !(l > r);
}

template <class ElementType, std::size_t Extent>
constexpr bool operator>(span<ElementType, Extent> l, span<ElementType, Extent> r) {
    return r < l;
}

template <class ElementType, std::size_t Extent>
constexpr bool operator>=(span<ElementType, Extent> l, span<ElementType, Extent> r) {
    return !(l < r);
}

namespace details {

// if we only supported compilers with good constexpr support then
// this pair of classes could collapse down to a constexpr function

// we should use a narrow_cast<> to go to std::size_t, but older compilers may not see it as
// constexpr and so will fail compilation of the template
template <class ElementType, std::size_t Extent>
struct calculate_byte_size : std::integral_constant<std::size_t,
	static_cast<std::size_t>(sizeof(ElementType) * static_cast<std::size_t>(Extent))> {
};

template <class ElementType>
struct calculate_byte_size<ElementType, dynamic_extent>
	: std::integral_constant<std::size_t, dynamic_extent> {
};

} // namespace details

template <class ElementType, std::size_t Extent>
span<const std::byte, details::calculate_byte_size<ElementType, Extent>::value>
as_bytes(span<ElementType, Extent> s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size_bytes()};
}

template <class ElementType, std::size_t Extent,
          class = std::enable_if_t<!std::is_const<ElementType>::value>>
span<std::byte, details::calculate_byte_size<ElementType, Extent>::value>
as_writeable_bytes(span<ElementType, Extent> s) noexcept {
    return {reinterpret_cast<std::byte*>(s.data()), s.size_bytes()};
}

// make_span() - Utility functions for creating spans
template <class ElementType>
constexpr span<ElementType> make_span(ElementType* ptr,
        typename span<ElementType>::index_type count) {
    return span<ElementType>(ptr, count);
}

template <class ElementType>
constexpr span<ElementType> make_span(ElementType* firstElem, ElementType* lastElem) {
    return span<ElementType>(firstElem, lastElem);
}

template <class ElementType, std::size_t N>
constexpr span<ElementType, N> make_span(ElementType (&arr)[N]) noexcept {
    return span<ElementType, N>(arr);
}

template <class Container>
constexpr span<typename Container::value_type> make_span(Container& cont) {
    return span<typename Container::value_type>(cont);
}

template <class Container>
constexpr span<const typename Container::value_type> make_span(const Container& cont) {
    return span<const typename Container::value_type>(cont);
}

template <class Ptr>
constexpr span<typename Ptr::element_type> make_span(Ptr& cont, std::size_t count) {
    return span<typename Ptr::element_type>(cont, count);
}

template <class Ptr>
constexpr span<typename Ptr::element_type> make_span(Ptr& cont) {
    return span<typename Ptr::element_type>(cont);
}

// taken directly from the standard
// http://eel.is/c++draft/span.overview
template<class T, size_t N> span(T (&)[N]) -> span<T, N>;
template<class T, size_t N> span(std::array<T, N>&) -> span<T, N>;
template<class T, size_t N> span(const std::array<T, N>&) -> span<const T, N>;
template<class Container> span(Container&) -> span<typename Container::value_type>;
template<class Container> span(const Container&) -> span<const typename Container::value_type>;

} // namespace nytl

#endif // header guard


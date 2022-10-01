#pragma once

#include <string_view>
#include <string>

namespace vil {

/// A null-terminated not-owned string.
/// Like std::string_view but guaranteed to be null-terminated.
/// Makes it actually useful as string type parameter for components
/// interacting with C.
template<typename Char>
class BasicStringParam : public std::basic_string_view<Char> {
public:
	using string_view = std::basic_string_view<Char>;
	using string = std::basic_string<Char>;

public:
	constexpr BasicStringParam() = default;
	constexpr BasicStringParam(const Char* cstr) : string_view(cstr) {}
	BasicStringParam(const string& str) : string_view(str.c_str()) {}

	const Char* c_str() const { return this->data(); }
};

using StringParam = BasicStringParam<char>;
using StringParam32 = BasicStringParam<char32_t>;
using StringParam16 = BasicStringParam<char16_t>;

} // namespace vil


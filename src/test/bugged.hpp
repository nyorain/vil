// This is free and unencumbered software released into the public domain.
//
// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.
//
// In jurisdictions that recognize copyright laws, the author or authors
// of this software dedicate any and all copyright interest in the
// software to the public domain. We make this dedication for the benefit
// of the public at large and to the detriment of our heirs and
// successors. We intend this dedication to be an overt act of
// relinquishment in perpetuity of all present and future rights to this
// software under copyright law.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// For more information, please refer to <http://unlicense.org>

// Written by nyorain.
// For issues and (appreciated) help, see https://github.com/nyorain/bugged.
// For more information and usage, see example.cpp or README.md
// Extremely lightweight header-only unit testing for C++.
// Use the TEST(name), EXPECT(expression, expected) and ERROR(expression, error) macros.
// Configuration useful for inline testing (define before including the file):
//  - BUGGED_NO_MAIN: don't include the main function that just executes all tests.
//  - BUGGED_NO_IMPL: don't include the implementation.

#pragma once

// Modified for vil.
#define BUGGED_NO_MAIN

#include <string> // std::string
#include <cstring> // std::strrchr
#include <typeinfo> // std::type_traits
#include <vector> // std::vector
#include <iostream> // std::cout
#include <sstream> // std::stringstream
#include <exception> // std::exception

namespace vil::bugged {

// Utility
namespace {
	template<typename... T> constexpr void unused(T&&...) {}
	template<typename... T> using void_t = void;
}

/// Class used for printing objects to the debug output.
/// Provided implementations just return the object when it is printable
/// a string with type and address of the object.
/// The call operations retrieves something from the object that can be printed
/// ot an ostream (the object itself or an alternative descriptive string).
/// \tparam T The type of objects to print.
template<typename T, typename C = void>
struct Printable {
	static std::string call(const T&) {
		static auto txt = std::string("<not printable : ") + typeid(T).name() + std::string(">");
		return txt;
	}
};

/// Default printable specialization for types that can be printed
template<typename T>
struct Printable<T, void_t<decltype(std::declval<std::ostream&>() << std::declval<T>())>> {
	static const T& call(const T& obj) { return obj; }
};

/// Uses the Printable template to retrieve something printable to an ostream
/// from the given object.
template<typename T>
auto printable(const T& obj) -> decltype(Printable<T>::call(obj)) {
	return Printable<T>::call(obj);
}

/// Strips the path from the given filename
inline const char* stripPath(const char* path) {
	auto pos = std::strrchr(path, '/');
	return pos ? pos + 1 : path;
}

/// Static class that holds all test to be run.
class Testing {
public:
	/// Holds all the available info for a failed test.
	struct FailInfo {
		int line;
		const char* file;
	};

	/// Represents a unit to test.
	struct Unit {
		using FuncPtr = void(*)();
		std::string name;
		FuncPtr func;
		const char* file;
		unsigned int line;
	};

	static Testing& get();

public:
	// Config variables
	// the ostream to output to. Must not be set to nullptr. Defaulted to std::cout
	std::ostream* output {&std::cout};

	// The width of the failure separator. Defaulted to 70.
	unsigned int separationWidth {55};

	// The char to use in the failure separator line. Defaulted to '-'
	char failSeparator {'-'};

	// The char to use in the bottom separator line. Defaulted to '='
	char bottomSeparator {'='};

	// The escape sequences to use to style the output.
	// Will all be empty if not on unix.
	struct Escape {
		static const char* testName;
		static const char* checkExpected;
		static const char* checkActual;
		static const char* errorExpected;
		static const char* exception;
		static const char* source;
		static const char* reset;
	};

public:
	/// Called when a check expect fails.
	/// Will increase the current fail count and print a debug message for
	/// the given information.
	/// Should be only called inside of a testing unit.
	template<typename V, typename E>
	void expectFailed(const FailInfo&, const V& value, const E& expected);

	/// Adds the given unit to the list of units to test.
	/// Always returns 0, useful for static calling.
	int add(const Unit&);

	/// Tests all registered units.
	/// Returns the number of units failed.
	unsigned int run(const char* pattern);

	/// Outputs a separation line.
	void separationLine(bool beginning);

protected:
	/// Returns a string for the given number of failed tests.
	std::string failString(unsigned int failCount, const char* type);

	/// Prints the error for an unexpected exception
	void unexpectedException(const std::string& errorString);

	std::vector<Unit> units;
	unsigned int currentFailed;
	unsigned int totalFailed;
	unsigned int unitsFailed;
	Unit* currentUnit;
};

// Utility method used by EXPECT to ensure the given expressions are evaluated
// exactly once
template<typename V, typename E>
inline void checkExpect(const Testing::FailInfo& info, const V& value, const E& expected) {
	if(value != expected) {
		Testing::get().expectFailed(info, value, expected);
	}
}

template<typename V, typename E>
inline void Testing::expectFailed(const FailInfo& info, const V& value, const E& expected) {
	separationLine(true);

	std::cout << "[" << Escape::source << info.file << ":" << info.line
			  << Escape::reset << " | " << Escape::testName << currentUnit->name
	  		  << Escape::reset << "] Check expect failed:\nGot: '"
		 	  << Escape::checkActual << printable(value)
			  << Escape::reset << "' instead of '" << Escape::checkExpected << printable(expected)
			  << Escape::reset << "'\n";

	++currentFailed;
}

} // namespace vil::bugged

/// Declares a new testing unit. After this macro the function body should follow like this:
/// ``` TEST(SampleTest) { EXPECT(1 + 1, 2); } ```
#define TEST(name) \
	static void BUGGED_##name##_U(); \
	namespace { static const auto BUGGED_##name = ::vil::bugged::Testing::get().add( \
		{#name, BUGGED_##name##_U, ::vil::bugged::stripPath(__FILE__), __LINE__}); } \
	static void BUGGED_##name##_U()

/// Expects the two given values to be equal.
#define EXPECT(expr, expected) \
	{ ::vil::bugged::checkExpect({__LINE__, ::vil::bugged::stripPath(__FILE__)}, expr, expected); }

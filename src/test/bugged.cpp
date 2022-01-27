#include "bugged.hpp"
#include <util/dlg.hpp>

namespace vil::bugged {

// unsigned int Testing::separationWidth = 55;
// char Testing::failSeparator = '-';
// char Testing::bottomSeparator = '=';

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
	const char* Testing::Escape::testName = "\033[33m";
	const char* Testing::Escape::checkExpected = "\033[32m";
	const char* Testing::Escape::checkActual = "\033[31m";
	const char* Testing::Escape::exception = "\033[31m";
	const char* Testing::Escape::source = "\033[36m";
	const char* Testing::Escape::reset = "\033[0m";
#else
	const char* Testing::Escape::testName = "";
	const char* Testing::Escape::checkExpected = "";
	const char* Testing::Escape::checkActual = "";
	const char* Testing::Escape::exception = "";
	const char* Testing::Escape::source = "";
	const char* Testing::Escape::reset = "";
#endif


// std::vector<Testing::Unit> Testing::units {};
// unsigned int Testing::currentFailed {};
// unsigned int Testing::totalFailed {};
// unsigned int Testing::unitsFailed {};
// Testing::Unit* Testing::currentUnit {};
// std::ostream* Testing::output = &std::cout;

Testing& Testing::get() {
	static Testing ret;
	return ret;
}

void Testing::separationLine(bool beginning) {
	if(beginning && !totalFailed && !currentFailed && !unitsFailed)
		return;

	for(auto i = 0u; i < separationWidth; ++i)
		std::cout << failSeparator;

	std::cout << "\n";
}

void Testing::unexpectedException(const std::string& errorString) {
	separationLine(true);

	std::cout << "[" << Escape::source << currentUnit->file << ":" << currentUnit->line
			  << Escape::reset << " | " << Escape::testName << currentUnit->name
			  << Escape::reset << "] Unexpected exception:\n"
	  		  << Escape::exception << errorString << Escape::reset << "\n";
}

int Testing::add(const Unit& unit) {
	units.push_back(unit);
	return 0;
}

unsigned int Testing::run(const char* pattern) {
	for(auto unit : units) {
		if(pattern && unit.name.find(pattern) == unit.name.npos) {
			continue;
		}

		*output << "Executing test '" << unit.name << "'\n";
		currentFailed = 0;

		currentUnit = &unit;
		auto thrown = false;

		try {
			unit.func();
		} catch(const std::exception& exception) {
			thrown = true;
			unexpectedException(std::string("std::exception: ") + exception.what());
		} catch(...) {
			thrown = true;
			unexpectedException("<Not a std::exception object>");
		}

		if(thrown || currentFailed)
			++unitsFailed;
		totalFailed += currentFailed;
	}

	if(totalFailed) {
		for(auto i = 0u; i < separationWidth; ++i)
			*output << bottomSeparator;
		*output << "\n";
	}

	*output << failString(unitsFailed, "unit") << ", "
			<< failString(totalFailed, "call") << "\n";
	return unitsFailed;
}

std::string Testing::failString(unsigned int failCount, const char* type) {
	if(failCount == 0) {
		return std::string("All ").append(type).append("s succeeded");
	} else if(failCount == 1) {
		return std::string("1 ").append(type).append(" failed");
	} else {
		return std::to_string(failCount).append(" ").append(type).append("s failed");
	}
}

} // namespace vil::bugged

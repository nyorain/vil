#include "../bugged.hpp"
#include <util/buffmt.hpp>
#include <threadContext.hpp>

using namespace vil;

TEST(unit_bufp_test1) {
	auto str = "struct Test { int a[8][4]; uint4 b; }; Test a;";

	ThreadMemScope memScope;
	auto ret = unwrap(parseType(str, memScope.customUse()));

	EXPECT(ret != nullptr, true);
	EXPECT(ret->type, Type::typeStruct);
	EXPECT(ret->vecsize, 1u);
	EXPECT(ret->members.size(), 1u);
	EXPECT(ret->members[0].name, "a");

	auto& t0 = *ret->members[0].type;
	EXPECT(t0.vecsize, 1u);
	EXPECT(t0.deco.name, "Test");
	EXPECT(t0.members.size(), 2u);

	auto& t00 = *t0.members[0].type;
	EXPECT(t00.vecsize, 1u);
	EXPECT(t00.array.size(), 2u);
	EXPECT(t00.array[0], 8u);
	EXPECT(t00.array[1], 4u);
	EXPECT(t00.deco.name, "int");
	EXPECT(t00.width, 32u);
}

TEST(unit_bufp_invalid1) {
	auto str = "struct Material { vec4 albed };";

	ThreadMemScope memScope;
	auto ret = parseType(str, memScope.customUse());

	EXPECT(ret.error != std::nullopt, true);
	// unwrap(ret); // TODO test error message?
}

TEST(unit_bufp_invalid2) {
	auto str = "uint d[";

	ThreadMemScope memScope;
	auto ret = parseType(str, memScope.customUse());

	EXPECT(ret.error != std::nullopt, true);
	// unwrap(ret); // TODO test error message?
}

#include <command/match.hpp>
#include <command/record.hpp>
#include <command/commands.hpp>
#include <command/alloc.hpp>
#include <command/builder.hpp>
#include <threadContext.hpp>
#include <vk/vulkan.h>
#include "../bugged.hpp"
#include "../approx.hpp"

using namespace vil;

thread_local LinAllocator localMem;
constexpr auto matchType = MatchType::identity;

struct LabelSection {
	BeginDebugUtilsLabelCmd* cmd;
	RecordBuilder& rb;

	LabelSection(LabelSection&&) = delete;
	LabelSection& operator=(LabelSection&&) = delete;

	LabelSection(RecordBuilder& rbx, const char* name) : rb(rbx) {
		cmd = &rb.add<BeginDebugUtilsLabelCmd, SectionType::begin>();
		cmd->name = copyString(*rb.record_, name);
	}

	~LabelSection() {
		rb.add<EndDebugUtilsLabelCmd, SectionType::end>();
	}
};

BeginDebugUtilsLabelCmd& emptyLabelSection(RecordBuilder& rb, const char* name) {
	LabelSection section(rb, name);
	return *section.cmd;
}

TEST(unit_command_match_barrier) {
	BarrierCmd b1;
	b1.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	b1.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	auto b2 = b1;

	EXPECT(eval(match(b1, b2, matchType)), approx(1.f));
	EXPECT(eval(match(b1, b2, matchType)), eval(match(b2, b1, matchType)));

	b1.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	EXPECT(eval(match(b1, b2, matchType)) < 1.f, true);

	WaitEventsCmd waitCmd;
	EXPECT(match(waitCmd, b2, matchType).match, 0.f);
	EXPECT(eval(match(waitCmd, b2, matchType)), 0.f);
}

TEST(unit_match_simple_record) {
	Device dev;
	dev.captureCmdStack.store(false);

	RecordBuilder rb(&dev);
	rb.add<BarrierCmd>();
	rb.add<BarrierCmd>();
	rb.add<BarrierCmd>();
	auto rec3Barriers = rb.record_;

	rb.reset(&dev);
	auto recEmpty = rb.record_;

	ThreadMemScope tms;
	LinAllocScope lms(localMem);
	auto [match1, _1, _2, matches1] = match(tms, lms, matchType,
		*rec3Barriers->commands, *recEmpty->commands);
	auto [match2, _3, _4, matches2] = match(tms, lms, matchType,
		*rec3Barriers->commands, *rec3Barriers->commands);
	dlg_trace("match1: {}/{}", match1.match, match1.total);
	dlg_trace("match2: {}/{}", match2.match, match2.total);

	dlg_assert(eval(match2) > eval(match1));
}

TEST(unit_match_labels) {
	Device dev;
	dev.captureCmdStack.store(false);

	RecordBuilder rb(&dev);
	auto& a1 = emptyLabelSection(rb, "1");
	auto& a2 = emptyLabelSection(rb, "2");
	auto& a3 = emptyLabelSection(rb, "3"); (void) a3;
	auto& a4 = emptyLabelSection(rb, "4");
	auto recA = rb.record_;

	rb.reset(&dev);
	auto& b1 = emptyLabelSection(rb, "1");
	auto& b2 = emptyLabelSection(rb, "2");
	auto& b4 = emptyLabelSection(rb, "4");
	auto recB = rb.record_;

	ThreadMemScope tms;
	LinAllocScope lms(localMem);
	auto [matchRes, _1, _2, matches] = match(tms, lms, matchType, *recA->commands, *recB->commands);
	auto matchVal = eval(matchRes);
	dlg_trace("match val: {}", matchVal);

	// don't care about specifics here. Might need to be changed in future
	// but then investigate why, this range is fairly permissive as is.
	dlg_assert(matchVal > 0.6f);
	dlg_assert(matchVal < 0.9f);

	dlg_assert(matches.size() == 3u);

	// reverse order atm
	dlg_assert(matches[0].a == &a1);
	dlg_assert(matches[0].b == &b1);

	dlg_assert(matches[1].a == &a2);
	dlg_assert(matches[1].b == &b2);

	dlg_assert(matches[2].a == &a4);
	dlg_assert(matches[2].b == &b4);

	// test symmetrical
	{
		LinAllocScope lms(localMem);
		auto [matchRes2, _1, _2, matches2] = match(tms, lms, matchType,
			*recB->commands, *recA->commands);
		auto matchVal2 = eval(matchRes2);
		dlg_trace("match val 2: {}", matchVal2);

		// don't care about specifics here. Might need to be changed in future
		// but then investigate why, this range is fairly permissive as is.
		dlg_assert(matchVal2 > 0.6f);
		dlg_assert(matchVal2 < 0.9f);
		dlg_assert(matchVal2 == approx(matchVal));

		dlg_assert(matches2.size() == 3u);

		// reverse order atm
		dlg_assert(matches2[0].b == &a1);
		dlg_assert(matches2[0].a == &b1);

		dlg_assert(matches2[1].b == &a2);
		dlg_assert(matches2[1].a == &b2);

		dlg_assert(matches2[2].b == &a4);
		dlg_assert(matches2[2].a == &b4);
	}
}


/*
struct CaptureType {
	Type* type {};
	bool converted {};
};

CaptureType buildCaptureType(ShaderPatch& patch, const CaptureProcess& cpt) {
}

struct CaptureProcess {
	u32 typeID;
	u32 loadedID;
	const spc::Meta::Decoration* memberDeco;
};

Type* processCapture(ShaderPatch& patch, const CaptureProcess& cpt) {
	auto [typeID, loadedID, memberDeco] = cpt;
	auto& alloc = patch.alloc;
	auto& compiler = patch.compiler;

	auto stype = &compiler.get_type(typeID);
	if(stype->pointer) {
		dlg_assert(stype->parent_type);
		typeID = stype->parent_type;
		stype = &compiler.get_type(typeID);

		// TODO load
	}

	auto& dst = alloc.construct<Type>();
	dst.deco.typeID = typeID;

	auto* meta = compiler.get_ir().find_meta(typeID);
	if(meta) {
		dst.deco.name = copy(alloc, meta->decoration.alias);
	}

	if(memberDeco) {
		if(memberDeco->decoration_flags.get(spv::DecorationRowMajor)) {
			dst.deco.flags |= Decoration::Bits::rowMajor;
		}
		if(memberDeco->decoration_flags.get(spv::DecorationColMajor)) {
			dst.deco.flags |= Decoration::Bits::colMajor;
		}
		if(memberDeco->decoration_flags.get(spv::DecorationMatrixStride)) {
			dst.deco.matrixStride = memberDeco->matrix_stride;
		}
	}

	// handle array
	if(!stype->array.empty()) {
		if(meta && meta->decoration.decoration_flags.get(spv::DecorationArrayStride)) {
			dst.deco.arrayStride = meta->decoration.array_stride;
		}

		dlg_assert(stype->array.size() == stype->array_size_literal.size());
		dst.array = alloc.alloc<u32>(stype->array.size());

		for(auto d = 0u; d < stype->array.size(); ++d) {
			if(stype->array_size_literal[d] == true) {
				dst.array[d] = stype->array[d];
			} else {
				dst.array[d] = compiler.evaluate_constant_u32(stype->array[d]);
			}
		}

		dst.deco.arrayTypeID = typeID;

		dlg_assert(stype->parent_type);
		typeID = stype->parent_type;
		stype = &compiler.get_type(typeID);
		meta = compiler.get_ir().find_meta(typeID);

		dst.deco.typeID = typeID;
	}

	if(stype->basetype == spc::SPIRType::Struct) {
		// handle struct
		dst.members = alloc.alloc<Type::Member>(stype->member_types.size());
		for(auto i = 0u; i < stype->member_types.size(); ++i) {
			auto memTypeID = stype->member_types[i];

			const spc::Meta::Decoration* deco {};
			if(meta && meta->members.size() > i) {
				deco = &meta->members[i];
			}

			// TODO PERF: remove allocation via dlg format here,
			// use linearAllocator instead if needed
			auto name = dlg::format("?{}", i);
			if(deco && !deco->alias.empty()) {
				// TODO PERF: we copy here with new, terrible
				name = deco->alias;
			}

			auto& mdst = dst.members[i];
			mdst.type = processCapture(patch, memTypeID, alloc, deco);
			mdst.name = copy(alloc, name);
			mdst.offset = deco ? deco->offset : 0u;

			if(!mdst.type) {
				return nullptr;
			}
		}

		dst.type = Type::typeStruct;
		return &dst;
	}

	// handle atom
	auto getBaseType = [](spc::SPIRType::BaseType t) -> std::optional<Type::BaseType> {
		switch(t) {
			case spc::SPIRType::Double:
			case spc::SPIRType::Float:
			case spc::SPIRType::Half:
				return Type::typeFloat;

			case spc::SPIRType::Int:
			case spc::SPIRType::Short:
			case spc::SPIRType::Int64:
			case spc::SPIRType::SByte:
				return Type::typeInt;

			case spc::SPIRType::UInt:
			case spc::SPIRType::UShort:
			case spc::SPIRType::UInt64:
			case spc::SPIRType::UByte:
				return Type::typeUint;

			case spc::SPIRType::Boolean:
				return Type::typeBool;

			default:
				return std::nullopt;
		}
	};

	auto bt = getBaseType(stype->basetype);
	if(!bt) {
		dlg_error("Unsupported shader type: {}", u32(stype->basetype));
		return nullptr;
	}

	dst.type = *bt;
	dst.width = stype->width;
	dst.vecsize = stype->vecsize;
	dst.columns = stype->columns;

	return &dst;
}
*/

/*
ProcessedCapture processCaptureNonArray(ShaderPatch& patch, LinAllocScope& tms,
		Type& type, span<const u32> loadedIDs) {
	u32 copiedTypeID = type.deco.typeID;
	span<const u32> retIDs = loadedIDs;

	if(!type.members.empty()) {
		dlg_assert(type.type == Type::typeStruct);
		auto copied = tms.alloc<u32>(loadedIDs.size());

		span<u32> typeIDs = tms.alloc<u32>(loadedIDs.size());
		span<span<const u32>> memberIDs =
			tms.alloc<span<const u32>>(loadedIDs.size());
		for(auto [i, member] : enumerate(type.members)) {
			span<u32> loadedMembers = tms.alloc<u32>(loadedIDs.size());
			for(auto [j, id] : enumerate(loadedIDs)) {
				loadedMembers[j] = patch.genOp(spv::OpCompositeExtract,
					member.type->array.empty() ? member.type->deco.typeID : member.type->deco.arrayTypeID,
					id, i);
			}

			auto capture = processCapture(patch, tms, *member.type, loadedMembers);
			memberIDs[i] = capture.ids;
			typeIDs[i] = capture.typeID;
		}

		copiedTypeID = ++patch.freeID;
		patch.decl<spv::OpTypeStruct>()
			.push(copiedTypeID)
			.push(typeIDs);

		// TODO offset deco
		// TODO copy other member decos!

		for(auto [i, ids] : enumerate(memberIDs)) {
			copied[i] = patch.genOp(spv::OpCompositeConstruct, copiedTypeID, ids);
		}

		retIDs = copied;
	} else if(type.type == Type::typeBool) {
		type.type = Type::typeUint;
		type.width = 32u;
		type.deco.typeID = patch.typeUint;
		copiedTypeID = patch.typeUint;

		auto copied = tms.alloc<u32>(loadedIDs.size());
		for(auto [i, src] : enumerate(loadedIDs)) {
			copied[i] = patch.genOp(spv::OpSelect, patch.typeUint,
				src, patch.const1, patch.const0);
		}

		retIDs = copied;
	}

	ProcessedCapture ret;
	ret.typeID = copiedTypeID;
	ret.ids = retIDs;
	return ret;
}

ProcessedCapture processCapture(ShaderPatch& patch, LinAllocScope& tms,
		Type& type, span<const u32> loadedIDs) {
	if(type.array.empty()) {
		return processCaptureNonArray(patch, tms, type, loadedIDs);
	}

	auto totalCount = 1u;
	for(auto dimSize : type.array) {
		totalCount *= dimSize;
	}

	span<u32> atomIDs = tms.alloc<u32>(loadedIDs.size() * totalCount);
	u32 typeID = type.deco.arrayTypeID;
	auto* spcType = &patch.compiler.get_type(typeID);

	for(auto [i, id] : enumerate(loadedIDs)) {
		atomIDs[i * totalCount] = id;
	}
	auto lastCount = loadedIDs.size();
	auto stride = totalCount;

	for(auto dimSize : reversed(type.array)) {
		dlg_assert(dimSize <= stride);

		for(auto srcOff = 0u; srcOff < lastCount; ++srcOff) {
			auto srcID = srcOff * stride;
			for(auto dstOff = 0u; dstOff < dimSize; ++dstOff) {
				auto dstID = srcID + dstOff;
				atomIDs[dstID] = patch.genOp(spv::OpCompositeExtract,
					typeID, atomIDs[srcID], dstOff);
			}
		}

		dlg_assert(spcType->parent_type);
		u32 typeID = spcType->parent_type;
		spcType = &patch.compiler.get_type(typeID);
		lastCount *= dimSize;
		stride /= dimSize;
	}

	dlg_assert(stride == 1u);
	dlg_assert(lastCount == totalCount * loadedIDs.size());

	auto baseCapture = processCaptureNonArray(patch, tms, type, atomIDs);
	auto copiedTypeID = baseCapture.typeID;
	std::copy(baseCapture.ids.begin(), baseCapture.ids.end(), atomIDs.begin());

	for(auto dimSize : type.array) {
		auto id = ++patch.freeID;
		patch.decl<spv::OpTypeArray>()
			.push(id)
			.push(copiedTypeID)
			.push(u32(dimSize));

		// TODO stride deco. member?

		copiedTypeID = id;
		dlg_assert(lastCount % dimSize == 0u);
		auto dstCount = lastCount / dimSize;

		for(auto dstOff = 0u; dstOff < dstCount; ++dstOff) {
			auto dstID = ++patch.freeID;
			auto builder = patch.instr(spv::OpCompositeConstruct);
			builder.push(copiedTypeID);
			builder.push(dstID);

			for(auto srcOff = 0u; srcOff < dimSize; ++srcOff) {
				auto srcID = dstOff * dimSize + srcOff;
				builder.push(atomIDs[srcID]);
			}

			atomIDs[dstOff] = dstID;
		}

		lastCount = dstCount;
	}

	dlg_assert(lastCount == loadedIDs.size());

	ProcessedCapture ret;
	ret.typeID = copiedTypeID;
	ret.ids = atomIDs.first(lastCount);
	return ret;
}

void fixDecorateCaptureType(ShaderPatch& patch, Type& type) {
	const auto& ir = patch.compiler.get_ir();
	if(!type.members.empty()) {
		dlg_assert(type.type == Type::typeStruct);

		auto* meta = ir.find_meta(type.deco.typeID);
		dlg_assert(meta && meta->members.size() == type.members.size());
		auto needsOffsetDeco = !meta->members[0].decoration_flags.get(spv::DecorationOffset);
		auto offset = 0u;

		for(auto [i, member] : enumerate(type.members)) {
			fixDecorateCaptureType(patch, *const_cast<Type*>(member.type));

			if(needsOffsetDeco) {
				dlg_assert(!meta->members[0].decoration_flags.get(spv::DecorationOffset));
				offset = vil::alignPOT(offset, align(type, patch.bufLayout));
				member.offset = offset;

				patch.decl<spv::OpMemberDecorate>()
					.push(type.deco.typeID)
					.push(u32(i))
					.push(spv::DecorationOffset)
					.push(offset);

				auto dstSize = size(*member.type, patch.bufLayout);
				offset += dstSize;
			}
		}
	}

	if(!type.array.empty()) {
		dlg_assert(type.deco.arrayTypeID != 0u);
		auto* meta = ir.find_meta(type.deco.arrayTypeID);
		if(!meta || !meta->decoration.decoration_flags.get(spv::DecorationArrayStride)) {
			dlg_assert(type.deco.arrayStride == 0u);

			auto tarray = type.array;
			type.array = {};
			type.deco.arrayStride = align(
				size(type, patch.bufLayout),
				align(type, patch.bufLayout));
			type.array = tarray;

			patch.decl<spv::OpDecorate>()
				.push(type.deco.arrayTypeID)
				.push(spv::DecorationArrayStride)
				.push(type.deco.arrayStride);
		} else {
			dlg_assert(type.deco.arrayStride);
		}
	}

	// TODO: matrixStride
	if(type.columns > 1u) {
		dlg_error("TODO: add matrixstride deco");
	}
}
*/



////
///
#include <fwd.hpp>
#include <commandDesc.hpp>
#include <cb.hpp>
#include <util/dlg.hpp>
#include <vk/vulkan.h>
#include <memory>
#include <vector>

namespace vil {

struct CommandHookSubmissionData {
	std::unique_ptr<CommandHookSubmissionData> next;

	virtual ~CommandHookSubmissionData();
};

struct CommandHookRecordData {
	std::unique_ptr<CommandHookRecordData> next;

	virtual std::unique_ptr<CommandHookSubmissionData> submit() { return nullptr; }
	virtual void finish(Device&) {}
	virtual ~CommandHookRecordData() = default;
};

struct CommandHookImpl {
	std::unique_ptr<CommandHookImpl> next {};

	virtual ~CommandHookImpl() = default;
	virtual std::unique_ptr<CommandHookRecordData> createRecord(Device&, CommandBuffer&) { return nullptr; }

	virtual void beforeHooked(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}
	virtual void afterHooked(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}

	virtual void beforeHookedChildren(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}
	virtual void afterHookedChildren(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}

	virtual void record(Device&, VkCommandBuffer, CommandHookRecordData*,
		Command& toBeRecorded, Command& hooked) {}

	virtual bool allowSubmitSkip() const { return true; }
	virtual bool allowMultiSubmit() const { return true; }
};

///
struct CommandHookSubmission {
	std::unique_ptr<CommandHookSubmissionData> impls;
	CommandHookRecord* record;
};

struct CommandHookRecord {
	CommandHook* hook {};
	CommandRecord* record {};
	u32 hookCounter {};

	// NOTE: could make this linked list of submissions instead
	u32 submissionCounter {};

	// linked list of records
	CommandHookRecord* next {};
	CommandHookRecord* prev {};

	VkCommandBuffer cb {};

	std::unique_ptr<CommandHookRecordData> impls;
};

struct CommandHook {
	std::unique_ptr<CommandHookImpl> impls;

	u32 counter {};
	std::vector<CommandDesc> desc {};
	CommandHookRecord* records {}; // linked list

	~CommandHook();
	VkCommandBuffer hook(CommandBuffer& hooked);
};

VkCommandBuffer CommandHook::hook(CommandBuffer& hooked) {
	// Check if it already has a valid record associated
	auto* record = hooked.lastRecordLocked();
	auto* hcommand = CommandDesc::find(record->commands, this->desc);
	if(!hcommand) {
		dlg_warn("Can't hook cb, can't find hooked command");
		return hooked.handle();
	}
}

//

struct TimeCommandHook : CommandHookImpl {
	u64 lastTime {};

	std::unique_ptr<CommandHookRecordData> createRecord(Device&, CommandBuffer&) override;
	void record(Device&, VkCommandBuffer cb, CommandHookRecordData* data,
		Command& cmd, Command& hooked) override;
};

struct TimeCommandHookRecord : CommandHookRecordData {
	TimeCommandHook* hook {};
	VkQueryPool queryPool {};
};

struct TimeCommandHookSubmission : CommandHookSubmissionData {
	TimeCommandHookRecord* record {};
	virtual ~TimeCommandHookSubmission();
};

std::unique_ptr<CommandHookRecordData> TimeCommandHook::createRecord(Device& dev,
		CommandBuffer&) {

	auto hook = std::make_unique<TimeCommandHookRecord>();
	hook->hook = this;

	VkQueryPoolCreateInfo qci {};
	qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	qci.queryCount = 2u;
	qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &hook->queryPool));
	nameHandle(dev, hook->queryPool, "TimeCommandHook:queryPool");

	dev.dispatch.CmdResetQueryPool(hook->cb, hook->queryPool, 0, 2);

	return hook;
}

void TimeCommandHook::record(Device& dev, VkCommandBuffer cb,
		CommandHookRecordData* recDataBase, Command& cmd, Command& hooked) {
	auto* recData = (TimeCommandHookRecord*) recDataBase;

	if(&cmd == hooked.next) {
		auto stage1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dev.dispatch.CmdWriteTimestamp(this->cb, stage1, this->queryPool, 1);
	}

	if(next) {
		next->record(dev, cb, cmd, hooked);
	} else {
		cmd.record(dev, cb);
	}

	if(&cmd == &hooked) {
		auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dev.dispatch.CmdWriteTimestamp(cb, stage0, recData->queryPool, 0);
	}
}

//

struct IndirectBufferReadHook : CommandHookImpl {
	VkDrawIndirectCommand lastCmd {};
};

struct IndirectBufferReadHookRecord : CommandHookRecordData {
	IndirectBufferReadHook* hook {};
	VkDeviceMemory dstMemory {};
	VkBuffer dstBuffer {};
};

struct IndirectBufferReadHookSubmission : CommandHookSubmissionData {
	IndirectBufferReadHookRecord* record {};
	virtual ~IndirectBufferReadHookSubmission();
};

} // namespace vil

----

size_t totalNumBindings(const DescriptorSetLayout& layout, u32 variableDescriptorCount) {
	if(layout.bindings.empty()) {
		return 0;
	}

	auto& last = layout.bindings.back();
	size_t ret = last.offset;

	if(last.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		ret += variableDescriptorCount;
	} else {
		ret += last.descriptorCount;
	}

	return ret;
}

---

class ThreadMemoryResource : public std::pmr::memory_resource {
	LinAllocScope* memScope_ {};

	void* do_allocate(std::size_t bytes, std::size_t alignment) override {
		return memScope_->allocBytes(bytes, alignment);
	}

	void do_deallocate(void*, std::size_t, std::size_t) override {
		// no-op
	}

	bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
		auto* tmr = dynamic_cast<const ThreadMemoryResource*>(&other);
		if(!tmr) {
			return false;
		}

		return tmr->memScope_ == this->memScope_;
	}
};



---

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::SPIRV()
{
	// From SHADERed
	// Copyright (c) 2018 - 2021 dfranx
	//
	// 	Permission is hereby granted, free of charge, to any person obtaining a copy
	// 	of this software and associated documentation files (the "Software"), to deal
	// 	in the Software without restriction, including without limitation the rights
	// 	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	// 	copies of the Software, and to permit persons to whom the Software is
	// 	furnished to do so, subject to the following conditions:
	//
	// 	The above copyright notice and this permission notice shall be included in all
	// 	copies or substantial portions of the Software.
	//
	// 	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	// 	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	// 	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	// 	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	// 	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	// 	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	// 	SOFTWARE.

	static bool inited = false;
	static LanguageDefinition langDef;
	if (!inited) {
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"", PaletteIndex::String));
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[ =\\t]Op[a-zA-Z]*", PaletteIndex::Keyword));
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("%[_a-zA-Z0-9]*", PaletteIndex::Identifier));
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", PaletteIndex::Number));
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[0-7]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", PaletteIndex::Number));

		langDef.mCommentStart = "/*";
		langDef.mCommentEnd = "*/";
		langDef.mSingleLineComment = ";";

		langDef.mCaseSensitive = true;
		langDef.mAutoIndentation = false;

		langDef.mName = "SPIR-V";

		inited = true;
	}
	return langDef;
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::GLSLFast()
{
	static bool inited = false;
	static LanguageDefinition langDef;
	if (!inited)
	{
		static const char* const keywords[] = {
			"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short",
			"signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary",
			"_Noreturn", "_Static_assert", "_Thread_local"
		};
		for (auto& k : keywords)
			langDef.mKeywords.insert(k);

		static const char* const identifiers[] = {
			"abort", "abs", "acos", "asin", "atan", "atexit", "atof", "atoi", "atol", "ceil", "clock", "cosh", "ctime", "div", "exit", "fabs", "floor", "fmod", "getchar", "getenv", "isalnum", "isalpha", "isdigit", "isgraph",
			"ispunct", "isspace", "isupper", "kbhit", "log10", "log2", "log", "memcmp", "modf", "pow", "putchar", "putenv", "puts", "rand", "remove", "rename", "sinh", "sqrt", "srand", "strcat", "strcmp", "strerror", "time", "tolower", "toupper"
		};
		for (auto& k : identifiers)
		{
			Identifier id;
			id.mDeclaration = "Built-in function";
			langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
		}

		langDef.mTokenize = [](const char * in_begin, const char * in_end, const char *& out_begin, const char *& out_end, PaletteIndex & paletteIndex) -> bool
		{
			paletteIndex = PaletteIndex::Max;

			while (in_begin < in_end && isascii(*in_begin) && isblank(*in_begin))
				in_begin++;

			if (in_begin == in_end)
			{
				out_begin = in_end;
				out_end = in_end;
				paletteIndex = PaletteIndex::Default;
			}
			else if (TokenizeCStyleString(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::String;
			else if (TokenizeCStyleCharacterLiteral(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::CharLiteral;
			else if (TokenizeCStyleIdentifier(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::Identifier;
			else if (TokenizeCStyleNumber(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::Number;
			else if (TokenizeCStylePunctuation(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::Punctuation;

			return paletteIndex != PaletteIndex::Max;
		};

		langDef.mCommentStart = "/*";
		langDef.mCommentEnd = "*/";
		langDef.mSingleLineComment = "//";

		langDef.mCaseSensitive = true;
		langDef.mAutoIndentation = true;

		langDef.mName = "GLSL";

		inited = true;
	}
	return langDef;
}

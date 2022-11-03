#include <util/buffmt.hpp>
#include <util/f16.hpp>
#include <nytl/bytes.hpp>
#include <gui/util.hpp>
#include <gui/gui.hpp>
#include <util/profiling.hpp>
#include <threadContext.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include <numeric>
#include <iomanip>

namespace vil {

using nytl::copy;

struct FormattedScalar {
	std::string scalar;
	std::string error {};
};

template<typename T8, typename T16, typename T32, typename T64, typename Cast = T64>
FormattedScalar formatScalar(span<const std::byte> data, u32 offset, u32 width, u32 precision) {
	dlg_assert(width % 8 == 0u);
	if(offset + width / 8 > data.size()) {
		return {"N/A", dlg::format("Out of bounds")};
	}

	data = data.subspan(offset, width / 8);
	std::string val;
	auto prec = std::setprecision(precision);
	switch(width) {
		case 8:  return {dlg::format("{}{}", prec, Cast(copy<T8> (data)))};
		case 16: return {dlg::format("{}{}", prec, Cast(copy<T16>(data)))};
		case 32: return {dlg::format("{}{}", prec, Cast(copy<T32>(data)))};
		case 64: return {dlg::format("{}{}", prec, Cast(copy<T64>(data)))};
		default:
			return {"N/A", dlg::format("Unsupported type width {}", width)};
	}
}

Type* buildType(const spc::Compiler& compiler, u32 typeID,
		LinAllocator& alloc, const spc::Meta::Decoration* memberDeco) {

	auto stype = &compiler.get_type(typeID);
	if(stype->pointer) {
		dlg_assert(stype->parent_type);
		typeID = stype->parent_type;
		stype = &compiler.get_type(typeID);
	}

	auto& dst = alloc.construct<Type>();

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
		if(memberDeco->decoration_flags.get(spv::DecorationOffset)) {
			dst.deco.offset = memberDeco->offset;
		}
	}

	// handle array
	if(!stype->array.empty()) {
		dlg_assert(meta && meta->decoration.decoration_flags.get(spv::DecorationArrayStride));
		dst.deco.arrayStride = meta->decoration.array_stride;

		dlg_assert(stype->array.size() == stype->array_size_literal.size());
		dst.array = alloc.alloc<u32>(stype->array.size());

		for(auto d = 0u; d < stype->array.size(); ++d) {
			if(stype->array_size_literal[d] == true) {
				dst.array[d] = stype->array[d];
			} else {
				dst.array[d] = compiler.evaluate_constant_u32(stype->array[d]);
			}
		}

		// apparently this is needed? not entirely sure why
		dlg_assert(stype->parent_type);
		typeID = stype->parent_type;
		stype = &compiler.get_type(typeID);
		meta = compiler.get_ir().find_meta(typeID);
	}

	if(stype->basetype == spc::SPIRType::Struct) {
		// handle struct
		dst.members = alloc.alloc<Type::Member>(stype->member_types.size());
		for(auto i = 0u; i < stype->member_types.size(); ++i) {
			auto memTypeID = stype->member_types[i];

			dlg_assert(meta && meta->members.size() > i);
			auto deco = &meta->members[i];
			auto off = deco->offset;

			// TODO PERF: remove allocation via dlg format here,
			// use linearAllocator instead if needed
			auto name = dlg::format("?{}", i);
			if(!deco->alias.empty()) {
				// TODO PERF: we copy here with new, terrible
				name = deco->alias;
			}

			auto& mdst = dst.members[i];
			mdst.type = buildType(compiler, memTypeID, alloc, deco);
			mdst.name = copy(alloc, name);
			mdst.offset = off;

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

Type* buildType(const spc::Compiler& compiler, u32 typeID,
		LinAllocator& alloc) {
	return buildType(compiler, typeID, alloc, nullptr);
}

FormattedScalar formatScalar(const Type& type, ReadBuf data, u32 offset, u32 precision) {
	switch(type.type) {
		case Type::typeInt:
			return formatScalar<i8, i16, i32, i64>(data, offset, type.width, precision);
		case Type::typeUint:
			return formatScalar<u8, u16, u32, u64>(data, offset, type.width, precision);
		case Type::typeFloat:
			dlg_assertm(type.width != 8, "Invalid float bit width");
			return formatScalar<u8, f16, float, double>(data, offset, type.width, precision);
		case Type::typeBool:
			return formatScalar<u8, u16, u32, u64, bool>(data, offset, type.width, precision);
		default:
			return {"N/A", dlg::format("Unsupported type {}", unsigned(type.type))};
	}
}
std::string atomTypeName(const Type& type) {
	bool scalar = (type.vecsize == 1u && type.columns == 1u);
	std::string t;
	switch(type.type) {
		case Type::typeFloat: t = "f"; break;
		case Type::typeInt: t = "i"; break;
		case Type::typeUint: t = "u"; break;
		case Type::typeBool: t = "b"; break;
		default:
			return "Unsupported";
	}

	t += dlg::format("{}", type.width);

	if(scalar) {
		return t;
	}

	if(type.vecsize > 1 && type.columns == 1u) {
		return dlg::format("{}vec{}", t, type.vecsize);
	}

	if(type.columns == type.vecsize) {
		return dlg::format("{}mat{}", t, type.vecsize);
	}

	// NOTE: we don't use glsl matCxR syntax here since it's confusing.
	// Usually number of rows is given first
	return dlg::format("{}mat, {} rows, {} colums", t, type.vecsize, type.columns);
}

void displayAtom(const char* baseName, const Type& type, ReadBuf data, u32 offset) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	ImGui::AlignTextToFramePadding();
	ImGui::Bullet();
	ImGui::SameLine();
	imGuiText("{} ", baseName);

	if(ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		imGuiText("{}", atomTypeName(type));

		if(type.deco.flags & Decoration::Bits::rowMajor) {
			imGuiText("Row major memory layout");
		}
		if(type.deco.flags & Decoration::Bits::colMajor) {
			imGuiText("Column major memory layout");
		}
		if(type.deco.matrixStride) {
			imGuiText("Matrix stride: {}", type.deco.matrixStride);
		}

		ImGui::EndTooltip();
	}

	ImGui::TableNextColumn();

	auto baseSize = type.width / 8;
	auto rowStride = baseSize;
	auto colStride = baseSize;
	auto rowMajor = bool(type.deco.flags & Decoration::Bits::rowMajor);

	// display size; we display vectors as row vectors
	auto numRows = type.vecsize;
	auto numColumns = type.columns;
	if(type.vecsize > 1 && type.columns == 1) {
		numColumns = type.vecsize;
		numRows = 1u;
	}

	dlg_assert(numRows > 0);
	dlg_assert(numColumns > 0);

	if(type.deco.matrixStride) {
		(rowMajor ? rowStride : colStride) = type.deco.matrixStride;
	}

	auto id = dlg::format("Value:{}:{}", baseName, &type);
	auto flags = ImGuiTableFlags_SizingFixedFit; // | ImGuiTableFlags_BordersInner;
	if(ImGui::BeginTable(id.c_str(), numColumns, flags)) {
		for(auto r = 0u; r < numRows; ++r) {
			ImGui::TableNextRow();
			for(auto c = 0u; c < numColumns; ++c) {
				ImGui::TableNextColumn();
				auto off = offset + r * rowStride + c * colStride;
				auto fs = formatScalar(type, data, off, 3);

				ImGui::AlignTextToFramePadding();
				imGuiText("{}", fs.scalar);
				if(ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					if(!fs.error.empty()) {
						imGuiText("Error: {}", fs.error);
					} else if(type.type == Type::typeFloat) {
						auto fs = formatScalar(type, data, off, 10);
						imGuiText("Exact: {}", fs.scalar);
					}

					imGuiText("Memory offset: {}", off);

					if(numRows > 1) {
						imGuiText("Row: {}", r);
					}

					if(numColumns > 1) {
						imGuiText("Column: {}", c);
					}

					ImGui::EndTooltip();
				}
			}
		}

		ImGui::EndTable();
	}
}

void displayStruct(const char* baseName, const Type& type, ReadBuf data, u32 offset) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	auto id = dlg::format("{}:{}", baseName, offset);
	auto flags = ImGuiTreeNodeFlags_FramePadding |
		ImGuiTreeNodeFlags_SpanFullWidth;

	// all structs are initially open
	ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	if(ImGui::TreeNodeEx(id.c_str(), flags, "%s", baseName)) {
		for(auto i = 0u; i < type.members.size(); ++i) {
			auto& member = type.members[i];

			auto off = member.offset;
			auto name = std::string(member.name); // ugh PERF
			if(name.empty()) {
				name = dlg::format("?{}", i);
			}

			display(name.c_str(), *member.type, data, offset + off);
		}

		ImGui::TreePop();
	}
}

void displayNonArray(const char* baseName, const Type& type, ReadBuf data, u32 offset) {
	if(type.type == Type::typeStruct) {
		displayStruct(baseName, type, data, offset);
	} else {
		displayAtom(baseName, type, data, offset);
	}
}

void displayArrayDim(const char* baseName, const Type& type, span<const u32> rem,
		ReadBuf data, u32 offset) {
	auto count = rem[0];
	rem = rem.subspan(1);

	std::string name;
	auto subSize = type.deco.arrayStride;
	dlg_assert(subSize);

	for(auto size : rem) {
		dlg_assert(size != 0u); // only first dimension can be runtime size
		subSize *= size;

		name += dlg::format("[{}]", size);
	}

	if(count == 0u) {
		// runtime array, find out real size.
		auto remSize = data.size() - offset;
		// doesn't have to be like that even though it's sus if the buffer
		// size isn't a multiple of the stride.
		// dlg_assert(remSize % subSize == 0u);
		count = remSize / subSize; // intentionally round down
	}

	name = dlg::format("{}: [{}]", baseName, count) + name;

	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	// all arrays are initially closed
	auto id = dlg::format("{}:{}", type.deco.name, offset);
	auto flags = ImGuiTreeNodeFlags_FramePadding |
		ImGuiTreeNodeFlags_SpanFullWidth;

	ImGui::SetNextItemOpen(false, ImGuiCond_Once);
	if(ImGui::TreeNodeEx(id.c_str(), flags, "%s", name.c_str())) {
		// draw paging mechanism in the right column
		constexpr auto pageSize = 100u;
		auto page = 0;
		if(count > pageSize) {
			auto maxPage = (count - 1) / pageSize;
			auto id = ImGui::GetID("arrayPage");

			page = ImGui::GetStateStorage()->GetInt(id, 0);
			page = std::clamp<int>(page, 0, maxPage);

			ImGui::TableNextColumn();

			auto prevDisabled = (page == 0);
			auto nextDisabled = (unsigned(page) == maxPage);

			imGuiText("Page:");
			ImGui::SameLine();

			pushDisabled(prevDisabled);
			if(ImGui::Button("<") && !prevDisabled) {
				--page;
			}
			popDisabled(prevDisabled);

			ImGui::SameLine();

			// TODO: find better way to quickly get to a specific entry
			imGuiText("{} / {}", page, maxPage);
			// ImGui::SetNextItemWidth(30.f);
			// ImGui::DragInt("", &page, 0, maxPage);

			ImGui::SameLine();

			pushDisabled(nextDisabled);
			if(ImGui::Button(">") && !nextDisabled) {
				++page;
			}
			popDisabled(nextDisabled);

			ImGui::GetStateStorage()->SetInt(id, page);
		}

		// draw children
		auto end = std::min(count, (page + 1) * pageSize);
		for(auto i = page * pageSize; i < end; ++i) {
			auto newName = dlg::format("{}[{}]", baseName, i);
			auto newOffset = offset + i * subSize;
			if(rem.empty()) {
				displayNonArray(newName.c_str(), type, data, newOffset);
			} else {
				displayArrayDim(newName.c_str(), type, rem, data, newOffset);
			}
		}

		ImGui::TreePop();
	}
}

void display(const char* name, const Type& type, ReadBuf data, u32 offset) {
	ZoneScoped;

	if(type.array.empty()) {
		displayNonArray(name, type, data, offset);
		return;
	}

	// display array
	displayArrayDim(name, type, type.array, data, offset);
}

void displayTable(const char* name, const Type& type, ReadBuf data, u32 offset) {
	auto flags = ImGuiTableFlags_BordersInner |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_SizingStretchSame;
	if(ImGui::BeginTable("Values", 2u, flags)) {
		ImGui::TableSetupColumn(nullptr, 0, 0.25f);
		ImGui::TableSetupColumn(nullptr, 0, 0.75f);

		display(name, type, data, offset);
		ImGui::EndTable();
	}
}

unsigned size(const Type& t, BufferLayout bl) {
	u32 arrayFac = std::accumulate(t.array.begin(), t.array.end(), 1u, std::multiplies{});
	switch(t.type) {
		case Type::typeBool:
		case Type::typeFloat:
		case Type::typeUint:
		case Type::typeInt: {
			auto vec = t.vecsize;
			if(bl == BufferLayout::std140 && vec == 3u) {
				vec = 4u;
			}
			return arrayFac * vec * t.columns * t.width / 8u;
		} case Type::typeStruct: {
			auto end = 0u;
			for(auto& member : t.members) {
				end = std::max(end, member.offset + size(*member.type, bl));
			}
			return arrayFac * end;
		}
	}

	dlg_error("unreachable");
	return 0u;
}

unsigned align(const Type& t, BufferLayout bl) {
	switch(t.type) {
		case Type::typeBool:
		case Type::typeFloat:
		case Type::typeUint:
		case Type::typeInt: {
			auto vec = t.vecsize;
			// For std140 *and* std430, vec3 has a 16-byte alignment
			if((bl == BufferLayout::std140 || bl == BufferLayout::std430) && vec == 3u) {
				vec = 4u;
			}
			return vec * t.width / 8u;
		} case Type::typeStruct: {
			auto ret = 0u;
			for(auto& member : t.members) {
				ret = std::max(ret, align(*member.type, bl));
			}
			return ret;
		}
	}

	dlg_error("unreachable");
	return 0u;
}

unsigned endAlign(const Type& t, BufferLayout bl) {
	// TODO
	(void) bl;
	(void) t;
	return 1u;
}

} // namespace vil

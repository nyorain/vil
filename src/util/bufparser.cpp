#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/trace.hpp>
#include <tao/pegtl/contrib/analyze.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>
#include <tao/pegtl/contrib/parse_tree_to_dot.hpp>
#include <dlg/dlg.hpp>
#include <util/buffmt.hpp>
#include <util/profiling.hpp>
#include <util/util.hpp>
#include <threadContext.hpp>

namespace pegtl = tao::pegtl;

namespace vil {
namespace syn {

// https://stackoverflow.com/questions/53427551/pegtl-how-to-skip-spaces-for-the-entire-grammar
struct LineComment : pegtl::seq<
		pegtl::sor<
			pegtl::string<'/', '/'>,
			pegtl::one<'#'>>,
		pegtl::until<
			pegtl::eolf,
			pegtl::utf8::any>> {};
struct InlineComment : pegtl::seq<
		pegtl::string<'/', '*'>,
		pegtl::until<
			pegtl::string<'*', '/'>,
			pegtl::utf8::any>> {};

struct Comment : pegtl::disable<pegtl::sor<LineComment, InlineComment>> {};
struct Separator : pegtl::sor<tao::pegtl::ascii::space, Comment> {};
struct Seps : tao::pegtl::star<Separator> {}; // Any separators, whitespace or comments

template<typename S, typename... R> using Interleaved = pegtl::seq<S, pegtl::seq<R, S>...>;
template<typename S, typename... R> using IfMustSep =
	pegtl::if_must<pegtl::pad<S, Separator>, pegtl::pad<R, Separator>...>;

struct Plus : pegtl::one<'+'> {};
struct Minus : pegtl::one<'-'> {};
struct Mult : pegtl::one<'*'> {};
struct Divide : pegtl::one<'/'> {};
struct Comma : pegtl::one<','> {};
struct Dot : pegtl::one<'.'> {};
struct Semicolon : pegtl::one<';'> {};
struct Colon : pegtl::one<':'> {};
struct Eof : pegtl::eof {};

struct Identifier : pegtl::seq<pegtl::alpha, pegtl::star<pegtl::alnum>> {};
struct Type : pegtl::seq<Identifier> {};
struct Number : pegtl::plus<pegtl::digit> {};

struct ArrayQualifierClose : pegtl::one<']'> {};
struct ArrayQualifierNumber : pegtl::seq<Number> {};
struct ArrayQualifier : IfMustSep<
	pegtl::one<'['>,
	ArrayQualifierNumber,
	ArrayQualifierClose
> {};
struct ArrayRuntimeQualifier : Interleaved<Seps,
	pegtl::one<'['>,
	pegtl::one<']'>
> {};
struct ArrayQualifiers : pegtl::seq<
	pegtl::pad_opt<ArrayRuntimeQualifier, Separator>, // must be first
	pegtl::star<pegtl::pad<ArrayQualifier, Separator>>
> {};

struct ValueDecl : Interleaved<Seps,
	Type,
	Identifier,
	ArrayQualifiers
> {};

struct StructMember : Interleaved<Seps, ValueDecl, Semicolon> {};
struct StructMemberList : pegtl::plus<StructMember> {};
struct StructOpen : pegtl::one<'{'> {};
struct StructClose : pegtl::one<'}'> {};
struct StructSemicolon : Semicolon {};
struct StructName : pegtl::seq<Identifier> {};
struct StructDecl : IfMustSep<
	pegtl::keyword<'s', 't', 'r', 'u', 'c', 't'>,
	StructName,
	StructOpen,
	StructMemberList,
	StructClose,
	StructSemicolon
> {};

struct StructDecls : pegtl::star<pegtl::pad<StructDecl, Separator>> {};
struct ValueDecls : pegtl::star<Interleaved<Seps, ValueDecl, Semicolon>> {};

struct Grammar : Interleaved<Seps, StructDecls, ValueDecls> {};
struct Whole : pegtl::must<Grammar, Eof> {};

// transform/select
// Just keeps the node as it was
struct Keep : pegtl::parse_tree::apply<Keep> {
	template<typename Node, typename... States>
	static void transform(std::unique_ptr<Node>&, States&&...) {
	}
};

// Keeps the node but discards all children
struct DiscardChildren : pegtl::parse_tree::apply<DiscardChildren> {
	template<typename Node, typename... States>
	static void transform(std::unique_ptr<Node>& n, States&&...) {
		n->children.clear();
	}
};

// Completely discards the node and all its children
struct Discard : pegtl::parse_tree::apply<Discard> {
	template<typename Node, typename... States>
	static void transform(std::unique_ptr< Node >& n, States&&...) {
		n.reset();
	}
};

// Discards the node if it does not have any children.
// Folds the node if it only has one child (i.e. replaces by child).
struct FoldDiscard : pegtl::parse_tree::apply<FoldDiscard> {
	template<typename Node, typename... States>
	static void transform(std::unique_ptr< Node >& n, States&&...) {
		if(n->children.size() == 1) {
			n = std::move(n->children.front());
		} else if(n->children.empty()) {
			n.reset();
		}
	}
};

// Discards node only if empty
struct DiscardIfEmpty : pegtl::parse_tree::apply<DiscardIfEmpty> {
	template<typename Node, typename... States>
	static void transform(std::unique_ptr< Node >& n, States&&...) {
		if(n->children.empty()) {
			n.reset();
		}
	}
};

template<typename Rule> struct selector : FoldDiscard {};
template<> struct selector<Identifier> : DiscardChildren {};
template<> struct selector<ValueDecls> : Keep {};
template<> struct selector<StructDecls> : Keep {};
template<> struct selector<StructDecl> : Keep {};
template<> struct selector<StructMemberList> : Keep {};
template<> struct selector<ValueDecl> : Keep {};
template<> struct selector<Type> : DiscardChildren {};
template<> struct selector<Seps> : Discard {};
template<> struct selector<ArrayQualifiers> : DiscardIfEmpty {};
template<> struct selector<ArrayQualifier> : FoldDiscard {};
template<> struct selector<ArrayRuntimeQualifier> : Keep {};
template<> struct selector<Number> : Keep {};

// error messages
template<typename> inline constexpr const char* error_message = nullptr;
template<> inline constexpr const char* error_message<StructOpen> = "Expected '{' after beginning of struct declaration";
template<> inline constexpr const char* error_message<StructClose> = "Expected '}' to close struct declaration";
template<> inline constexpr const char* error_message<StructMemberList> = "Expected a non-empty list of struct members";
template<> inline constexpr const char* error_message<StructSemicolon> = "Expected a semicolon after a struct declaration";
template<> inline constexpr const char* error_message<StructName> = "Expected identifier as struct name";
template<> inline constexpr const char* error_message<Identifier> = "Expected identifier";

template<> inline constexpr const char* error_message<ArrayQualifierClose> = "Expected a ']' to close the array qualifier";
template<> inline constexpr const char* error_message<ArrayQualifierNumber> = "Expected a number literal as array size";

template<> inline constexpr const char* error_message<Eof> = "Expected end of file";
template<> inline constexpr const char* error_message<Seps> = "??Seps"; // should never fail
template<> inline constexpr const char* error_message<Grammar> = "??Grammar"; // should never fail

template<typename T> inline constexpr const char* error_message<pegtl::pad<T, Separator>> = error_message<T>;

struct error {
	template<typename Rule> static constexpr auto message = error_message<Rule>;
};

template<typename Rule> using control = tao::pegtl::must_if<error>::control<Rule>;

} // namespace syn

// tree parser
const Type* parseBuiltin(std::string_view str) {
	constexpr auto createAtomPair = [](std::string_view name, Type::BaseType type,
			u32 width, u32 vec, u32 col) {
		Type ret;
		ret.type = type;
		ret.width = width;
		ret.vecsize = vec;
		ret.columns = col;
		ret.deco.name = name;
		if(col > 1u) {
			// TODO: would need to consider buffer layout
			ret.deco.matrixStride = vec * width / 8;
		}

		return std::pair(name, ret);
	};

	static const std::unordered_map<std::string_view, Type> builtins = {
		// base types
		createAtomPair("float", Type::typeFloat, 32, 1, 1),
		createAtomPair("f32", Type::typeFloat, 32, 1, 1),
		createAtomPair("vec1", Type::typeFloat, 32, 1, 1),
		createAtomPair("float1", Type::typeFloat, 32, 1, 1),

		createAtomPair("uint", Type::typeUint, 32, 1, 1),
		createAtomPair("u32", Type::typeUint, 32, 1, 1),
		createAtomPair("uvec1", Type::typeUint, 32, 1, 1),
		createAtomPair("uint1", Type::typeUint, 32, 1, 1),

		createAtomPair("int", Type::typeInt, 32, 1, 1),
		createAtomPair("i32", Type::typeInt, 32, 1, 1),
		createAtomPair("ivec1", Type::typeInt, 32, 1, 1),
		createAtomPair("int1", Type::typeInt, 32, 1, 1),

		createAtomPair("half", Type::typeFloat, 16, 1, 1),
		createAtomPair("f16", Type::typeFloat, 16, 1, 1),
		createAtomPair("f16vec1", Type::typeFloat, 16, 1, 1),
		createAtomPair("half1", Type::typeFloat, 16, 1, 1),
		createAtomPair("float16_t", Type::typeFloat, 16, 1, 1),

		createAtomPair("double", Type::typeFloat, 64, 1, 1),
		createAtomPair("f64", Type::typeFloat, 64, 1, 1),
		createAtomPair("dvec1", Type::typeFloat, 64, 1, 1),
		createAtomPair("double1", Type::typeFloat, 64, 1, 1),

		createAtomPair("u64", Type::typeUint, 64, 1, 1),
		createAtomPair("uint64_t", Type::typeUint, 64, 1, 1),
		createAtomPair("u16", Type::typeUint, 16, 1, 1),
		createAtomPair("ushort", Type::typeUint, 16, 1, 1),
		createAtomPair("u8", Type::typeUint, 8, 1, 1),
		createAtomPair("ubyte", Type::typeUint, 8, 1, 1),

		createAtomPair("i64", Type::typeInt, 64, 1, 1),
		createAtomPair("int64_t", Type::typeInt, 64, 1, 1),
		createAtomPair("i16", Type::typeInt, 16, 1, 1),
		createAtomPair("short", Type::typeInt, 16, 1, 1),
		createAtomPair("i8", Type::typeInt, 8, 1, 1),
		createAtomPair("byte", Type::typeInt, 8, 1, 1),

		createAtomPair("bool", Type::typeBool, 32, 1, 1),

		// vectors
		createAtomPair("vec2", Type::typeFloat, 32, 2, 1),
		createAtomPair("float2", Type::typeFloat, 32, 2, 1),
		createAtomPair("vec3", Type::typeFloat, 32, 3, 1),
		createAtomPair("float3", Type::typeFloat, 32, 3, 1),
		createAtomPair("vec4", Type::typeFloat, 32, 4, 1),
		createAtomPair("float4", Type::typeFloat, 32, 4, 1),

		createAtomPair("uvec2", Type::typeUint, 32, 2, 1),
		createAtomPair("uint2", Type::typeUint, 32, 2, 1),
		createAtomPair("uvec3", Type::typeUint, 32, 3, 1),
		createAtomPair("uint3", Type::typeUint, 32, 3, 1),
		createAtomPair("uvec4", Type::typeUint, 32, 4, 1),
		createAtomPair("uint4", Type::typeUint, 32, 4, 1),
		createAtomPair("u32vec2", Type::typeUint, 32, 2, 1),
		createAtomPair("u32int2", Type::typeUint, 32, 2, 1),
		createAtomPair("u32vec3", Type::typeUint, 32, 3, 1),
		createAtomPair("u32int3", Type::typeUint, 32, 3, 1),
		createAtomPair("u32vec4", Type::typeUint, 32, 4, 1),
		createAtomPair("u32int4", Type::typeUint, 32, 4, 1),

		createAtomPair("u8vec2", Type::typeUint, 8, 2, 1),
		createAtomPair("u8vec3", Type::typeUint, 8, 3, 1),
		createAtomPair("u8vec4", Type::typeUint, 8, 4, 1),

		createAtomPair("i8vec2", Type::typeInt, 8, 2, 1),
		createAtomPair("i8vec3", Type::typeInt, 8, 3, 1),
		createAtomPair("i8vec4", Type::typeInt, 8, 4, 1),

		createAtomPair("bvec2", Type::typeBool, 32, 2, 1),
		createAtomPair("bvec3", Type::typeBool, 32, 3, 1),
		createAtomPair("bvec4", Type::typeBool, 32, 4, 1),

		createAtomPair("u16vec2", Type::typeUint, 16, 2, 1),
		createAtomPair("ushort2", Type::typeUint, 16, 2, 1),
		createAtomPair("u16vec3", Type::typeUint, 16, 3, 1),
		createAtomPair("ushort3", Type::typeUint, 16, 3, 1),
		createAtomPair("u16vec4", Type::typeUint, 16, 4, 1),
		createAtomPair("ushort4", Type::typeUint, 16, 4, 1),

		createAtomPair("u64vec2", Type::typeUint, 64, 2, 1),
		createAtomPair("uint64_t2", Type::typeUint, 64, 2, 1),
		createAtomPair("u64vec3", Type::typeUint, 64, 3, 1),
		createAtomPair("uint64_t3", Type::typeUint, 64, 3, 1),
		createAtomPair("u64vec4", Type::typeUint, 64, 4, 1),
		createAtomPair("uint64_t4", Type::typeUint, 64, 4, 1),

		createAtomPair("ivec2", Type::typeInt, 32, 2, 1),
		createAtomPair("int2", Type::typeInt, 32, 2, 1),
		createAtomPair("ivec3", Type::typeInt, 32, 3, 1),
		createAtomPair("int3", Type::typeInt, 32, 3, 1),
		createAtomPair("ivec4", Type::typeInt, 32, 4, 1),
		createAtomPair("int4", Type::typeInt, 32, 4, 1),

		createAtomPair("i16vec2", Type::typeInt, 16, 2, 1),
		createAtomPair("short2", Type::typeInt, 16, 2, 1),
		createAtomPair("i16vec3", Type::typeInt, 16, 3, 1),
		createAtomPair("short3", Type::typeInt, 16, 3, 1),
		createAtomPair("i16vec4", Type::typeInt, 16, 4, 1),
		createAtomPair("short4", Type::typeInt, 16, 4, 1),

		createAtomPair("f16vec2", Type::typeFloat, 16, 2, 1),
		createAtomPair("half2", Type::typeFloat, 16, 2, 1),
		createAtomPair("f16vec3", Type::typeFloat, 16, 3, 1),
		createAtomPair("half3", Type::typeFloat, 16, 3, 1),
		createAtomPair("f16vec4", Type::typeFloat, 16, 4, 1),
		createAtomPair("half4", Type::typeFloat, 16, 4, 1),

		createAtomPair("f64vec2", Type::typeFloat, 64, 2, 1),
		createAtomPair("dvec2", Type::typeFloat, 64, 2, 1),
		createAtomPair("double2", Type::typeFloat, 64, 2, 1),
		createAtomPair("f64vec3", Type::typeFloat, 64, 3, 1),
		createAtomPair("dvec3", Type::typeFloat, 64, 3, 1),
		createAtomPair("double3", Type::typeFloat, 64, 3, 1),
		createAtomPair("f64vec4", Type::typeFloat, 64, 4, 1),
		createAtomPair("dvec4", Type::typeFloat, 64, 4, 1),
		createAtomPair("double4", Type::typeFloat, 64, 4, 1),
	};

	auto it = builtins.find(str);
	if(it == builtins.end()) {
		return nullptr;
	}

	return &it->second;
}

struct TreeParser {
	using ParseTreeNode = tao::pegtl::parse_tree::node;

	const Type* parseType(const ParseTreeNode& node) const {
		dlg_assert(node.is_type<syn::Type>());
		auto name = node.string_view();

		auto it = structs_.find(name);
		if(it != structs_.end()) {
			return it->second;
		}

		auto t = parseBuiltin(name);
		if(t == nullptr) {
			auto str = dlg::format("Invalid type {}", name);
			throw std::runtime_error(str);
		}

		return t;
	}

	const Type* applyArrayQualifiers(const ParseTreeNode& quals, const Type& in) {
		dlg_assert(quals.is_type<syn::ArrayQualifiers>());
		dlg_assert_or(!quals.children.empty(), return &in);

		auto& dst = *memScope_.allocRaw<Type>();
		dst = in;
		dst.deco.arrayStride = size(in, bufferLayout_);

		for(auto& qual : quals.children) {
			if(qual->is_type<syn::ArrayRuntimeQualifier>()) {
				dlg_assert(dst.array.empty());
				dst.array.push_back(0);
			} else {
				dlg_assert(qual->is_type<syn::Number>());
				u32 dim;
				auto success = stoi(qual->string(), dim);
				dlg_assert(success);
				dst.array.push_back(dim);
			}
		}

		return &dst;
	}

	void appendValueDecl(const ParseTreeNode& decl, Type& dstStruct, unsigned& offset) {
		dlg_assert(decl.is_type<syn::ValueDecl>());
		dlg_assert(decl.children.size() >= 2u);
		dlg_assert(decl.children.size() <= 3u);

		auto& dst = dstStruct.members.emplace_back();
		dst.type = parseType(*decl.children[0]);
		dst.name = decl.children[1]->string_view();

		offset = align(offset, align(*dst.type, bufferLayout_));
		dst.offset = offset;

		if(decl.children.size() >= 3) {
			dst.type = applyArrayQualifiers(*decl.children[2], *dst.type);
		}

		offset += size(*dst.type, bufferLayout_);
	}

	void parseStruct(const ParseTreeNode& decl) {
		dlg_assert(decl.is_type<syn::StructDecl>());
		auto& t = *memScope_.allocRaw<Type>();
		t.type = Type::typeStruct;

		dlg_assert(decl.children.size() == 2u);
		t.deco.name = decl.children[0]->string_view();

		auto offset = 0u;
		for(auto& member : decl.children[1]->children) {
			appendValueDecl(*member, t, offset);
		}

		structs_.emplace(std::string_view(t.deco.name), &t);
	}

	void parseStructDecls(const ParseTreeNode& decls) {
		dlg_assert(decls.is_type<syn::StructDecls>());

		for(auto& decl : decls.children) {
			parseStruct(*decl);
		}
	}

	void parseValueDecls(const ParseTreeNode& decls) {
		dlg_assert(decls.is_type<syn::ValueDecls>());

		auto* sdst = memScope_.allocRaw<Type>();
		sdst->type = Type::typeStruct;
		sdst->deco.name = "main";

		auto offset = 0u;
		for(auto& member : decls.children) {
			appendValueDecl(*member, *sdst, offset);
		}

		main_ = sdst;
	}

	void parseModule(const ParseTreeNode& module) {
		dlg_assert(module.is_type<syn::Grammar>());
		parseStructDecls(*module.children[0]);
		parseValueDecls(*module.children[1]);
	}

	ThreadMemScope& memScope_;
	std::unordered_map<std::string_view, const Type*> structs_ {};
	const Type* main_ {};
	BufferLayout bufferLayout_ {BufferLayout::std430}; // TODO
};

const Type* parseType(std::string_view str, ThreadMemScope& memScope) {
	ZoneScoped;

	pegtl::string_input in {std::string(str), "input"};

	std::unique_ptr<tao::pegtl::parse_tree::node> root;

	try {
		root = tao::pegtl::parse_tree::parse<syn::Whole, syn::selector,
			pegtl::nothing, syn::control>(in);
	} catch(const pegtl::parse_error& error) {
		auto& pos = error.positions()[0];
		auto msg = error.message();
		std::cout << pos.source << ":" << pos.line << ":" << pos.column << ": " << msg << "\n";

		// TODO: make it work with tabs
		auto line = in.line_at(pos);
		auto tabCount = std::count(line.begin(), line.end(), '\t');
		std::cout << line << "\n";

		// hard to say what tab size is... eh. Maybe just replace it?
		auto col = pos.column + tabCount * (4 - 1);
		for(auto i = 1u; i < col; ++i) {
			std::cout << " ";
		}

		std::cout << "^\n";
		return nullptr;
	}

	dlg_assert(root);

	{
		ExtZoneScopedN("output dot");
		auto of = std::ofstream("test.dot");
		pegtl::parse_tree::print_dot(of, *root);
	}

	dlg_assert(root->children.size() == 1u);

	try {
		TreeParser parser{memScope};
		parser.parseModule(*root->children[0]);
		return parser.main_;
	} catch(const std::exception& err) {
		dlg_error("Error: {}", err.what());
		return nullptr;
	}
}

} // namespace vil

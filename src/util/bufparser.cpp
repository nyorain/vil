#include <util/buffmt.hpp>
#include <util/dlg.hpp>
#include <util/profiling.hpp>
#include <util/util.hpp>
#include <threadContext.hpp>
#include <dlg/output.h>
#include <fstream>
#include <unordered_set>
#include <string>
#include <string_view>
#include <algorithm>

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/trace.hpp>
#include <tao/pegtl/contrib/analyze.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>
#include <tao/pegtl/contrib/parse_tree_to_dot.hpp>

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

struct ValueDecl : IfMustSep<
	Type,
	Identifier,
	ArrayQualifiers,
	Semicolon
> {};

struct StructMemberList : pegtl::plus<ValueDecl> {};
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
struct ValueDecls : pegtl::star<ValueDecl> {};

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
template<> struct selector<ArrayQualifier> : FoldDiscard {};
template<> struct selector<ArrayRuntimeQualifier> : Keep {};
template<> struct selector<Number> : Keep {};

template<> struct selector<ArrayQualifiers> : pegtl::parse_tree::apply<selector<ArrayQualifiers>> {
	template<typename Node, typename... States>
	static void transform(std::unique_ptr< Node >& n, States&&...) {
		if(n->children.empty()) {
			n.reset();
		} else if(n->children.back()->children.size() > 1) {
			// get rid of extra star rule for list of static qualifiers
			auto staticQuals = std::move(n->children.back());
			n->children.pop_back();
			for(auto& qual : staticQuals->children) {
				n->emplace_back(std::move(qual));
			}
		}
	}
};

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
template<> inline constexpr const char* error_message<Semicolon> = "Expected ';'";

template<> inline constexpr const char* error_message<Eof> = "Expected end of file";

// Those should never fail but we need them to avoid static asserts in pegtl
template<> inline constexpr const char* error_message<Seps> = "??Seps";
template<> inline constexpr const char* error_message<Grammar> = "??Grammar";
template<> inline constexpr const char* error_message<ArrayQualifiers> = "??ArrayQualifiers";

template<typename T> inline constexpr const char* error_message<pegtl::pad<T, Separator>> = error_message<T>;

struct error {
	template<typename Rule> static constexpr auto message = error_message<Rule>;
	template<typename Rule> static constexpr auto raise_on_failure = false;
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
		ret.deco.name = name; // static string so it's ok to set it here
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

	struct InternalError : std::exception {
		const ParseTreeNode* node;
		std::string assertion;
		InternalError(const ParseTreeNode& n, std::string a) :
			node(&n), assertion(std::move(a)) {}
	};

	struct SemanticError : std::exception {
		const ParseTreeNode* node;
		std::string message;
		SemanticError(const ParseTreeNode& n, std::string a) :
			node(&n), message(std::move(a)) {}
	};

#define passert(x, node) if(!(x)) { throw InternalError(node, #x); }

	template<typename T>
	void checkType(const ParseTreeNode& node) const {
		if(!node.is_type<T>()) {
			auto msg = dlg::format("node.is_type<{}>()", pegtl::demangle<T>);
			throw InternalError(node, msg);
		}
	}

	const Type* parseType(const ParseTreeNode& node) const {
		checkType<syn::Type>(node);
		auto name = node.string_view();

		auto it = structs_.find(name);
		if(it != structs_.end()) {
			return it->second;
		}

		auto t = parseBuiltin(name);
		if(t == nullptr) {
			auto msg = dlg::format("Invalid type '{}'", name);
			throw SemanticError(node, msg);
		}

		return t;
	}

	const Type* applyArrayQualifiers(const ParseTreeNode& quals, const Type& in) {
		checkType<syn::ArrayQualifiers>(quals);
		passert(!quals.children.empty(), quals);

		auto& dst = alloc_.construct<Type>();
		dst = in;
		dst.deco.arrayStride = size(in, bufferLayout_);
		dst.array = alloc_.alloc<u32>(quals.children.size());

		for(auto [i, qual] : enumerate(quals.children)) {
			if(qual->is_type<syn::ArrayRuntimeQualifier>()) {
				passert(i == 0u, *qual);
				dst.array[i] = 0;
			} else {
				checkType<syn::Number>(*qual);
				// initialization here only done because auf shitty msvc
				// compiler warnings, breaks my heart :(
				u32 dim {};
				auto success = stoi(qual->string(), dim);
				passert(success, *qual);
				dst.array[i] = dim;
			}
		}

		return &dst;
	}

	void parseValueDecl(const ParseTreeNode& decl, Type::Member& dst, unsigned& offset) {
		checkType<syn::ValueDecl>(decl);
		passert(decl.children.size() >= 2u, decl);
		passert(decl.children.size() <= 3u, decl);

		dst.type = parseType(*decl.children[0]);
		dst.name = copy(alloc_, decl.children[1]->string_view());

		offset = align(offset, align(*dst.type, bufferLayout_));
		dst.offset = offset;

		if(decl.children.size() >= 3) {
			dst.type = applyArrayQualifiers(*decl.children[2], *dst.type);
		}

		offset += size(*dst.type, bufferLayout_);
	}

	void parseStruct(const ParseTreeNode& decl) {
		checkType<syn::StructDecl>(decl);
		auto& t = alloc_.construct<Type>();
		t.type = Type::typeStruct;

		passert(decl.children.size() == 2u, decl);
		t.deco.name = copy(alloc_, decl.children[0]->string_view());

		auto offset = 0u;

		auto& members = decl.children[1]->children;
		t.members = alloc_.alloc<Type::Member>(members.size());
		for(auto [i, member] : enumerate(members)) {
			parseValueDecl(*member, t.members[i], offset);

			auto& type = *t.members[i].type;
			if(!type.array.empty() && type.array.front() == 0u) {
				throw SemanticError(decl, "Runtime array not allowed in struct");
			}
		}

		structs_.emplace(t.deco.name, &t);
	}

	void parseStructDecls(const ParseTreeNode& decls) {
		checkType<syn::StructDecls>(decls);

		for(auto& decl : decls.children) {
			parseStruct(*decl);
		}
	}

	void parseValueDecls(const ParseTreeNode& decls) {
		checkType<syn::ValueDecls>(decls);

		auto* sdst = alloc_.allocRaw<Type>();
		sdst->type = Type::typeStruct;
		sdst->deco.name = "main";

		auto offset = 0u;
		auto runtimeArray = false;

		auto& members = decls.children;
		sdst->members = alloc_.alloc<Type::Member>(members.size());
		for(auto [i, member] : enumerate(decls.children)) {
			if(runtimeArray) {
				throw SemanticError(*member, "No values allowed after runtime-array");
			}
			parseValueDecl(*member, sdst->members[i], offset);

			auto& type = *sdst->members[i].type;
			if(!type.array.empty() && type.array.front() == 0u) {
				runtimeArray = true;
			}
		}

		main_ = sdst;
	}

	void parseModule(const ParseTreeNode& mod) {
		checkType<syn::Grammar>(mod);
		parseStructDecls(*mod.children[0]);
		parseValueDecls(*mod.children[1]);
	}

	LinAllocator& alloc_;
	// TODO: use linear allocator here as well?
	// Should probably just use a vector (or map/linked list with
	// LinearAllocator), we don't have so many types that we need an
	// unordered map
	std::unordered_map<std::string_view, const Type*> structs_ {};
	const Type* main_ {};
	BufferLayout bufferLayout_ {BufferLayout::std430}; // TODO
};

ParseTypeResult parseType(std::string_view str, LinAllocator& alloc) {
	ZoneScoped;

	pegtl::string_input in {std::string(str), "input"};
	std::unique_ptr<tao::pegtl::parse_tree::node> root;

	try {
		root = tao::pegtl::parse_tree::parse<syn::Whole, syn::selector,
			pegtl::nothing, syn::control>(in);
	} catch(const pegtl::parse_error& error) {
		ParseTypeResult res;
		auto pos = error.positions()[0];
		auto& err = res.error.emplace();
		err.message = error.message();
		err.loc.line = pos.line;
		err.loc.col = pos.column;
		err.loc.lineContent = in.line_at(pos);
		return res;
	}

	dlg_assert(root);

	TreeParser parser{alloc};
	try {
		parser.parseModule(*root->children[0]);
	} catch(const TreeParser::InternalError& err) {
		auto node_printer = [&](std::ostream& os, auto& n) {
			auto type = n.is_root() ? "ROOT" : n.type;
			using pegtl::parse_tree::internal::escape;

			os << "  x" << &n << " [ label=\"";
			escape( os, type );
			if( n.has_content() ) {
				os << "\\n\\\"";
				escape( os, n.string_view() );
				os << "\\\"";
			}

			if (&n == err.node) {
				os << "\\nInternal Error: \\\"";
				escape( os, err.assertion);
				os << "\\\"";
			}

			os << "\"";

			if (&n == err.node) {
				os << " color=red ";
			}

			os << "]\n";
		};

		auto msg = dlg::format("Internal buftype compiler error. Assertion '{}' failed.",
			err.assertion);

		// Print dot file for internal errors
		// Make sure to never print the errror for the same input twice since
		// that might be problematic with continuous editing.
		static thread_local std::unordered_set<std::string> seen;
		if(seen.insert(std::string(str)).second) {
			char name[100];
			std::time_t now = std::time(0);
			std::strftime(name, sizeof(name), "vil_err_%Y_%m_%d_%H_%M_%S.dot", localtime(&now));
			auto of = std::ofstream(name);
			pegtl::parse_tree::print_dot(of, *root, node_printer);
			of.close();

			msg += dlg::format(" See {}.", name);
		}

		dlg_error("{}", msg);

		ParseTypeResult res;
		auto& dstErr = res.error.emplace();
		auto pos = err.node->begin();
		dstErr.loc.line = pos.line;
		dstErr.loc.col = pos.column;
		dstErr.loc.lineContent = in.line_at(pos);
		dstErr.message = msg;
		return res;
	} catch(const TreeParser::SemanticError& err) {
		ParseTypeResult res;
		auto& dstErr = res.error.emplace();
		auto pos = err.node->begin();
		dstErr.loc.line = pos.line;
		dstErr.loc.col = pos.column;
		dstErr.loc.lineContent = in.line_at(pos);
		dstErr.message = err.message;
		return res;
	}

	ParseTypeResult res;
	res.type = parser.main_;
	return res;
}

// Will simply output any errors to console.
const Type* unwrap(const ParseTypeResult& res) {
	if(res.type) {
		dlg_assert(!res.error);
		return res.type;
	}

	dlg_assert(res.error);
	auto& err = *res.error;

	char buf[12];
	dlg_escape_sequence(dlg_default_output_styles[dlg_level_error], buf);

	std::string msg = buf;
	msg += dlg::format("input:{}:{}: {}\n", err.loc.line, err.loc.col, err.message);

	// TODO: make it work with tabs
	auto& line = err.loc.lineContent;
	auto tabCount = std::count(line.begin(), line.end(), '\t');
	msg += line;
	msg += "\n";

	// hard to say what tab size is... eh. Maybe just replace it?
	auto col = err.loc.col + tabCount * (4 - 1);
	for(auto i = 1u; i < col; ++i) {
		msg += " ";
	}

	msg += "^\n";
	msg += dlg_reset_sequence;

	std::cerr << msg;
	return nullptr;
}

} // namespace vil

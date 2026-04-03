/**
 * @file TypeAST.hpp
 * @project LUC Compiler
 * @responsibility Defines the syntactic representation of types (Primitive, Union, Array, Pointer).
 * 
 * @hierarchy BaseAST -> TypeAST -> [Concrete Nodes]
 * @note These represent types AS WRITTEN in source. The semantic pass later resolves 
 *       these into actual Type objects.
 */

#pragma once

#include "BaseAST.hpp"
#include "../Tokens.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// TypeAST.hpp — all type nodes
//
// Every node here inherits from TypeAST (defined in BaseAST.hpp).
// The parser constructs these while parsing type annotations — they are never
// constructed by the semantic pass.
//
// Include order for other family headers:
//   ExprAST.hpp   →  #include "TypeAST.hpp"  (nodes hold TypePtr fields)
//   DeclAST.hpp   →  #include "TypeAST.hpp"  (params, return types, etc.)
//
// Node inventory:
//   PrimitiveTypeAST      — bool, int, float, string, any, ...
//   NamedTypeAST          — user-defined type by name, optional generic args
//   NullableTypeAST       — wraps any type with ?
//   UnionTypeAST          — int | string | bool
//   FixedArrayTypeAST     — [N]T   compile-time size, stack allocated
//   SliceTypeAST          — []T    fat-pointer view, no ownership
//   DynamicArrayTypeAST   — [*]T   heap-owned, growable
//   RefTypeAST            — &T     safe managed reference
//   PtrTypeAST            — @T     raw pointer, extern/FFI only
//   FuncTypeAST           — (params) return?   first-class function type
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveKind — mirrors the TYPE_* tokens from Tokens.hpp but as a
// self-contained enum so the rest of the AST doesn't need to include
// Tokens.hpp just to inspect a primitive type node.
//
// The parser maps TYPE_INT → PrimitiveKind::Int, etc.
// The semantic pass and codegen read PrimitiveKind directly.
// ─────────────────────────────────────────────────────────────────────────────

enum class PrimitiveKind {
    // Boolean
    Bool,

    // Signed integers
    Byte,     // int8,  -128..127
    Short,    // int16
    Int,      // int32
    Long,     // int64

    // Unsigned integers
    Ubyte,    // uint8,  0..255
    Ushort,   // uint16
    Uint,     // uint32
    Ulong,    // uint64

    // Fixed-width aliases — critical for Vulkan struct layouts
    Int8,
    Int16,
    Int32,
    Int64,
    Uint8,
    Uint16,
    Uint32,
    Uint64,

    // Floating point
    Float,    // 32-bit
    Double,   // 64-bit
    Decimal,  // 128-bit, high precision

    // Text
    String,
    Char,

    // Dynamic type — accepts any value, resolved at runtime
    Any,
};

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveTypeAST
//
// Represents any built-in primitive keyword used as a type.
//
//   let x int    = 5       →  PrimitiveTypeAST { kind = Int }
//   let s string = "hi"    →  PrimitiveTypeAST { kind = String }
//   let v any    = getData()→  PrimitiveTypeAST { kind = Any }
// ─────────────────────────────────────────────────────────────────────────────

struct PrimitiveTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PrimitiveType;

    PrimitiveKind primitiveKind;  // renamed from 'kind' to avoid clash with BaseAST::kind

    explicit PrimitiveTypeAST(PrimitiveKind k)
        : TypeAST(ASTKind::PrimitiveType), primitiveKind(k) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// NamedTypeAST
//
// A user-defined type referenced by name, with optional generic arguments.
//
//   Vec2               →  NamedTypeAST { name = "Vec2",    genericArgs = {} }
//   Buffer<int>        →  NamedTypeAST { name = "Buffer",  genericArgs = [Int] }
//   Map<string, Vec2>  →  NamedTypeAST { name = "Map",     genericArgs = [String, Vec2] }
//
// genericArgs holds the concrete types supplied at the use site — e.g. the
// <int> in Buffer<int>. These are TypeAST nodes, not GenericParamAST nodes
// (which are on declarations). The semantic pass resolves the name against
// the symbol table and verifies the arg count matches the declaration.
// ─────────────────────────────────────────────────────────────────────────────

struct NamedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NamedType;

    std::string            name;         // "Vec2", "Buffer", "Map", ...
    std::vector<TypePtr>   genericArgs;  // concrete type args — empty if non-generic

    explicit NamedTypeAST(std::string n)
        : TypeAST(ASTKind::NamedType), name(std::move(n)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// NullableTypeAST
//
// Wraps an inner type with the nullable suffix `?`.
//
//   int?            →  NullableTypeAST { inner = PrimitiveTypeAST(Int) }
//   Vec2?           →  NullableTypeAST { inner = NamedTypeAST("Vec2") }
//   ((int) string)? →  NullableTypeAST { inner = FuncTypeAST(...) }
//
// Grammar rules enforced by the semantic pass:
//   - val declarations forbid ? anywhere in the entire type tree
//   - .? chain operator is only valid on NullableTypeAST targets
//   - every .? chain must be terminated by ??
// ─────────────────────────────────────────────────────────────────────────────

struct NullableTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NullableType;

    TypePtr inner;  // the type being made nullable

    explicit NullableTypeAST(TypePtr t)
        : TypeAST(ASTKind::NullableType), inner(std::move(t)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// UnionTypeAST
//
// A type that can hold any one of its member types at runtime.
//
//   int | string          →  UnionTypeAST { members = [Int, String] }
//   int | string | bool   →  UnionTypeAST { members = [Int, String, Bool] }
//
// The grammar allows chaining: type '|' type { '|' type }
// The parser flattens nested unions into a single members list — the semantic
// pass never sees nested UnionTypeASTs.
//
// members always has at least two entries — a single-member union is a
// semantic error caught by the parser.
// ─────────────────────────────────────────────────────────────────────────────

struct UnionTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::UnionType;

    std::vector<TypePtr> members;  // at least two

    UnionTypeAST() : TypeAST(ASTKind::UnionType) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// FixedArrayTypeAST
//
// An array with a compile-time constant size. Allocated inline (stack or
// struct field). Size is part of the type — [4]float and [16]float are
// different types.
//
//   [4]float    →  FixedArrayTypeAST { size = 4,  element = Float }
//   [16]float   →  FixedArrayTypeAST { size = 16, element = Float }
//   [4][4]float →  FixedArrayTypeAST { size = 4,
//                      element = FixedArrayTypeAST { size = 4, element = Float } }
//
// size is stored as uint64_t — INT_LITERAL from the lexer, always non-negative.
// The semantic pass checks that size > 0 and fits the target platform's limits.
// ─────────────────────────────────────────────────────────────────────────────

struct FixedArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FixedArrayType;

    std::uint64_t size;     // compile-time constant from INT_LITERAL
    TypePtr       element;  // element type — may itself be an array type

    FixedArrayTypeAST(std::uint64_t sz, TypePtr elem)
        : TypeAST(ASTKind::FixedArrayType), size(sz), element(std::move(elem)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// SliceTypeAST
//
// A non-owning view into an existing array (fixed or dynamic). Internally a
// fat pointer: { ptr, len, cap }. Cannot grow, does not own memory.
//
//   []int        →  SliceTypeAST { element = Int }
//   []Vec2       →  SliceTypeAST { element = NamedTypeAST("Vec2") }
//   [][*]float   →  SliceTypeAST { element = DynamicArrayTypeAST { element = Float } }
//
// Slice expressions (nums[1..3]) produce a SliceTypeAST as their resolved type.
// The semantic pass enforces that slices cannot be used as assignment targets
// (they share memory with the original — writing through the slice is valid,
// but rebinding the slice variable to a new array is not if declared imt/val).
// ─────────────────────────────────────────────────────────────────────────────

struct SliceTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::SliceType;

    TypePtr element;  // element type

    explicit SliceTypeAST(TypePtr elem)
        : TypeAST(ASTKind::SliceType), element(std::move(elem)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// DynamicArrayTypeAST
//
// A heap-owned, growable array. Tracks length and capacity. Supports push,
// pop, insert, remove, and all other mutating built-in methods.
//
//   [*]int      →  DynamicArrayTypeAST { element = Int }
//   [*]Vec2     →  DynamicArrayTypeAST { element = NamedTypeAST("Vec2") }
//   [*][*]float →  DynamicArrayTypeAST { element = DynamicArrayTypeAST { element = Float } }
//
// Semantic rules:
//   - Mutating methods (.push, .pop, .insert, .remove, .clear, .reserve) are
//     only valid when the variable is declared with 'let'
//   - Concatenation with + produces a new [*]T
// ─────────────────────────────────────────────────────────────────────────────

struct DynamicArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::DynamicArrayType;

    TypePtr element;  // element type

    explicit DynamicArrayTypeAST(TypePtr elem)
        : TypeAST(ASTKind::DynamicArrayType), element(std::move(elem)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// RefTypeAST
//
// A safe managed reference to another value. Used for struct fields that
// point to other structs (linked lists, trees) and for passing large values
// by reference without copying.
//
//   &int    →  RefTypeAST { inner = PrimitiveTypeAST(Int) }
//   &Vec2   →  RefTypeAST { inner = NamedTypeAST("Vec2") }
//
// References are always valid (non-nullable by default). To express a
// nullable reference, wrap in NullableTypeAST: &Vec2?
// ─────────────────────────────────────────────────────────────────────────────

struct RefTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::RefType;

    TypePtr inner;  // the referenced type

    explicit RefTypeAST(TypePtr t)
        : TypeAST(ASTKind::RefType), inner(std::move(t)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// PtrTypeAST
//
// A raw, unmanaged pointer. Only valid inside extern declarations — using @T
// outside of an extern context is a semantic error.
//
//   @uint8    →  PtrTypeAST { inner = PrimitiveTypeAST(Uint8) }
//   @VkInstance → PtrTypeAST { inner = NamedTypeAST("VkInstance") }
//
// The semantic pass enforces the extern-only restriction by checking that
// every PtrTypeAST appears only inside an ExternDeclAST subtree.
// ─────────────────────────────────────────────────────────────────────────────

struct PtrTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PtrType;

    TypePtr inner;  // the pointed-to type

    explicit PtrTypeAST(TypePtr t)
        : TypeAST(ASTKind::PtrType), inner(std::move(t)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// FuncTypeAST
//
// The type of a first-class function value. Covers both the standard form and
// the nullable-function form.
//
// Standard form:
//   (int) string          →  FuncTypeAST { params = [Int], returnType = String,
//                                          isNullable = false }
//   (req Request) Response →  FuncTypeAST { params = [Request], returnType = Response,
//                                           isNullable = false }
//   ()                    →  FuncTypeAST { params = [],    returnType = nil,
//                                          isNullable = false }
//
// Nullable function form — the function itself may be nil:
//   ((int) string)?       →  FuncTypeAST { params = [Int], returnType = String,
//                                          isNullable = true }
//
// Note: a nullable *return type* is expressed as returnType = NullableTypeAST,
// not by setting isNullable = true. isNullable = true means the function
// variable itself can be nil — these are two different things:
//
//   (int) string?         →  return type is nullable  (isNullable = false)
//   ((int) string)?       →  function itself nullable (isNullable = true)
//
// params stores only the types — parameter names are not part of a function
// type, only part of a function declaration. The parser discards names here.
//
// Curried functions desugar at the declaration level — a curried type
// (a int)(b int) int is represented as:
//   FuncTypeAST { params = [Int], returnType = FuncTypeAST { params = [Int],
//                                                            returnType = Int } }
// ─────────────────────────────────────────────────────────────────────────────

struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;

    std::vector<TypePtr>  params;       // parameter types in order, no names
    TypePtr               returnType;   // nullptr = no return (void equivalent)
    bool                  isNullable;   // true → ((params) ret)?  function itself is nil-able

    explicit FuncTypeAST(bool nullable = false)
        : TypeAST(ASTKind::FuncType), returnType(nullptr), isNullable(nullable) {}

    // Convenience — returns true when the function has no return value
    bool isVoid() const { return returnType == nullptr; }

    void accept(ASTVisitor& v) override { v.visit(*this); }
};
/**
 * @file TypeAST.hpp
 *
 * @responsibility Defines the syntactic representation of types (Primitive, Array, Pointer).
 * 
 * @hierarchy BaseAST -> TypeAST -> [Concrete Nodes]
 *
 * @related_files 
 *   - src/parser/ParserType.cpp (The primary producer)
 * 
 * @note These represent types AS WRITTEN in source. The semantic pass later resolves 
 *       these into actual Type objects.
 */

#pragma once

#include "BaseAST.hpp"
#include "registry/QualifierRegistry.hpp"

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
//   FixedArrayTypeAST     — [N]T   compile-time size, stack allocated
//   SliceTypeAST          — []T    fat-pointer view, no ownership
//   DynamicArrayTypeAST   — [*]T   heap-owned, growable
//   RefTypeAST            — &T     safe managed reference
//   PtrTypeAST            — *T     raw pointer, extern/FFI only
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
//
// isGenericParam (Semantic Phase):
//   Set to true by TypeResolver::visit(NamedTypeAST) when the name matches a
//   generic type parameter declared on the enclosing declaration (e.g. T in
//   struct Box<T> or let process<T>). This distinguishes abstract parameters
//   like T from concrete types like Circle or int.
//
//   Codegen uses this flag in Pass 0 to skip instantiation collection for
//   abstract uses — InstKey{"Box", ["T"]} is meaningless and must not be
//   recorded. Only NamedTypeASTs with isGenericParam == false represent
//   concrete types suitable for monomorphization.
// ─────────────────────────────────────────────────────────────────────────────

struct NamedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NamedType;

    InternedString         name;              // "Vec2", "Buffer", "Map", ...
    std::vector<TypePtr>   genericArgs;       // concrete type args — empty if non-generic

    // ── Semantic annotation (written by TypeResolver, read by codegen) ────────
    // True when this name refers to a generic type parameter (e.g. T, K, V)
    // rather than a declared struct, enum, or type alias. Set during Phase 2a
    // (TypeResolver::visit). Never true after TypeAlias unwrapping.
    bool isGenericParam = false;

    explicit NamedTypeAST(InternedString n)
        : TypeAST(ASTKind::NamedType), name(n) {}

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
//   - ?. chain operator is only valid on NullableTypeAST targets
//   - every ?. chain must be terminated by ??
// ─────────────────────────────────────────────────────────────────────────────

struct NullableTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NullableType;

    TypePtr inner;  // the type being made nullable

    explicit NullableTypeAST(TypePtr t)
        : TypeAST(ASTKind::NullableType), inner(std::move(t)) {}

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
// A raw, unmanaged pointer.
//
// THE SEALED CONDUIT MODEL:
// Raw pointers (*T) are treated as "sealed conduits". You can carry them,
// pass them to @extern functions, and check if they are nil.
// To work with the memory they point to, you must explicitly "unseal" them
// by crossing the safety boundary using the @ptrToRef intrinsic.
//
// Allowed Operations (Zero unsafe surface):
//   1. Store the pointer in a variable (const buf *uint8 = malloc(1024))
//   2. Pass to @extern functions (free(buf))
//   3. Nil check (if buf == nil { ... })
//   4. Print the pointer (for debugging/experimenting with memory addresses)
//
// Forbidden Operations (Compiler Error):
//   - Dereference (*ptr)     — Syntax not supported for pointers
//   - Field access (ptr.f)   — Must cross to reference first
//   - Indexing (ptr[i])      — Must cross to slice or reference
//   - Arithmetic (ptr + 4)   — Use @ptrOffset intrinsic instead
//   - Assignment (*ptr = x)  — Must cross to reference first
//
// Boundary Crossing:
//   @ptrToRef(T, ptr) -> &T   (Explicit assertion of validity)
//   @refToPtr(ref)    -> *T   (Convert safe reference back to raw pointer)
//   @ptrOffset(ptr, n)-> *T   (Pointer arithmetic)
//
// PtrTypeAST is only valid in:
//   - @extern-decorated declarations
//   - Input/output types of pointer-related intrinsics (@ptrToRef, etc.)
//   - Variables/parameters holding values returned by @extern / intrinsics
// ─────────────────────────────────────────────────────────────────────────────

struct PtrTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PtrType;

    TypePtr inner;  // the pointed-to type

    explicit PtrTypeAST(TypePtr t)
        : TypeAST(ASTKind::PtrType), inner(std::move(t)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// FuncSignature
//
// Plain data — not a BaseAST, not visited.
// Holds the signature data shared by FuncTypeAST and declarations.
// ─────────────────────────────────────────────────────────────────────────────
struct FuncSignature {
    std::vector<std::vector<ASTPtr<ParamAST>>> paramGroups;
    TypePtr                  returnType;
    bool                     isNullable   = false;
    uint32_t                 qualifiers   = 0;
    std::vector<InternedString> rawQualifiers;  // only needed during parsing

    // Zero‑cost helpers — direct bitmask test
    bool hasQualifier(uint32_t bit) const { return (qualifiers & bit) != 0; }
    bool isAsync()    const { return hasQualifier(QualifierBits::Async); }
    bool isParallel() const { return hasQualifier(QualifierBits::Parallel); }

    bool hasParams()  const {
        for (const auto& group : paramGroups) {
            if (!group.empty()) return true;
        }
        return false;
    }
    size_t totalParamCount() const {
        size_t count = 0;
        for (const auto& group : paramGroups) {
            count += group.size();
        }
        return count;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FuncTypeAST
//
// THE UNIFIED FUNCTION TYPE NODE
//
// Used for function TYPES in annotations (e.g., let callback (int) string).
// Declarations now embed FuncSignature directly to avoid the "embedded node" problem.
// ─────────────────────────────────────────────────────────────────────────────
struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;
    
    FuncSignature sig;
    
    explicit FuncTypeAST() : TypeAST(ASTKind::FuncType) {}
    
    void accept(ASTVisitor& v) override { v.visit(*this); }
};


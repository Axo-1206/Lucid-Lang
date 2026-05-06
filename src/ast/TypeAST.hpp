/**
 * @file TypeAST.hpp
 *
 * @responsibility Defines the syntactic representation of types (Primitive, Union, Array, Pointer).
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
#include "Tokens.hpp"
#include "QualifierRegistry.hpp"

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

    std::string            name;              // "Vec2", "Buffer", "Map", ...
    std::vector<TypePtr>   genericArgs;       // concrete type args — empty if non-generic

    // ── Semantic annotation (written by TypeResolver, read by codegen) ────────
    // True when this name refers to a generic type parameter (e.g. T, K, V)
    // rather than a declared struct, enum, or type alias. Set during Phase 2a
    // (TypeResolver::visit). Never true after TypeAlias unwrapping.
    bool isGenericParam = false;

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
// ParamInfo
//
// Represents a single parameter in a function type or declaration.
// In type position (e.g., (int, string) -> bool), names are empty.
// In declaration position (e.g., (x int, y string)), names are preserved.
// ─────────────────────────────────────────────────────────────────────────────
struct ParamInfo {
    std::string name;           // Empty for type position, filled for declarations
    TypePtr type;               // Parameter type
    bool isVariadic = false;    // true for ...type (variadic parameter)
    SourceLocation loc;         // For error reporting
    
    ParamInfo() = default;
    ParamInfo(std::string n, TypePtr t, bool variadic = false, SourceLocation l = {})
        : name(std::move(n)), type(std::move(t)), isVariadic(variadic), loc(l) {}
};

// Convenience aliases
using ParamInfoPtr = std::unique_ptr<ParamInfo>;
using ParamGroup = std::vector<ParamInfo>;

// ─────────────────────────────────────────────────────────────────────────────
// FuncTypeAST
//
// THE UNIFIED FUNCTION TYPE NODE
//
// Used for BOTH:
//   1. Function TYPES in annotations (e.g., let callback (int) string)
//   2. Function SIGNATURES in declarations (e.g., let add (a int) (b int) int)
//
// Parameter names are optional:
//   - In type position: name fields are empty
//   - In declaration position: name fields contain parameter names
//
// This eliminates the need for separate FuncSignatureAST or duplicating
// paramGroups across FuncDeclAST, MethodDeclAST, TraitMethodAST, AnonFuncExprAST.
//
// Standard form:
//   (int) string          →  paramGroups = [[{name="", type=Int}]], returnType=String
//   (a int) (b int) int   →  paramGroups = [[{name="a", type=Int}], [{name="b", type=Int}]], returnType=Int
//   ()                    →  paramGroups = [], returnType=nullptr
//
// Nullable function form — the function itself may be nil:
//   ((int) string)?       →  isNullable = true
//
// Note: a nullable *return type* is expressed as returnType = NullableTypeAST,
// not by setting isNullable = true.
//
// Type Qualifiers (~async, ~noinline, etc.)
// ─────────────────────────────────────────────────────────────────────────────
// DESIGN: "Parse first, validate later"
//
// Phase 1 (Parser):
//   - Parser collects raw qualifier names as strings
//   - No validation, no bitmask conversion
//   - Raw names stored in rawQualifiers vector
//
// Phase 2 (TypeResolver - Semantic):
//   - Validates each qualifier name against QualifierRegistry
//   - Converts valid names to bitmask in 'qualifiers' field
//   - Reports error for unknown qualifiers
//   - Clears rawQualifiers after conversion
// ─────────────────────────────────────────────────────────────────────────────
struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;
    
    // ── Core signature components ────────────────────────────────────────────
    std::vector<ParamGroup> paramGroups;  // Outer vector = curry groups
    TypePtr returnType;                    // nullptr = void
    bool isNullable = false;               // Function itself nullable? ((T) U)?
    
    // ── Type qualifiers (bitmask for runtime efficiency) ─────────────────────
    // Set by TypeResolver during semantic phase
    uint32_t qualifiers = 0;
    
    // ── Raw qualifier names (parser output, semantic input) ──────────────────
    std::vector<std::string> rawQualifiers;
    
    // ── Helper methods ───────────────────────────────────────────────────────
    
    // Qualifier checks
    bool hasQualifier(const std::string& name) const {
        uint32_t bit = QualifierRegistry::instance().getBit(name);
        return bit != 0 && (qualifiers & bit);
    }
    
    bool isAsync() const { return hasQualifier("async"); }
    bool isNoInline() const { return hasQualifier("noinline"); }
    
    // Check if qualifiers have been resolved
    bool isResolved() const { return rawQualifiers.empty(); }
    
    // Parameter queries
    bool hasParams() const {
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
    
    // For type comparison: get parameter types only (ignoring names)
    std::vector<std::vector<TypeAST*>> getParamTypes() const {
        std::vector<std::vector<TypeAST*>> result;
        result.reserve(paramGroups.size());
        for (const auto& group : paramGroups) {
            std::vector<TypeAST*> types;
            types.reserve(group.size());
            for (const auto& param : group) {
                types.push_back(param.type.get());
            }
            result.push_back(std::move(types));
        }
        return result;
    }
    
    // Check structural equality (ignores parameter names)
    bool structurallyEquals(const FuncTypeAST& other) const {
        if (qualifiers != other.qualifiers) return false;
        if (isNullable != other.isNullable) return false;
        
        // Compare param groups count
        if (paramGroups.size() != other.paramGroups.size()) return false;
        
        // Compare each param group's types
        for (size_t i = 0; i < paramGroups.size(); ++i) {
            if (paramGroups[i].size() != other.paramGroups[i].size()) return false;
            for (size_t j = 0; j < paramGroups[i].size(); ++j) {
                // Need TypeChecker::isEqual for this, but that's in semantic phase
                // For now, just compare pointer equality or use a helper
                if (paramGroups[i][j].type.get() != other.paramGroups[i][j].type.get()) {
                    // Different type pointers - may still be structurally equal
                    // This will be handled by TypeChecker::isEqual later
                    return false;
                }
            }
        }
        
        // Compare return types
        return returnType.get() == other.returnType.get();
    }
    
    explicit FuncTypeAST(bool nullable = false)
        : TypeAST(ASTKind::FuncType), isNullable(nullable) {}
    
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

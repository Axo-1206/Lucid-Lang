/// @file TypeAST.hpp
/// 
/// @responsibility Defines the syntactic representation of types (Primitive, Array, Pointer, Function).
/// 
/// @hierarchy BaseAST → TypeAST → [Concrete Nodes]
/// 
/// @related_files
///   - src/parser/ParserType.cpp – primary producer of these nodes
///   - src/semantic/TypeResolver.cpp – resolves types to semantic representations
/// 
/// @note These represent types **as written** in source. The semantic pass later
///       resolves these into actual resolved Type objects.

#pragma once

#include "BaseAST.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

/// @brief Distinguishes the three array types in Lucid.
/// 
/// - Slice   : non‑owning view (`[_]T`)
/// - Dynamic : heap‑owned, growable (`[*]T`)
/// - Fixed   : stack/inline, compile‑time size (`[N]T`)
enum class ArrayKind {
    Slice,   // [_]T
    Dynamic, // [*]T
    Fixed    // [N]T
};

/// @brief Identifies a primitive type in the type system.
/// 
/// The parser maps token types (e.g., `TYPE_INT`) to this enum.
/// The semantic pass and codegen read `PrimitiveKind` directly.
/// 
/// @note Fixed‑width types (`int8`, `uint32`, etc.) are critical for
///       Vulkan struct layouts and FFI compatibility.
enum class PrimitiveKind {
    // Boolean
    Bool,

    // Signed integers (machine‑dependent sizes)
    Byte,     // int8,  -128..127
    Short,    // int16
    Int,      // int32
    Long,     // int64

    // Unsigned integers (machine‑dependent sizes)
    Ubyte,    // uint8,  0..255
    Ushort,   // uint16
    Uint,     // uint32
    Ulong,    // uint64

    // Fixed‑width aliases – critical for Vulkan struct layouts
    Int8,
    Int16,
    Int32,
    Int64,
    Uint8,
    Uint16,
    Uint32,
    Uint64,

    // Floating point
    Float,    // 32‑bit
    Double,   // 64‑bit
    Decimal,  // 128‑bit, high precision

    // Text
    String,
    Char,
};

/// @brief Represents a primitive type keyword.
/// 
/// @example
///   let x int    = 5       → PrimitiveKind::Int
///   let s string = "hi"    → PrimitiveKind::String
///   let b bool   = true    → PrimitiveKind::Bool
struct PrimitiveTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PrimitiveType;

    PrimitiveKind primitiveKind;

    explicit PrimitiveTypeAST(PrimitiveKind k)
        : TypeAST(ASTKind::PrimitiveType), primitiveKind(k) {}
};

/// @brief References a user‑defined type by name, with optional generic arguments.
/// 
/// @example
///   Vec2               → name = "Vec2",    genericArgs = {}
///   Buffer<int>        → name = "Buffer",  genericArgs = [Int]
///   Map<string, Vec2>  → name = "Map",     genericArgs = [String, Vec2]
/// 
/// `genericArgs` holds the concrete types supplied at the use site (e.g., the
/// `<int>` in `Buffer<int>`). These are `TypeAST` nodes, not `GenericParamAST`.
/// The semantic pass resolves the name against the symbol table and verifies
/// the argument count matches the declaration.
/// 
/// ## Semantic Annotation: `isGenericParam`
/// 
/// Set to `true` by `TypeResolver` when the name matches a generic type parameter
/// declared on the enclosing declaration (e.g., `T` in `struct Box<T>` or `const process<T>`).
/// This distinguishes abstract parameters like `T` from concrete types like `Circle`.
/// 
/// Codegen uses this flag to skip instantiation collection for abstract uses –
/// `InstKey{"Box", ["T"]}` is meaningless and must not be recorded.
/// 
/// @note Named types hold generic arguments, not generic parameters.
struct NamedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NamedType;

    InternedString name;
    ArenaSpan<TypeAST*> genericArgs;

    explicit NamedTypeAST(InternedString n)
        : TypeAST(ASTKind::NamedType), name(n) {}
};

/// @brief Wraps an inner type with the nullable suffix `?`.
/// 
/// @example
///   int?        → inner = PrimitiveTypeAST(Int)
///   Vec2?       → inner = NamedTypeAST("Vec2")
///   User?       → inner = NamedTypeAST("User")
/// 
/// Grammar rules enforced by the semantic pass:
///   - `?` attaches to value types only (primitives, structs, enums, traits)
///   - `?` is **not** valid on array types (`[*]int?`) — use `[*]int?` for
///     array of nullable elements, or `~nullable [*]int` for nullable array
///   - `?` is **not** valid on function types (`(int) -> bool?`)
/// 
/// @see CombinedTypeAST for `T?!` (nullable + fallible combined)
struct NullableTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NullableType;

    TypeAST* inner = nullptr;

    explicit NullableTypeAST(TypeAST* t)
        : TypeAST(ASTKind::NullableType), inner(t) {}
};

/// @brief Wraps an inner type with the fallible suffix `!`.
/// 
/// @example
///   int!        → inner = PrimitiveTypeAST(Int)
///   string!     → inner = PrimitiveTypeAST(String)
///   User!       → inner = NamedTypeAST("User")
/// 
/// Grammar rules enforced by the semantic pass:
///   - `!` attaches to value types only (primitives, structs, enums, traits)
///   - `!` is **not** valid on array types (`[*]int!`) — use `[*]int!` for
///     array of fallible elements
///   - `!` is **not** valid on function types (`(int) -> bool!`)
/// 
/// @see CombinedTypeAST for `T?!` (nullable + fallible combined)
struct FallibleTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FallibleType;

    TypeAST* inner = nullptr;

    explicit FallibleTypeAST(TypeAST* t)
        : TypeAST(ASTKind::FallibleType), inner(t) {}
};

/// @brief Represents a type that is both nullable and fallible: `T?!`
/// 
/// @example
///   int?!       → inner = PrimitiveTypeAST(Int)
///   User?!      → inner = NamedTypeAST("User")
/// 
/// A `T?!` value is a genuine three-state value. Narrowing must rule out both
/// sentinels before the plain `T` is usable.
/// 
/// Grammar rules enforced by the semantic pass:
///   - `?!` is the only valid order — `!?` is rejected by the parser
///   - Same restrictions as `?` and `!` individually apply
/// 
/// @note This is a distinct type from `NullableTypeAST` + `FallibleTypeAST`
///       composition. The combined type has three states (T, nil, err) while
///       `T?` has two (T, nil) and `T!` has two (T, err).
struct CombinedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::CombinedType;

    TypeAST* inner = nullptr;

    explicit CombinedTypeAST(TypeAST* t)
        : TypeAST(ASTKind::CombinedType), inner(t) {}
};

/// @brief The pending result of an `async` operation — `Future<T>`.
/// 
/// @example
///   async result = fetchData(url);   -- result : Future<int>
///   await result;                    -- result : int, from here on
/// 
/// ## Linear Value Rules
/// 
/// A `Future<T>` represents a scheduled operation that has not yet
/// completed. It is a **linear value**: it must be consumed by exactly one
/// `await` before it can be used, and exactly one `await` is all it can
/// ever accept. This is a distinct category from every other type in the
/// Ownership Categories table — not Owned (copying it cannot duplicate the
/// single underlying scheduled operation), not Shared/refcounted (it is not
/// meant to have more than one simultaneous holder at all), and not
/// Borrowed (it has no source it must not outlive; the hazard is
/// double-consumption, not dangling).
/// 
/// 0. **`inner` is always written explicitly at the `async`/`spawn` site,
///    never inferred.** `AsyncStmtAST::binding`/`SpawnStmtAST::binding` are
///    always `VarDeclAST*` with a non-null `type` — the parser wraps the
///    written inner type into `FutureTypeAST` eagerly, the same way it
///    wraps `T` into `NullableTypeAST`/`FallibleTypeAST` for `?`/`!`.
///    There is no inferred-type binding form anywhere in Lucid, and this
///    construct does not introduce the first one — see `AsyncStmtAST` and
///    `SpawnStmtAST` for the full reasoning.
/// 1. **Use-before-await is a compile error.** Resolved via the same
///    flow-sensitive narrowing machinery as `T?`/`T!` — `ExprAST::valueState`
///    — not a separate runtime check. Before `await`, the identifier's
///    state is `Future<T>` (unresolved); after, narrowing rewrites it to
///    plain `T` for the rest of the enclosing scope, exactly as a
///    successful nil-check narrows `T?` to `T`.
/// 2. **Not copyable.** `let copy = result;` is a compile error while
///    `result`'s type is `Future<T>`. There must never be two bindings that
///    could each independently attempt to consume the same underlying
///    operation — this is what prevents double-await, rather than relying
///    on a lookup table to correctly propagate "pending" status through
///    every possible copy site (assignment, parameter pass, return,
///    struct/array storage).
/// 3. **May only exist as a local variable or a function parameter.**
///    Never a struct field, never an array/slice element. This closes off
///    every indirect path into rule 4 below — a `Future<T>` cannot be
///    smuggled into a closure's capture set through a container it's
///    allowed to sit in.
/// 4. **A closure literal may never capture a `Future<T>`**, whether the
///    variable arrived by direct capture from an enclosing scope or as the
///    enclosing function's own parameter. This is a **separate** rule from
///    the Downward Flow Rule's closure-capture restriction on `&T`/`[_]T`
///    — that one exists because a closure might *outlive* a borrowed
///    view's source; this one exists because a closure might *run more
///    than once*, and a plain function body (which runs exactly once per
///    call) does not have that problem. Do not conflate the two
///    justifications when diagnosing a rejection.
/// 5. **Must be consumed on every control-flow path out of the scope that
///    holds it.** A live, un-awaited `Future<T>` reaching scope exit —
///    including via `return`, `break`, or any branch of an `if`/`switch`
///    — is a compile error, not a warning. This requires genuine
///    reachability analysis (every exit path checked for a live unresolved
///    future), the same family of dataflow pass as return-exhaustiveness
///    checking, run in the must-consume direction instead of the
///    must-assign direction.
/// 
/// @see ThreadTypeAST for the `spawn`/`join` equivalent — identical rules,
///      substituting `spawn` for `async` and `join` for `await`.
struct FutureTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FutureType;

    TypeAST* inner = nullptr;

    explicit FutureTypeAST(TypeAST* t)
        : TypeAST(ASTKind::FutureType), inner(t) {}
};

/// @brief The pending result of a `spawn` operation — `Thread<T>`.
/// 
/// @example
///   spawn result = computeHeavyData();   -- result : Thread<int>
///   join result;                         -- result : int, from here on
///   spawn _ = logToFile("started");      -- discard pattern, no Thread<T> binding at all
/// 
/// Identical linear-value rules to `FutureTypeAST` (use-before-join is a
/// compile error via narrowing, not copyable, local/parameter-only storage,
/// never captured by a closure, must be joined on every control-flow path)
/// — see `FutureTypeAST` for the full rationale behind each rule. The
/// discard pattern (`spawn _ = fn()`) never produces a `Thread<T>` binding
/// in the first place, so none of these rules apply to it — there is
/// nothing to double-join, capture, or leave unconsumed.
struct ThreadTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ThreadType;

    TypeAST* inner = nullptr;

    explicit ThreadTypeAST(TypeAST* t)
        : TypeAST(ASTKind::ThreadType), inner(t) {}
};

/// @brief Represents a concrete array type: slice, dynamic, or fixed.
/// 
/// This node unifies the three array kinds under a single representation.
/// The `kind` field determines which memory model applies.
/// 
/// Grammar:
///   array_type := '[' '*' ']' type      -- owned heap array
///               | '[' '_' ']' type      -- slice (borrowed view)
///               | '[' INT_LITERAL ']' type   -- fixed-size stack array
/// 
/// Examples:
///   [*]int   → kind = Dynamic, element = Int
///   [_]float → kind = Slice,   element = Float
///   [4]Vec2  → kind = Fixed,   size = 4, element = Vec2
/// 
/// @note `?` and `!` annotations apply to the element type, not the array itself:
///   `[*]int?`  → array of nullable int
///   `[*]int!`  → array of fallible int
/// 
/// @field kind    The array kind (Slice, Dynamic, Fixed).
/// @field size    Only valid when `kind == Fixed`; ignored otherwise (should be 0).
/// @field element The element type (may itself be an array, function type, etc.).
struct ArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayType;

    ArrayKind arrayKind;
    uint64_t size;
    TypeAST* element = nullptr;

    ArrayTypeAST(ArrayKind k, uint64_t sz, TypeAST* elem)
        : TypeAST(ASTKind::ArrayType), arrayKind(k), size(sz), element(elem) {}

    bool isFixed()   const { return arrayKind == ArrayKind::Fixed; }
    bool isSlice()   const { return arrayKind == ArrayKind::Slice; }
    bool isDynamic() const { return arrayKind == ArrayKind::Dynamic; }
};

/// @brief A safe managed reference to another value.
/// 
/// @example
///   &int    → inner = PrimitiveTypeAST(Int)
///   &Vec2   → inner = NamedTypeAST("Vec2")
/// 
/// References are always valid (non‑nullable by default). To express a nullable
/// reference, wrap in `NullableTypeAST`: `&Vec2?`.
/// 
/// ## The Downward Flow Rule (Reference Scoping)
/// 
/// References (`&T`) are strictly scoped. They are allowed to flow *downward*
/// (into nested calls), but never *upward or sideways*:
/// 
/// 1. **No Struct Storage:** A struct field cannot have a reference type.
/// 2. **No Array/Slice Storage:** An array or slice cannot store reference types.
/// 3. **No Reference Returns:** A function cannot return a reference type.
/// 
/// As a result, a reference (`&T`) can only exist in two places:
///   - As a **function parameter** (e.g., `const process (p &Player)`)
///   - As a **local variable alias** inside a block (e.g., `let ref &Weapon = player.weapon`)
struct RefTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::RefType;

    TypeAST* inner = nullptr;

    explicit RefTypeAST(TypeAST* t)
        : TypeAST(ASTKind::RefType), inner(t) {}
};

/// @brief A raw, unmanaged pointer – the **sealed conduit**.
/// 
/// ## The Sealed Conduit Model
/// 
/// Raw pointers (`*T`) are sealed conduits. You can carry them, pass them to
/// `@[foreign("C")]` functions, check for nil, but never dereference directly.
/// 
/// **Allowed operations:**
/// 1. Store in a variable, struct field, or parameter
/// 2. Pass to a `@[foreign("C")]` function
/// 3. Nil check (`== nil`, `!= nil`)
/// 4. Pass to pointer intrinsics (`#toRef`, `#ptrOffset`, etc.)
/// 5. Print the address for debugging
/// 
/// **Forbidden operations (compiler error):**
///   - Dereferencing: `*ptr`
///   - Field access: `ptr.field`
///   - Indexing: `ptr[i]`
///   - Arithmetic: `ptr + 4` – use `#ptrOffset` instead
///   - Assignment: `*ptr = value`
///   - Type casting/conversion: `ptr<float>(x)` – use `#toRef` or `#toPtr`
/// 
/// **Boundary crossing (intrinsics):**
///   - `#toRef(ptr) -> &T`   (assert validity, cross to safe reference)
///   - `#toPtr(ref) -> *T` (convert back to raw pointer)
///   - `#ptrOffset(ptr, n) -> *T` (pointer arithmetic)
///   - `#ptrDiff(p1, p2) -> int64` (distance between pointers)
/// 
/// **Valid contexts for `PtrTypeAST`:**
///   - `@[foreign("C")]`-decorated declarations
///   - Input/output types of pointer‑related intrinsics
///   - Variables/parameters holding values returned by `@[foreign("C")]` / intrinsics
struct PtrTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PtrType;

    TypeAST* inner = nullptr;

    explicit PtrTypeAST(TypeAST* t)
        : TypeAST(ASTKind::PtrType), inner(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FuncTypeAST — function type.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a function type with a single parameter group.
/// 
/// This is a recursive design: a function type consists of one parameter group
/// and one return type. If the function is curried, the return type
/// is another FuncTypeAST.
/// 
/// Grammar (desugared):
///   func_type := param_group [ '->' returnType ]
/// 
/// The parser desugars multiple parameter groups (e.g., `(a int)(b int) -> int`)
/// into nested FuncTypeAST: `(a int) -> (b int) -> int`
/// 
/// Examples of nested structure:
///   - `(a int) -> int`                    → params=[a], returnType = int
///   - `(a int) -> (int) -> int`           → params=[a], returnType = FuncTypeAST(...)
///   - `(a int) -> Pair<int, string>`      → params=[a], returnType = Pair<int, string>
///   - `(a int)(b int) -> int`             → desugars to `(a int) -> (b int) -> int`
/// 
/// @field params        The parameters for this group (raw pointers to ParamAST)
/// @field returnType   Return type – a plain TypeAST or another FuncTypeAST
struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;

    ArenaSpan<ParamAST*> params;      // parameters for this group
    TypeAST* returnType = nullptr;     // return types (may contain FuncTypeAST)
    bool hasArrow = false;            // semantic enforce return statement inside the body
                                      // and codegen will automatically wrap function

    explicit FuncTypeAST() : TypeAST(ASTKind::FuncType) {}
    
    // Returns true if the return type is a function type (currying)
    bool isCurried() const { 
        return returnType->isa<FuncTypeAST>();
    }

    // Returns the inner function type if curried, otherwise nullptr
    FuncTypeAST* getNext() const {
        if (isCurried()) {
            return returnType->as<FuncTypeAST>();
        }
        return nullptr;
    }
};

/// @brief Accesses a type from a module via the ':' operator.
/// 
/// This is used ONLY for type names, not values. It resolves a type
/// reference that is qualified with a module name.
/// 
/// @example
///   parser:Result      → module = "parser", typeName = "Result"
///   sql:Result         → module = "sql", typeName = "Result"
///   std:io:File        → module = "std:io", typeName = "File" (nested module)
/// 
/// @note This node is used when a type name conflict exists and the user
///       must disambiguate with module qualification. For unqualified
///       type names, the resolver looks up the type directly.
/// 
/// @field moduleName    The module name (left-hand side of `:`).
/// @field typeName      The type name (right-hand side of `:`).
/// @field genericArgs   Generic arguments if the type is generic.
struct ModuleTypeAccessAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ModuleTypeAccess;

    InternedString moduleName;
    InternedString typeName;
    ArenaSpan<TypeAST*> genericArgs;

    ModuleTypeAccessAST() : TypeAST(ASTKind::ModuleTypeAccess) {}
};
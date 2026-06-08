/**
 * @file SemanticSymbol.hpp
 * @brief Semantic representation of resolved declarations for name lookup.
 * 
 * ============================================================================
 * DESIGN PRINCIPLES
 * ============================================================================
 * 
 * The symbol table is for NAME LOOKUP only. It does NOT store:
 *   - Modules (ProgramAST) – tracked by the driver
 *   - Packages – collections of modules, not lookup targets
 *   - Imports (UseDeclAST) – handled by module resolution, not symbol lookup
 * 
 * What belongs in the symbol table:
 *   - Types that can be referenced by name (struct, enum, trait, type alias)
 *   - Values that can be referenced by name (var, func, param, field, method)
 *   - Enum variants (accessed as TypeName.Variant)
 * 
 * ============================================================================
 * SYMBOL HIERARCHY (matches AST kind hierarchy)
 * ============================================================================
 * 
 * AST Kind                    → Symbol Kind
 * ----------------------------|-----------------------------------------------
 * ProgramAST                  → NOT a symbol (compilation unit)
 * PackageDeclAST              → NOT a symbol (package declaration)
 * UseDeclAST                  → NOT a symbol (import, handled by module loader)
 * 
 * StructDeclAST               → SymbolKind::Struct
 * EnumDeclAST                 → SymbolKind::Enum
 * TraitDeclAST                → SymbolKind::Trait
 * TypeAliasDeclAST            → SymbolKind::TypeAlias
 * 
 * FuncDeclAST                 → SymbolKind::Func
 * VarDeclAST                  → SymbolKind::Var
 * ParamAST                    → SymbolKind::Param
 * FieldDeclAST                → SymbolKind::Field
 * MethodDeclAST               → SymbolKind::Method
 * EnumVariantAST              → SymbolKind::EnumVariant
 * 
 * GenericParamAST             → SymbolKind::GenericParam (for type parameters)
 * ImplDeclAST                 → SymbolKind::Impl (for conformance checking)
 * FromDeclAST                 → SymbolKind::From (for implicit conversions)
 * 
 * ============================================================================
 * NAMESPACE SEPARATION
 * ============================================================================
 * 
 * Types and values live in separate namespaces, allowing:
 *   struct Point { ... }   -- 'Point' in type namespace
 *   let Point = 42         -- 'Point' in value namespace (allowed!)
 * 
 * Value namespace:  Func, Var, Param, Field, Method, EnumVariant
 * Type namespace:   Struct, Enum, Trait, TypeAlias
 * Both:             GenericParam, Impl, From (context dependent)
 * 
 * ============================================================================
 * OVERLOADING DESIGN DECISIONS
 * ============================================================================
 * 
 * ─── Return Type NOT Considered ───────────────────────────────────────────
 * 
 *   DECISION: Return type is NOT considered in overload resolution.
 * 
 *   RATIONALE:
 *     1. Preserves type inference: `let x = get()` would be ambiguous
 *     2. Consistent with pipeline operator: `42 |> get` must be unambiguous
 *     3. Match expressions: `case 0 => get()` has no type context
 *     4. C++/Java/C#/Rust precedent – return type overloading is rare
 *     5. Simpler implementation and error messages
 * 
 *   Example (VALID):
 *     let add (a int, b int) -> int = { return a + b }
 *     let add (a float, b float) -> float = { return a + b }
 *     add(1, 2)     -- calls int version
 *     add(1.5, 2.5) -- calls float version
 * 
 *   Example (INVALID – would be rejected):
 *     let get () -> int = { return 42 }
 *     let get () -> string = { return "hello" }
 *     let x = get()  -- ERROR: ambiguous call
 * 
 * ─── Curried Functions Cannot Be Overloaded ───────────────────────────────
 * 
 *   DECISION: Curried functions cannot participate in overloading.
 * 
 *   RATIONALE:
 *     1. Partial application is ambiguous: `let f = add(5)` – which version?
 *     2. Curried functions are first-class values; overloading requires
 *        complete signature at call site
 *     3. The return type of a curried function is another function,
 *        creating infinite ambiguity
 *     4. Languages with currying (Haskell, OCaml, F#) do not support
 *        overloading on curried functions
 * 
 *   Example (VALID – single curried function):
 *     let add (a int)(b int) -> int = { return a + b }
 *     let add5 = add(5)    -- partial application – works
 * 
 *   Example (INVALID – would be rejected):
 *     let add (a int)(b int) -> int = { return a + b }
 *     let add (a float)(b float) -> float = { return a + b }
 *     ERROR: curried functions cannot be overloaded
 * 
 * ─── Overload Resolution Priority ─────────────────────────────────────────
 * 
 *   Priority (highest to lowest):
 *     1. Exact parameter type match
 *     2. Numeric promotion (int → float, byte → int)
 *     3. Implicit conversion (via `from` declarations)
 *     4. Generic instantiation (with explicit type args)
 * 
 *   NOT considered:
 *     - Return type
 *     - Currying arity (handled by type system, not overload resolution)
 *     - Parameter names
 * 
 * ─── Overloadable Symbol Kinds ────────────────────────────────────────────
 * 
 *   Only these symbols can be overloaded:
 *     - Func (regular functions)
 *     - ExternFunc (extern functions)
 *     - Method (methods inside impl blocks)
 * 
 *   These symbols CANNOT be overloaded:
 *     - Var (variables)
 *     - Param (parameters)
 *     - Field (struct fields)
 *     - EnumVariant (enum variants)
 *     - Struct, Enum, Trait, TypeAlias (types)
 * 
 * ============================================================================
 * METHOD OVERLOADING (within impl blocks)
 * ============================================================================
 * 
 *   Methods in the same impl block can be overloaded based on parameter types.
 *   The receiver (self/alias) is NOT part of the overload signature.
 * 
 *   Example (VALID):
 *     impl Vec2 {
 *         add (v Vec2) -> Vec2 = { ... }
 *         add (scalar float) -> Vec2 = { ... }
 *     }
 *     v:add(otherVec)     // calls first overload
 *     v:add(5.0)          // calls second overload
 * 
 *   Example (INVALID – would be rejected):
 *     impl Vec2 {
 *         add (v Vec2) -> Vec2 = { ... }
 *         add (v Vec2) -> float = { ... }  -- same parameter signature
 *     }
 * 
 * ============================================================================
 * TYPE ALIAS OVERLOADING RULES
 * ============================================================================
 * 
 * Type aliases are TRANSPARENT – they are equivalent to their underlying type
 * for all purposes, including overload resolution.
 * 
 * ─── Example ──────────────────────────────────────────────────────────────
 * 
 *   type Number = int
 *   
 *   let process (n int) -> int = { return n * 2 }
 *   let process (n Number) -> int = { return n * 2 }
 *   
 *   ERROR: Duplicate definition! Number is just int.
 * 
 * ─── Why This Design ──────────────────────────────────────────────────────
 * 
 *   1. Type aliases are meant for readability, not for creating new types
 *   2. Consistent with substitution principle – alias can be replaced with
 *      underlying type anywhere
 *   3. Simpler implementation – no need for alias-aware overload resolution
 * 
 * ─── Workaround for Distinct Types ────────────────────────────────────────
 * 
 *   If you need distinct types for overloading, use struct wrappers:
 *   
 *     struct UserId { value int }
 *     struct ProductId { value int }
 *     
 *     let lookup (id UserId) -> string = { ... }
 *     let lookup (id ProductId) -> string = { ... }
 * 
 *   Future: Consider `opaque type` or `newtype` for zero-cost distinct types.
 *
 * ============================================================================
 * CURRYING VS OVERLOADING
 * ============================================================================
 * 
 *   These are distinct concepts handled by different parts of the compiler:
 * 
 *   | Feature        | Overloading                    | Currying                |
 *   |----------------|--------------------------------|-------------------------|
 *   | What           | Multiple functions, same name  | One function, called in steps |
 *   | Resolution     | Compile-time (argument types)  | Runtime (argument count) |
 *   | AST            | Multiple FuncDeclAST           | One FuncDeclAST with nested FuncTypeAST |
 *   | Symbol table   | OverloadSet with candidates    | Single Symbol |
 *   | Return type    | NOT considered                 | Affects partial application |
 *   | Can be combined? | No – mutually exclusive   | N/A |
 * 
 *   A function cannot be both curried AND overloaded. If a curried function
 *   is declared, attempting to add another overload is a compile error.
 * 
 * ============================================================================
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include <string_view>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// SymbolKind – categories of symbols that can be looked up by name
// 
// These directly correspond to AST declaration types that introduce names
// into scopes.
// ─────────────────────────────────────────────────────────────────────────────
enum class SymbolKind {
    // ========================================================================
    // TYPE NAMESPACE – declarations that define types
    // ========================================================================
    
    /**
     * @brief Represents a struct type declaration.
     * 
     * AST: StructDeclAST
     * Example: `struct Vec2 { x float, y float }`
     * Lookup: type namespace
     */
    Struct,
    
    /**
     * @brief Represents an enum type declaration.
     * 
     * AST: EnumDeclAST
     * Example: `enum Color { Red, Green, Blue }`
     * Lookup: type namespace
     */
    Enum,
    
    /**
     * @brief Represents a trait (interface) declaration.
     * 
     * AST: TraitDeclAST
     * Example: `trait Drawable { draw () }`
     * Lookup: type namespace
     */
    Trait,
    
    /**
     * @brief Represents a type alias declaration.
     * 
     * AST: TypeAliasDeclAST
     * Example: `type ID = int`
     * Lookup: type namespace (resolves to underlying type)
     */
    TypeAlias,
    
    // ========================================================================
    // VALUE NAMESPACE – declarations that produce values
    // ========================================================================
    
    /**
     * @brief Represents a function declaration.
     * 
     * AST: FuncDeclAST
     * Example: `let add (a int, b int) -> int = { return a + b }`
     * Lookup: value namespace
     * Overloadable: by parameter types only, NOT return type
     * 
     * @note Curried functions cannot be overloaded (see design notes above)
     */
    Func,
    
    /**
     * @brief Represents an extern function (linked from C/OS).
     * 
     * AST: FuncDeclAST with @extern attribute
     * Example: `@extern("printf") const printf (fmt *uint8, args ...any) -> int`
     * Lookup: value namespace
     * Overloadable: same rules as Func
     */
    ExternFunc,
    
    /**
     * @brief Represents a variable declaration (let or const).
     * 
     * AST: VarDeclAST
     * Example: `let x int = 42`, `const PI float = 3.14`
     * Lookup: value namespace
     */
    Var,
    
    /**
     * @brief Represents a function parameter.
     * 
     * AST: ParamAST
     * Example: In `add(a int, b int)`, `a` and `b` are parameters
     * Lookup: value namespace (function body scope only)
     */
    Param,
    
    /**
     * @brief Represents a struct field.
     * 
     * AST: FieldDeclAST
     * Example: In `struct Vec2 { x float, y float }`, `x` and `y` are fields
     * Lookup: value namespace (within struct scope, via dot access)
     */
    Field,
    
    /**
     * @brief Represents a method (function inside impl block).
     * 
     * AST: MethodDeclAST
     * Example: `impl Vec2 { length () -> float = { ... } }`
     * Lookup: value namespace (called via colon: `vec:length()`)
     * Overloadable: within same impl block, receiver not part of signature
     */
    Method,
    
    /**
     * @brief Represents an enum variant.
     * 
     * AST: EnumVariantAST
     * Example: In `enum Color { Red, Green, Blue }`, `Red` is a variant
     * Lookup: value namespace (accessed as `Color.Red`)
     */
    EnumVariant,
    
    // ========================================================================
    // GENERICS
    // ========================================================================
    
    /**
     * @brief Represents a generic type parameter.
     * 
     * AST: GenericParamAST
     * Example: In `struct Box<T>`, `T` is a generic parameter
     * Lookup: special scope within generic declaration
     */
    GenericParam,
    
    // ========================================================================
    // BEHAVIOUR / CONVERSIONS (looked up by type, not by name)
    // ========================================================================
    
    /**
     * @brief Represents an impl block (method implementations for a type).
     * 
     * AST: ImplDeclAST
     * Not looked up by name – used for method resolution on types
     */
    Impl,
    
    /**
     * @brief Represents a from block (implicit conversion).
     * 
     * AST: FromDeclAST
     * Not looked up by name – used for implicit conversion resolution
     */
    From,
};

// ============================================================================
// GenericParameter – for generic declarations
// ============================================================================

/**
 * @brief Represents a generic type parameter on a declaration.
 * 
 * Generic parameters appear on structs, enums, traits, functions, and impl blocks.
 * Each parameter has a name and optional trait constraints.
 * 
 * Example: `struct Box<T : Printable + Serialize>`
 *   - name = "T"
 *   - constraints = ["Printable", "Serialize"]
 * 
 * @note The actual type arguments are provided at instantiation sites.
 *       This structure only stores the declaration of the parameter.
 */
struct GenericParameter {
    InternedString name;                         ///< Parameter name (e.g., "T", "K", "V")
    ArenaSpan<InternedString> constraints;       ///< Trait names this parameter must satisfy
    
    // For debugging/display
    std::string_view getName(const StringPool& pool) const { return pool.lookup(name); }
};

// Forward declaration for OverloadSet
struct Symbol;

/**
 * @brief Collection of function symbols with the same name (overloaded).
 * 
 * Overload resolution selects the best candidate based on argument types
 * at the call site. This happens entirely at compile time – no runtime
 * dispatch or virtual tables required.
 * 
 * ─── Important Design Constraints ─────────────────────────────────────────
 * 
 *   1. Return type is NOT considered in overload resolution
 *   2. Curried functions cannot participate in overloading
 *   3. Only Func, ExternFunc, and Method symbols can be overloaded
 *   4. Overload resolution considers ALL parameters, not just the first
 * 
 * ─── Resolution Priority ──────────────────────────────────────────────────
 * 
 *   1. Exact parameter type match (highest)
 *   2. Numeric promotion (int → float, byte → int)
 *   3. Implicit conversion (via `from` declarations)
 *   4. Generic instantiation (with explicit type args)
 * 
 * ─── Example ──────────────────────────────────────────────────────────────
 * 
 *   let add (a int, b int) -> int       // candidate 1
 *   let add (a float, b float) -> float // candidate 2
 * 
 *   add(1, 2)      → calls candidate 1 (exact match)
 *   add(1.5, 2.5)  → calls candidate 2 (exact match)
 *   add(1, 2.5)    → ambiguous? Depends on conversion rules
 * 
 * @see findBestMatch() for overload resolution logic
 */
struct OverloadSet {
    std::vector<Symbol> candidates;      ///< All overloaded symbols with this name
    
    void add(const Symbol& sym) { candidates.push_back(sym); }
    bool empty() const { return candidates.empty(); }
    size_t size() const { return candidates.size(); }
    
    /**
     * @brief Finds the best matching overload for given argument types.
     * 
     * This is called during semantic analysis to resolve which overloaded
     * function to call.
     * 
     * IMPORTANT: Return type is NOT considered in this algorithm.
     * 
     * @param argTypes The types of arguments at the call site
     * @return Symbol* The best matching overload, or nullptr if none/ambiguous
     */
    Symbol* findBestMatch(const std::vector<TypeAST*>& argTypes);
};

// ============================================================================
// Symbol – resolved declaration for name lookup
// ============================================================================

/**
 * @brief Represents a resolved declaration after semantic analysis.
 * 
 * The symbol table stores these for O(1) name lookup during type checking.
 * 
 * ─── Memory Management ─────────────────────────────────────────────────────
 *   - name: InternedString (4 bytes, O(1) comparison)
 *   - decl: raw pointer to AST node (arena-allocated)
 *   - type: raw pointer to resolved type (may be from AST or built-in)
 *   - genericParams: vector of generic parameters (for generic declarations)
 * 
 * ─── Overloads ────────────────────────────────────────────────────────────
 *   Functions can be overloaded. isOverloaded and overloadSet point to
 *   the shared set of candidates for this name.
 * 
 *   DESIGN NOTE: Return type is NOT considered in overload resolution.
 *   Curried functions CANNOT be overloaded.
 * 
 * ─── Namespaces ───────────────────────────────────────────────────────────
 *   Symbols are stored in either value namespace or type namespace:
 *     - Value: Var, Func, ExternFunc, Param, Field, Method, EnumVariant
 *     - Type:  Struct, Enum, Trait, TypeAlias
 *     - Both:  GenericParam, Impl, From (context dependent)
 */
struct Symbol {
    // ========================================================================
    // Basic Identification
    // ========================================================================
    
    InternedString name;                ///< Interned symbol name (O(1) comparison)
    InternedString file;                ///< Source file where this symbol is declared
    SymbolKind kind = SymbolKind::Var;  ///< What kind of declaration this is
    SourceLocation loc;                 ///< Source location for error messages
    
    // ========================================================================
    // Declaration Metadata
    // ========================================================================
    
    DeclKeyword declKw = DeclKeyword::Let;      ///< let vs const (for Var/Func)
    Visibility visibility = Visibility::Private; ///< private, pub, or export
    BaseAST* decl = nullptr;                    ///< Back-pointer to AST node
    
    // ========================================================================
    // Type Information
    // ========================================================================
    
    TypeAST* type = nullptr;            ///< Resolved type (set during type resolution)
    
    // ========================================================================
    // Generic Support
    // ========================================================================
    
    std::vector<GenericParameter> genericParams;  ///< For generic declarations
    
    // ========================================================================
    // Overload Support (only for Func/ExternFunc/Method)
    // ========================================================================
    
    bool isOverloaded = false;          ///< True if part of overload set
    OverloadSet* overloadSet = nullptr; ///< Pointer to shared overload set (if any)
    
    // ========================================================================
    // Extern Metadata (for ExternFunc)
    // ========================================================================
    
    bool isExtern = false;              ///< True → symbol is linker-resolved
    InternedString externSymbol;        ///< C/OS symbol name (e.g., "malloc")
    InternedString callingConv;         ///< Calling convention (default "C")
    
    // ========================================================================
    // Convenience Methods
    // ========================================================================
    
    std::string_view getName(const StringPool& pool) const { return pool.lookup(name); }
    std::string_view getExternSymbol(const StringPool& pool) const { return pool.lookup(externSymbol); }
    std::string_view getCallingConv(const StringPool& pool) const { return pool.lookup(callingConv); }
    
    bool isExternWithSymbol(InternedString sym) const { return isExtern && externSymbol == sym; }
    bool hasCallingConv(InternedString conv) const { return callingConv == conv; }
    
    // Generic helpers
    bool isGeneric() const { return !genericParams.empty(); }
    size_t genericParamCount() const { return genericParams.size(); }
    
    /**
     * @brief Returns true if this function is curried.
     * 
     * A function is curried if its return type is another function type.
     * Curried functions cannot be overloaded.
     */
    bool isCurried() const {
        if (kind != SymbolKind::Func && kind != SymbolKind::ExternFunc) return false;
        if (auto* func = decl->as<FuncDeclAST>()) {
            return func->funcType && func->funcType->isCurried();
        }
        return false;
    }
};

// ============================================================================
// Utility Functions
// ============================================================================

namespace SymbolUtils {
    /**
     * @brief Converts SymbolKind to a human-readable string (for debugging).
     */
    std::string_view kindToString(SymbolKind kind);
    
    /**
     * @brief Returns true if the kind belongs to the value namespace.
     * 
     * Value namespace includes: Var, Func, ExternFunc, Param, Field, Method, EnumVariant
     */
    bool isValueKind(SymbolKind kind);
    
    /**
     * @brief Returns true if the kind belongs to the type namespace.
     * 
     * Type namespace includes: Struct, Enum, Trait, TypeAlias
     */
    bool isTypeKind(SymbolKind kind);
    
    /**
     * @brief Returns true if the kind represents a callable entity.
     * 
     * Callable includes: Func, ExternFunc, Method
     */
    bool isCallableKind(SymbolKind kind);
    
    /**
     * @brief Returns true if the kind can be overloaded.
     * 
     * Overloadable includes: Func, ExternFunc, Method
     * 
     * IMPORTANT: Curried functions are overloadable = false even though
     * their kind is overloadable. Check isCurried() separately.
     */
    bool isOverloadableKind(SymbolKind kind);
}
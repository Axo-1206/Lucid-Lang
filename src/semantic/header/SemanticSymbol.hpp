/**
 * @file SemanticSymbol.hpp
 * @responsibility Defines the semantic representation of resolved declarations.
 *
 * The AST provides syntax; symbols provide semantics. A Symbol represents a
 * resolved declaration (variable, function, struct, enum, trait, etc.) after
 * name resolution. Each symbol holds a back‑pointer to its AST node, its
 * resolved type, visibility, and other metadata needed for type checking
 * and code generation.
 *
 * @related
 *   - SymbolTable.hpp – scope stack for symbol lookup
 *   - SemanticCollector.hpp – Phase 1 visitor that populates symbols
 *   - SemanticAnalyzer.hpp – orchestrates the entire semantic pipeline
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Why symbols are necessary
 * ─────────────────────────────────────────────────────────────────────────────
 * - The AST only knows structure, not identity. Two different places may refer
 *   to the same `Vec2` struct; symbols give those references a single canonical
 *   representation.
 * - Symbol tables resolve names to symbols, enabling cross‑reference and scope
 *   management (handled by `SymbolTable`).
 * - Many semantic rules (visibility, constness, @extern) are attached to
 *   symbols, not to AST nodes directly.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Design decisions
 * ─────────────────────────────────────────────────────────────────────────────
 * - **InternedString for names** – the `name` field is an `InternedString`
 *   (a 32‑bit ID) instead of `std::string`. This reduces memory (4 bytes vs 32+),
 *   makes comparisons O(1) integer equality, and works seamlessly with
 *   arena‑allocated AST nodes because interned strings have no destructor.
 * - **Non‑owning pointers** – `type` and `decl` are raw pointers that point
 *   into the AST arena or the type system. The symbol table does not own the
 *   underlying nodes; the arena does.
 * - **Explicit StringPool dependency** – To display a symbol name (e.g., in
 *   diagnostics), you must pass a `StringPool` reference. This makes the
 *   dependency visible and avoids hidden global state.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Usage example
 * ─────────────────────────────────────────────────────────────────────────────
 * @code
 * // During semantic collection (Phase 1)
 * Symbol sym;
 * sym.name = pool.intern("player");
 * sym.kind = SymbolKind::Var;
 * sym.declKw = DeclKeyword::Let;
 * sym.visibility = Visibility::Private;
 * sym.decl = &varDeclNode;
 * sym.loc = varDeclNode.loc;
 * symbols.declare(sym);
 *
 * // Later in type checking (Phase 3)
 * Symbol* found = symbols.lookup(pool.intern("player"));
 * if (found && found->kind == SymbolKind::Var) {
 *     TypeAST* varType = found->type;   // already resolved
 * }
 * @endcode
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note The symbol table stores names as InternedString IDs. To convert an
 *       InternedString to a readable string, use `pool.lookup(name)`.
 * ─────────────────────────────────────────────────────────────────────────────
 */
#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"

#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// SymbolKind  — Enumerates the kinds of entities a symbol can represent
//
// Differentiates the syntactic form a symbol takes within the language. 
// Enables semantic passes to safely assert their expectations (e.g. ensuring
// a variable lookup actually returns a Variable, not a Struct).
// ─────────────────────────────────────────────────────────────────────────────
enum class SymbolKind {
    Var,
    Func,
    ExternFunc,   // function declared with @extern("sym") — resolved by the linker
    Struct,
    Enum,
    Trait,
    TypeAlias,
    Param,
    Field,
    Method,
    EnumVariant,
    Casting
};

// ─────────────────────────────────────────────────────────────────────────────
// Symbol  — A resolved semantic entity bound to a scope
//
// Acts as the definitive structure representing a parsed concept like a let 
// variable, function, or structural type constraint. It bridges the AST nodes
// to their semantic meta-properties, carrying scope resolution facts forward
// to be utilized by the constraint checkers and code generators.
//
// ## InternedString
//
// The `name` field uses `InternedString` instead of `std::string` to:
//   - Reduce memory footprint (4 bytes vs 32+ bytes per symbol)
//   - Enable O(1) comparisons (integer comparison instead of string comparison)
//   - Maintain arena compatibility
//
// ## String Pool Access
//
// To convert an InternedString to a displayable string (for diagnostics, codegen,
// or debugging), you must pass a StringPool reference:
//
//   std::string_view nameStr = pool.lookup(symbol.name);
//
// This forces the dependency to be explicit and avoids hidden global state.
//
// ## Usage Example
//
// ```cpp
// // Creating a symbol
// Symbol sym;
// sym.name = pool.intern("player");
// sym.kind = SymbolKind::Var;
//
// // Later, when you need the string
// std::string_view name = pool.lookup(sym.name);
// ```
// ─────────────────────────────────────────────────────────────────────────────
struct Symbol {
    InternedString name;        // interned symbol name (e.g., "player", "Vec2")
    SymbolKind kind = SymbolKind::Var;
    DeclKeyword declKw = DeclKeyword::Let;         // Let / Const (for Var/Func)
    Visibility visibility = Visibility::Private;
    TypeAST *type = nullptr;              // resolved type (non-owning)
    BaseAST *decl = nullptr;              // back-pointer to the AST node
    SourceLocation loc;

    // ── @extern metadata ──────────────────────────────────────────────────────
    // Populated by SemanticCollector when it encounters an @extern attribute.
    // Codegen uses these to emit the correct LLVM external declaration.
    bool        isExtern     = false;  // true → symbol is linker-resolved
    InternedString externSymbol;       // C/OS symbol name, e.g. "malloc"
    InternedString callingConv = InternedString(); // calling convention, default "C"

    // ── Convenience methods (require StringPool) ──────────────────────────────
    
    // Get the symbol name as a string_view (requires pool for lookup)
    std::string_view getName(const StringPool& pool) const {
        return pool.lookup(name);
    }
    
    // Get the extern symbol name (if isExtern) as string_view
    std::string_view getExternSymbol(const StringPool& pool) const {
        return pool.lookup(externSymbol);
    }
    
    // Get the calling convention as string_view
    std::string_view getCallingConv(const StringPool& pool) const {
        return pool.lookup(callingConv);
    }
    
    // Check if this symbol represents an extern function with a specific name
    bool isExternWithSymbol(InternedString sym) const {
        return isExtern && externSymbol == sym;
    }
    
    // Check if this symbol uses a specific calling convention
    bool hasCallingConv(InternedString conv) const {
        return callingConv == conv;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Symbol Utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace SymbolUtils {
    
    // Convert SymbolKind to a human-readable string (for diagnostics)
    inline const char* kindToString(SymbolKind kind) {
        switch (kind) {
            case SymbolKind::Var:         return "variable";
            case SymbolKind::Func:        return "function";
            case SymbolKind::ExternFunc:  return "extern function";
            case SymbolKind::Struct:      return "struct";
            case SymbolKind::Enum:        return "enum";
            case SymbolKind::Trait:       return "trait";
            case SymbolKind::TypeAlias:   return "type alias";
            case SymbolKind::Param:       return "parameter";
            case SymbolKind::Field:       return "field";
            case SymbolKind::Method:      return "method";
            case SymbolKind::EnumVariant: return "enum variant";
            case SymbolKind::Casting:     return "casting";
            default:                      return "unknown";
        }
    }
    
    // Create a diagnostic-friendly string for a symbol (requires pool)
    inline std::string formatSymbol(const Symbol& sym, const StringPool& pool) {
        std::string result = kindToString(sym.kind);
        result += " '";
        result += pool.lookup(sym.name);
        result += "'";
        return result;
    }
    
} // namespace SymbolUtils

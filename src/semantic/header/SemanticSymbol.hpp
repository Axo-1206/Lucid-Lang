/**
 * @file SemanticSymbol.hpp
 *
 * @nutshell Defines what a 'Symbol' actually represents semantically.
 *
 * @reason Nodes in an AST don't inherently represent unique global entities across files; this provides the data definitions linking AST trees back to recognized global definitions.
 *
 * @responsibility The symbol type itself, representing declared entities (variables, functions, types, etc.).
 *
 * @related SymbolTable.hpp
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

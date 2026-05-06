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
#include <string>

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
// ─────────────────────────────────────────────────────────────────────────────
struct Symbol {
    std::string name;
    SymbolKind kind;
    DeclKeyword declKw; // Let / Const (for Var/Func)
    Visibility visibility;
    TypeAST *type;      // resolved type (non-owning)
    BaseAST *decl;      // back-pointer to the AST node
    SourceLocation loc;

    // ── @extern metadata ──────────────────────────────────────────────────────
    // Populated by SemanticCollector when it encounters an @extern attribute.
    // Codegen uses these to emit the correct LLVM external declaration.
    bool        isExtern     = false;  // true → symbol is linker-resolved
    std::string externSymbol;          // C/OS symbol name, e.g. "malloc"
    std::string callingConv  = "C";    // calling convention, default "C"
};
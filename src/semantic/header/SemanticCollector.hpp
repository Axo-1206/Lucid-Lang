/**
 * @file SemanticCollector.hpp
 * @responsibility Phase 1 of semantic analysis: gathers top‑level declarations into the symbol table.
 *
 * SemanticCollector is an ASTVisitor that walks the parse tree before type checking.
 * It collects struct, enum, function, trait, impl, from, and type alias names,
 * and declares them in the global symbol table. This enables forward references
 * during later phases (type resolution and checking).
 *
 * @related
 *   - SymbolTable.hpp – stores the collected symbols
 *   - SemanticAnalyzer.hpp – orchestrates the four semantic phases
 *   - TypeResolver.hpp – resolves type annotations using the collected symbols
 *   - NameMangler.hpp – generates unique names for methods, variants, and conversions
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Why a separate collection phase is necessary
 * ─────────────────────────────────────────────────────────────────────────────
 * - Luc supports forward references: a type can be used before it is defined
 *   (e.g., a struct field that points to another struct defined later).
 * - Collecting all declarations first ensures that every name is already in
 *   the symbol table when type resolution begins.
 * - Without this phase, resolving a type like `Vec2` would fail because the
 *   symbol wouldn't exist yet if `struct Vec2` appears later in the file.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Design decisions
 * ─────────────────────────────────────────────────────────────────────────────
 * - **No type resolution** – the collector only records names and their kinds.
 *   Type information (e.g., `sym->type`) is left nullptr; it is filled later
 *   by TypeResolver.
 * - **Mangled names for methods, variants, and conversions** – the collector
 *   uses `NameMangler` to generate unique identifiers (e.g., `Struct::method`,
 *   `Enum::variant`, `Target::from::ParamType`). This avoids collisions and
 *   enables qualified lookup.
 * - **Explicit StringPool dependency** – the collector holds a reference to
 *   StringPool to convert InternedString names to readable strings for
 *   diagnostics.
 * - **Error on duplicate names** – if the same name is declared twice in the
 *   same scope (e.g., two structs with the same name), the collector reports
 *   an error immediately.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Usage example
 * ─────────────────────────────────────────────────────────────────────────────
 * @code
 * SymbolTable symbols;
 * SemanticCollector collector(symbols, dc, pool);
 *
 * for (ProgramAST* prog : files) {
 *     collector.collectProgram(*prog);
 * }
 * // Now the symbol table contains all top‑level declarations.
 * @endcode
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note The collector only processes top‑level declarations. Function bodies,
 *       block scopes, and expressions are ignored – they are handled by later
 *       phases. Only declarations that create symbols (struct, enum, func, etc.)
 *       are visited.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "SymbolTable.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticEngine.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// SemanticCollector  — Phase 1 AST Visitor for symbol collection
//
// Traverses the abstract syntax tree strictly to populate the SymbolTable with
// top-level names (structs, functions, enums, traits). It creates no bindings 
// or type guarantees; it merely asserts that names are not duplicated in the
// global scope, establishing the map necessary to support forward references
// during Phase 2 (Type Resolution).
//
// ## String Pool Usage
//
// The SemanticCollector holds a reference to StringPool to convert InternedString
// names to string_view when needed for diagnostics. However, the symbol table
// itself stores names as InternedString IDs for efficiency.
// ─────────────────────────────────────────────────────────────────────────────
class SemanticCollector : public ASTVisitor {
public:
    explicit SemanticCollector(SymbolTable& symbols, DiagnosticEngine& dc, 
                                StringPool& pool);

    // ─────────────────────────────────────────────────────────────────────────────
    // collectProgram  — Bootstraps the semantic collection phase
    //
    // Iterates through all root-level declarations parsed for a given program/file,
    // dispatching the ASTVisitor logic to index them correctly.
    // ─────────────────────────────────────────────────────────────────────────────
    void collectProgram(ProgramAST& program);

    // ── ASTVisitor overrides for top-level declarations ───────────────
    void visit(UseDeclAST& node) override;
    void visit(VarDeclAST& node) override;
    void visit(FuncDeclAST& node) override;
    void visit(StructDeclAST& node) override;
    void visit(EnumDeclAST& node) override;
    void visit(TraitDeclAST& node) override;
    void visit(ImplDeclAST& node) override;
    void visit(FromDeclAST& node) override;
    void visit(TypeAliasDeclAST& node) override;

    // ── Helper to check if a name is already declared (for error recovery) ──
    bool isDeclared(InternedString name) const {
        return _symbols.lookup(name) != nullptr;
    }

    // Called after collection to retrieve the trait implementation map.
    const std::unordered_map<InternedString, std::vector<InternedString>>& getStructTraits() const {
        return _structTraits;
    }

private:
    SymbolTable& _symbols;
    DiagnosticEngine& _dc;
    StringPool& _pool;  // For converting InternedString to readable names in diagnostics

    // The _structTraits map is populated in visit(ImplDeclAST) 
    // because that's where trait conformance is declared. 
    // In Luc, a struct implements a trait via an impl block with a : TraitName suffix, 
    // not in the struct definition itself. For example
    //
    // struct Circle { radius float }
    // impl Circle : Drawable {   // <-- trait conformance declared here
    //      draw () = { ... }
    // }
    std::unordered_map<InternedString, std::vector<InternedString>> _structTraits;

    // ─────────────────────────────────────────────────────────────────────────────
    // declareSymbol  — Core logic to bind an AST node symbol
    //
    // Submits the formulated Symbol data structure into the SymbolTable. Handles
    // logging a diagnostic error if the name already conflicts in the same scope.
    // ─────────────────────────────────────────────────────────────────────────────
    void declareSymbol(const Symbol& sym);
    
    // Helper to extract extern metadata from attributes
    void extractExternMetadata(const std::vector<AttributePtr>& attrs, Symbol& sym);
    
    // Helper to get the string representation of an InternedString for diagnostics
    std::string_view getNameString(InternedString name) const {
        return _pool.lookup(name);
    }
};
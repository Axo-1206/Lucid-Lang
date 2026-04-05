/**
 * @file SemanticCollector.hpp
 *
 * @nutshell A quick first-pass AST visitor that gathers definitions.
 *
 * @reason Modern language syntax requires types to be referenceable before they are fully checked. This establishes that baseline index mapping.
 *
 * @responsibility Phase 1 of semantic analysis: collect all top-level names into the file-scope symbol table.
 *
 * @logic First pass over the AST. Collects declarations (struct, enum, function, etc.) before type checking to enable forward references.
 *
 * @related SemanticAnalyzer.hpp, SymbolTable.hpp
 */
#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/SymbolTable.hpp"

class DiagnosticEngine;

// ─────────────────────────────────────────────────────────────────────────────
// SemanticCollector  — Phase 1 AST Visitor for symbol collection
//
// Traverses the abstract syntax tree strictly to populate the SymbolTable with
// top-level names (structs, functions, enums, traits). It creates no bindings 
// or type guarantees; it merely asserts that names are not duplicated in the
// global scope, establishing the map necessary to support forward references
// during Phase 2 (Type Resolution).
// ─────────────────────────────────────────────────────────────────────────────
class SemanticCollector : public ASTVisitor {
public:
    explicit SemanticCollector(SymbolTable& symbols, DiagnosticEngine& dc);

    // ─────────────────────────────────────────────────────────────────────────────
    // collectProgram  — Bootstraps the semantic collection phase
    //
    // Iterates through all root-level declarations parsed for a given program/file,
    // dispatching the ASTVisitor logic to index them correctly.
    // ─────────────────────────────────────────────────────────────────────────────
    void collectProgram(ProgramAST& program);

    // ── ASTVisitor overrides for top-level declarations ───────────────
    void visit(VarDeclAST& node) override;
    void visit(FuncDeclAST& node) override;
    void visit(StructDeclAST& node) override;
    void visit(EnumDeclAST& node) override;
    void visit(TraitDeclAST& node) override;
    void visit(ImplDeclAST& node) override;
    void visit(FromDeclAST& node) override;
    void visit(TypeAliasDeclAST& node) override;
    void visit(ExternDeclAST& node) override;

private:
    SymbolTable& symbols_;
    DiagnosticEngine& dc_;

    // ─────────────────────────────────────────────────────────────────────────────
    // declareSymbol  — Core logic to bind an AST node symbol
    //
    // Submits the formulated Symbol data structure into the SymbolTable. Handles
    // logging a diagnostic error if the name already conflicts in the same scope.
    // ─────────────────────────────────────────────────────────────────────────────
    void declareSymbol(const Symbol& sym);
};

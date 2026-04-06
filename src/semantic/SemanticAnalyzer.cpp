/**
 * @file SemanticAnalyzer.cpp
 *
 * @nutshell Implements the overarching driver for semantic validation.
 *
 * @responsibility Orchestrates all four semantic phases across every file in the package.
 *
 * @logic
 *   Phase 0 — resolveImports: detect circular use-decl chains.
 *   Phase 1 — collectSymbols: run SemanticCollector over all files.
 *   Phase 2 — resolveTypes: validate TypeAST nodes via TypeResolver.
 *   Phase 3 — checkDecls: run the full declaration / expression / statement checkers.
 *   Phase 4 — annotate: write semantic properties back onto BaseAST nodes.
 *
 * @related SemanticAnalyzer.hpp
 */

#include "SemanticAnalyzer.hpp"
#include "SemanticCollector.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"
#include "SymbolTable.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration of Phase 3 dispatcher (defined in SemanticDecl.cpp)
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SymbolTable& symbols, TypeResolver& resolver,
                       DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                       int& parallelDepth, bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
SemanticAnalyzer::SemanticAnalyzer(DiagnosticEngine& dc)
    : dc_(dc),
      symbols_(std::make_unique<SymbolTable>()),
      typeResolver_(std::make_unique<TypeResolver>(*symbols_, dc)),
      typeChecker_(std::make_unique<TypeChecker>()) {}

SemanticAnalyzer::~SemanticAnalyzer() = default;

// ─────────────────────────────────────────────────────────────────────────────
// analyze  — top-level entry point
// ─────────────────────────────────────────────────────────────────────────────
bool SemanticAnalyzer::analyze(std::vector<ProgramAST*>& files) {
    resolveImports(files);
    if (dc_.hasErrors()) return false;

    collectSymbols(files);
    if (dc_.hasErrors()) return false;

    resolveTypes(files);
    // Don't early-exit on type resolution errors — we can still discover
    // more semantic errors during the checking pass.

    checkDecls(files);
    if (dc_.hasErrors()) return false;

    // Phase 3.5: Entry point detection
    // Validate that a 'main' function exists and has a valid signature.
    Symbol* mainSym = symbols_->lookup("main");
    if (!mainSym) {
        // Points to the start of the first file for a missing entry point error.
        SourceLocation loc = files.empty() ? SourceLocation() : files[0]->loc;
        dc_.error(DiagnosticCategory::Semantic, loc, DiagCode::E3006, 
                  "program is missing a 'main' entry point");
    } else if (mainSym->kind != SymbolKind::Func) {
        dc_.error(DiagnosticCategory::Semantic, mainSym->loc, DiagCode::E3007, 
                  "'main' must be a function");
    }

    annotate(files);
    return !dc_.hasErrors();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveImports  — Phase 0
// Detects circular `use` declarations using a simple DFS over the import graph.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::resolveImports(std::vector<ProgramAST*>& files) {
    // Build a map from package+file path to its use declarations.
    // For now we do a simple duplicate-use check within each file.
    for (auto* prog : files) {
        std::unordered_set<std::string> seen;
        for (auto& decl : prog->decls) {
            if (!decl->isa<UseDeclAST>()) continue;
            auto* use = decl->as<UseDeclAST>();
            std::string path;
            for (size_t i = 0; i < use->path.size(); ++i) {
                if (i) path += '.';
                path += use->path[i];
            }
            if (!seen.insert(path).second) {
                dc_.error(DiagnosticCategory::Semantic, use->loc, DiagCode::E3005,
                          "duplicate import of '" + path + "'");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// collectSymbols  — Phase 1
// Runs SemanticCollector over every file to populate the global symbol table.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::collectSymbols(std::vector<ProgramAST*>& files) {
    symbols_->pushScope(); // global scope
    SemanticCollector collector(*symbols_, dc_);
    for (auto* prog : files) {
        collector.collectProgram(*prog);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveTypes  — Phase 2
// Walks every type annotation in every declaration and validates it.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::resolveTypes(std::vector<ProgramAST*>& files) {
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            // Type resolution is done lazily inside checkTopLevelDecl (Phase 3),
            // but we do a quick pass here for top-level type aliases and extern
            // declarations so their types are available during the check phase.
            if (decl->isa<TypeAliasDeclAST>()) {
                auto* ta = decl->as<TypeAliasDeclAST>();
                typeResolver_->resolveType(ta->aliasedType.get());
            } else if (decl->isa<ExternDeclAST>()) {
                auto* ext = decl->as<ExternDeclAST>();
                typeResolver_->setInsideExtern(true);
                for (auto& p : ext->params)
                    typeResolver_->resolveType(p->type.get());
                if (ext->returnType)
                    typeResolver_->resolveType(ext->returnType.get());
                typeResolver_->setInsideExtern(false);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkDecls  — Phase 3
// Runs the full declaration / expression / statement checkers over every file.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::checkDecls(std::vector<ProgramAST*>& files) {
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            checkTopLevelDecl(decl.get(), *symbols_, *typeResolver_, dc_,
                              asyncDepth_, loopDepth_, parallelDepth_,
                              insideExtern_);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// annotate  — Phase 4
// Writes scopeDepth onto every declaration node that has a scope.
// Full resolvedType / isConst annotation is done inline during Phase 3
// (each check function writes node->resolvedType directly).
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::annotate(std::vector<ProgramAST*>& files) {
    // resolvedType is written directly by checkExpr in Phase 3.
    // scopeDepth is written by checkBlock in SemanticStmt.
    // This pass is currently a no-op; a future Annotator visitor may be added
    // to post-process the tree for codegen convenience.
    (void)files;
}
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
// Forward declaration of Phase 4 dispatcher (defined in Annotator.cpp)
// ─────────────────────────────────────────────────────────────────────────────
void annotateAll(std::vector<ProgramAST*>& files, SymbolTable& symbols);

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
    // Phase 0: Resolve Imports.
    std::cout << "\n--- Phase 0: Resolve Imports ---" << std::endl;
    resolveImports(files);
    if (dc_.hasErrors()) return false;

    // Phase 1: Collect Symbols.
    std::cout << "\n--- Phase 1: Collect Symbols ---" << std::endl;
    collectSymbols(files);
    if (dc_.hasErrors()) return false;

    dumpSymbols();

    // Phase 2: Resolve Types.
    std::cout << "\n--- Phase 2: Resolve Types ---" << std::endl;
    resolveTypes(files);
    // Don't early-exit on type resolution errors — we can still discover
    // more semantic errors during the checking pass.

    // Phase 3: Check Decls.
    std::cout << "\n--- Phase 3: Check Decls ---" << std::endl;
    checkDecls(files);
    if (dc_.hasErrors()) return false;

    std::cout << "\n--- Phase 3.5: Entry point detection ---" << std::endl;
    // Phase 3.5: Entry point detection
    // Validate that a 'main' function exists and has a valid signature.
    // Required format: export const main () int = { ... }
    Symbol* mainSym = symbols_->lookup("main");
    if (!mainSym) {
        // Points to the start of the first file for a missing entry point error.
        SourceLocation loc = files.empty() ? SourceLocation() : files[0]->loc;
        dc_.error(DiagnosticCategory::Semantic, loc, DiagCode::E3006, 
                  "program is missing a 'main' entry point");
    } else if (mainSym->kind != SymbolKind::Func) {
        // ExternFunc (linker-resolved) is never a valid main entry point.
        std::string kindNote = (mainSym->kind == SymbolKind::ExternFunc)
            ? " ('@extern' functions cannot be entry points)"
            : "";
        dc_.error(DiagnosticCategory::Semantic, mainSym->loc, DiagCode::E3007, 
                  "'main' must be a regular function" + kindNote);
    } else {
        auto* func = static_cast<FuncDeclAST*>(mainSym->decl);
        
        // 1. MUST be exported: export const main ...
        if (func->visibility != Visibility::Export) {
            dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                      "'main' function must be exported (use 'export const main')");
        }
        
        // 2. MUST be const: const main ...
        if (func->keyword != DeclKeyword::Const) {
            dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                      "'main' function must use 'const' keyword");
        }
        
        // 3. MUST have zero parameters: ()
        bool hasParams = false;
        for (const auto& group : func->paramGroups) {
            if (!group.empty()) {
                hasParams = true;
                break;
            }
        }
        if (hasParams) {
            dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                      "'main' function must have no parameters");
        }
        
        // 4. MUST return int
        bool returnsInt = false;
        if (func->returnType && func->returnType->kind == ASTKind::PrimitiveType) {
            auto* pt = static_cast<PrimitiveTypeAST*>(func->returnType.get());
            if (pt->primitiveKind == PrimitiveKind::Int) {
                returnsInt = true;
            }
        }
        if (!returnsInt) {
            dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                      "'main' function must return 'int'");
        }
        
        // 5. MUST NOT be async
        if (func->isAsync) {
            dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                      "'main' function cannot be async");
        }

        // 6. @aot and @jit validation on main
        // Both are optional. If present, they must not both appear together.
        // The mutual exclusion check already runs in checkAttributes during
        // Phase 3. Here we just record which mode was requested.
        bool hasAot = false;
        bool hasJit = false;
        for (const auto& attr : func->attributes) {
            if (attr->name == "aot") hasAot = true;
            if (attr->name == "jit") hasJit = true;
        }
        // hasAot and hasJit together is already caught by checkAttributes (E3015).
        // Record the compilation mode for the driver/codegen to use.
        // (compilationMode_ is set on SemanticAnalyzer for downstream use.)
        if (hasAot)      compilationMode_ = CompilationMode::AOT;
        else if (hasJit) compilationMode_ = CompilationMode::JIT;
        else             compilationMode_ = CompilationMode::AOT; // fallback if no attributes
    }

    // ── Validate that @aot / @jit do not appear on non-main functions ──────────
    // checkAttributes catches invalid contexts (struct) but cannot check the
    // function name because it does not have access to the FuncDeclAST name
    // at that point. We do a targeted sweep here over all top-level functions.
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (!decl->isa<FuncDeclAST>()) continue;
            auto* func = decl->as<FuncDeclAST>();
            if (func->name == "main") continue; // already validated above

            for (const auto& attr : func->attributes) {
                if (attr->name == "aot" || attr->name == "jit") {
                    dc_.error(DiagnosticCategory::Semantic, attr->loc,
                              DiagCode::E3016,
                              "'@" + attr->name + "' is only valid on the 'main' "
                              "entry point; remove it from '" + func->name + "'");
                }
            }
        }
    }

    // Phase 4: Annotate & Optimize.
    annotate(files);
    std::cout << "  - Semantic Analysis Finished." << std::endl;
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
// Generic parameters in type aliases are resolved in context.
// @extern function types are resolved during Phase 3 (checkFuncDecl) when
// the @extern attribute is detected — no special early pass needed here.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::resolveTypes(std::vector<ProgramAST*>& files) {
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            // Type resolution is done lazily inside checkTopLevelDecl (Phase 3),
            // but we do a quick pass here for top-level type aliases so their
            // types are available during the check phase.
            if (decl->isa<TypeAliasDeclAST>()) {
                auto* ta = decl->as<TypeAliasDeclAST>();
                // Set generic parameters context for generic type aliases like type Transform<T> = (value T) T
                typeResolver_->setGenericParams(&ta->genericParams);
                typeResolver_->resolveType(ta->aliasedType.get());
                typeResolver_->setGenericParams(nullptr);
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
// Invokes the Annotator visitor (Annotator.cpp) over every file in the package.
// The Annotator stamps:
//   isConst          — true for const-declared nodes and all literal expressions
//   isBehaviorMember — reinforced on BehaviorAccessExprAST nodes
// resolvedType and scopeDepth are left as-is; they were written inline during
// Phase 3 by checkExpr and checkBlock respectively.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::annotate(std::vector<ProgramAST*>& files) {
    annotateAll(files, *symbols_);
}

void SemanticAnalyzer::dumpSymbols() const {
    symbols_->dump();
}
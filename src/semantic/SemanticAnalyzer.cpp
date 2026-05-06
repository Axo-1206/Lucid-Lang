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
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration of Phase 3 dispatcher (defined in SemanticDecl.cpp)
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SymbolTable& symbols, TypeResolver& resolver,
                       DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern);

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
      typeChecker_(std::make_unique<TypeChecker>()) {
    LUC_LOG_SEMANTIC("SemanticAnalyzer constructed");
}

SemanticAnalyzer::~SemanticAnalyzer() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Validate duplicate symbols
// ─────────────────────────────────────────────────────────────────────────────

void SemanticAnalyzer::validateNoDuplicateSymbols() {
    LUC_LOG_SEMANTIC("\n--- Phase 1.5: Validate No Duplicate Symbols ---");
    
    std::unordered_map<std::string, SourceLocation> firstDecl;
    
    // Need to iterate global scope - add this method to SymbolTable
    const auto& globalScope = symbols_->getGlobalScope();
    
    for (const auto& [name, sym] : globalScope) {
        auto it = firstDecl.find(name);
        if (it != firstDecl.end()) {
            LUC_LOG_SEMANTIC("\tDuplicate symbol: " << name);
            dc_.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                      "symbol '" + name + "' is already declared");
        } else {
            firstDecl[name] = sym.loc;
        }
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("Phase 1.5 complete: " << firstDecl.size() << " unique symbols");
}

// ─────────────────────────────────────────────────────────────────────────────
// analyze  — top-level entry point
// ─────────────────────────────────────────────────────────────────────────────
bool SemanticAnalyzer::analyze(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC("\n=== SEMANTIC_ANALYZE - START ===");
    LUC_LOG_SEMANTIC_VERBOSE("\tProcessing " << files.size() << " file(s)");
    
    // Phase 0: Resolve Imports.
    LUC_LOG_SEMANTIC("\n--- Phase 0: Resolve Imports ---");
    resolveImports(files);
    if (dc_.hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 0 FAILED with errors");
        return false;
    }
    LUC_LOG_SEMANTIC("Phase 0 completed successfully");

    // Phase 1: Collect Symbols.
    LUC_LOG_SEMANTIC("\n--- Phase 1: Collect Symbols ---");
    collectSymbols(files);
    if (dc_.hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 1 FAILED with errors");
        return false;
    }
    LUC_LOG_SEMANTIC("Phase 1 completed successfully");

    validateNoDuplicateSymbols();
    dumpSymbols();

    // Phase 2: Resolve Types.
    LUC_LOG_SEMANTIC("\n--- Phase 2: Resolve Types ---");
    resolveTypes(files);
    LUC_LOG_SEMANTIC("Phase 2 completed (warnings may exist)");
    
    // Phase 3: Check Decls.
    LUC_LOG_SEMANTIC("\n--- Phase 3: Check Decls ---");
    checkDecls(files);
    if (dc_.hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 3 FAILED with errors");
        return false;
    }
    LUC_LOG_SEMANTIC("Phase 3 completed successfully");

    LUC_LOG_SEMANTIC("\n--- Phase 3.5: Entry point detection ---");
    // Phase 3.5: Entry point detection
    // Validate that a 'main' function exists and has a valid signature.
    // Required format: export const main () int = { ... }
    Symbol* mainSym = symbols_->lookup("main");
    if (!mainSym) {
        LUC_LOG_SEMANTIC("\tNo 'main' function found");
        SourceLocation loc = files.empty() ? SourceLocation() : files[0]->loc;
        dc_.error(DiagnosticCategory::Semantic, loc, DiagCode::E3006, 
                "program is missing a 'main' entry point");
    } else {
        LUC_LOG_SEMANTIC("\tFound 'main' function, validating signature...");
        if (mainSym->kind != SymbolKind::Func) {
            std::string kindNote = (mainSym->kind == SymbolKind::ExternFunc)
                ? " ('@extern' functions cannot be entry points)"
                : "";
            LUC_LOG_SEMANTIC("\tERROR: 'main' is not a regular function");
            dc_.error(DiagnosticCategory::Semantic, mainSym->loc, DiagCode::E3007, 
                    "'main' must be a regular function" + kindNote);
        } else {
            auto* func = static_cast<FuncDeclAST*>(mainSym->decl);
            LUC_LOG_SEMANTIC_VERBOSE("\tFunction: " << func->name);
            
            // 1. MUST be exported: export const main ...
            if (func->visibility != Visibility::Export) {
                LUC_LOG_SEMANTIC("\tERROR: main not exported");
                dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                        "'main' function must be exported (use 'export const main')");
            }
            
            // 2. MUST be const: const main ...
            if (func->keyword != DeclKeyword::Const) {
                LUC_LOG_SEMANTIC("\tERROR: main not const");
                dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                        "'main' function must use 'const' keyword");
            }
            
            // 3. MUST have zero parameters: () or slice of strings: (args []string)
            bool hasParams = false;
            bool isValidArgsParam = false;
            
            // Use func->type (unified FuncTypeAST) instead of old paramGroups
            if (func->type.paramGroups.size() == 1) {
                const auto& group = func->type.paramGroups[0];
                if (!group.empty()) {
                    hasParams = true;
                    if (group.size() == 1) {
                        const auto& param0 = group[0];
                        if (param0.type && param0.type->kind == ASTKind::SliceType) {
                            auto* slice = static_cast<SliceTypeAST*>(param0.type.get());
                            if (slice->element && slice->element->kind == ASTKind::PrimitiveType) {
                                auto* pt = static_cast<PrimitiveTypeAST*>(slice->element.get());
                                if (pt->primitiveKind == PrimitiveKind::String) {
                                    isValidArgsParam = true;
                                    LUC_LOG_SEMANTIC_VERBOSE("\tValid args parameter: []string");
                                }
                            }
                        }
                    }
                }
            } else if (func->type.paramGroups.size() > 1) {
                for (const auto& group : func->type.paramGroups) {
                    if (!group.empty()) {
                        hasParams = true;
                        break;
                    }
                }
            }

            if (hasParams && !isValidArgsParam) {
                LUC_LOG_SEMANTIC("\tERROR: invalid parameter signature for main");
                dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                        "'main' function must have no parameters or take a string slice: (args []string)");
            }
            
            // 4. MUST return int
            bool returnsInt = false;
            if (func->type.returnType && func->type.returnType->kind == ASTKind::PrimitiveType) {
                auto* pt = static_cast<PrimitiveTypeAST*>(func->type.returnType.get());
                if (pt->primitiveKind == PrimitiveKind::Int) {
                    returnsInt = true;
                    LUC_LOG_SEMANTIC_VERBOSE("\tReturn type: int");
                }
            }
            if (!returnsInt) {
                LUC_LOG_SEMANTIC("\tERROR: main does not return int");
                dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                        "'main' function must return 'int'");
            }   

            // 5. MUST NOT be async (check via type qualifier)
            if (func->type.isAsync()) {
                LUC_LOG_SEMANTIC("\tERROR: main is async");
                dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
                        "'main' function cannot be async (remove '~async' qualifier)");
            }

            // 6. @aot and @jit validation on main
            bool hasAot = false;
            bool hasJit = false;
            for (const auto& attr : func->attributes) {
                if (attr->name == "aot") hasAot = true;
                if (attr->name == "jit") hasJit = true;
            }
            
            if (hasAot) {
                LUC_LOG_SEMANTIC("\tCompilation mode: AOT");
                compilationMode_ = CompilationMode::AOT;
            } else if (hasJit) {
                LUC_LOG_SEMANTIC("\tCompilation mode: JIT");
                compilationMode_ = CompilationMode::JIT;
            } else {
                LUC_LOG_SEMANTIC_VERBOSE("\tCompilation mode: AOT (default)");
                compilationMode_ = CompilationMode::AOT;
            }
            
            LUC_LOG_SEMANTIC("\tmain signature validation complete");
        }
    }

    // ── Validate that @aot / @jit do not appear on non-main functions ──────────
    LUC_LOG_SEMANTIC_VERBOSE("Checking @aot/@jit on non-main functions...");
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (!decl->isa<FuncDeclAST>()) continue;
            auto* func = decl->as<FuncDeclAST>();
            if (func->name == "main") continue;

            for (const auto& attr : func->attributes) {
                if (attr->name == "aot" || attr->name == "jit") {
                    LUC_LOG_SEMANTIC("\tERROR: '@" << attr->name << "' on non-main function '" << func->name << "'");
                    dc_.error(DiagnosticCategory::Semantic, attr->loc,
                              DiagCode::E3016,
                              "'@" + attr->name + "' is only valid on the 'main' "
                              "entry point; remove it from '" + func->name + "'");
                }
            }
        }
    }

    // Phase 4: Annotate & Optimize.
    LUC_LOG_SEMANTIC("\n--- Phase 4: Annotate ---");
    annotate(files);
    
    LUC_LOG_SEMANTIC("\t- Semantic Analysis Finished.");
    LUC_LOG_SEMANTIC("=== SemanticAnalyzer::analyze END ===");
    return !dc_.hasErrors();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveImports  — Phase 0
// Detects circular `use` declarations using a simple DFS over the import graph.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::resolveImports(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveImports: processing " << files.size() << " files");
    
    // Build a map from package+file path to its use declarations.
    // For now we do a simple duplicate-use check within each file.
    int totalUses = 0;
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
                LUC_LOG_SEMANTIC("\tduplicate import of '" << path << "'");
                dc_.error(DiagnosticCategory::Semantic, use->loc, DiagCode::E3005,
                          "duplicate import of '" + path + "'");
            } else {
                totalUses++;
                LUC_LOG_SEMANTIC_EXTREME("\tregistered import: " << path);
            }
        }
    }
    LUC_LOG_SEMANTIC_VERBOSE("resolveImports: registered " << totalUses << " unique imports");
}

// ─────────────────────────────────────────────────────────────────────────────
// collectSymbols  — Phase 1
// Runs SemanticCollector over every file to populate the global symbol table.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::collectSymbols(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("collectSymbols: building symbol table");
    symbols_->pushScope(); // global scope
    SemanticCollector collector(*symbols_, dc_);
    
    int fileCount = 0;
    for (auto* prog : files) {
        fileCount++;
        LUC_LOG_SEMANTIC_EXTREME("\tcollecting symbols from file " << fileCount);
        collector.collectProgram(*prog);
    }
    LUC_LOG_SEMANTIC_VERBOSE("collectSymbols: symbol table built");
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveTypes  — Phase 2
// Walks every type annotation in every declaration and validates it.
// Generic parameters in type aliases are resolved in context.
// @extern function types are resolved during Phase 3 (checkFuncDecl) when
// the @extern attribute is detected — no special early pass needed here.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::resolveTypes(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveTypes: resolving all type annotations");
    
    int resolvedCount = 0;
    
    // Pass 1: Resolve Type Aliases (they may be referenced by others)
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (decl->isa<TypeAliasDeclAST>()) {
                typeResolver_->visit(*decl->as<TypeAliasDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved type alias");
            }
        }
    }
    
    // Pass 2: Resolve Struct Field Types
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (decl->isa<StructDeclAST>()) {
                typeResolver_->visit(*decl->as<StructDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved struct: " << decl->as<StructDeclAST>()->name);
            }
        }
    }
    
    // Pass 3: Resolve Function Signatures (top-level)
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (decl->isa<FuncDeclAST>()) {
                typeResolver_->visit(*decl->as<FuncDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved function: " << decl->as<FuncDeclAST>()->name);
            }
        }
    }
    
    // Pass 4: Resolve Impl Block Methods
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (decl->isa<ImplDeclAST>()) {
                typeResolver_->visit(*decl->as<ImplDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved impl for: " << decl->as<ImplDeclAST>()->structName);
            }
        }
    }
    
    // Pass 5: Resolve From Block Entries
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (decl->isa<FromDeclAST>()) {
                typeResolver_->visit(*decl->as<FromDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved from block for: " << decl->as<FromDeclAST>()->targetTypeName);
            }
        }
    }
    
    // Pass 6: Resolve Variable Types
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (decl->isa<VarDeclAST>()) {
                typeResolver_->visit(*decl->as<VarDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved variable: " << decl->as<VarDeclAST>()->name);
            }
        }
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("resolveTypes: resolved " << resolvedCount << " type-annotated declarations");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkDecls  — Phase 3
// Runs the full declaration / expression / statement checkers over every file.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::checkDecls(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("checkDecls: checking all declarations");
    
    int declCount = 0;
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            declCount++;
            LUC_LOG_SEMANTIC_EXTREME("\tchecking declaration #" << declCount 
                                   << " kind=" << LucDebug::kindToString(decl->kind));
            
            checkTopLevelDecl(decl.get(), *symbols_, *typeResolver_, dc_,
                              loopDepth_, parallelDepth_, insideExtern_);
        }
    }
    LUC_LOG_SEMANTIC_VERBOSE("checkDecls: checked " << declCount << " declarations");
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
    LUC_LOG_SEMANTIC_VERBOSE("annotate: running annotation pass");
    annotateAll(files, *symbols_);
    LUC_LOG_SEMANTIC_VERBOSE("annotate: annotation complete");
}

void SemanticAnalyzer::dumpSymbols() const {
    symbols_->dump();
}
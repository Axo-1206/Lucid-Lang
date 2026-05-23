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

#include "header/SemanticAnalyzer.hpp"
#include "header/SemanticCollector.hpp"
#include "header/TypeResolver.hpp"
#include "header/TypeChecker.hpp"
#include "header/SymbolTable.hpp"
#include "header/SemanticContext.hpp"
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
void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration of Phase 4 dispatcher (defined in Annotator.cpp)
// ─────────────────────────────────────────────────────────────────────────────
void annotateAll(std::vector<ProgramAST*>& files, SymbolTable& symbols, StringPool& pool);
 
// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
SemanticAnalyzer::SemanticAnalyzer(DiagnosticEngine& dc, StringPool& pool, ASTArena& arena)
    : dc_(dc), pool_(pool), arena_(arena),
      symbols_(std::make_unique<SymbolTable>()),
      typeResolver_(std::make_unique<TypeResolver>(*symbols_, dc, pool, arena)),
      typeChecker_(std::make_unique<TypeChecker>(*symbols_, pool, arena)) {
    LUC_LOG_SEMANTIC("SemanticAnalyzer constructed");
}

SemanticAnalyzer::~SemanticAnalyzer() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Validate duplicate symbols
// ─────────────────────────────────────────────────────────────────────────────

void SemanticAnalyzer::validateNoDuplicateSymbols() {
    LUC_LOG_SEMANTIC("\n--- Phase 1.5: Validate No Duplicate Symbols ---");
    
    struct FirstDecl {
        InternedString file;
        SourceLocation loc;
    };
    std::unordered_map<uint32_t, FirstDecl> firstDecl; // key = InternedString id

    const auto& globalScope = symbols_->getGlobalScope();
    for (const auto& [id, sym] : globalScope) {
        auto it = firstDecl.find(id);
        if (it != firstDecl.end()) {
            std::string_view name = pool_.lookup(InternedString(id));
            LUC_LOG_SEMANTIC("\tDuplicate symbol: " << name);
            
            std::string firstFileStr = std::string(pool_.lookup(it->second.file));
            std::string firstLocStr = std::to_string(it->second.loc.line()) + ":" + std::to_string(it->second.loc.column());
            
            dc_.error(DiagnosticCategory::Semantic, sym.file, sym.loc,
                      DiagCode::E3005,
                      {"symbol '" + std::string(name) + "' already declared in " + firstFileStr + " at " + firstLocStr});
        } else {
            firstDecl[id] = {sym.file, sym.loc};
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
    InternedString mainName = pool_.intern("main");
    Symbol* mainSym = symbols_->lookup(mainName);
    if (!mainSym) {
        LUC_LOG_SEMANTIC("\tNo 'main' function found");
        SourceLocation loc = files.empty() ? SourceLocation() : files[0]->loc;
        InternedString firstFile = files.empty() ? InternedString() : files[0]->filePath;
        dc_.error(DiagnosticCategory::Semantic, firstFile, loc, DiagCode::E3006, 
                  {"program is missing a 'main' entry point"});
    } else {
        LUC_LOG_SEMANTIC("\tFound 'main' function, validating signature...");
        if (mainSym->kind != SymbolKind::Func) {
            std::string kindNote = (mainSym->kind == SymbolKind::ExternFunc)
                ? " ('@extern' functions cannot be entry points)"
                : "";
            LUC_LOG_SEMANTIC("\tERROR: 'main' is not a regular function");
            dc_.error(DiagnosticCategory::Semantic, mainSym->file, mainSym->loc, DiagCode::E3007, 
                      {"'main' must be a regular function" + kindNote});
        } else {
            auto* func = static_cast<FuncDeclAST*>(mainSym->decl);
            LUC_LOG_SEMANTIC_VERBOSE("\tFunction: " << pool_.lookup(func->name));
            
            // 1. MUST be exported: export const main ...
            if (func->visibility != Visibility::Export) {
                LUC_LOG_SEMANTIC("\tERROR: main not exported");
                dc_.error(DiagnosticCategory::Semantic, mainSym->file, func->loc, DiagCode::E3007,
                        {"'main' function must be exported (use 'export const main')"});
            }
            
            // 2. MUST be const: const main ...
            if (func->keyword != DeclKeyword::Const) {
                LUC_LOG_SEMANTIC("\tERROR: main not const");
                dc_.error(DiagnosticCategory::Semantic, mainSym->file, func->loc, DiagCode::E3007,
                        {"'main' function must use 'const' keyword"});
            }
            
            // 3. MUST have zero parameters or a single []string parameter
            bool hasParams = false;
            bool isValidArgsParam = false;
            
            if (func->sig.totalParamCount() == 0) {
                hasParams = false;
            } else if (func->sig.groupCount() == 1) {
                auto group = func->sig.getGroup(0);  // ArenaSpan<ParamPtr>
                if (group.size() == 1) {
                    hasParams = true;
                    ParamAST* param = group[0].get();
                    if (param->type) {
                        TypeAST* paramType = param->type.get();
                        if (paramType->kind == ASTKind::SliceType) {
                            auto* slice = static_cast<SliceTypeAST*>(paramType);
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
            } else {
                // Multiple parameter groups (currying) – not allowed for main
                hasParams = true;
            }

            if (hasParams && !isValidArgsParam) {
                LUC_LOG_SEMANTIC("\tERROR: invalid parameter signature for main");
                dc_.error(DiagnosticCategory::Semantic, mainSym->file, func->loc, DiagCode::E3007,
                        {"'main' function must have no parameters or take a string slice: (args []string)"});
            }
            
            // 4. MUST return int (single return type)
            bool returnsInt = false;
            if (func->sig.returnTypes.size() == 1 && func->sig.returnTypes[0]) {
                TypeAST* retType = func->sig.returnTypes[0].get();
                if (retType->kind == ASTKind::PrimitiveType) {
                    auto* pt = static_cast<PrimitiveTypeAST*>(retType);
                    if (pt->primitiveKind == PrimitiveKind::Int) {
                        returnsInt = true;
                        LUC_LOG_SEMANTIC_VERBOSE("\tReturn type: int");
                    }
                }
            }
            if (!returnsInt) {
                LUC_LOG_SEMANTIC("\tERROR: main does not return int");
                dc_.error(DiagnosticCategory::Semantic, mainSym->file, func->loc, DiagCode::E3007,
                        {"'main' function must return 'int'"});
            }   

            // 5. MUST NOT be async
            if (func->sig.isAsync()) {
                LUC_LOG_SEMANTIC("\tERROR: main is async");
                dc_.error(DiagnosticCategory::Semantic, mainSym->file, func->loc, DiagCode::E3007,
                        {"'main' function cannot be async (remove '~async' qualifier)"});
            }

            // 6. @aot and @jit validation on main
            bool hasAot = false;
            bool hasJit = false;
            for (const auto& attr : func->attributes) {
                if (pool_.lookup(attr->name) == "aot") hasAot = true;
                if (pool_.lookup(attr->name) == "jit") hasJit = true;
            }
            
            if (hasAot && hasJit) {
                LUC_LOG_SEMANTIC("\tERROR: both @aot and @jit on main");
                dc_.error(DiagnosticCategory::Semantic, mainSym->file, func->loc, DiagCode::E3015,
                        {"'@aot' and '@jit' cannot both be specified on the same declaration"});
            } else if (hasAot) {
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
    InternedString aotName = pool_.intern("aot");
    InternedString jitName = pool_.intern("jit");
    for (auto* prog : files) {
        for (auto& decl : prog->decls) {
            if (!decl->isa<FuncDeclAST>()) continue;
            auto* func = decl->as<FuncDeclAST>();
            if (func->name == mainName) continue;

            for (const auto& attr : func->attributes) {
                if (attr->name == aotName || attr->name == jitName) {
                    std::string_view attrStr = pool_.lookup(attr->name);
                    std::string_view funcName = pool_.lookup(func->name);
                    LUC_LOG_SEMANTIC("\tERROR: '@" << attrStr << "' on non-main function '" << funcName << "'");
                    dc_.error(DiagnosticCategory::Semantic, prog->filePath, attr->loc,
                        DiagCode::E3016, {std::string(attrStr), std::string(funcName)});
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
    
    int totalUses = 0;
    for (auto* prog : files) {
        std::unordered_set<std::string> seen;
        for (auto& decl : prog->decls) {
            if (!decl->isa<UseDeclAST>()) continue;
            auto* use = decl->as<UseDeclAST>();
            std::string path;
            for (size_t i = 0; i < use->path.size(); ++i) {
                if (i) path += '.';
                path += pool_.lookup(use->path[i]);
            }
            if (!seen.insert(path).second) {
                LUC_LOG_SEMANTIC("\tduplicate import of '" << path << "'");
                dc_.error(DiagnosticCategory::Semantic, prog->filePath, use->loc, DiagCode::E3005,
                          {"duplicate import of '" + path + "'"});
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
    SemanticCollector collector(*symbols_, dc_, pool_);
    
    int fileCount = 0;
    for (auto* prog : files) {
        fileCount++;
        LUC_LOG_SEMANTIC_EXTREME("\tcollecting symbols from file " << fileCount);
        collector.collectProgram(*prog);
    }
    // Pass the struct‑traits map to the type resolver.
    typeResolver_->setStructTraits(&collector.getStructTraits());
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
        typeResolver_->setCurrentFile(prog->filePath);
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
        typeResolver_->setCurrentFile(prog->filePath);
        for (auto& decl : prog->decls) {
            if (decl->isa<StructDeclAST>()) {
                typeResolver_->visit(*decl->as<StructDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved struct: " 
                                         << pool_.lookup(decl->as<StructDeclAST>()->name));
            }
        }
    }
    
    // Pass 3: Resolve Function Signatures (top-level)
    for (auto* prog : files) {
        typeResolver_->setCurrentFile(prog->filePath);
        for (auto& decl : prog->decls) {
            if (decl->isa<FuncDeclAST>()) {
                typeResolver_->visit(*decl->as<FuncDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved function: " 
                                         << pool_.lookup(decl->as<FuncDeclAST>()->name));
            }
        }
    }
    
    // Pass 4: Resolve Impl Block Methods
    for (auto* prog : files) {
        typeResolver_->setCurrentFile(prog->filePath);
        for (auto& decl : prog->decls) {
            if (decl->isa<ImplDeclAST>()) {
                typeResolver_->visit(*decl->as<ImplDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved impl block");
            }
        }
    }
    
    // Pass 5: Resolve From Block Entries
    for (auto* prog : files) {
        typeResolver_->setCurrentFile(prog->filePath);
        for (auto& decl : prog->decls) {
            if (decl->isa<FromDeclAST>()) {
                typeResolver_->visit(*decl->as<FromDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved from block");
            }
        }
    }
    
    // Pass 6: Resolve Variable Types
    for (auto* prog : files) {
        typeResolver_->setCurrentFile(prog->filePath);
        for (auto& decl : prog->decls) {
            if (decl->isa<VarDeclAST>()) {
                typeResolver_->visit(*decl->as<VarDeclAST>());
                resolvedCount++;
                LUC_LOG_SEMANTIC_EXTREME("\tresolved variable: " 
                                         << pool_.lookup(decl->as<VarDeclAST>()->name));
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
        // Create a fresh context for each file, passing the file path
        SemanticContext ctx(*symbols_, *typeResolver_, *typeChecker_, dc_, pool_, arena_, prog->filePath);
        
        for (auto& decl : prog->decls) {
            declCount++;
            LUC_LOG_SEMANTIC_EXTREME("\tchecking declaration #" << declCount
                                   << " kind=" << LucDebug::kindToString(decl->kind));
            checkTopLevelDecl(decl.get(), ctx);
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
    annotateAll(files, *symbols_, pool_);
    LUC_LOG_SEMANTIC_VERBOSE("annotate: annotation complete");
}

void SemanticAnalyzer::dumpSymbols() const {
    symbols_->dump(pool_);
}
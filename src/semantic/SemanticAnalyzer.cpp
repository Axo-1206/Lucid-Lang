/**
 * @file SemanticAnalyzer.cpp
 *
 * @responsibility Orchestrates all four semantic phases across every file in the package.
 *
 * All diagnostic reporting uses the global diagnostic module via SemanticContext.
 */

#include "SemanticAnalyzer.hpp"
#include "collectors/SemanticCollector.hpp"
#include "resolveType/TypeResolver.hpp"
#include "SymbolTable.hpp"
#include "helpers/SemanticContext.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx);
void annotateAll(std::vector<ProgramAST*>& files, SemanticContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Constructor – initialises components and context
// ─────────────────────────────────────────────────────────────────────────────
SemanticAnalyzer::SemanticAnalyzer(StringPool& pool, ASTArena& arena)
    : symbols_(),
      ctx_(pool, arena, &symbols_),           // ctx_ initialized with symbols_
      resolver_(ctx_),                        // resolver_ gets ctx_ reference
      collector_() {
    LUC_LOG_SEMANTIC("SemanticAnalyzer constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// analyze  — top‑level entry point
// ─────────────────────────────────────────────────────────────────────────────
bool SemanticAnalyzer::analyze(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC("\n=== SEMANTIC_ANALYZE - START ===");
    LUC_LOG_SEMANTIC_VERBOSE("\tProcessing " << files.size() << " file(s)");

    // Phase 0: Resolve Imports (duplicate detection)
    LUC_LOG_SEMANTIC("\n--- Phase 0: Resolve Imports ---");
    resolveImports(files);
    if (diagnostic::hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 0 FAILED with errors");
        return false;
    }

    // Phase 1: Collect Symbols.
    LUC_LOG_SEMANTIC("\n--- Phase 1: Collect Symbols ---");
    collectSymbols(files);
    if (diagnostic::hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 1 FAILED with errors");
        return false;
    }

    validateNoDuplicateSymbols();
    dumpSymbols();

    // Phase 2: Resolve Types.
    LUC_LOG_SEMANTIC("\n--- Phase 2: Resolve Types ---");
    resolveTypes(files);
    LUC_LOG_SEMANTIC("Phase 2 completed (warnings may exist)");

    // Phase 3: Check Decls.
    LUC_LOG_SEMANTIC("\n--- Phase 3: Check Decls ---");
    checkDecls(files);
    if (diagnostic::hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 3 FAILED with errors");
        return false;
    }

    // Phase 3.5: Entry point detection and compilation mode
    LUC_LOG_SEMANTIC("\n--- Phase 3.5: Entry point detection ---");
    validateEntryPoint();

    // Phase 4: Annotate.
    LUC_LOG_SEMANTIC("\n--- Phase 4: Annotate ---");
    annotate(files);

    LUC_LOG_SEMANTIC("=== SemanticAnalyzer::analyze END ===");
    return !diagnostic::hasErrors();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveImports  — Phase 0 – detects duplicate `use` in the same file
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
                path += ctx_.pool.lookup(use->path[i]);
            }
            if (!seen.insert(path).second) {
                ctx_.error(use->loc, DiagCode::E2005, "duplicate import of '", path, "'");
            } else {
                totalUses++;
                LUC_LOG_SEMANTIC_EXTREME("\tregistered import: " << path);
            }
        }
    }
    LUC_LOG_SEMANTIC_VERBOSE("resolveImports: registered " << totalUses << " unique imports");
}

// ─────────────────────────────────────────────────────────────────────────────
// collectSymbols  — Phase 1 – populates global symbol table
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::collectSymbols(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("collectSymbols: building symbol table");
    ctx_.symbols->pushScope(); // global scope

    for (auto* prog : files) {
        ctx_.currentFile = prog->filePath;
        collector_.collectProgram(*prog, ctx_);
    }
    resolver_.setStructTraits(&collector_.getStructTraits());
    LUC_LOG_SEMANTIC_VERBOSE("collectSymbols: symbol table built");
}

// ─────────────────────────────────────────────────────────────────────────────
// validateNoDuplicateSymbols  — global duplicate check
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::validateNoDuplicateSymbols() {
    LUC_LOG_SEMANTIC("\n--- Phase 1.5: Validate No Duplicate Symbols ---");

    struct FirstDecl {
        InternedString file;
        SourceLocation loc;
    };
    std::unordered_map<uint32_t, FirstDecl> firstDecl;

    const auto& globalScope = ctx_.symbols->getGlobalScope();
    for (const auto& [id, sym] : globalScope) {
        auto it = firstDecl.find(id);
        if (it != firstDecl.end()) {
            std::string_view name = ctx_.pool.lookup(InternedString(id));
            std::string firstFileStr = std::string(ctx_.pool.lookup(it->second.file));
            std::string firstLocStr = std::to_string(it->second.loc.line()) + ":" +
                                      std::to_string(it->second.loc.column());
            ctx_.error(sym.loc, DiagCode::E2005,
                      "symbol '", name, "' already declared in ", firstFileStr, " at ", firstLocStr);
        } else {
            firstDecl[id] = {sym.file, sym.loc};
        }
    }
    LUC_LOG_SEMANTIC_VERBOSE("Phase 1.5 complete: " << firstDecl.size() << " unique symbols");
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveTypes  — Phase 2 – resolves all type annotations
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::resolveTypes(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveTypes: resolving all type annotations");

    int resolvedCount = 0;

    for (auto* prog : files) {
        ctx_.currentFile = prog->filePath;
        for (auto& decl : prog->decls) {
            if (decl->isa<TypeAliasDeclAST>()) {
                resolver_.resolveTypeAlias(*decl->as<TypeAliasDeclAST>());
                resolvedCount++;
            } else if (decl->isa<StructDeclAST>()) {
                resolver_.resolveStructFields(*decl->as<StructDeclAST>());
                resolvedCount++;
            } else if (decl->isa<FuncDeclAST>()) {
                resolver_.resolveFunctionSignature(*decl->as<FuncDeclAST>());
                resolvedCount++;
            } else if (decl->isa<ImplDeclAST>()) {
                resolver_.resolveImplMethods(*decl->as<ImplDeclAST>());
                resolvedCount++;
            } else if (decl->isa<FromDeclAST>()) {
                resolver_.resolveFromEntries(*decl->as<FromDeclAST>());
                resolvedCount++;
            } else if (decl->isa<VarDeclAST>()) {
                resolver_.resolveVarType(*decl->as<VarDeclAST>());
                resolvedCount++;
            }
        }
    }

    LUC_LOG_SEMANTIC_VERBOSE("resolveTypes: resolved " << resolvedCount << " type-annotated declarations");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkDecls  — Phase 3 – full semantic checking
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::checkDecls(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("checkDecls: checking all declarations");

    int declCount = 0;
    for (auto* prog : files) {
        ctx_.currentFile = prog->filePath;
        for (auto& decl : prog->decls) {
            declCount++;
            LUC_LOG_SEMANTIC_EXTREME("\tchecking declaration #" << declCount
                                    << " kind=" << LucDebug::kindToString(decl->kind));
            checkTopLevelDecl(decl.get(), ctx_);
        }
    }
    LUC_LOG_SEMANTIC_VERBOSE("checkDecls: checked " << declCount << " declarations");
}

// ─────────────────────────────────────────────────────────────────────────────
// validateEntryPoint  — checks 'main' and sets compilation mode
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::validateEntryPoint() {
    LUC_LOG_SEMANTIC("validateEntryPoint: looking for 'main' function");

    InternedString mainName = ctx_.pool.intern("main");
    Symbol* mainSym = ctx_.symbols->lookup(mainName);
    if (!mainSym) {
        ctx_.error(SourceLocation(), DiagCode::E2006, "program is missing a 'main' entry point");
        return;
    }

    if (mainSym->kind != SymbolKind::Func) {
        ctx_.error(mainSym->loc, DiagCode::E2007, "'main' must be a regular function (not extern)");
        return;
    }

    auto* func = static_cast<FuncDeclAST*>(mainSym->decl);

    // 1. Must be exported
    if (func->visibility != Visibility::Export) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function must be exported (use 'export const main')");
    }

    // 2. Must be const
    if (func->keyword != DeclKeyword::Const) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function must use 'const' keyword");
    }

    // 3. Parameter validation
    bool hasParams = false;
    bool isValidArgsParam = false;

    const FuncSignature& sig = func->funcType->sig;
    size_t totalParams = sig.totalParamCount();

    if (totalParams == 0) {
        hasParams = false;
    } else if (sig.groupCount() == 1) {
        auto group = sig.getGroup(0);
        if (group.size() == 1) {
            hasParams = true;
            ParamAST* param = group[0].get();
            if (param->type && param->type->isa<ArrayTypeAST>()) {
                auto* arrType = param->type->as<ArrayTypeAST>();
                if (arrType->arrayKind == ArrayKind::Slice) {
                    TypeAST* elemType = arrType->element.get();
                    if (elemType && elemType->isa<PrimitiveTypeAST>()) {
                        auto* prim = elemType->as<PrimitiveTypeAST>();
                        if (prim->primitiveKind == PrimitiveKind::String) {
                            isValidArgsParam = true;
                        }
                    }
                }
            }
        }
    } else {
        hasParams = true;
    }

    if (hasParams && !isValidArgsParam) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function must have no parameters or take a string slice: (args []string)");
    }

    // 4. Return type must be int
    bool returnsInt = false;
    if (sig.returnTypes.size() == 1 && sig.returnTypes[0]) {
        TypeAST* retType = sig.returnTypes[0].get();
        if (retType->isa<PrimitiveTypeAST>()) {
            auto* prim = retType->as<PrimitiveTypeAST>();
            if (prim->primitiveKind == PrimitiveKind::Int) {
                returnsInt = true;
            }
        }
    }

    if (!returnsInt) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function must return 'int'");
    }

    // 5. Must not be async
    if (func->funcType && func->funcType->isAsync()) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function cannot be async (remove '~async' qualifier)");
    }

    // 6. @aot / @jit validation
    bool hasAot = false, hasJit = false;
    for (const auto& attr : func->attributes) {
        std::string_view attrName = ctx_.pool.lookup(attr->name);
        if (attrName == "aot") hasAot = true;
        if (attrName == "jit") hasJit = true;
    }

    if (hasAot && hasJit) {
        ctx_.error(func->loc, DiagCode::E2013, "'@aot' and '@jit' cannot both be specified on the same declaration");
    } else if (hasAot) {
        compilationMode_ = CompilationMode::AOT;
    } else if (hasJit) {
        compilationMode_ = CompilationMode::JIT;
    } else {
        compilationMode_ = CompilationMode::AOT;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// annotate  — Phase 4
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::annotate(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("annotate: running annotation pass");
    annotateAll(files, ctx_);
    LUC_LOG_SEMANTIC_VERBOSE("annotate: annotation complete");
}

// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::dumpSymbols() const {
    ctx_.symbols->dump(ctx_.pool);
}
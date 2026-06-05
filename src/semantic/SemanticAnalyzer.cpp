/**
 * @file SemanticAnalyzer.cpp
 *
 * @responsibility Orchestrates all four semantic phases across every file in the package.
 *
 * All diagnostic reporting uses the global diagnostic module via SemanticContext.
 */

#include "SemanticAnalyzer.hpp"
#include "collectors/SemanticCollector.hpp"
#include "resolveType/TypeDispatcher.hpp"
#include "SymbolTable.hpp"
#include "helpers/SemanticContext.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/SymbolDumper.hpp"
#include "semantic/helpers/NameMangler.hpp"

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
      dispatcher_(ctx_),                      // dispatcher_ gets ctx_ reference
      collector_() {
    LUC_LOG_SEMANTIC("SemanticAnalyzer constructed");
    
    // Wire up the dispatcher to the context
    ctx_.dispatcher = &dispatcher_;
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
    
    if (LucDebug::isDebugEnabled("DUMP_SYMBOL")) {
        std::string dump = LucDebug::dumpSymbolTable(symbols_, ctx_.pool, LucDebug::getVerbosity());
        LucDebug::getDebugStream() << dump << std::endl;
    }

    // Phase 2: Resolve Types.
    LUC_LOG_SEMANTIC("\n--- Phase 2: Resolve Types ---");
    resolveTypes(files);
    LUC_LOG_SEMANTIC("Phase 2 completed (warnings may exist)");
    
    // Phase 2.5: Build Trait Conformance Map
    LUC_LOG_SEMANTIC("\n--- Phase 2.5: Build Trait Conformance Map ---");
    buildTraitConformanceMap();
    LUC_LOG_SEMANTIC("Phase 2.5 completed");

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
                dispatcher_.resolveTypeAlias(*decl->as<TypeAliasDeclAST>());
                resolvedCount++;
            } else if (decl->isa<StructDeclAST>()) {
                dispatcher_.resolveStructFields(*decl->as<StructDeclAST>());
                resolvedCount++;
            } else if (decl->isa<FuncDeclAST>()) {
                dispatcher_.resolveFunctionSignature(*decl->as<FuncDeclAST>());
                resolvedCount++;
            } else if (decl->isa<ImplDeclAST>()) {
                dispatcher_.resolveImplMethods(*decl->as<ImplDeclAST>());
                resolvedCount++;
            } else if (decl->isa<FromDeclAST>()) {
                dispatcher_.resolveFromEntries(*decl->as<FromDeclAST>());
                resolvedCount++;
            } else if (decl->isa<VarDeclAST>()) {
                dispatcher_.resolveVarType(*decl->as<VarDeclAST>());
                resolvedCount++;
            }
        }
    }

    LUC_LOG_SEMANTIC_VERBOSE("resolveTypes: resolved " << resolvedCount << " type-annotated declarations");
}

// ─────────────────────────────────────────────────────────────────────────────
// buildTraitConformanceMap  — Phase 2.5 – builds type → traits mapping
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::buildTraitConformanceMap() {
    LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: building trait conformance map");
    
    // Clear existing map
    ctx_.typeTraits.clear();
    
    // Find all impl declarations in the symbol table
    // Iterate through all symbols in global scope
    const auto& globalScope = ctx_.symbols->getGlobalScope();
    int implCount = 0;
    int traitCount = 0;
    
    for (const auto& [id, sym] : globalScope) {
        if (sym.kind != SymbolKind::Impl) continue;
        
        auto* impl = static_cast<ImplDeclAST*>(sym.decl);
        if (!impl) continue;
        
        // Skip impls without trait conformance
        if (!impl->traitRef) continue;
        
        // Resolve the target type
        if (!impl->targetType) {
            LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: impl has no target type");
            continue;
        }
        
        // Resolve the target type through the dispatcher
        TypeAST* resolvedTarget = dispatcher_.resolveType(impl->targetType.get());
        if (!resolvedTarget) {
            ctx_.error(impl->loc, DiagCode::E2016, 
                      "cannot resolve target type for trait conformance");
            continue;
        }
        
        // Get canonical mangled key for this type
        // Unwrap aliases first
        TypeAST* unwrapped = resolvedTarget;
        while (unwrapped && unwrapped->isa<NamedTypeAST>()) {
            auto* named = unwrapped->as<NamedTypeAST>();
            Symbol* typeSym = ctx_.symbols->lookup(named->name);
            if (!typeSym || typeSym->kind != SymbolKind::TypeAlias) break;
            if (typeSym->type) {
                unwrapped = typeSym->type;
            } else {
                break;
            }
        }
        
        InternedString typeKey = ctx_.pool.intern(
            NameMangler::mangleType(unwrapped, ctx_.pool, ctx_.symbols)
        );
        
        InternedString traitName = impl->traitRef->name;
        
        // Check for duplicate conformance
        auto& traitList = ctx_.typeTraits[typeKey];
        bool alreadyExists = false;
        for (InternedString existing : traitList) {
            if (existing == traitName) {
                alreadyExists = true;
                ctx_.warning(impl->loc, DiagCode::W6015,
                            "type '" + std::string(ctx_.pool.lookup(typeKey)) + 
                            "' already implements trait '" + 
                            std::string(ctx_.pool.lookup(traitName)) + "'");
                break;
            }
        }
        
        if (!alreadyExists) {
            traitList.push_back(traitName);
            traitCount++;
            LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: " 
                                     << ctx_.pool.lookup(typeKey) << " implements "
                                     << ctx_.pool.lookup(traitName));
        }
        
        implCount++;
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: processed " << implCount 
                            << " impl blocks, recorded " << traitCount 
                            << " trait conformances across " << ctx_.typeTraits.size() 
                            << " types");
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
    if (!func) return;

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

    // Ensure funcType exists
    if (!func->funcType) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function has no type signature");
        return;
    }

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
        ctx_.error(func->loc, DiagCode::E2007, 
                  "'main' function must have no parameters or take a string slice: (args [_, string])");
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
        ctx_.error(func->loc, DiagCode::E2007, 
                  "'main' function cannot be async (remove '~async' qualifier)");
    }

    // 6. @aot / @jit validation
    bool hasAot = false, hasJit = false;
    for (const auto& attr : func->attributes) {
        std::string_view attrName = ctx_.pool.lookup(attr->name);
        if (attrName == "aot") hasAot = true;
        if (attrName == "jit") hasJit = true;
    }

    if (hasAot && hasJit) {
        ctx_.error(func->loc, DiagCode::E2013, 
                  "'@aot' and '@jit' cannot both be specified on the same declaration");
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

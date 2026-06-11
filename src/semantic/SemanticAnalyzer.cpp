/**
 * @file SemanticAnalyzer.cpp
 * @brief Implementation of the semantic analysis orchestrator.
 */

#include "SemanticAnalyzer.hpp"
#include "semantic/checker/decl/DeclChecker.hpp"
#include "semantic/checker/expr/ExprChecker.hpp"
#include "semantic/checker/stmt/StmtChecker.hpp"
#include "semantic/checker//TypeChecker.hpp"
#include "Annotator.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "ast/DeclAST.hpp"
#include "ast/TypeAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "semantic/helpers/NameMangler.hpp"

#include <unordered_set>

// ============================================================================
// Constructor
// ============================================================================

SemanticAnalyzer::SemanticAnalyzer(StringPool& pool, ASTArena& arena)
    : scope_()
    , ctx_(pool, arena, scope_)
    , typeResolver_(ctx_)
    , collector_(ctx_)
    , compilationMode_(CompilationMode::AOT) {
    LUC_LOG_SEMANTIC("SemanticAnalyzer constructed");
    
    // Wire up the type resolver to the context
    ctx_.typeResolver = &typeResolver_;
}

// ============================================================================
// analyze – Main Entry Point
// ============================================================================

bool SemanticAnalyzer::analyze(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC("\n=== SEMANTIC_ANALYZE - START ===");
    LUC_LOG_SEMANTIC_VERBOSE("\tProcessing " << files.size() << " file(s)");

    // Phase 1: Collect Declarations
    LUC_LOG_SEMANTIC("\n--- Phase 1: Collect Declarations ---");
    collectDeclarations(files);
    if (diagnostic::hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 1 FAILED with errors");
        return false;
    }

    // Phase 1.5: Validate No Duplicate Declarations
    validateNoDuplicateDeclarations();
    
    // Phase 2: Resolve Types
    LUC_LOG_SEMANTIC("\n--- Phase 2: Resolve Types ---");
    resolveTypes(files);
    if (diagnostic::hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 2 FAILED with errors");
        return false;
    }
    LUC_LOG_SEMANTIC("Phase 2 completed");
    
    // Phase 2.5: Build Trait Conformance Map
    LUC_LOG_SEMANTIC("\n--- Phase 2.5: Build Trait Conformance Map ---");
    buildTraitConformanceMap();
    LUC_LOG_SEMANTIC("Phase 2.5 completed");

    // Phase 3: Check Declarations
    LUC_LOG_SEMANTIC("\n--- Phase 3: Check Declarations ---");
    checkDeclarations(files);
    if (diagnostic::hasErrors()) {
        LUC_LOG_SEMANTIC("Phase 3 FAILED with errors");
        return false;
    }

    // Phase 3.5: Entry point validation
    LUC_LOG_SEMANTIC("\n--- Phase 3.5: Entry point detection ---");
    validateEntryPoint();

    // Phase 4: Annotate
    LUC_LOG_SEMANTIC("\n--- Phase 4: Annotate ---");
    annotate(files);

    LUC_LOG_SEMANTIC("=== SemanticAnalyzer::analyze END ===");
    return !diagnostic::hasErrors();
}

// ============================================================================
// Phase 1: Collect Declarations
// ============================================================================

void SemanticAnalyzer::collectDeclarations(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("collectDeclarations: building scope stack");
    
    // Push global scope
    scope_.push();
    
    for (auto* prog : files) {
        ctx_.currentFile = prog->filePath;
        
        // Collect all declarations in this file
        collector_.collectProgram(prog);
        
        // Also track use declarations for duplicate detection
        collectUseDeclarations(prog);
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("collectDeclarations: scope stack built, depth=" << scope_.depth());
}

void SemanticAnalyzer::collectUseDeclarations(ProgramAST* prog) {
    std::unordered_set<std::string> seen;
    
    for (auto& decl : prog->decls) {
        if (!decl->isa<UseDeclAST>()) continue;
        
        auto* use = decl->as<UseDeclAST>();
        
        // Build path string
        std::string path;
        for (size_t i = 0; i < use->path.size(); ++i) {
            if (i > 0) path += '.';
            path += ctx_.pool.lookup(use->path[i]);
        }
        
        // Check for duplicate within this file
        if (!seen.insert(path).second) {
            ctx_.error(use->loc, DiagCode::E2005, "duplicate import of '", path, "'");
        }
    }
}

// ============================================================================
// Phase 1.5: Validate No Duplicate Declarations
// ============================================================================

void SemanticAnalyzer::validateNoDuplicateDeclarations() {
    LUC_LOG_SEMANTIC("\n--- Phase 1.5: Validate No Duplicate Declarations ---");

    struct FirstDecl {
        InternedString file;
        SourceLocation loc;
    };
    
    // Check value namespace duplicates
    std::unordered_map<uint32_t, FirstDecl> firstValueDecl;
    const auto& globalScope = scope_.getGlobalScope();
    
    for (const auto& [id, decl] : globalScope.values) {
        auto it = firstValueDecl.find(id);
        if (it != firstValueDecl.end()) {
            std::string_view name = ctx_.pool.lookup(InternedString(id));
            ctx_.error(decl->loc, DiagCode::E2005,
                      "value '", name, "' already declared in file ",
                      std::string(ctx_.pool.lookup(it->second.file)));
        } else {
            firstValueDecl[id] = {decl->file, decl->loc};
        }
    }
    
    // Check type namespace duplicates
    std::unordered_map<uint32_t, FirstDecl> firstTypeDecl;
    for (const auto& [id, decl] : globalScope.types) {
        auto it = firstTypeDecl.find(id);
        if (it != firstTypeDecl.end()) {
            std::string_view name = ctx_.pool.lookup(InternedString(id));
            ctx_.error(decl->loc, DiagCode::E2005,
                      "type '", name, "' already declared in file ",
                      std::string(ctx_.pool.lookup(it->second.file)));
        } else {
            firstTypeDecl[id] = {decl->file, decl->loc};
        }
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("Phase 1.5 complete: " 
                             << firstValueDecl.size() << " unique values, "
                             << firstTypeDecl.size() << " unique types");
}

// ============================================================================
// Phase 2: Resolve Types
// ============================================================================

void SemanticAnalyzer::resolveTypes(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveTypes: resolving all type annotations");

    int resolvedCount = 0;

    for (auto* prog : files) {
        ctx_.currentFile = prog->filePath;
        
        for (auto& decl : prog->decls) {
            // Type aliases – resolve underlying type eagerly
            if (auto* alias = decl->as<TypeAliasDeclAST>()) {
                typeResolver_.resolveTypeAlias(alias);
                resolvedCount++;
            }
            // Structs – resolve field types, create self-type
            else if (auto* structDecl = decl->as<StructDeclAST>()) {
                typeResolver_.resolveStructFields(structDecl);
                resolvedCount++;
            }
            // Enums – create self-type
            else if (auto* enumDecl = decl->as<EnumDeclAST>()) {
                typeResolver_.resolveEnum(enumDecl);
                resolvedCount++;
            }
            // Traits – create self-type
            else if (auto* traitDecl = decl->as<TraitDeclAST>()) {
                typeResolver_.resolveTrait(traitDecl);
                resolvedCount++;
            }
            // Functions – resolve signature
            else if (auto* func = decl->as<FuncDeclAST>()) {
                typeResolver_.resolveFunctionSignature(func);
                resolvedCount++;
            }
            // Impl blocks – resolve target type and method signatures
            else if (auto* impl = decl->as<ImplDeclAST>()) {
                typeResolver_.resolveImpl(impl);
                resolvedCount++;
            }
            // From blocks – resolve conversion entries
            else if (auto* from = decl->as<FromDeclAST>()) {
                typeResolver_.resolveFrom(from);
                resolvedCount++;
            }
            // Variables – resolve declared type
            else if (auto* var = decl->as<VarDeclAST>()) {
                typeResolver_.resolveVarType(var);
                resolvedCount++;
            }
        }
    }

    LUC_LOG_SEMANTIC_VERBOSE("resolveTypes: resolved " << resolvedCount << " type-annotated declarations");
}

// ============================================================================
// Phase 2.5: Build Trait Conformance Map
// ============================================================================

void SemanticAnalyzer::buildTraitConformanceMap() {
    LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: building trait conformance map");
    
    ctx_.typeTraits.clear();
    
    // Get all impl declarations collected during Phase 1
    const auto& impls = collector_.getImpls();
    int implCount = 0;
    int traitCount = 0;
    
    for (ImplDeclAST* impl : impls) {
        // Skip impls without trait conformance
        if (!impl->traitRef) continue;
        
        // Resolve the target type
        if (!impl->targetType) {
            LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: impl has no target type");
            continue;
        }
        
        // Resolve the target type through the resolver
        TypeAST* resolvedTarget = typeResolver_.resolve(impl->targetType);
        if (!resolvedTarget) {
            ctx_.error(impl->loc, DiagCode::E2016, 
                      "cannot resolve target type for trait conformance");
            continue;
        }
        
        // Unwrap aliases to get canonical type
        TypeAST* unwrapped = typeResolver_.unwrapAlias(resolvedTarget);
        
        // Get canonical mangled key for this type
        std::string typeKey = NameMangler::mangleType(unwrapped, ctx_.pool);
        InternedString traitName = impl->traitRef->name;
        
        // Check for duplicate conformance
        auto& traitList = ctx_.typeTraits[typeKey];
        bool alreadyExists = false;
        for (InternedString existing : traitList) {
            if (existing == traitName) {
                alreadyExists = true;
                ctx_.warning(impl->loc, DiagCode::W6015,
                            "type '" + typeKey + 
                            "' already implements trait '" + 
                            std::string(ctx_.pool.lookup(traitName)) + "'");
                break;
            }
        }
        
        if (!alreadyExists) {
            traitList.push_back(traitName);
            traitCount++;
            LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: " 
                                     << typeKey << " implements "
                                     << ctx_.pool.lookup(traitName));
        }
        
        implCount++;
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("buildTraitConformanceMap: processed " << implCount 
                            << " impl blocks, recorded " << traitCount 
                            << " trait conformances across " << ctx_.typeTraits.size() 
                            << " types");
}

// ============================================================================
// Phase 3: Check Declarations
// ============================================================================

void SemanticAnalyzer::checkDeclarations(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("checkDeclarations: checking all declarations");

    int declCount = 0;
    for (auto* prog : files) {
        ctx_.currentFile = prog->filePath;
        for (auto& decl : prog->decls) {
            declCount++;
            LUC_LOG_SEMANTIC_EXTREME("\tchecking declaration #" << declCount
                                    << " kind=" << LucDebug::kindToString(decl->kind));
            checkTopLevelDecl(decl, ctx_);
        }
    }
    LUC_LOG_SEMANTIC_VERBOSE("checkDeclarations: checked " << declCount << " declarations");
}

// ============================================================================
// Phase 3.5: Validate Entry Point
// ============================================================================

void SemanticAnalyzer::validateEntryPoint() {
    LUC_LOG_SEMANTIC("validateEntryPoint: looking for 'main' function");

    InternedString mainName = ctx_.pool.intern("main");
    
    // Look up in value namespace (main is a function)
    ValueDeclAST* decl = scope_.lookupValue(mainName);
    if (!decl) {
        ctx_.error(SourceLocation(), DiagCode::E2006, "program is missing a 'main' entry point");
        return;
    }

    auto* func = decl->as<FuncDeclAST>();
    if (!func) {
        ctx_.error(decl->loc, DiagCode::E2007, "'main' must be a function");
        return;
    }

    // 1. Must be exported
    if (func->visibility != Visibility::Export) {
        ctx_.error(func->loc, DiagCode::E2007, 
                  "'main' function must be exported (use 'export const main')");
    }

    // 2. Must be const
    if (func->keyword != DeclKeyword::Const) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function must use 'const' keyword");
    }

    // 3. Parameter validation
    bool hasParams = false;
    bool isValidArgsParam = false;

    if (!func->funcType) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function has no type signature");
        return;
    }

    size_t totalParams = func->funcType->params.size();
    
    if (totalParams == 0) {
        hasParams = false;
    } else if (totalParams == 1) {
        hasParams = true;
        ParamAST* param = func->funcType->params[0];
        if (param->type && param->type->isa<ArrayTypeAST>()) {
            auto* arrType = param->type->as<ArrayTypeAST>();
            if (arrType->arrayKind == ArrayKind::Slice) {
                TypeAST* elemType = arrType->element;
                if (elemType && elemType->isa<PrimitiveTypeAST>()) {
                    auto* prim = elemType->as<PrimitiveTypeAST>();
                    if (prim->primitiveKind == PrimitiveKind::String) {
                        isValidArgsParam = true;
                    }
                }
            }
        }
    } else {
        hasParams = true;  // Too many parameters
    }

    if (hasParams && !isValidArgsParam) {
        ctx_.error(func->loc, DiagCode::E2007, 
                  "'main' function must have no parameters or take a string slice: (args [_, string])");
    }

    // 4. Return type must be int
    bool returnsInt = false;
    if (func->funcType->returnTypes.size() == 1) {
        TypeAST* retType = func->funcType->returnTypes[0];
        if (auto* prim = retType->as<PrimitiveTypeAST>()) {
            if (prim->primitiveKind == PrimitiveKind::Int) {
                returnsInt = true;
            }
        }
    }

    if (!returnsInt) {
        ctx_.error(func->loc, DiagCode::E2007, "'main' function must return 'int'");
    }

    // 5. Must not be async
    if (func->funcType->isAsync()) {
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
    
    LUC_LOG_SEMANTIC_VERBOSE("validateEntryPoint: compilation mode = " 
                             << (compilationMode_ == CompilationMode::AOT ? "AOT" : "JIT"));
}

// ============================================================================
// Phase 4: Annotate
// ============================================================================

void SemanticAnalyzer::annotate(std::vector<ProgramAST*>& files) {
    LUC_LOG_SEMANTIC_VERBOSE("annotate: running annotation pass");
    annotateAll(files, ctx_);
    LUC_LOG_SEMANTIC_VERBOSE("annotate: annotation complete");
}

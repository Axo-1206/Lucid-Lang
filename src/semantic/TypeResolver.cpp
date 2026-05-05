/**
 * @file TypeResolver.cpp
 *
 * @nutshell Crawls through basic type strings and turns them into explicit system bindings.
 *
 * @reason A string parsing 'std.vector<int>' is useless until bound. This logic processes those strings structurally and maps them against the SymbolTable.
 *
 * @responsibility Implementation of the Phase 2a semantic pass (resolving primitive and complex type ASTs).
 *
 * @logic Validates that named types exist, checks generics, validates nullable configurations, and enforces array semantics.
 *
 * @related TypeResolver.hpp, SemanticAnalyzer.cpp
 */

#include "SemanticHelpers.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// TypeResolver (constructor)  — Initializes the TypeResolver with core dependencies
//
// Binds the active symbol table to allow looking up global type names and the
// diagnostic engine to log error messages directly during resolution.
// ─────────────────────────────────────────────────────────────────────────────
TypeResolver::TypeResolver(SymbolTable& symbols, DiagnosticEngine& dc)
    : symbols_(symbols), dc_(dc) {
    LUC_LOG_SEMANTIC("TypeResolver constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveType  — Main entry point to validate and bind a type node
//
// Accepts a raw type node parsed from the AST, dispatches it to the correct 
// visitor implementation, and returns the resolved node if successful. 
// If resolution fails (e.g., undeclared identifier), it returns nullptr.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeResolver::resolveType(TypeAST* typeNode) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveType: starting with kind=" 
                        << (typeNode ? LucDebug::kindToString(typeNode->kind) : "null"));
    
    if (!typeNode) {
        LUC_LOG_SEMANTIC("resolveType: null type node -> nullptr");
        return nullptr;
    }
    
    resolved_ = nullptr;
    typeNode->accept(*this);
    
    bool success = (resolved_ != nullptr);
    LUC_LOG_SEMANTIC_VERBOSE("resolveType: " << (success ? "success" : "failed"));
    
    return resolved_;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(PrimitiveTypeAST)  — Resolves primitive language types
//
// Primitives (int, float, bool) are inherently valid built-in language targets and 
// do not require verification against a symbol map. They resolve immediately.
// ─────────────────────────────────────────────────────────────────────────────

void TypeResolver::visit(PrimitiveTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(PrimitiveTypeAST): kind=" 
                        << static_cast<int>(node.primitiveKind));
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(NamedTypeAST)  — Resolves custom user-defined types (Struct, Enum, etc)
//
// Verifies that the string name provided genuinely maps to a recognizable type 
// entity within the SymbolTable. Refuses aliases mapped to variables/functions 
// and recurses correctly into verifying any generic inner arguments included.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(NamedTypeAST& node) {
    LUC_LOG_SEMANTIC("visit(NamedTypeAST): name='" << node.name << "'");
    LUC_LOG_SEMANTIC_VERBOSE("\tgenericArgs count=" << node.genericArgs.size());
    LUC_LOG_SEMANTIC_VERBOSE("\thas genericParams_=" << (genericParams_ ? "true" : "false"));
    LUC_LOG_SEMANTIC_VERBOSE("\thas substitutionMap_=" << (substitutionMap_ ? "true" : "false"));
    
    // CHECK 1: Is this name a generic type parameter?
    if (genericParams_) {
        for (auto& gp : *genericParams_) {
            if (gp && gp->name == node.name) {
            LUC_LOG_SEMANTIC_VERBOSE("\tfound as generic param: '" << node.name << "'");
                
                // If we have a substitution map and this parameter is in it,
                // resolve to the substituted concrete type.
                if (substitutionMap_) {
                    auto it = substitutionMap_->find(node.name);
                    if (it != substitutionMap_->end()) {
                        LUC_LOG_SEMANTIC("\tsubstituting '" << node.name  << "' with concrete type");
                        resolved_ = it->second;
                        return;
                    }
                }

                node.isGenericParam = true;
                LUC_LOG_SEMANTIC("\tresolved as generic parameter (isGenericParam=true)");
                resolved_ = &node;
                return;
            }
        }
    }

    // If we reach here, this name is NOT a generic parameter.
    // Explicitly set false — important when the same NamedTypeAST node is
    // re-resolved in a different generic context (e.g. multiple checkFuncDecl
    // calls that each set different genericParams_ on the resolver).
    node.isGenericParam = false;
    LUC_LOG_SEMANTIC_VERBOSE("\tnot a generic param, looking up in symbol table");

    // CHECK 2: Lookup the identifier in the global symbol table.
    // Lookup the identifier natively defined by the programmer in the symbol table.
    Symbol* sym = symbols_.lookup(node.name);
    if (!sym) {
        LUC_LOG_SEMANTIC("\tERROR: type '" << node.name << "' not declared");
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                  "type '" + node.name + "' is not declared");
        resolved_ = nullptr;
        return;
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("\tfound symbol: kind=" << static_cast<int>(sym->kind));
    
    // Strict typing: We can only resolve identifiers that actually represent a type.
    if (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::Trait && sym->kind != SymbolKind::TypeAlias) {
        LUC_LOG_SEMANTIC("\tERROR: '" << node.name << "' is a value, not a type");
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "'" + node.name + "' is a value, not a type");
        resolved_ = nullptr;
        return;
    }
    
    // Transparently unwrap TypeAlias types.
    if (sym->kind == SymbolKind::TypeAlias) {
        LUC_LOG_SEMANTIC("\tunwrapping type alias '" << node.name << "'");
        TypeAST* resolvedAlias = resolveType(sym->type);
        if (resolvedAlias) {
            LUC_LOG_SEMANTIC("\talias resolved to: " << LucDebug::kindToString(resolvedAlias->kind));
            resolved_ = resolvedAlias;
        } else {
            LUC_LOG_SEMANTIC("\tERROR: failed to resolve alias");
            resolved_ = nullptr;
        }
        return;
    }

    // Drill down recursively into generic sub-arguments.
    LUC_LOG_SEMANTIC_VERBOSE("\tresolving " << node.genericArgs.size() << " generic arguments");
    for (size_t i = 0; i < node.genericArgs.size(); ++i) {
        LUC_LOG_SEMANTIC_EXTREME("\t\tresolving arg " << i);
        resolveType(node.genericArgs[i].get());
    }
    
    LUC_LOG_SEMANTIC("\tresolved NamedTypeAST: '" << node.name << "'");
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(NullableTypeAST)  — Safely verifies a nullable constraint wrapper
//
// Merely ensures that whatever inner type constraint is being requested as 
// nullable maps securely to a legitimate definition.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(NullableTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(NullableTypeAST)");
    if (node.inner) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving inner type");
        resolveType(node.inner.get());
    }
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FixedArrayTypeAST)  — Validates standard sized array declarations
//
// Array definitions map structurally correctly as long as their inner element 
// defines properly. Asserts that an array structurally cannot dictate a 
// capacity of strictly 0 items.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FixedArrayTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(FixedArrayTypeAST): size=" << node.size);
    
    if (node.size == 0) {
        LUC_LOG_SEMANTIC("\tERROR: array size must be > 0");
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "fixed array size must be greater than zero");
    }
    if (node.element) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving element type");
        resolveType(node.element.get());
    }
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(SliceTypeAST)  — Validates dynamic fat-pointer views
//
// Merely requires the internal element it presents a viewing lens over is mapped 
// realistically.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(SliceTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(SliceTypeAST)");
    if (node.element) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving element type");
        resolveType(node.element.get());
    }
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(DynamicArrayTypeAST)  — Validates heap-owned growable arrays
//
// Delegates resolution requirements straight down to its individual member element.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(DynamicArrayTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(DynamicArrayTypeAST)");
    if (node.element) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving element type");
        resolveType(node.element.get());
    }
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(RefTypeAST)  — Maps safe memory reference boundaries
//
// Assures the internal struct/primitive resolving to this safe view accurately maps.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(RefTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(RefTypeAST)");
    if (node.inner) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving inner type");
        resolveType(node.inner.get());
    }
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(PtrTypeAST)  — Validates raw C pointers in @extern contexts only
//
// The language forbids raw pointer types (*T) everywhere except declarations
// carrying the @extern attribute. The TypeResolver's insideExtern_ flag is
// set by checkFuncDecl / checkVarDecl when they detect @extern on the decl.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(PtrTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(PtrTypeAST)");
    LUC_LOG_SEMANTIC_VERBOSE("\tinsideExtern_=" << (insideExtern_ ? "true" : "false"));
    
    // Raw pointers are now allowed anywhere (just storage).
    // Operation restrictions are enforced in checkBinaryExpr, checkIndexExpr, etc.
    if (node.inner) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving inner type");
        resolveType(node.inner.get());
    }
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncTypeAST)  — Resolves dynamic callable functional configurations
//
// Safely iterates every incoming execution argument type, confirming mappings,
// before resolving any potential outgoing returned type mappings.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FuncTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(FuncTypeAST): params=" << node.params.size() 
                        << ", isNullable=" << node.isNullable);
    
    for (size_t i = 0; i < node.params.size(); ++i) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving param " << i);
        resolveType(node.params[i].get());
    }
    if (node.returnType) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving return type");
        resolveType(node.returnType.get());
    }
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncDeclAST)  — Resolves function parameter and return types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FuncDeclAST): name='" << node.name 
                   << "', paramGroups=" << node.paramGroups.size());
    
    resolveFunctionSignature(node);
    
    // CRITICAL: Point directly to AST-owned signature - NO CLONE
    Symbol* sym = symbols_.lookup(node.name);
    if (sym && node.signature) {
        sym->type = node.signature.get();
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << node.name 
                               << "' type to signature");
    }
    
    resolved_ = node.signature.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveFunctionSignature  — Core function signature resolution
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveFunctionSignature(FuncDeclAST& node) {
    // Set generic parameters context
    auto* savedGenericParams = genericParams_;
    genericParams_ = &node.genericParams;
    
    // Resolve return type
    TypeAST* returnType = nullptr;
    if (node.returnType) {
        returnType = resolveType(node.returnType.get());
        if (!returnType) {
            LUC_LOG_SEMANTIC("\tERROR: failed to resolve return type for '" << node.name << "'");
        }
    }
    
    // Resolve all parameter types
    for (auto& group : node.paramGroups) {
        for (auto& param : group) {
            TypeAST* paramType = resolveType(param->type.get());
            if (!paramType) {
                LUC_LOG_SEMANTIC("\tERROR: failed to resolve parameter type for '" 
                               << param->name << "' in function '" << node.name << "'");
            }
        }
    }
    
    // Build the resolved signature
    node.signature = buildResolvedSignature(node);
    
    // Restore generic parameters context
    genericParams_ = savedGenericParams;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveFunctionSignature  — Core method signature resolution
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveMethodSignature(MethodDeclAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveMethodSignature: " << node.name
                           << ", paramGroups=" << node.paramGroups.size());
    
    // Methods don't have their own generic parameters - they inherit from
    // the impl block's generic parameters, which are already set via
    // genericParams_ when visit(ImplDeclAST) is called.
    
    // Resolve return type
    if (node.returnType) {
        TypeAST* returnType = resolveType(node.returnType.get());
        if (!returnType) {
            LUC_LOG_SEMANTIC("\tERROR: failed to resolve return type for method '" << node.name << "'");
        }
    }
    
    // Resolve all parameter types in all curry groups
    for (auto& group : node.paramGroups) {
        for (auto& param : group) {
            if (param->type) {
                TypeAST* paramType = resolveType(param->type.get());
                if (!paramType) {
                    LUC_LOG_SEMANTIC("\tERROR: failed to resolve parameter type for '" 
                                   << param->name << "' in method '" << node.name << "'");
                }
            }
        }
    }
    
    // IMPORTANT: DO NOT rebuild the signature here
    // The signature was already built in Phase 1 (SemanticCollector) WITHOUT self
    // We only need to ensure the resolved types are available through the signature
    // The signature should already point to the correct TypeAST nodes
    
    // If for some reason the signature is null (defensive programming), build it now
    if (!node.signature) {
        LUC_LOG_SEMANTIC("\tWARNING: method signature was null, building now (should not happen)");
        node.signature = SemanticHelpers::buildResolvedMethodSignature(node);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// buildResolvedSignature  — Creates a FuncTypeAST from resolved parameter types
// ─────────────────────────────────────────────────────────────────────────────
TypePtr TypeResolver::buildResolvedSignature(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("buildResolvedSignature: " << node.name);
    
    // Get resolved return type
    TypeAST* returnType = nullptr;
    if (node.returnType) {
        returnType = node.returnType->resolvedType 
                    ? static_cast<TypeAST*>(node.returnType->resolvedType) 
                    : node.returnType.get();
    }
    
    if (node.paramGroups.empty()) {
        // No parameters - signature is just the return type (or void)
        if (returnType) {
            return SemanticHelpers::cloneType(returnType);
        }
        return nullptr;
    }
    
    // Start with return type as the innermost
    TypePtr curReturn = returnType ? SemanticHelpers::cloneType(returnType) : nullptr;
    
    // Wrap from last parameter group to first
    for (int i = static_cast<int>(node.paramGroups.size()) - 1; i >= 0; --i) {
        auto funcType = std::make_unique<FuncTypeAST>();
        funcType->isNullable = false;
        funcType->loc = node.loc;
        
        // Add resolved parameters for this group
        for (auto& param : node.paramGroups[i]) {
            TypeAST* paramType = param->type->resolvedType 
                     ? static_cast<TypeAST*>(param->type->resolvedType) 
                     : param->type.get();
            funcType->params.push_back(SemanticHelpers::cloneType(paramType));
        }
        
        // Set return type
        if (curReturn) {
            funcType->returnType = std::move(curReturn);
        }
        
        curReturn = std::move(funcType);
    }
    
    return curReturn;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(StructDeclAST)  — Resolves struct field types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(StructDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(StructDeclAST): name='" << node.name 
                   << "', fields=" << node.fields.size());
    
    resolveStructFields(node);
    
    // Ensure selfType exists
    if (!node.selfType) {
        node.selfType = std::make_unique<NamedTypeAST>(node.name);
        node.selfType->loc = node.loc;
    }
    
    // Point directly to AST-owned selfType
    Symbol* sym = symbols_.lookup(node.name);
    if (sym) {
        sym->type = node.selfType.get();
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << node.name 
                               << "' type to selfType");
    }
    
    resolved_ = node.selfType.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveStructFields  — Resolves each field's type
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveStructFields(StructDeclAST& node) {
    // Set generic parameters context for generic structs
    auto* savedGenericParams = genericParams_;
    genericParams_ = &node.genericParams;
    
    for (auto& field : node.fields) {
        if (field->type) {
            TypeAST* resolvedFieldType = resolveType(field->type.get());
            if (!resolvedFieldType) {
                LUC_LOG_SEMANTIC("\tERROR: failed to resolve field type for '" 
                               << field->name << "' in struct '" << node.name << "'");
            }
        }
    }
    
    genericParams_ = savedGenericParams;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)  — Resolves variable type annotations
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(VarDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(VarDeclAST): name='" << node.name << "'");
    
    TypeAST* resolvedType = nullptr;
    if (node.type) {
        resolvedType = resolveType(node.type.get());
        if (resolvedType) {
            node.resolvedType = resolvedType;
        }
    }
    
    // Point directly to resolved type
    Symbol* sym = symbols_.lookup(node.name);
    if (sym && resolvedType) {
        sym->type = resolvedType;
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << node.name 
                               << "' type to resolved type");
    }
    
    resolved_ = resolvedType;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ImplDeclAST)  — Resolves method signatures in impl blocks
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(ImplDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(ImplDeclAST): structName='" << node.structName 
                   << "', methods=" << node.methods.size());
    
    auto* savedGenericParams = genericParams_;
    genericParams_ = &node.genericParams;
    
    // Build substitution map for concrete generic args
    if (!node.structGenericArgs.empty()) {
        Symbol* structSym = symbols_.lookup(node.structName);
        if (structSym && structSym->kind == SymbolKind::Struct) {
            auto* structDecl = structSym->decl->as<StructDeclAST>();
            if (structDecl->genericParams.size() == node.structGenericArgs.size()) {
                std::unordered_map<std::string, TypeAST*> substitutionMap;
                for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
                    TypeAST* concreteType = resolveType(node.structGenericArgs[i].get());
                    if (concreteType) {
                        substitutionMap[structDecl->genericParams[i]->name] = concreteType;
                    }
                }
                setSubstitutionMap(&substitutionMap);
            }
        }
    }
    
    // Resolve each method's signature
    for (auto& method : node.methods) {
        resolveMethodSignature(*method);
        
        // Update symbol with the resolved signature (which has no self)
        std::string mangledName = node.structName + "." + method->name;
        Symbol* sym = symbols_.lookup(mangledName);
        if (sym && method->signature) {
            // Point directly to the method's signature (no cloning)
            // The signature already has no self parameter
            sym->type = method->signature.get();
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName 
                                   << "' type to resolved method signature (no self)");
        }
    }
    
    setSubstitutionMap(nullptr);
    genericParams_ = savedGenericParams;
    resolved_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FromDeclAST)  — Resolves from-entry parameter and return types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FromDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FromDeclAST): targetType='" << node.targetTypeName 
                   << "', entries=" << node.entries.size());
    
    resolveFromEntries(node);
    resolved_ = nullptr;  // From blocks don't produce a type
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveFromEntries  — Resolves each casting entry's types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveFromEntries(FromDeclAST& node) {
    for (auto& entry : node.entries) {
        if (!entry) continue;
        
        // Resolve all parameter types
        for (auto& group : entry->paramGroups) {
            for (auto& param : group) {
                if (param->type) {
                    resolveType(param->type.get());
                }
            }
        }
        
        // Return type is the target type, which is a NamedType
        // No need to resolve separately as it's just the name
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FromEntryAST)  — Resolves from-entry parameter types
//
// FromEntryAST represents a single casting entry inside a from block:
//   (s Seconds) Minutes = { return Minutes { val = s.val / 60 } }
//
// This method resolves the parameter types so they can be type-checked
// in Phase 3. Return type resolution is handled by the parent FromDeclAST
// because all entries share the same target type.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FromEntryAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(FromEntryAST)");
    
    // Resolve all parameter types in all curry groups
    for (auto& group : node.paramGroups) {
        for (auto& param : group) {
            if (param->type) {
                TypeAST* paramType = resolveType(param->type.get());
                if (!paramType) {
                    LUC_LOG_SEMANTIC("\tERROR: failed to resolve parameter type for '" 
                                   << param->name << "' in from entry");
                }
            }
        }
    }
    
    // FromEntryAST doesn't produce a type itself - the parent FromDeclAST
    // handles the target type. The resolved type is the target NamedType.
    resolved_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TypeAliasDeclAST)  — Properly resolves type alias
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TypeAliasDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TypeAliasDeclAST): name='" << node.name << "'");
    
    // Set generic parameters context for generic type aliases
    auto* savedGenericParams = genericParams_;
    genericParams_ = &node.genericParams;
    
    if (node.aliasedType) {
        TypeAST* resolved = resolveType(node.aliasedType.get());
        if (resolved) {
            node.resolvedType = resolved;
        }
    }
    
    genericParams_ = savedGenericParams;
    resolved_ = node.resolvedType 
            ? static_cast<TypeAST*>(node.resolvedType) 
            : (node.aliasedType ? node.aliasedType.get() : nullptr);
}

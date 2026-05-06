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

#include "QualifierRegistry.hpp"
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
    LUC_LOG_SEMANTIC_VERBOSE("visit(FuncTypeAST): paramGroups=" << node.paramGroups.size() 
                        << ", isNullable=" << node.isNullable
                        << ", rawQualifiers=" << node.rawQualifiers.size());
    
    // ── Resolve type qualifiers (Phase 2) ────────────────────────────────────
    for (const auto& qualName : node.rawQualifiers) {
        uint32_t bit = QualifierRegistry::instance().getBit(qualName);
        if (bit == 0) {
            dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E2010,
                      "unknown type qualifier '~" + qualName + "'; " +
                      "known qualifiers: " + QualifierRegistry::instance().allNames());
        } else {
            node.qualifiers |= bit;
            LUC_LOG_SEMANTIC_EXTREME("\tadded qualifier '~" << qualName 
                                    << "' bit=0x" << std::hex << bit);
        }
    }
    
    // Clear raw qualifiers to free memory
    node.rawQualifiers.clear();
    
    // ── Resolve parameter types in all curry groups ──────────────────────────
    for (auto& group : node.paramGroups) {
        for (auto& param : group) {
            if (param.type) {
                LUC_LOG_SEMANTIC_EXTREME("\tresolving param: " << param.name);
                resolveType(param.type.get());
            }
        }
    }
    
    // ── Resolve return type ──────────────────────────────────────────────────
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
    LUC_LOG_SEMANTIC("visit(FuncDeclAST): name='" << node.name << "'");
    
    // Resolve the function's type (contains params, return, qualifiers)
    resolveType(&node.type);
    
    // Update symbol's type to point to resolved FuncTypeAST
    Symbol* sym = symbols_.lookup(node.name);
    if (sym) {
        sym->type = &node.type;
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << node.name << "' type");
    }
    
    resolved_ = &node.type;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(MethodDeclAST)  — Resolves methods
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(MethodDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(MethodDeclAST): name='" << node.name << "'");
    
    // Methods don't have their own generic parameters - they inherit from
    // the impl block's generic parameters, which are already set via
    // genericParams_ when visit(ImplDeclAST) is called.
    
    // Resolve the method's type (contains params, return, qualifiers)
    resolveType(&node.type);
    
    // Note: Symbol update is handled by visit(ImplDeclAST) because
    // the mangled name (structName.methodName) is known there.
    
    resolved_ = &node.type;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitDeclAST)  — Resolves trait method signatures
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TraitDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TraitDeclAST): name='" << node.name 
                   << "', methods=" << node.methods.size());
    
    // Set generic parameters context for generic traits
    auto* savedGenericParams = genericParams_;
    genericParams_ = &node.genericParams;
    
    // Resolve each method's type
    for (auto& method : node.methods) {
        // Resolve the method's type (FuncTypeAST)
        resolveType(&method->type);
        
        // Update symbol's type (mangled name: TraitName.methodName)
        std::string mangledName = node.name + "." + method->name;
        Symbol* sym = symbols_.lookup(mangledName);
        if (sym) {
            sym->type = &method->type;
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName 
                                   << "' type to resolved trait method signature");
        } else {
            // Symbol should have been created by SemanticCollector
            LUC_LOG_SEMANTIC("\tWARNING: symbol not found for trait method: " << mangledName);
        }
    }
    
    genericParams_ = savedGenericParams;
    resolved_ = nullptr;  // TraitDecl itself doesn't produce a type
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(MethodDeclAST)  — Resolves methods
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TraitMethodAST& node) {
    LUC_LOG_SEMANTIC("visit(TraitMethodAST): name='" << node.name << "'");
    
    // Resolve the trait method's type
    resolveType(&node.type);
    
    // Note: Symbol update is handled by visit(TraitDeclAST)
    
    resolved_ = &node.type;
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
    
    // Resolve each method's type (no separate resolveMethodSignature needed)
    for (auto& method : node.methods) {
        // Resolve the method's type
        resolveType(&method->type);
        
        // Update symbol with resolved type
        std::string mangledName = node.structName + "." + method->name;
        Symbol* sym = symbols_.lookup(mangledName);
        if (sym) {
            sym->type = &method->type;
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName 
                                   << "' type to resolved method signature");
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
    
    // Resolve the target type (NamedType) for type checking
    // Create a temporary NamedTypeAST to resolve
    NamedTypeAST targetType(node.targetTypeName);
    targetType.loc = node.loc;
    resolveType(&targetType);
    
    // Resolve each casting entry's parameter types
    for (auto& entry : node.entries) {
        if (!entry) continue;
        
        // Resolve all parameter types in all curry groups
        for (auto& group : entry->paramGroups) {
            for (auto& param : group) {
                if (param.type) {
                    resolveType(param.type.get());
                }
            }
        }
    }
    
    resolved_ = nullptr;  // From blocks don't produce a type
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

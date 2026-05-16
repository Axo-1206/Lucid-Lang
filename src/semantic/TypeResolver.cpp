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

#include "header/TypeResolver.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "header/SymbolTable.hpp"
#include "registry/QualifierRegistry.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "header/NameMangler.hpp"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// TypeResolver (constructor)  — Initializes the TypeResolver with core dependencies
//
// Binds the active symbol table to allow looking up global type names and the
// diagnostic engine to log error messages directly during resolution.
// ─────────────────────────────────────────────────────────────────────────────
TypeResolver::TypeResolver(SymbolTable& symbols, DiagnosticEngine& dc, StringPool& pool, ASTArena& arena)
    : symbols_(symbols), dc_(dc), pool_(pool), arena_(arena) {
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
// Generic parameter stack helpers
// ─────────────────────────────────────────────────────────────────────────────

void TypeResolver::pushGenericParams(const std::vector<GenericParamPtr>* params) {
    genericParamsStack_.push_back(params);
    LUC_LOG_SEMANTIC_EXTREME("pushGenericParams: depth=" << genericParamsStack_.size()
                            << ", params=" << (params ? std::to_string(params->size()) : "null"));
}

void TypeResolver::popGenericParams() {
    if (!genericParamsStack_.empty()) {
        genericParamsStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("popGenericParams: depth=" << genericParamsStack_.size());
    } else {
        LUC_LOG_SEMANTIC("popGenericParams: ERROR - stack empty");
    }
}

bool TypeResolver::isGenericParam(InternedString name) const {
    // Search from innermost (back) to outermost (front)
    for (auto it = genericParamsStack_.rbegin(); it != genericParamsStack_.rend(); ++it) {
        const auto* params = *it;
        if (!params) continue;
        for (const auto& gp : *params) {
            if (gp && gp->name == name) {
                LUC_LOG_SEMANTIC_EXTREME("isGenericParam: found " << pool_.lookup(name) << " at depth "
                                         << (genericParamsStack_.rend() - it - 1));
                return true;
            }
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Substitution map stack helpers
// ─────────────────────────────────────────────────────────────────────────────

void TypeResolver::pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map) {
    substitutionMapStack_.push_back(map);
    LUC_LOG_SEMANTIC_EXTREME("pushSubstitutionMap: depth=" << substitutionMapStack_.size()
                            << ", map=" << (map ? std::to_string(map->size()) : "null"));
}

void TypeResolver::popSubstitutionMap() {
    if (!substitutionMapStack_.empty()) {
        substitutionMapStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("popSubstitutionMap: depth=" << substitutionMapStack_.size());
    } else {
        LUC_LOG_SEMANTIC("popSubstitutionMap: ERROR - stack empty");
    }
}

TypeAST* TypeResolver::lookupSubstitution(InternedString name) const {
    // Search from innermost (back) to outermost (front)
    for (auto it = substitutionMapStack_.rbegin(); it != substitutionMapStack_.rend(); ++it) {
        const auto* map = *it;
        if (!map) continue;
        auto found = map->find(name);
        if (found != map->end()) {
            LUC_LOG_SEMANTIC_EXTREME("lookupSubstitution: found " << pool_.lookup(name) 
                                     << " at depth " << (substitutionMapStack_.rend() - it - 1));
            return found->second;
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveStructFields  — Resolves each field's type
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveStructFields(StructDeclAST& node) {
    pushGenericParams(&node.genericParams);
    
    for (auto& field : node.fields) {
        if (field && field->type) {
            TypeAST* resolvedFieldType = resolveType(field->type.get());
            if (!resolvedFieldType) {
                LUC_LOG_SEMANTIC("\tERROR: failed to resolve field type for '" 
                               << pool_.lookup(field->name) << "' in struct '" 
                               << pool_.lookup(node.name) << "'");
            }
        }
    }
    
    popGenericParams();
}

void TypeResolver::resolveFunctionType(FuncTypeAST& type) {
    for (auto& group : type.sig.paramGroups) {
        for (auto& param : group) {
            if (param && param->type) {
                resolveType(param->type.get());
            }
        }
    }
    for (auto& retType : type.sig.returnTypes) {
        if (retType) {
            resolveType(retType.get());
        }
    }
}

std::vector<TypeAST*> TypeResolver::getFunctionReturnTypes(FuncTypeAST& type) {
    std::vector<TypeAST*> result;
    for (auto& retType : type.sig.returnTypes) {
        if (retType) {
            result.push_back(resolveType(retType.get()));
        }
    }
    return result;
}

TypeAST* TypeResolver::getFunctionReturnType(FuncTypeAST& type, const SourceLocation* loc) {
    if (type.sig.returnTypes.empty()) {
        return nullptr;
    }
    if (type.sig.returnTypes.size() > 1) {
        if (loc) {
            dc_.error(DiagnosticCategory::Semantic, *loc, DiagCode::E3002,
                      "function has multiple return types but single return expected");
        }
        return nullptr;
    }
    return resolveType(type.sig.returnTypes[0].get());
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
    LUC_LOG_SEMANTIC("visit(NamedTypeAST): name='" << std::string(pool_.lookup(node.name)) << "'");
    LUC_LOG_SEMANTIC_VERBOSE("\tgenericArgs count=" << node.genericArgs.size());
    //LUC_LOG_SEMANTIC_VERBOSE("\thas genericParams_=" << (genericParams_ ? "true" : "false"));
    // LUC_LOG_SEMANTIC_VERBOSE("\thas substitutionMap_=" << (substitutionMap_ ? "true" : "false"));
    
    // CHECK 1: Is this name a generic type parameter?
    if (isGenericParam(node.name)) {
        LUC_LOG_SEMANTIC_VERBOSE("\tfound as generic param: '" << pool_.lookup(node.name) << "'");
             
        TypeAST* subst = lookupSubstitution(node.name);
        if (subst) {
            LUC_LOG_SEMANTIC("\tsubstituting '" << pool_.lookup(node.name) << "' with concrete type");
            resolved_ = subst;
            return;
        }
        
        node.isGenericParam = true;
        LUC_LOG_SEMANTIC("\tresolved as generic parameter (isGenericParam=true)");
        resolved_ = &node;
        return;
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
        LUC_LOG_SEMANTIC("\tERROR: type '" << std::string(pool_.lookup(node.name)) << "' not declared");
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                  "type '" + std::string(pool_.lookup(node.name)) + "' is not declared");
        resolved_ = nullptr;
        return;
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("\tfound symbol: kind=" << static_cast<int>(sym->kind));
    
    // Strict typing: We can only resolve identifiers that actually represent a type.
    if (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::Trait && sym->kind != SymbolKind::TypeAlias) {
        LUC_LOG_SEMANTIC("\tERROR: '" << std::string(pool_.lookup(node.name)) << "' is a value, not a type");
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "'" + std::string(pool_.lookup(node.name)) + "' is a value, not a type");
        resolved_ = nullptr;
        return;
    }
    
    // Transparently unwrap TypeAlias types.
    if (sym->kind == SymbolKind::TypeAlias) {
        LUC_LOG_SEMANTIC("\tunwrapping type alias '" << std::string(pool_.lookup(node.name)) << "'");
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
    
    LUC_LOG_SEMANTIC("\tresolved NamedTypeAST: '" << std::string(pool_.lookup(node.name)) << "'");
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
    LUC_LOG_SEMANTIC_VERBOSE("visit(FuncTypeAST): paramGroups=" << node.sig.paramGroups.size() 
                        << ", returnTypes=" << node.sig.returnTypes.size());
    
    // ── Resolve type qualifiers (Phase 2) ────────────────────────────────────
    for (const auto& qualName : node.sig.rawQualifiers) {
        uint32_t bit = QualifierRegistry::instance().getBit(qualName);
        if (bit == 0) {
            dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E2010,
                      "unknown type qualifier '~" + std::string(pool_.lookup(qualName)) + "'; " +
                      "known qualifiers: " + QualifierRegistry::instance().allNames());
        } else {
            node.sig.qualifiers |= bit;
            LUC_LOG_SEMANTIC_EXTREME("\tadded qualifier '~" << std::string(pool_.lookup(qualName)) 
                                    << "' bit=0x" << std::hex << bit);
        }
    }
    
    // Clear raw qualifiers to free memory
    node.sig.rawQualifiers.clear();
    
    // ── Resolve parameter types in all curry groups ──────────────────────────
    for (auto& group : node.sig.paramGroups) {
        for (auto& param : group) {
            if (param->type) {
                LUC_LOG_SEMANTIC_EXTREME("\tresolving param: " << std::string(pool_.lookup(param->name)));
                resolveType(param->type.get());
            }
        }
    }
    
    // ── Resolve return types ─────────────────────────────────────────────────
    for (auto& retType : node.sig.returnTypes) {
        if (retType) {
            LUC_LOG_SEMANTIC_EXTREME("\tresolving return type");
            resolveType(retType.get());
        }
    }
    
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncDeclAST)  — Resolves function parameter and return types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FuncDeclAST): name='" << pool_.lookup(node.name) << "'");
    
    pushGenericParams(&node.genericParams);
    
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->sig = node.sig;
    funcType->loc = node.loc;
    TypeAST* resolvedType = resolveType(funcType.get());
    
    popGenericParams();
    
    Symbol* sym = symbols_.lookup(node.name);
    if (sym && resolvedType) {
        sym->type = resolvedType;
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << pool_.lookup(node.name) << "' type");
    }
    
    resolved_ = resolvedType;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)  — Resolves variable type annotations
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(VarDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(VarDeclAST): name='" << std::string(pool_.lookup(node.name)) << "'");
    
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
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << std::string(pool_.lookup(node.name)) 
                               << "' type to resolved type");
    }
    
    resolved_ = resolvedType;
}


// ─────────────────────────────────────────────────────────────────────────────
// visit(StructDeclAST)  — Resolves struct field types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(StructDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(StructDeclAST): name='" << std::string(pool_.lookup(node.name)) 
                   << "', fields=" << node.fields.size());
    
    resolveStructFields(node);
    
    // Ensure selfType exists – now allocate via arena
    if (!node.selfType) {
        node.selfType = arena_.make<NamedTypeAST>(node.name);
        node.selfType->loc = node.loc;
    }
    
    Symbol* sym = symbols_.lookup(node.name);
    if (sym) {
        sym->type = node.selfType.get();
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << std::string(pool_.lookup(node.name)) 
                               << "' type to selfType");
    }
    
    resolved_ = node.selfType.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ImplDeclAST)  — Resolves method signatures in impl blocks
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(ImplDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(ImplDeclAST): structName='" << pool_.lookup(node.structName)
                   << "', methods=" << node.methods.size());
    
    pushGenericParams(&node.genericParams);
    
    // Build substitution map for concrete generic args and push onto stack
    std::unordered_map<InternedString, TypeAST*> localSubstitutionMap;
    if (!node.structGenericArgs.empty()) {
        Symbol* structSym = symbols_.lookup(node.structName);
        if (structSym && structSym->kind == SymbolKind::Struct) {
            auto* structDecl = structSym->decl->as<StructDeclAST>();
            if (structDecl && structDecl->genericParams.size() == node.structGenericArgs.size()) {
                for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
                    if (i < node.structGenericArgs.size() && node.structGenericArgs[i]) {
                        TypeAST* concreteType = resolveType(node.structGenericArgs[i].get());
                        if (concreteType && structDecl->genericParams[i]) {
                            localSubstitutionMap[structDecl->genericParams[i]->name] = concreteType;
                        }
                    }
                }
            }
        }
    }
    
    // Push the substitution map (even if empty) so inner resolutions see it
    pushSubstitutionMap(&localSubstitutionMap);
    
    // Resolve each method's type
    for (auto& method : node.methods) {
        if (!method) continue;
        
        auto funcType = arena_.make<FuncTypeAST>();
        funcType->sig = method->sig;
        funcType->loc = method->loc;
        TypeAST* resolvedType = resolveType(funcType.get());
        
        std::string mangledName = NameMangler::mangleMethod(pool_.lookup(node.structName), pool_.lookup(method->name));
        Symbol* sym = symbols_.lookup(pool_.intern(mangledName));
        if (sym && resolvedType) {
            sym->type = resolvedType;
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName 
                                   << "' type to resolved method signature");
        }
    }
    
    popSubstitutionMap();
    popGenericParams();
    resolved_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(MethodDeclAST)  — Resolves methods
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(MethodDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(MethodDeclAST): name='" << std::string(pool_.lookup(node.name)) << "'");
    
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->sig = node.sig;
    funcType->loc = node.loc;
    
    TypeAST* resolvedType = resolveType(funcType.get());
    resolved_ = resolvedType;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FromDeclAST)  — Resolves from-entry parameter and return types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FromDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FromDeclAST): targetType='" << std::string(pool_.lookup(node.targetTypeName ))
                   << "', entries=" << node.entries.size());
    
    // Resolve the target type (NamedType) for type checking
    // Create a temporary NamedTypeAST to resolve
    NamedTypeAST targetType(node.targetTypeName);
    targetType.loc = node.loc;
    resolveType(&targetType);
    
    // Resolve each casting entry's parameter types and return types
    for (auto& entry : node.entries) {
        if (!entry) continue;
        
        // Resolve all parameter types in all curry groups
        for (auto& group : entry->sig.paramGroups) {
            for (auto& param : group) {
                if (param->type) {
                    resolveType(param->type.get());
                }
            }
        }
        
        // Resolve return type
        if (entry->returnType) {
            resolveType(entry->returnType.get());
        }

        // Update the symbol table so the type checker can find this conversion
        TypeAST* firstParamType = nullptr;
        if (!entry->sig.paramGroups.empty() && !entry->sig.paramGroups[0].empty() && entry->sig.paramGroups[0][0]) {
            firstParamType = entry->sig.paramGroups[0][0]->type.get();
        }
        std::string mangledName = NameMangler::mangleFrom(pool_.lookup(node.targetTypeName), firstParamType, pool_);
        Symbol* sym = symbols_.lookup(pool_.intern(mangledName));
        
        if (sym) {
            auto funcType = arena_.make<FuncTypeAST>();
            funcType->sig = entry->sig;
            funcType->loc = entry->loc;
            if (entry->returnType) {
                funcType->sig.returnTypes.push_back(entry->returnType);
            }
            sym->type = resolveType(funcType.get());
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName << "' type");
        }
    }
    
    resolved_ = nullptr;  // From blocks don't produce a type
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TypeAliasDeclAST)  — Properly resolves type alias
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TypeAliasDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TypeAliasDeclAST): name='" << pool_.lookup(node.name) << "'");
    
    pushGenericParams(&node.genericParams);
    
    if (node.aliasedType) {
        TypeAST* resolved = resolveType(node.aliasedType.get());
        if (resolved) {
            node.resolvedType = resolved;
        }
    }
    
    popGenericParams();
    resolved_ = node.resolvedType 
            ? static_cast<TypeAST*>(node.resolvedType) 
            : (node.aliasedType ? node.aliasedType.get() : nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitDeclAST)  — Resolves trait method signatures
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TraitDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TraitDeclAST): name='" << pool_.lookup(node.name) 
                   << "', methods=" << node.methods.size());
    
    pushGenericParams(&node.genericParams);
    
    for (auto& method : node.methods) {
        if (!method) continue;
        
        auto funcType = arena_.make<FuncTypeAST>();
        funcType->sig = method->sig;
        funcType->loc = method->loc;
        TypeAST* resolvedType = resolveType(funcType.get());
        
        std::string mangledName = NameMangler::mangleMethod(pool_.lookup(node.name), pool_.lookup(method->name));
        Symbol* sym = symbols_.lookup(pool_.intern(mangledName));
        if (sym && resolvedType) {
            sym->type = resolvedType;
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName 
                                   << "' type to resolved trait method signature");
        }
    }
    
    popGenericParams();
    resolved_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitMethodAST)  — Resolves trait method signatures
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TraitMethodAST& node) {
    LUC_LOG_SEMANTIC("visit(TraitMethodAST): name='" << std::string(pool_.lookup(node.name)) << "'");
    
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->sig = node.sig;
    funcType->loc = node.loc;
    
    TypeAST* resolvedType = resolveType(funcType.get());
    resolved_ = resolvedType;
}

void TypeResolver::visit(TraitRefAST& node) {
    // Resolve the trait name
    Symbol* sym = symbols_.lookup(node.name);
    if (!sym || sym->kind != SymbolKind::Trait) {
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                  "trait '" + std::string(pool_.lookup(node.name)) + "' not found");
        resolved_ = nullptr;
        return;
    }
    // Resolve generic arguments
    for (auto& arg : node.genericArgs) {
        resolveType(arg.get());
    }
    resolved_ = nullptr; // TraitRef is not a type; just a reference
}
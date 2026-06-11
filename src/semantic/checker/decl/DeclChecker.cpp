/**
 * @file DeclChecker.cpp
 * @brief Implementation of declaration checkers.
 */

#include "DeclChecker.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "semantic/checker/expr/ExprChecker.hpp"
#include "semantic/checker/stmt/StmtChecker.hpp"
#include "semantic/checker/TypeChecker.hpp"
#include "semantic/resolver/TypeResolver.hpp"
#include "registry/AttributeRegistry.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <unordered_set>

// ============================================================================
// Helper Functions
// ============================================================================

std::string getDeclName(const DeclAST* decl, const StringPool& pool) {
    if (!decl) return "<null>";
    
    if (auto* valueDecl = decl->as<ValueDeclAST>()) {
        return std::string(pool.lookup(valueDecl->name));
    }
    if (auto* typeDecl = decl->as<TypeDeclAST>()) {
        return std::string(pool.lookup(typeDecl->name));
    }
    if (auto* packageDecl = decl->as<PackageDeclAST>()) {
        return std::string(pool.lookup(packageDecl->name));
    }
    if (auto* useDecl = decl->as<UseDeclAST>()) {
        return "<use>";
    }
    if (auto* fromDecl = decl->as<FromDeclAST>()) {
        return "<from>";
    }
    if (auto* implDecl = decl->as<ImplDeclAST>()) {
        return "<impl>";
    }
    return "<anonymous>";
}

DeclKeyword getDeclKeyword(const DeclAST* decl) {
    if (auto* var = decl->as<VarDeclAST>()) {
        return var->keyword;
    }
    if (auto* func = decl->as<FuncDeclAST>()) {
        return func->keyword;
    }
    return DeclKeyword::Let;  // Default for other declaration types
}

AttributeContext getAttributeContextForDecl(const DeclAST* decl) {
    if (!decl) return AttributeContext::None;
    
    switch (decl->kind) {
        case ASTKind::FuncDecl:
            return AttributeContext::Func;
        case ASTKind::VarDecl:
            return AttributeContext::Var;
        case ASTKind::StructDecl:
            return AttributeContext::Struct;
        case ASTKind::ImplDecl:
            return AttributeContext::Impl;
        case ASTKind::EnumDecl:
            return AttributeContext::Enum;
        case ASTKind::TraitDecl:
            return AttributeContext::Trait;
        case ASTKind::FromDecl:
            return AttributeContext::From;
        case ASTKind::TypeAliasDecl:
            return AttributeContext::TypeAlias;
        default:
            return AttributeContext::None;
    }
}

// ============================================================================
// Dispatcher
// ============================================================================

void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx) {
    if (!decl) return;
    
    LUC_LOG_SEMANTIC_EXTREME("checkTopLevelDecl: kind=" << LucDebug::kindToString(decl->kind));
    
    switch (decl->kind) {
        case ASTKind::VarDecl:
            checkVarDecl(decl->as<VarDeclAST>(), ctx);
            break;
        case ASTKind::FuncDecl:
            checkFuncDecl(decl->as<FuncDeclAST>(), ctx);
            break;
        case ASTKind::StructDecl:
            checkStructDecl(decl->as<StructDeclAST>(), ctx);
            break;
        case ASTKind::EnumDecl:
            checkEnumDecl(decl->as<EnumDeclAST>(), ctx);
            break;
        case ASTKind::TraitDecl:
            checkTraitDecl(decl->as<TraitDeclAST>(), ctx);
            break;
        case ASTKind::ImplDecl:
            checkImplDecl(decl->as<ImplDeclAST>(), ctx);
            break;
        case ASTKind::FromDecl:
            checkFromDecl(decl->as<FromDeclAST>(), ctx);
            break;
        case ASTKind::TypeAliasDecl:
            checkTypeAliasDecl(decl->as<TypeAliasDeclAST>(), ctx);
            break;
        default:
            // PackageDecl, UseDecl, etc. – no checking needed
            break;
    }
}

// ============================================================================
// Attribute Checking (Using Registry)
// ============================================================================

void checkAttributes(DeclAST* decl, SemanticContext& ctx) {
    if (!decl || decl->attributes.empty()) {
        return;
    }
    
    // Get the declaration context for attribute validation
    AttributeContext declContext = getAttributeContextForDecl(decl);
    std::string declName = getDeclName(decl, ctx.pool);
    DeclKeyword declKw = getDeclKeyword(decl);
    
    // First, validate each attribute individually
    for (auto* attr : decl->attributes) {
        std::string_view attrName = ctx.pool.lookup(attr->name);
        
        // Look up attribute in registry
        const AttributeEntry* entry = attribute::lookup(attrName);
        
        if (!entry) {
            // Unknown attribute
            ctx.error(decl->loc, DiagCode::E3001,
                      "unknown attribute '@", attrName, 
                      "'. Known attributes: ", attribute::allNames());
            continue;
        }
        
        // Validate the attribute using the registry
        attribute::validateAttribute(
            *entry,
            attr->args,
            declContext,
            declName,
            declKw,
            ctx.currentFile,
            decl->loc
        );
    }
    
    // Second, check mutual exclusion between pairs of attributes
    // This is O(n²) but attribute lists are small (typically 1-3 attributes)
    for (size_t i = 0; i < decl->attributes.size(); ++i) {
        for (size_t j = i + 1; j < decl->attributes.size(); ++j) {
            InternedString id1 = decl->attributes[i]->name;
            InternedString id2 = decl->attributes[j]->name;
            
            attribute::checkMutualExclusion(id1, id2, decl->loc);
        }
    }
}

// ============================================================================
// Constant Expression Checking
// ============================================================================

bool isConstExpr(ExprAST* expr, SemanticContext& ctx) {
    if (!expr) return false;
    
    // If already marked const, return true
    if (expr->isConst) return true;
    
    // Check literal expressions
    if (auto* literal = expr->as<LiteralExprAST>()) {
        if (literal->kind == LiteralKind::Int ||
            literal->kind == LiteralKind::Float ||
            literal->kind == LiteralKind::String ||
            literal->kind == LiteralKind::Char ||
            literal->kind == LiteralKind::True ||
            literal->kind == LiteralKind::False) {
            expr->isConst = true;
            return true;
        }
        return false;
    }
    
    // Binary operations can be const if both operands are const
    if (auto* binary = expr->as<BinaryExprAST>()) {
        if (isConstExpr(binary->left, ctx) && isConstExpr(binary->right, ctx)) {
            expr->isConst = true;
            return true;
        }
        return false;
    }
    
    // Unary operations can be const if operand is const
    if (auto* unary = expr->as<UnaryExprAST>()) {
        if (isConstExpr(unary->operand, ctx)) {
            expr->isConst = true;
            return true;
        }
        return false;
    }
    
    return false;
}

// ============================================================================
// Variable Declaration
// ============================================================================

void checkVarDecl(VarDeclAST* var, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkVarDecl: " << ctx.pool.lookup(var->name));
    
    // Check attributes using registry
    checkAttributes(var, ctx);
    
    // Check const initialization
    if (var->keyword == DeclKeyword::Const) {
        if (!var->init) {
            ctx.error(var->loc, DiagCode::E2001,
                      "const variable '", ctx.pool.lookup(var->name), "' must be initialized");
        } else if (!isConstExpr(var->init, ctx)) {
            ctx.error(var->init->loc, DiagCode::E2001,
                      "const variable initializer must be a constant expression");
        }
    }
    
    // Check non-nullable initialization
    if (var->valueType && !TypeChecker::isNullable(var->valueType, *ctx.typeResolver)) {
        if (!var->init) {
            ctx.error(var->loc, DiagCode::E2001,
                      "non-nullable variable '", ctx.pool.lookup(var->name), "' must be initialized");
        }
    }
    
    // Check initializer type
    if (var->init && var->valueType) {
        TypeAST* initType = checkExpr(var->init, ctx);
        if (initType && !TypeChecker::isAssignable(initType, var->valueType, ctx)) {
            ctx.error(var->init->loc, DiagCode::E2001,
                      "cannot initialize '", ctx.pool.lookup(var->name),
                      "' of type with value of different type");
        }
    }
}

// ============================================================================
// Function Declaration
// ============================================================================

void checkFuncDecl(FuncDeclAST* func, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkFuncDecl: " << ctx.pool.lookup(func->name));
    
    // Check attributes using registry
    checkAttributes(func, ctx);
    
    // Check if this is an extern function (using registry for consistent lookup)
    bool isExtern = false;
    for (auto* attr : func->attributes) {
        if (attr->name == attribute::getExternId()) {
            isExtern = true;
            break;
        }
    }
    
    if (isExtern) {
        if (func->body) {
            ctx.error(func->loc, DiagCode::E2001,
                      "extern function '", ctx.pool.lookup(func->name), "' cannot have a body");
        }
        return;
    }
    
    if (!func->body) {
        ctx.error(func->loc, DiagCode::E2001,
                  "function '", ctx.pool.lookup(func->name), "' must have a body");
        return;
    }
    
    // Check async restrictions
    if (func->funcType && func->funcType->isAsync()) {
        // Async functions must be called with await
        // This is checked at call sites
    }
    
    // Push function scope (already has parameters from collector)
    ctx.scope.push();
    
    // Check function body
    TypeAST* expectedReturn = func->resolvedReturnType;
    checkStmt(func->body, ctx, expectedReturn);
    
    // Pop function scope
    ctx.scope.pop();
}

// ============================================================================
// Struct Declaration
// ============================================================================

void checkStructDecl(StructDeclAST* structDecl, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkStructDecl: " << ctx.pool.lookup(structDecl->name));
    
    // Check attributes using registry
    checkAttributes(structDecl, ctx);
    
    // Check for duplicate field names
    std::unordered_set<uint32_t> fieldNames;
    for (auto* field : structDecl->fields) {
        uint32_t id = field->name.id;
        if (fieldNames.find(id) != fieldNames.end()) {
            ctx.error(field->loc, DiagCode::E2001,
                      "duplicate field name '", ctx.pool.lookup(field->name),
                      "' in struct '", ctx.pool.lookup(structDecl->name), "'");
        }
        fieldNames.insert(id);
        
        // Check default value type if present
        if (field->defaultVal && field->valueType) {
            TypeAST* defaultType = checkExpr(field->defaultVal, ctx);
            if (defaultType && !TypeChecker::isAssignable(defaultType, field->valueType, ctx)) {
                ctx.error(field->defaultVal->loc, DiagCode::E2001,
                          "default value type does not match field type");
            }
        }
    }
    
    // Check that fields with no default are at the end (optional, but good practice)
    // This is a style warning, not an error
    bool seenDefault = false;
    for (auto* field : structDecl->fields) {
        if (field->defaultVal) {
            seenDefault = true;
        } else if (seenDefault) {
            ctx.warning(field->loc, DiagCode::W6001,
                        "fields without default values should come before fields with defaults");
        }
    }
}

// ============================================================================
// Enum Declaration
// ============================================================================

void checkEnumDecl(EnumDeclAST* enumDecl, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkEnumDecl: " << ctx.pool.lookup(enumDecl->name));
    
    // Check attributes using registry
    checkAttributes(enumDecl, ctx);
    
    // Check for duplicate variant names and explicit values
    std::unordered_set<uint32_t> variantNames;
    std::unordered_set<int64_t> explicitValues;
    int64_t nextAutoValue = 0;
    
    for (auto* variant : enumDecl->variants) {
        // Check duplicate name
        uint32_t id = variant->name.id;
        if (variantNames.find(id) != variantNames.end()) {
            ctx.error(variant->loc, DiagCode::E2001,
                      "duplicate variant name '", ctx.pool.lookup(variant->name),
                      "' in enum '", ctx.pool.lookup(enumDecl->name), "'");
        }
        variantNames.insert(id);
        
        // Check explicit value
        if (variant->explicitValue.has_value()) {
            int64_t val = variant->explicitValue.value();
            if (explicitValues.find(val) != explicitValues.end()) {
                ctx.error(variant->loc, DiagCode::E2001,
                          "duplicate explicit value ", val,
                          " in enum '", ctx.pool.lookup(enumDecl->name), "'");
            }
            explicitValues.insert(val);
            nextAutoValue = val + 1;
        } else {
            // Auto-assign value
            // Check for collision with explicit values
            while (explicitValues.find(nextAutoValue) != explicitValues.end()) {
                nextAutoValue++;
            }
            variant->explicitValue = nextAutoValue;
            nextAutoValue++;
        }
    }
}

// ============================================================================
// Trait Declaration
// ============================================================================

void checkTraitDecl(TraitDeclAST* trait, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkTraitDecl: " << ctx.pool.lookup(trait->name));
    
    // Check attributes using registry
    checkAttributes(trait, ctx);
    
    // Check for duplicate method names
    std::unordered_set<uint32_t> methodNames;
    for (auto* method : trait->methods) {
        uint32_t id = method->name.id;
        if (methodNames.find(id) != methodNames.end()) {
            ctx.error(method->loc, DiagCode::E2001,
                      "duplicate method name '", ctx.pool.lookup(method->name),
                      "' in trait '", ctx.pool.lookup(trait->name), "'");
        }
        methodNames.insert(id);
        
        // Check that trait methods have no body
        // (TraitMethodAST doesn't have a body field)
    }
}

// ============================================================================
// Impl Block
// ============================================================================

bool checkDuplicateMethods(ArenaSpan<MethodDeclPtr> methods, SemanticContext& ctx) {
    std::unordered_set<uint32_t> methodNames;
    bool noDuplicates = true;
    
    for (auto* method : methods) {
        uint32_t id = method->name.id;
        if (methodNames.find(id) != methodNames.end()) {
            ctx.error(method->loc, DiagCode::E2001,
                      "duplicate method '", ctx.pool.lookup(method->name), "' in impl block");
            noDuplicates = false;
        }
        methodNames.insert(id);
    }
    
    return noDuplicates;
}

bool checkTraitFulfillment(ImplDeclAST* impl, SemanticContext& ctx) {
    if (!impl->traitRef) return true;
    
    // Look up the trait
    TypeDeclAST* traitDecl = ctx.scope.lookupType(impl->traitRef->name);
    if (!traitDecl) {
        ctx.error(impl->loc, DiagCode::E2001,
                  "undefined trait '", ctx.pool.lookup(impl->traitRef->name), "'");
        return false;
    }
    
    auto* trait = traitDecl->as<TraitDeclAST>();
    if (!trait) {
        ctx.error(impl->loc, DiagCode::E2001,
                  "'", ctx.pool.lookup(impl->traitRef->name), "' is not a trait");
        return false;
    }
    
    // Check that all trait methods are implemented
    std::unordered_set<uint32_t> implMethods;
    for (auto* method : impl->methods) {
        implMethods.insert(method->name.id);
    }
    
    bool allImplemented = true;
    for (auto* traitMethod : trait->methods) {
        uint32_t id = traitMethod->name.id;
        if (implMethods.find(id) == implMethods.end()) {
            ctx.error(impl->loc, DiagCode::E2001,
                      "trait '", ctx.pool.lookup(trait->name),
                      "' requires method '", ctx.pool.lookup(traitMethod->name),
                      "' but it is not implemented");
            allImplemented = false;
        }
    }
    
    return allImplemented;
}

void checkImplDecl(ImplDeclAST* impl, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkImplDecl");
    
    // Check attributes using registry
    checkAttributes(impl, ctx);
    
    // Check for duplicate method names
    checkDuplicateMethods(impl->methods, ctx);
    
    // Check trait fulfillment
    checkTraitFulfillment(impl, ctx);
    
    // Check each method
    for (auto* method : impl->methods) {
        if (method->isInlineBody()) {
            // Push impl scope for method checking
            ctx.scope.push();
            
            // Inject receiver symbol if present
            if (impl->receiverAlias.isValid()) {
                // The receiver is already declared in the scope by the collector
                // Just need to check that it's used correctly
            }
            
            // Check method body
            TypeAST* expectedReturn = nullptr;
            if (method->funcType && !method->funcType->returnTypes.empty()) {
                expectedReturn = method->funcType->returnTypes[0];
            }
            
            if (method->body) {
                checkStmt(method->body, ctx, expectedReturn);
            } else if (method->assignmentRef) {
                // Assignment form – check the reference
                checkExpr(method->assignmentRef, ctx);
            }
            
            ctx.scope.pop();
        } else if (method->assignmentRef) {
            // Assignment form – check the reference
            checkExpr(method->assignmentRef, ctx);
        }
    }
}

// ============================================================================
// From Block
// ============================================================================

void checkFromDecl(FromDeclAST* from, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkFromDecl");
    
    // Check attributes using registry
    checkAttributes(from, ctx);
    
    // Check each entry
    std::unordered_set<std::string> sourceTypes;
    
    for (auto* entry : from->entries) {
        if (entry->kind == FromEntryKind::Inline) {
            // Inline entry – check function signature and body
            if (entry->funcType) {
                // Get source type from first parameter
                if (!entry->funcType->params.empty()) {
                    ParamAST* sourceParam = entry->funcType->params[0];
                    if (sourceParam->valueType) {
                        // Check for duplicate source type
                        std::string sourceKey = NameMangler::mangleType(sourceParam->valueType, ctx.pool);
                        if (sourceTypes.find(sourceKey) != sourceTypes.end()) {
                            ctx.error(entry->loc, DiagCode::E2001,
                                      "duplicate conversion from same source type");
                        }
                        sourceTypes.insert(sourceKey);
                    }
                }
            }
            
            // Check body
            if (entry->body) {
                // The body should return the target type
                TypeAST* expectedReturn = from->targetType;
                checkStmt(entry->body, ctx, expectedReturn);
            }
        } else if (entry->kind == FromEntryKind::Path && entry->path) {
            // Path entry – check the reference
            TypeAST* pathType = checkExpr(entry->path, ctx);
            if (pathType && !TypeChecker::isCallable(pathType, *ctx.typeResolver)) {
                ctx.error(entry->path->loc, DiagCode::E2001,
                          "path entry must reference a callable function");
            }
        }
    }
}

// ============================================================================
// Type Alias
// ============================================================================

void checkTypeAliasDecl(TypeAliasDeclAST* alias, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkTypeAliasDecl: " << ctx.pool.lookup(alias->name));
    
    // Check attributes using registry
    checkAttributes(alias, ctx);
    
    // The type resolver already resolved the alias
    // Just check that the aliased type is valid
    if (!alias->aliasedType) {
        ctx.error(alias->loc, DiagCode::E2001,
                  "type alias '", ctx.pool.lookup(alias->name), "' has no aliased type");
    }
}

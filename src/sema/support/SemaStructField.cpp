/// @file SemaStructField.cpp
/// @brief Implementation of struct field analysis.

#include "SemaStructField.hpp"
#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

namespace {

/// @brief Check if a NamedTypeAST refers to the same struct instantiation.
bool isSameStructInstantiation(const NamedTypeAST* named,
                                const StructDeclAST* currentStruct) {
    if (!named || !currentStruct) return false;

    if (named->name != currentStruct->name) return false;
    if (named->genericArgs.size() != currentStruct->genericParams.size()) {
        return false;
    }

    for (size_t i = 0; i < named->genericArgs.size(); ++i) {
        TypeAST* arg = named->genericArgs[i];
        const GenericParamDeclAST* param = currentStruct->genericParams[i];

        if (arg->isa<NamedTypeAST>()) {
            NamedTypeAST* argNamed = arg->as<NamedTypeAST>();
            if (argNamed->name != param->name) {
                return false;
            }
        } else {
            return false;
        }
    }

    return true;
}

SelfReferenceInfo checkSelfReferenceImpl(const TypeAST* fieldType,
                                          const StructDeclAST* currentStruct,
                                          SemaContext& ctx) {
    SelfReferenceInfo result;
    result.isSelfReference = false;

    if (!fieldType || !currentStruct) return result;

    bool isNullable = false;
    bool isPointer = false;
    const TypeAST* innerType = fieldType;

    if (fieldType->isa<NullableTypeAST>()) {
        isNullable = true;
        innerType = fieldType->as<NullableTypeAST>()->inner;
    }

    if (innerType->isa<PtrTypeAST>()) {
        isPointer = true;
        innerType = innerType->as<PtrTypeAST>()->inner;
    }

    if (!innerType->isa<NamedTypeAST>()) {
        return result;
    }

    const NamedTypeAST* named = innerType->as<NamedTypeAST>();

    if (named->name != currentStruct->name) {
        return result;
    }

    if (!isSameStructInstantiation(named, currentStruct)) {
        return result;
    }

    result.isSelfReference = true;
    result.isPointer = isPointer;
    result.isNullable = isNullable;
    result.isNonNullable = !isNullable && !isPointer;
    result.namedType = named;

    return result;
}

void checkDuplicateFieldNames(const StructDeclAST* decl, SemaContext& ctx) {
    for (const FieldDeclAST* field : decl->fields) {
        for (const FieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(field->name), "'");
                break;
            }
        }
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1: Registration
// ─────────────────────────────────────────────────────────────────────────────

void registerStructFieldName(const FieldDeclAST* field,
                              const StructDeclAST* currentStruct,
                              SemaContext& ctx) {
    if (!field) return;
    ctx.symbols.insertValue(field);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2: Resolution
// ─────────────────────────────────────────────────────────────────────────────

static void resolveStructField(const FieldDeclAST* field,
                                const StructDeclAST* currentStruct,
                                SemaContext& ctx) {
    if (!field) return;

    attr::validateAttributes(field, ctx);

    // ─── 1. Resolve type ──────────────────────────────────────────────────
    TypeAST* fieldType = resolveType(field->type, ctx);
    if (!fieldType) return;

    // ─── 2. Self-reference detection ─────────────────────────────────────
    SelfReferenceInfo selfInfo = checkSelfReferenceImpl(fieldType, currentStruct, ctx);

    if (selfInfo.isSelfReference) {
        if (selfInfo.isPointer) {
            // OK: *Node<T>
        } else if (selfInfo.isNullable) {
            // OK: Node<T>?
        } else {
            if (field->isConst) {
                ctx.error(field, DiagCode::E3003,
                        "const field '", ctx.pool().lookup(field->name),
                        "' has non-nullable self-reference which would create infinite size");
            } else {
                ctx.error(field, DiagCode::E3003,
                        "non-nullable self-reference '", ctx.pool().lookup(field->name),
                        "' would create infinite size; use '",
                        ctx.pool().lookup(field->name), "?' or '*",
                        ctx.pool().lookup(field->name), "' instead");
            }
        }
    }

    // ─── 3. Const field validation ────────────────────────────────────────
    if (field->isConst && (isNullableType(fieldType) || isFallibleType(fieldType))) {
        ctx.error(field, DiagCode::E3004,
                  "const field '", ctx.pool().lookup(field->name),
                  "' must be definite (not nullable or fallible)");
    }

    // ─── 4. Default value ──────────────────────────────────────────────────
    if (field->defaultVal && !fieldType->isa<FuncTypeAST>()) {
        TypeAST* defaultType = resolveExpr(field->defaultVal, ctx);
        if (!defaultType || defaultType->isa<UnknownTypeAST>()) {
            ctx.error(field->defaultVal, DiagCode::E3003,
                      "default value for field '", ctx.pool().lookup(field->name),
                      "' has unknown type");
        } else if (!isAssignable(fieldType, defaultType, ctx)) {
            ctx.error(field->defaultVal, DiagCode::E3003,
                      "default value type mismatch for field '",
                      ctx.pool().lookup(field->name), "'");
        }
    }

    // ─── 5. Reference type validation ─────────────────────────────────────
    if (fieldType->isa<RefTypeAST>()) {
        ctx.error(field, DiagCode::E3004,
                  "reference type (&T) cannot be stored in struct field '",
                  ctx.pool().lookup(field->name), "'");
    }
}

void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx) {
    checkDuplicateFieldNames(decl, ctx);

    for (const FieldDeclAST* field : decl->fields) {
        resolveStructField(field, decl, ctx);
        if (!ctx.canContinue()) return;
    }

    // Analyze function field bodies (after all types are resolved)
    for (const FieldDeclAST* field : decl->fields) {
        if (field->type && field->type->isa<FuncTypeAST>()) {
            analyzeFunctionFieldBody(field, decl, ctx);
        }
        if (!ctx.canContinue()) return;
    }
}

void analyzeFunctionFieldBody(const FieldDeclAST* field,
                               const StructDeclAST* currentStruct,
                               SemaContext& ctx) {
    if (!field || !field->type || !field->type->isa<FuncTypeAST>()) {
        return;
    }

    FuncTypeAST* funcType = field->type->as<FuncTypeAST>();

    if (funcType->params.empty()) {
        // TODO: Add diagnostic
        return;
    }

    if (!field->defaultVal) {
        // TODO: Add diagnostic
        return;
    }

    ctx.symbols.pushScope();

    // Register self parameter
    registerParamName(funcType->params[0], ctx);

    // Register rest of parameters
    for (size_t i = 1; i < funcType->params.size(); ++i) {
        registerParamName(funcType->params[i], ctx);
    }

    // Analyze body
    if (field->defaultVal->isa<BlockStmtAST>()) {
        resolveBlock(field->defaultVal->as<BlockStmtAST>(), ctx);
    } else {
        ctx.error(field->defaultVal, DiagCode::E3003,
                  "expression bodies in struct functions not yet supported");
    }

    ctx.symbols.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// Self-Reference Detection (Public)
// ─────────────────────────────────────────────────────────────────────────────

SelfReferenceInfo checkSelfReference(const TypeAST* fieldType,
                                      const StructDeclAST* currentStruct,
                                      SemaContext& ctx) {
    return checkSelfReferenceImpl(fieldType, currentStruct, ctx);
}

bool isRecursiveValueType(const TypeAST* fieldType,
                           const StructDeclAST* currentStruct,
                           SemaContext& ctx) {
    SelfReferenceInfo info = checkSelfReferenceImpl(fieldType, currentStruct, ctx);
    return info.isSelfReference && !info.isPointer;
}

bool isPointerSelfReference(const TypeAST* fieldType,
                             const StructDeclAST* currentStruct,
                             SemaContext& ctx) {
    SelfReferenceInfo info = checkSelfReferenceImpl(fieldType, currentStruct, ctx);
    return info.isSelfReference && info.isPointer;
}

} // namespace sema
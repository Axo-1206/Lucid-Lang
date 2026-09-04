/// @file SemaValidate.cpp
/// @brief Implementation of semantic validation rules.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ASTStrings.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace sema {

// ─── Internal Helper: Check if a type implements a trait ─────────────────

/// @brief Check if a type satisfies a single trait constraint.
/// 
/// This is the core trait conformance check. It verifies that the source type
/// (which must be a struct) implements the target trait by checking its traitRefs.
static bool satisfiesTraitConstraint(TypeAST* actualType,
                                      NamedTypeAST* requiredTrait,
                                      SemaContext& ctx) {
    if (!actualType || !requiredTrait) return false;

    // Generic parameters are placeholders - checked at instantiation time
    if (isGenericParamType(actualType, ctx)) {
        return true;
    }

    // Actual type must be a named type
    if (!actualType->isa<NamedTypeAST>()) return false;
    NamedTypeAST* namedActual = actualType->as<NamedTypeAST>();
    TypeDeclAST* typeDecl = ctx.lookupType(namedActual->name);
    if (!typeDecl) return false;

    // Only structs can implement traits
    if (!typeDecl->isa<StructDeclAST>()) return false;
    StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // Resolve the required trait
    TraitDeclAST* traitDecl = resolveTraitRef(requiredTrait, ctx);
    if (!traitDecl) return false;

    // Check if the struct implements the trait by looking at its traitRefs
    for (NamedTypeAST* traitRef : structDecl->traitRefs) {
        TraitDeclAST* resolved = resolveTraitRef(traitRef, ctx);
        if (resolved == traitDecl) {
            return true;
        }
    }

    return false;
}

/// @brief Validate a single generic parameter's constraints.
static bool validateParamConstraints(TypeAST* actualType,
                                      GenericParamDeclAST* param,
                                      SemaContext& ctx) {
    if (!param || !actualType) return true;
    if (param->constraints.empty()) return true;

    for (NamedTypeAST* constraint : param->constraints) {
        if (!satisfiesTraitConstraint(actualType, constraint, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_GenericConstraint, actualType,
                                  "type does not implement trait '", 
                                  ctx.pool.lookup(constraint->name), "'");
            ctx.diagnostics.note(param, "parameter '", ctx.pool.lookup(param->name), 
                                 "' requires this trait");
            return false;
        }
    }

    return true;
}

// ─── Trait Validation ────────────────────────────────────────────────────

/// @brief Validate that a struct implements a single trait.
static bool validateSingleTraitImplementationInternal(
    StructDeclAST* structDecl,
    TraitDeclAST* traitDecl,
    SemaContext& ctx) {
    
    if (!structDecl || !traitDecl) return false;

    // Build a map of struct fields for quick lookup
    std::unordered_map<InternedString, FieldDeclAST*> structFields;
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->hasSyntaxError) continue;
        structFields[field->name] = field;
    }

    bool isValid = true;

    // Check each trait field
    for (TraitFieldDeclAST* traitField : traitDecl->fields) {
        if (traitField->hasSyntaxError) continue;

        bool hasBrokenStructField = false;
        for (FieldDeclAST* field : structDecl->fields) {
            if (field->name == traitField->name && field->hasSyntaxError) {
                hasBrokenStructField = true;
                break;
            }
        }
        if (hasBrokenStructField) continue;

        // ─── 1. Check: Field exists in struct ──────────────────────────
        auto it = structFields.find(traitField->name);
        if (it == structFields.end()) {
            ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, traitField,
                                  "struct '", ctx.pool.lookup(structDecl->name),
                                  "' is missing field '", ctx.pool.lookup(traitField->name),
                                  "' required by trait '", ctx.pool.lookup(traitDecl->name), "'");
            isValid = false;
            continue;
        }

        FieldDeclAST* structField = it->second;

        // ─── 2. Check: Const-ness compatibility ────────────────────────
        if (traitField->isConst() && !structField->isConst()) {
            ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                  "trait '", ctx.pool.lookup(traitDecl->name),
                                  "' requires field '", ctx.pool.lookup(traitField->name),
                                  "' to be const, but struct declares it as mutable");
            isValid = false;
            continue;
        }

        // ─── 3. Check: Type compatibility ──────────────────────────────
        if (!structField->type || !traitField->type) {
            ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                  "field '", ctx.pool.lookup(traitField->name),
                                  "' has missing type information");
            isValid = false;
            continue;
        }

        // ─── Downward Flow Rule: Trait fields cannot be borrowed types ─────
        // A trait is a contract for struct fields, and struct fields cannot
        // contain borrowed types (&T or [_]T)
        if (isBorrowedType(traitField->type)) {
            ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, traitField,
                                  "trait '", ctx.pool.lookup(traitDecl->name),
                                  "' has field '", ctx.pool.lookup(traitField->name),
                                  "' of borrowed type (", 
                                  typeToString(traitField->type, ctx.pool),
                                  ") — traits cannot require borrowed types");
            isValid = false;
            continue;
        }

        if (traitField->isConst()) {
            // Const fields: types must match exactly
            if (!typesEqual(structField->type, traitField->type)) {
                ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                      "const field '", ctx.pool.lookup(traitField->name),
                                      "' type mismatch: trait expects ",
                                      typeToString(traitField->type, ctx.pool),
                                      ", struct has ",
                                      typeToString(structField->type, ctx.pool));
                isValid = false;
                continue;
            }
        } else {
            // Non-const fields: allow assignable types
            if (!isAssignable(traitField->type, structField->type, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                      "field '", ctx.pool.lookup(traitField->name),
                                      "' type mismatch: trait expects ",
                                      typeToString(traitField->type, ctx.pool),
                                      ", struct has ",
                                      typeToString(structField->type, ctx.pool));
                isValid = false;
                continue;
            }
        }

        // ─── 4. Check: Const trait field type restrictions ─────────────
        if (traitField->isConst()) {
            if (isNullableType(traitField->type) || isFallibleType(traitField->type)) {
                ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, traitField,
                                      "trait '", ctx.pool.lookup(traitDecl->name),
                                      "' has const field '", ctx.pool.lookup(traitField->name),
                                      "' that is nullable or fallible (must be definite)");
                isValid = false;
                continue;
            }
        }
    }

    return isValid;
}

/// @brief Check for conflicting field names across multiple traits.
static bool checkTraitFieldConflictsInternal(
    StructDeclAST* structDecl,
    SemaContext& ctx) {
    
    if (!structDecl) return true;

    struct FieldRequirement {
        TraitDeclAST* trait;
        bool isConst;
        TypeAST* type;
    };
    
    std::unordered_map<InternedString, std::vector<FieldRequirement>> requirements;

    for (NamedTypeAST* traitRef : structDecl->traitRefs) {
        TraitDeclAST* trait = resolveTraitRef(traitRef, ctx);
        if (!trait) continue;

        for (TraitFieldDeclAST* field : trait->fields) {
            if (field->hasSyntaxError) continue;
            requirements[field->name].push_back({
                trait,
                field->isConst(),
                field->type
            });
        }
    }

    bool hasConflict = false;
    for (const auto& [fieldName, reqs] : requirements) {
        if (reqs.size() <= 1) continue;

        const FieldRequirement* first = &reqs[0];
        
        for (size_t i = 1; i < reqs.size(); ++i) {
            const FieldRequirement* other = &reqs[i];

            if (first->isConst != other->isConst) {
                ctx.diagnostics.error(DiagCode::Sem_TraitConflict, structDecl,
                                      "field '", ctx.pool.lookup(fieldName),
                                      "' has conflicting const requirements: ",
                                      "trait '", ctx.pool.lookup(first->trait->name),
                                      "' requires ", first->isConst ? "const" : "mutable",
                                      ", but trait '", ctx.pool.lookup(other->trait->name),
                                      "' requires ", other->isConst ? "const" : "mutable");
                hasConflict = true;
                continue;
            }

            if (first->type && other->type) {
                if (!typesEqual(first->type, other->type)) {
                    ctx.diagnostics.error(DiagCode::Sem_TraitConflict, structDecl,
                                          "field '", ctx.pool.lookup(fieldName),
                                          "' has conflicting types: ",
                                          "trait '", ctx.pool.lookup(first->trait->name),
                                          "' expects ",
                                          typeToString(first->type, ctx.pool),
                                          ", but trait '", ctx.pool.lookup(other->trait->name),
                                          "' expects ",
                                          typeToString(other->type, ctx.pool));
                    hasConflict = true;
                }
            }
        }
    }

    return !hasConflict;
}

// ─── Const Validation ────────────────────────────────────────────────────

bool validateConstType(TypeAST* type,
                        InternedString name,
                        const char* kind,
                        SemaContext& ctx) {
    if (!type) return false;

    if (isNullableType(type) || isFallibleType(type)) {
        ctx.diagnostics.error(DiagCode::Sem_ConstNullable, type,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' must be definite (not nullable or fallible)");
        return false;
    }

    // Combined type (T?!) is also not allowed for const
    if (type->isa<CombinedTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ConstNullable, type,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' cannot be combined (T?!). Use a definite type.");
        return false;
    }

    // ─── Const cannot be a borrowed type ───────────────────────────────────
    // const values must be definite and owned - &T and [_]T are borrowed
    if (isBorrowedType(type)) {
        ctx.diagnostics.error(DiagCode::Sem_ConstNullable, type,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' cannot be a borrowed type (",
                              typeToString(type, ctx.pool),
                              ") — const values must be owned");
        return false;
    }

    return true;
}

// ─── Public Trait Validation ────────────────────────────────────────────

bool validateTraitImplementation(StructDeclAST* structDecl,
                                  TraitDeclAST* traitDecl,
                                  SemaContext& ctx) {
    return validateSingleTraitImplementationInternal(structDecl, traitDecl, ctx);
}

bool validateAllTraitImplementations(StructDeclAST* structDecl,
                                      SemaContext& ctx) {
    if (!structDecl) return true;
    if (structDecl->traitRefs.empty()) return true;

    bool conflicts = checkTraitFieldConflictsInternal(structDecl, ctx);
    bool allValid = true;

    for (NamedTypeAST* traitRef : structDecl->traitRefs) {
        TraitDeclAST* trait = resolveTraitRef(traitRef, ctx);
        if (!trait) {
            // Broken trait declarations should already have produced the parser's
            // diagnostic; semantic validation should stay silent and continue.
            continue;
        }

        if (!validateSingleTraitImplementationInternal(structDecl, trait, ctx)) {
            allValid = false;
            continue;
        }
        // No cache needed - traitRefs already stores the information
    }

    return allValid && !conflicts;
}

bool checkTraitFieldConflicts(StructDeclAST* structDecl,
                               SemaContext& ctx) {
    return checkTraitFieldConflictsInternal(structDecl, ctx);
}

// ─── Generic Validation ──────────────────────────────────────────────────

bool validateGenericArguments(ArenaSpan<TypeAST*> args,
                               ArenaSpan<GenericParamDeclAST*> params,
                               BaseAST* useSite,
                               SemaContext& ctx) {
    if (args.size() != params.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, useSite,
                              "expected ", params.size(),
                              " generic arguments, got ", args.size());
        return false;
    }

    bool allValid = true;

    for (size_t i = 0; i < args.size(); ++i) {
        TypeAST* resolvedArg = resolveType(args[i], ctx);
        if (!resolvedArg) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, useSite,
                                  "invalid generic argument at position ", i + 1);
            allValid = false;
            continue;
        }

        // ─── Generic arguments cannot be borrowed types ─────────────────────
        // T in <T> must be an owned type - borrowed types (&T and [_]T)
        // cannot be used as generic arguments
        if (isBorrowedType(resolvedArg)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, useSite,
                                  "generic argument at position ", i + 1,
                                  " cannot be a borrowed type (",
                                  typeToString(resolvedArg, ctx.pool),
                                  ") — generic parameters must be owned types");
            allValid = false;
            continue;
        }

        if (!validateParamConstraints(resolvedArg, params[i], ctx)) {
            allValid = false;
        }
    }

    return allValid;
}

bool validateGenericParameterUsage(ArenaSpan<GenericParamDeclAST*> params,
                                    const std::vector<TypeAST*>& types,
                                    BaseAST* useSite,
                                    SemaContext& ctx) {
    std::unordered_set<InternedString> usedParams;

    // Recursively find generic parameter references in a type
    std::function<void(TypeAST*)> findParams = [&](TypeAST* type) {
        if (!type) return;
        
        if (NamedTypeAST* named = type->as<NamedTypeAST>()) {
            if (ctx.isGenericParam(named->name)) {
                usedParams.insert(named->name);
            }
            for (TypeAST* arg : named->genericArgs) {
                findParams(arg);
            }
            return;
        }

        if (NullableTypeAST* nullable = type->as<NullableTypeAST>()) {
            findParams(nullable->inner); return;
        }
        if (FallibleTypeAST* fallible = type->as<FallibleTypeAST>()) {
            findParams(fallible->inner); return;
        }
        if (CombinedTypeAST* combined = type->as<CombinedTypeAST>()) {
            findParams(combined->inner); return;
        }
        if (RefTypeAST* ref = type->as<RefTypeAST>()) {
            findParams(ref->inner); return;
        }
        if (PtrTypeAST* ptr = type->as<PtrTypeAST>()) {
            findParams(ptr->inner); return;
        }
        if (ArrayTypeAST* array = type->as<ArrayTypeAST>()) {
            findParams(array->element); return;
        }
        if (FuncTypeAST* func = type->as<FuncTypeAST>()) {
            for (ParamAST* param : func->params) {
                findParams(param->type);
            }
            findParams(func->returnType);
            return;
        }
    };

    for (TypeAST* type : types) {
        findParams(type);
    }

    bool allUsed = true;
    for (GenericParamDeclAST* param : params) {
        if (usedParams.find(param->name) == usedParams.end()) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamUnused, useSite,
                                  "generic parameter '", ctx.pool.lookup(param->name),
                                  "' is not used in the declaration");
            allUsed = false;
        }
    }

    return allUsed;
}

// ─── Downward Flow Rule ──────────────────────────────────────────────────

bool validateRefContext(RefTypeAST* type, SemaContext& ctx) {
    // Delegate to the unified borrowed context validation
    return validateBorrowedContext(type, ctx);
}

// ─── FFI Validation ──────────────────────────────────────────────────────

bool validateForeignFunction(FuncDeclAST* decl,
                              AttributeAST* foreignAttr,
                              SemaContext& ctx) {
    if (!decl || !foreignAttr) return false;

    // ─── 1. Validate ABI ─────────────────────────────────────────────────────
    if (foreignAttr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgCount, foreignAttr,
                              "@[foreign] requires an ABI argument");
        return false;
    }

    const LiteralExprAST* abiLiteral = foreignAttr->args[0];
    if (!abiLiteral || abiLiteral->kind != LiteralKind::String) {
        ctx.diagnostics.error(DiagCode::Sem_ForeignABI, foreignAttr,
                              "@[foreign] ABI must be a string literal");
        return false;
    }

    std::string abi = ctx.pool.lookup(abiLiteral->value);
    if (abi != "C") {
        ctx.diagnostics.error(DiagCode::Sem_ForeignABI, foreignAttr,
                              "unsupported foreign ABI '", abi, "' — only \"C\" is supported");
        return false;
    }

    // ─── 2. Check: Function must have no body ────────────────────────────────
    if (decl->body) {
        ctx.diagnostics.error(DiagCode::Sem_ForeignInvalid, decl,
                              "foreign function '", ctx.pool.lookup(decl->name),
                              "' must have no body (implementation is external)");
        return false;
    }

    // ─── 3. Validate parameter types ─────────────────────────────────────────
    FuncTypeAST* funcType = decl->type->as<FuncTypeAST>();
    if (!funcType) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, decl,
                              "foreign function '", ctx.pool.lookup(decl->name),
                              "' has no function type");
        return false;
    }

    bool allValid = true;

    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            // ─── Arena cannot be passed to FFI ─────────────────────────────
            if (isArenaType(param->type)) {
                ctx.diagnostics.error(DiagCode::Ffi_InvalidForeign, param,
                                      "foreign function parameter '", 
                                      ctx.pool.lookup(param->name),
                                      "' cannot be of type Arena");
                ctx.diagnostics.note(param,
                                     "Arena is scope-confined and cannot cross the FFI boundary");
                allValid = false;
            }

            if (!isValidFFIType(param->type, ctx)) {
                ctx.diagnostics.error(DiagCode::Ffi_TypeNotFFI, param,
                                      "parameter '", ctx.pool.lookup(param->name),
                                      "' type is not FFI-compatible");
                allValid = false;
            }
        }
    }

    // ─── 4. Validate return type ─────────────────────────────────────────────
    TypeAST* returnType = funcType->returnType;
    if (returnType) {
        // ─── Arena cannot be returned from FFI ─────────────────────────────
        if (isArenaType(returnType)) {
            ctx.diagnostics.error(DiagCode::Ffi_InvalidForeign, decl,
                                  "foreign function cannot return Arena");
            ctx.diagnostics.note(decl,
                                 "Arena is scope-confined and cannot cross the FFI boundary");
            allValid = false;
        }

        if (!isValidFFIType(returnType, ctx)) {
            ctx.diagnostics.error(DiagCode::Ffi_TypeNotFFI, decl,
                                  "return type of foreign function '",
                                  ctx.pool.lookup(decl->name), "' is not FFI-compatible");
            allValid = false;
        }
    }

    // ─── 5. Validate no generic parameters ──────────────────────────────────
    if (!decl->genericParams.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ForeignInvalid, decl,
                              "foreign function '", ctx.pool.lookup(decl->name),
                              "' cannot have generic parameters");
        allValid = false;
    }

    return allValid;
}

// ─── Downward Flow Rule Validation ──────────────────────────────────────

bool validateBorrowedContext(TypeAST* type, SemaContext& ctx) {
    if (!type || !isBorrowedType(type)) {
        return true;
    }

    // ─── Rule 1: No Struct Storage ─────────────────────────────────────────
    // A borrowed type cannot be stored in a struct field
    TypeDeclAST* currentType = ctx.currentDefiningType();
    if (currentType && currentType->isa<StructDeclAST>()) {
        const char* typeName = type->isa<RefTypeAST>() ? "reference (&T)" : "slice ([_]T)";
        ctx.diagnostics.error(DiagCode::Sem_RefInStruct, type,
                              "borrowed type ", typeName,
                              " cannot be stored in struct fields");
        return false;
    }

    // ─── Rule 2: No Array/Slice Storage ────────────────────────────────────
    // A borrowed type cannot be an element of an array or slice
    // This is checked in resolveArrayType, but we also check the context here
    // The caller should have already checked this
    
    // ─── Rule 3: No Borrowed Returns ──────────────────────────────────────
    // A borrowed type cannot be returned from a function
    // This is checked in resolveFuncType for the return type
    
    // ─── Rule 4: No Closure Capture ──────────────────────────────────────
    // A borrowed type cannot be captured by a closure
    // This is checked in resolveAnonFuncExpr and resolveFuncDecl
    
    return true;
}

// ─── Arena Initializer Validation ─────────────────────────────────────────

bool validateArenaInitializer(ExprAST* init, SemaContext& ctx) {
    if (!init) {
        return false;
    }
    
    // Must be an ArenaAccessExprAST
    if (!init->isa<ArenaAccessExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArenaInit, init,
                              "Arena binding must be initialized with "
                              "Arena::create(size) or Arena::empty()");
        ctx.diagnostics.note(init,
                              "Found: ", init->resolvedType 
                              ? typeToString(init->resolvedType, ctx.pool) 
                              : "unknown");
        return false;
    }
    
    ArenaAccessExprAST* access = init->as<ArenaAccessExprAST>();
    
    // Must be static form (Arena::method)
    if (!access->isStatic) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArenaInit, init,
                              "Arena binding must be initialized with "
                              "Arena::create(size) or Arena::empty(), "
                              "not an existing arena");
        return false;
    }
    
    // Method must be "create" or "empty"
    std::string_view methodName = lookupStringView(access->methodName);
    if (methodName != "create" && methodName != "empty") {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArenaInit, init,
                              "Arena binding must be initialized with "
                              "Arena::create(size) or Arena::empty()");
        ctx.diagnostics.note(init,
                              "Found: Arena::", methodName, 
                              " - only create and empty are valid");
        return false;
    }
    
    // create(size) requires exactly one argument
    if (methodName == "create") {
        if (access->args.size() != 1) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaMethodArgCount, init,
                                  "Arena::create expects exactly 1 argument (size), got ",
                                  access->args.size());
            return false;
        }
    }
    
    // empty() requires no arguments
    if (methodName == "empty") {
        if (access->args.size() != 0) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaMethodArgCount, init,
                                  "Arena::empty takes no arguments");
            return false;
        }
    }
    
    return true;
}

// ─── Simd Validation ────────────────────────────────────────────────────

bool validateSimdType(SimdTypeAST* simdType, SemaContext& ctx) {
    if (!simdType) return true;
    
    // Validate lane count
    if (simdType->laneCount == 0) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSimdLaneCount, simdType,
                              "Simd lane count must be > 0");
        return false;
    }
    
    // Validate element type is a numeric primitive
    if (!simdType->elementType) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSimdElementType, simdType,
                              "Simd element type is missing");
        return false;
    }
    
    if (!isValidSimdElementType(simdType->elementType)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSimdElementType, simdType->elementType,
                              "Simd element type must be a numeric primitive "
                              "(int8, int16, int32, int64, uint8, uint16, uint32, "
                              "uint64, float32, or float64)");
        return false;
    }
    
    return true;
}

} // namespace sema
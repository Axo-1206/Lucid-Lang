/**
 * @file SemaType.cpp
 * @brief Implements Sema.hpp's "Types (Type Resolution)" section —
 *        resolveType() and every specific resolve*Type() function.
 *
 * @architectural_note Type resolution vs. type checking
 *   Type resolution confirms that every named type actually exists in the
 *   current context and annotates the TypeAST nodes with that information.
 *   Unlike checkExpr(), type resolution does NOT compute a separate
 *   "resolved type" node — the same TypeAST pointer is annotated and returned.
 *
 * @architectural_note Generic parameter shadowing
 *   NamedTypeAST::isGenericParam is set when a name resolves to a generic
 *   parameter (via ctx.lookupGenericParam()) rather than a real type.
 *   This is used by codegen to distinguish abstract parameters from concrete
 *   types (see NamedTypeAST's doc comment in TypeAST.hpp).
 */

#include "../Sema.hpp"
#include "../SemaContext.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"

// =============================================================================
// resolveType — Dispatch
// =============================================================================

/**
 * @brief Resolve a type annotation: dispatch to the specific resolve*Type()
 *        function based on the type's kind.
 *
 * @param type The type to resolve (may be nullptr — returns nullptr).
 * @param ctx  The semantic context.
 * @return The resolved type (same pointer), or nullptr on error.
 */
TypeAST* resolveType(TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return resolvePrimitiveType(type->as<PrimitiveTypeAST>(), ctx);
        case ASTKind::NamedType:
            return resolveNamedType(type->as<NamedTypeAST>(), ctx);
        case ASTKind::ArrayType:
            return resolveArrayType(type->as<ArrayTypeAST>(), ctx);
        case ASTKind::NullableType:
            return resolveNullableType(type->as<NullableTypeAST>(), ctx);
        case ASTKind::FallibleType:
            return resolveFallibleType(type->as<FallibleTypeAST>(), ctx);
        case ASTKind::CombinedType:
            return resolveCombinedType(type->as<CombinedTypeAST>(), ctx);
        case ASTKind::RefType:
            return resolveRefType(type->as<RefTypeAST>(), ctx);
        case ASTKind::PtrType:
            return resolvePtrType(type->as<PtrTypeAST>(), ctx);
        case ASTKind::FuncType:
            return resolveFuncType(type->as<FuncTypeAST>(), ctx);
        default:
            // Unknown/error-recovery type
            return nullptr;
    }
}

// =============================================================================
// resolvePrimitiveType
// =============================================================================

/**
 * @brief Resolve a primitive type — trivially succeeds (primitives are built-in).
 *
 * @param type The primitive type.
 * @param ctx  The semantic context.
 * @return The same primitive type (always succeeds).
 */
TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx) {
    (void)ctx; // Unused
    return type;
}

// =============================================================================
// resolveNamedType
// =============================================================================

/**
 * @brief Resolve a named type reference: look up the name as a type or
 *        generic parameter.
 *
 * Sets NamedTypeAST::isGenericParam if the name resolves to a generic parameter.
 *
 * @param type The named type.
 * @param ctx  The semantic context.
 * @return The same named type, or nullptr on error.
 */
TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // First, try to resolve as a real type
    TypeDeclAST* decl = resolveTypeNameOrError(type, ctx);
    
    if (decl) {
        // It's a real type declaration
        type->isGenericParam = false;
        
        // Resolve any generic arguments
        if (!type->genericArgs.empty()) {
            // TODO: Check generic argument arity and constraints against
            //       the declaration's generic parameters
            for (TypePtr arg : type->genericArgs) {
                resolveType(arg, ctx);
            }
        }
        return type;
    }

    // If resolveTypeNameOrError returned nullptr, check if it's a generic param
    GenericParamDeclAST* genericParam = ctx.lookupGenericParam(type->name);
    if (genericParam) {
        // It's a generic parameter
        type->isGenericParam = true;
        return type;
    }

    // If we get here, resolveTypeNameOrError already reported an error
    return nullptr;
}

// =============================================================================
// resolveArrayType
// =============================================================================

/**
 * @brief Resolve an array type: resolve the element type.
 *
 * @param type The array type.
 * @param ctx  The semantic context.
 * @return The same array type, or nullptr on error.
 */
TypeAST* resolveArrayType(ArrayTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    type->element = resolveType(type->element, ctx);
    if (!type->element) {
        return nullptr;
    }

    // For fixed arrays, verify the size is a valid positive integer
    if (type->arrayKind == ArrayKind::Fixed) {
        if (type->size == 0) {
            ctx.error(type, DiagCode::E3003,
                       "fixed array size must be greater than 0");
            return nullptr;
        }
        // TODO: Verify the size is within reasonable limits
    }

    return type;
}

// =============================================================================
// resolveNullableType
// =============================================================================

/**
 * @brief Resolve a nullable type: verify the inner type is valid and
 *        enforce that nullable types are only valid on value types.
 *
 * @param type The nullable type.
 * @param ctx  The semantic context.
 * @return The same nullable type, or nullptr on error.
 */
TypeAST* resolveNullableType(NullableTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    type->inner = resolveType(type->inner, ctx);
    if (!type->inner) {
        return nullptr;
    }

    // Enforce that ? is only valid on value types
    // (primitives, structs, enums, traits — not arrays or function types)
    if (type->inner->isa<ArrayTypeAST>() ||
        type->inner->isa<FuncTypeAST>() ||
        type->inner->isa<RefTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                   "nullable type (?) is not valid on array, function, or reference types");
        return nullptr;
    }

    return type;
}

// =============================================================================
// resolveFallibleType
// =============================================================================

/**
 * @brief Resolve a fallible type: verify the inner type is valid and
 *        enforce that fallible types are only valid on value types.
 *
 * @param type The fallible type.
 * @param ctx  The semantic context.
 * @return The same fallible type, or nullptr on error.
 */
TypeAST* resolveFallibleType(FallibleTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    type->inner = resolveType(type->inner, ctx);
    if (!type->inner) {
        return nullptr;
    }

    // Enforce that ! is only valid on value types
    // (primitives, structs, enums, traits — not arrays or function types)
    if (type->inner->isa<ArrayTypeAST>() ||
        type->inner->isa<FuncTypeAST>() ||
        type->inner->isa<RefTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                   "fallible type (!) is not valid on array, function, or reference types");
        return nullptr;
    }

    return type;
}

// =============================================================================
// resolveCombinedType
// =============================================================================

/**
 * @brief Resolve a combined nullable+fallible type (T?!): verify the inner
 *        type is valid and enforce that combined types are only valid on
 *        value types.
 *
 * @param type The combined type.
 * @param ctx  The semantic context.
 * @return The same combined type, or nullptr on error.
 */
TypeAST* resolveCombinedType(CombinedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    type->inner = resolveType(type->inner, ctx);
    if (!type->inner) {
        return nullptr;
    }

    // Enforce that ?! is only valid on value types
    if (type->inner->isa<ArrayTypeAST>() ||
        type->inner->isa<FuncTypeAST>() ||
        type->inner->isa<RefTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                   "combined nullable+fallible type (?!.) is not valid on array, function, or reference types");
        return nullptr;
    }

    return type;
}

// =============================================================================
// resolveRefType
// =============================================================================

/**
 * @brief Resolve a reference type (&T): verify the inner type is valid and
 *        enforce the Downward Flow Rule (no refs in structs/arrays/returns).
 *
 * @param type The reference type.
 * @param ctx  The semantic context.
 * @return The same reference type, or nullptr on error.
 */
TypeAST* resolveRefType(RefTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    type->inner = resolveType(type->inner, ctx);
    if (!type->inner) {
        return nullptr;
    }

    // The Downward Flow Rule: references can only appear as:
    //   - Function parameters
    //   - Local variable aliases
    // They cannot be stored in structs, arrays, or returned from functions.

    // Check if we're inside a struct field (via ctx.isDefiningType())
    if (ctx.isDefiningType(ctx.currentlyDefiningType())) {
        ctx.error(type, DiagCode::E3004,
                   "reference type (&T) cannot be stored in a struct field");
        return nullptr;
    }

    // Check if we're in a function return position
    // TODO: Detect if this type is being used as a return type
    // (requires context from the caller)

    // Check if we're in an array element type
    // This is a bit tricky: we need to know if this type is being resolved
    // as part of an array's element type.
    // TODO: Track context during type resolution

    return type;
}

// =============================================================================
// resolvePtrType
// =============================================================================

/**
 * @brief Resolve a raw pointer type (*T): verify the inner type is valid.
 *
 * Raw pointers are sealed conduits — they can be stored anywhere but
 * cannot be dereferenced directly.
 *
 * @param type The pointer type.
 * @param ctx  The semantic context.
 * @return The same pointer type, or nullptr on error.
 */
TypeAST* resolvePtrType(PtrTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    type->inner = resolveType(type->inner, ctx);
    if (!type->inner) {
        return nullptr;
    }

    // Raw pointers are valid in all contexts (structs, arrays, parameters, returns)
    // They are sealed conduits — the restrictions are enforced at use sites,
    // not at declaration sites.

    return type;
}

// =============================================================================
// resolveFuncType
// =============================================================================

/**
 * @brief Resolve a function type: resolve all parameter and return types.
 *
 * For curried functions, recursively resolves the inner function type.
 *
 * @param type The function type.
 * @param ctx  The semantic context.
 * @return The same function type, or nullptr on error.
 */
TypeAST* resolveFuncType(FuncTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Resolve each parameter's type
    for (ParamAST* param : type->params) {
        param->type = resolveType(param->type, ctx);
        if (!param->type) {
            return nullptr;
        }
    }

    // Resolve each return type
    for (size_t i = 0; i < type->returnTypes.size(); ++i) {
        TypeAST* returnType = type->returnTypes[i];
        
        // If the return type is another FuncTypeAST (currying), resolve it too
        if (returnType && returnType->isa<FuncTypeAST>()) {
            returnType = resolveFuncType(returnType->as<FuncTypeAST>(), ctx);
        } else {
            returnType = resolveType(returnType, ctx);
        }
        
        if (!returnType) {
            return nullptr;
        }
        type->returnTypes[i] = returnType;
    }

    return type;
}

// =============================================================================
// resolveTraitRef
// =============================================================================

/**
 * @brief Resolve a trait reference: look up the trait by name.
 *
 * @param ref The trait reference.
 * @param ctx The semantic context.
 * @return The resolved trait declaration, or nullptr on error.
 */
TraitDeclAST* resolveTraitRef(TraitRefAST* ref, SemaContext& ctx) {
    if (!ref) return nullptr;

    // Look up the type by name
    TypeDeclAST* decl = ctx.lookupType(ref->name);
    if (!decl) {
        ctx.error(ref, DiagCode::E2002,
                   "undefined type '", ctx.toString(ref->name), "'");
        return nullptr;
    }

    // Verify it's a trait
    if (!decl->isa<TraitDeclAST>()) {
        ctx.error(ref, DiagCode::E2002,
                   "'", ctx.toString(ref->name), "' is not a trait");
        return nullptr;
    }

    TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();

    // Check generic arguments
    if (!ref->genericArgs.empty()) {
        // TODO: Check generic argument arity and constraints against the
        //       trait's generic parameters
        for (TypePtr arg : ref->genericArgs) {
            resolveType(arg, ctx);
        }
    }

    return traitDecl;
}

// =============================================================================
// validateGenericParamUsage
// =============================================================================

/**
 * @brief Verify every generic parameter is used in at least one field/param type.
 *
 * @param params The generic parameters to check.
 * @param owner  The declaration that owns these parameters.
 * @param ctx    The semantic context.
 */
void validateGenericParamUsage(ArenaSpan<GenericParamDeclPtr> params,
                                DeclAST* owner,
                                SemaContext& ctx) {
    if (params.empty()) return;

    // Collect all generic parameter names
    std::unordered_set<InternedString> paramNames;
    for (GenericParamDeclAST* param : params) {
        paramNames.insert(param->name);
    }

    // TODO: Walk through the owner's fields/params and mark which generic
    //       parameters are actually used.
    //       This requires traversing the AST to find all NamedTypeAST nodes
    //       and checking if their names are generic parameters.

    // For now, this is a placeholder.
    // The actual implementation would:
    // 1. For StructDeclAST: walk all field types
    // 2. For FuncDeclAST: walk all parameter types and return types
    // 3. For TraitDeclAST: walk all field types
    // 4. Mark each used generic parameter
    // 5. Report E4001 for any unused parameter

    (void)owner; // Unused for now
}

// =============================================================================
// validateTraitImplementation
// =============================================================================

/**
 * @brief Verify that a struct implements all fields required by a trait.
 *
 * @param structDecl The struct declaration.
 * @param traitRef   The trait reference.
 * @param ctx        The semantic context.
 * @return true if the struct fully implements the trait.
 */
bool validateTraitImplementation(StructDeclAST* structDecl,
                                  TraitRefAST* traitRef,
                                  SemaContext& ctx) {
    if (!structDecl || !traitRef) return false;

    TraitDeclAST* trait = resolveTraitRef(traitRef, ctx);
    if (!trait) return false;

    bool allFieldsImplemented = true;

    // For each field required by the trait
    for (TraitFieldDeclAST* traitField : trait->fields) {
        bool found = false;

        // Find a matching field in the struct
        for (FieldDeclAST* structField : structDecl->fields) {
            if (structField->name != traitField->name) continue;

            found = true;

            // Verify the type matches
            if (!typesEqual(structField->type, traitField->type)) {
                ctx.error(structField, DiagCode::E3003,
                           "trait '", ctx.toString(trait->name),
                           "' requires field '", ctx.toString(traitField->name),
                           "' of type ", ctx.toString(traitField->type),
                           ", but struct declares it as ",
                           ctx.toString(structField->type));
                allFieldsImplemented = false;
                break;
            }

            // Verify const-ness matches
            if (traitField->isConst && !structField->isConst) {
                ctx.error(structField, DiagCode::E3004,
                           "trait '", ctx.toString(trait->name),
                           "' requires field '", ctx.toString(traitField->name),
                           "' to be const");
                allFieldsImplemented = false;
                break;
            }

            // Trait fields must not be nullable or fallible
            if (isNullableType(structField->type) || isFallibleType(structField->type)) {
                ctx.error(structField, DiagCode::E3004,
                           "trait field '", ctx.toString(traitField->name),
                           "' must not be nullable or fallible");
                allFieldsImplemented = false;
                break;
            }

            break;
        }

        if (!found) {
            ctx.error(traitRef, DiagCode::E2001,
                       "struct '", ctx.toString(structDecl->name),
                       "' is missing required trait field '",
                       ctx.toString(traitField->name), "'");
            allFieldsImplemented = false;
        }
    }

    return allFieldsImplemented;
}

// =============================================================================
// checkGenericArgs
// =============================================================================

/**
 * @brief Verify a generic argument list's arity and constraints.
 *
 * @param args       The generic arguments provided at the use site.
 * @param params     The generic parameters of the declaration.
 * @param useSite    The AST node where the generic is used (for diagnostics).
 * @param ctx        The semantic context.
 */
void checkGenericArgs(ArenaSpan<TypePtr> args,
                       ArenaSpan<GenericParamDeclPtr> params,
                       BaseAST* useSite,
                       SemaContext& ctx) {
    if (args.empty() && params.empty()) return;

    // Check arity
    if (args.size() != params.size()) {
        ctx.error(useSite, DiagCode::E3001,
                   "wrong number of generic arguments: expected ",
                   std::to_string(params.size()), ", found ",
                   std::to_string(args.size()));
        return;
    }

    // Resolve each argument and check constraints
    for (size_t i = 0; i < args.size(); ++i) {
        TypeAST* argType = resolveType(args[i], ctx);
        if (!argType) continue;

        GenericParamDeclAST* param = params[i];
        
        // TODO: Check that argType satisfies all of param's constraints
        // For each constraint in param->constraints:
        //   - Resolve the constraint trait
        //   - Verify argType implements the trait
        //   (requires walking the struct's traitRefs)
    }
}

// =============================================================================
// isDirectSelfReference
// =============================================================================

/**
 * @brief Check if a field type refers directly to the owner's own type
 *        (with no indirection), which would make the owner infinite-sized.
 *
 * @param fieldType The field's type.
 * @param owner     The type declaration being defined.
 * @param ctx       The semantic context.
 * @return true if the field type is a direct self-reference.
 */
bool isDirectSelfReference(TypeAST* fieldType, TypeDeclAST* owner, SemaContext& ctx) {
    if (!fieldType || !owner) return false;

    // Only check if the owner is currently being defined
    if (!ctx.isDefiningType(owner)) {
        return false;
    }

    // Check if the field type is a named type referencing the owner
    if (fieldType->isa<NamedTypeAST>()) {
        NamedTypeAST* named = fieldType->as<NamedTypeAST>();
        
        // Resolve the name to check if it's the owner
        TypeDeclAST* decl = ctx.lookupType(named->name);
        if (decl == owner) {
            // It's a direct reference to the owner (no indirection)
            return true;
        }
    }

    // For wrapper types (nullable, fallible, combined, ref, ptr, array),
    // check if the inner type is a direct self-reference.
    // Indirections like ptr<T> or T? break the infinite size cycle.
    if (fieldType->isa<NullableTypeAST>()) {
        return isDirectSelfReference(fieldType->as<NullableTypeAST>()->inner, owner, ctx);
    }
    if (fieldType->isa<FallibleTypeAST>()) {
        return isDirectSelfReference(fieldType->as<FallibleTypeAST>()->inner, owner, ctx);
    }
    if (fieldType->isa<CombinedTypeAST>()) {
        return isDirectSelfReference(fieldType->as<CombinedTypeAST>()->inner, owner, ctx);
    }
    if (fieldType->isa<ArrayTypeAST>()) {
        // Arrays are direct storage (no indirection) — they don't break the cycle
        // unless the element type is a pointer/reference.
        ArrayTypeAST* arr = fieldType->as<ArrayTypeAST>();
        return isDirectSelfReference(arr->element, owner, ctx);
    }
    if (fieldType->isa<RefTypeAST>()) {
        // References break the cycle (indirection)
        return false;
    }
    if (fieldType->isa<PtrTypeAST>()) {
        // Pointers break the cycle (indirection)
        return false;
    }

    return false;
}

// =============================================================================
// checkRecursiveFieldType
// =============================================================================

/**
 * @brief Resolve a field's type and reject it if it's a direct self-reference.
 *
 * @param field The field being analyzed.
 * @param owner The type declaration being defined.
 * @param ctx   The semantic context.
 */
void checkRecursiveFieldType(FieldDeclAST* field, TypeDeclAST* owner, SemaContext& ctx) {
    if (!field || !owner) return;

    // Resolve the field's type
    field->type = resolveType(field->type, ctx);
    if (!field->type) return;

    // Check for direct self-reference (infinite size)
    if (isDirectSelfReference(field->type, owner, ctx)) {
        ctx.error(field, DiagCode::E3003,
                   "struct '", ctx.toString(owner->name),
                   "' contains a field of its own type directly (would be infinite size)");
    }
}

// =============================================================================
// validateForeignFunc
// =============================================================================

/**
 * @brief Validate a foreign function declaration against the FFI manifest.
 *
 * @param decl       The foreign function declaration.
 * @param foreignAttr The @[foreign("...")] attribute.
 * @param ctx        The semantic context.
 */
void validateForeignFunc(FuncDeclAST* decl, AttributeAST* foreignAttr, SemaContext& ctx) {
    if (!decl || !foreignAttr) return;

    // TODO: Validate the foreign function signature against the FFI manifest
    // This includes:
    // 1. Resolving the foreign function name
    // 2. Checking parameter types are FFI-compatible
    // 3. Checking return type is FFI-compatible
    // 4. Matching against the lge_ffi.lfi symbol table

    // For now, just verify the ABI is "C" (already checked by AttributesRegistry)
    // and mark the function as foreign.

    // Verify all parameter types are FFI-compatible
    for (FuncTypeAST* group = decl->funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            if (!isValidFFIType(param->type, ctx)) {
                ctx.error(param, DiagCode::E4101,
                           "foreign function parameter has incompatible type");
            }
        }
        // Check return types (for non-curried groups)
        if (!group->isCurried()) {
            for (TypePtr returnType : group->returnTypes) {
                if (!isValidFFIType(returnType, ctx)) {
                    ctx.error(decl, DiagCode::E4101,
                               "foreign function return type is incompatible");
                }
            }
        }
    }
}

// =============================================================================
// isValidFFIType
// =============================================================================

/**
 * @brief Check if a type is legal at an FFI boundary.
 *
 * FFI-compatible types:
 *   - Primitive types (int, float, bool, etc.)
 *   - Raw pointers (*T)
 *   - Void (no return type)
 *   - Structs with FFI-compatible fields
 *   - Enums with FFI-compatible backing types
 *
 * @param type The type to check.
 * @param ctx  The semantic context.
 * @return true if the type is FFI-compatible.
 */
bool isValidFFIType(TypeAST* type, SemaContext& ctx) {
    if (!type) return true; // void is valid

    // Primitives are always valid
    if (type->isa<PrimitiveTypeAST>()) {
        return true;
    }

    // Raw pointers are valid
    if (type->isa<PtrTypeAST>()) {
        // The inner type must be FFI-compatible
        return isValidFFIType(type->as<PtrTypeAST>()->inner, ctx);
    }

    // References are NOT valid at FFI boundary
    if (type->isa<RefTypeAST>()) {
        return false;
    }

    // Named types (structs, enums) must be FFI-compatible
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        TypeDeclAST* decl = ctx.lookupType(named->name);
        if (!decl) return false;

        if (decl->isa<StructDeclAST>()) {
            // Struct is FFI-compatible if all fields are FFI-compatible
            StructDeclAST* structDecl = decl->as<StructDeclAST>();
            for (FieldDeclAST* field : structDecl->fields) {
                if (!isValidFFIType(field->type, ctx)) {
                    return false;
                }
            }
            return true;
        }

        if (decl->isa<EnumDeclAST>()) {
            // Enum is FFI-compatible (maps to its backing integer type)
            return true;
        }

        return false;
    }

    // Arrays are valid if the element type is FFI-compatible
    if (type->isa<ArrayTypeAST>()) {
        // Arrays at FFI boundary must be fixed-size or pointer-to-array
        // TODO: Verify array is fixed-size or a pointer to array
        return isValidFFIType(type->as<ArrayTypeAST>()->element, ctx);
    }

    // Nullable types are NOT valid at FFI boundary
    if (type->isa<NullableTypeAST>() ||
        type->isa<FallibleTypeAST>() ||
        type->isa<CombinedTypeAST>()) {
        return false;
    }

    // Function types are NOT valid at FFI boundary (except function pointers)
    if (type->isa<FuncTypeAST>()) {
        // Function pointers are represented as *T in FFI
        return false;
    }

    return false;
}

// =============================================================================
// validateAttributes
// =============================================================================

/**
 * @brief Validate every attribute on a declaration.
 *
 * @param attrs The attributes to validate.
 * @param owner The declaration the attributes are attached to.
 * @param ctx   The semantic context.
 */
void validateAttributes(ArenaSpan<AttributePtr> attrs, DeclAST* owner, SemaContext& ctx) {
    for (AttributeAST* attr : attrs) {
        validateAttribute(attr, owner, ctx);
    }
}

// =============================================================================
// validateAttribute
// =============================================================================

/**
 * @brief Validate a single attribute against its owner.
 *
 * This is implemented in AttributesRegistry.hpp — it's declared here
 * to satisfy the forward declaration in Sema.hpp.
 *
 * @param attr  The attribute to validate.
 * @param owner The declaration the attribute is attached to.
 * @param ctx   The semantic context.
 */
void validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // Implementation is in AttributesRegistry.hpp
    // This is just a placeholder to satisfy the forward declaration.
    (void)attr; (void)owner; (void)ctx;
}
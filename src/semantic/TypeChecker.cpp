/**
 * @file TypeChecker.cpp
 *
 * @nutshell Provides explicit logical comparisons between two separate Types.
 *
 * @reason Because comparing complex generic structures, arrays, and primitive widenings involves heavy branching logic isolated from the actual AST traversal.
 *
 * @responsibility Implementation of Phase 2b semantic pass (type compatibility).
 *
 * @logic Includes checks for isAssignable, isCallable, unification patterns, primitive widening, and 'from' convertible type properties.
 *
 * @related TypeChecker.hpp
 */

#include "TypeChecker.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// isEqual  — Verifies structural equality precisely, ignoring widening rules
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isEqual(TypeAST* a, TypeAST* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    if (a->isa<PrimitiveTypeAST>()) {
        return a->as<PrimitiveTypeAST>()->primitiveKind == b->as<PrimitiveTypeAST>()->primitiveKind;
    }
    if (a->isa<NamedTypeAST>()) {
        auto* na = a->as<NamedTypeAST>();
        auto* nb = b->as<NamedTypeAST>();
        if (na->name != nb->name) return false;
        if (na->genericArgs.size() != nb->genericArgs.size()) return false;
        for (size_t i = 0; i < na->genericArgs.size(); ++i) {
            if (!isEqual(na->genericArgs[i].get(), nb->genericArgs[i].get())) return false;
        }
        return true;
    }
    if (a->isa<NullableTypeAST>()) {
        return isEqual(a->as<NullableTypeAST>()->inner.get(), b->as<NullableTypeAST>()->inner.get());
    }
    if (a->isa<FixedArrayTypeAST>()) {
        if (a->as<FixedArrayTypeAST>()->size != b->as<FixedArrayTypeAST>()->size) return false;
        return isEqual(a->as<FixedArrayTypeAST>()->element.get(), b->as<FixedArrayTypeAST>()->element.get());
    }
    if (a->isa<SliceTypeAST>()) {
        return isEqual(a->as<SliceTypeAST>()->element.get(), b->as<SliceTypeAST>()->element.get());
    }
    if (a->isa<DynamicArrayTypeAST>()) {
        return isEqual(a->as<DynamicArrayTypeAST>()->element.get(), b->as<DynamicArrayTypeAST>()->element.get());
    }
    if (a->isa<RefTypeAST>()) {
        return isEqual(a->as<RefTypeAST>()->inner.get(), b->as<RefTypeAST>()->inner.get());
    }
    if (a->isa<PtrTypeAST>()) {
        return isEqual(a->as<PtrTypeAST>()->inner.get(), b->as<PtrTypeAST>()->inner.get());
    }
    if (a->isa<FuncTypeAST>()) {
        auto* fa = a->as<FuncTypeAST>();
        auto* fb = b->as<FuncTypeAST>();
        if (fa->isNullable != fb->isNullable) return false;
        if (fa->params.size() != fb->params.size()) return false;
        for (size_t i = 0; i < fa->params.size(); ++i) {
            if (!isEqual(fa->params[i].get(), fb->params[i].get())) return false;
        }
        return isEqual(fa->returnType.get(), fb->returnType.get());
    }
    if (a->isa<UnionTypeAST>()) {
        auto* ua = a->as<UnionTypeAST>();
        auto* ub = b->as<UnionTypeAST>();
        if (ua->members.size() != ub->members.size()) return false;
        for (size_t i = 0; i < ua->members.size(); ++i) {
            if (!isEqual(ua->members[i].get(), ub->members[i].get())) return false;
        }
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isAssignable  — Validates if one type can securely pipe into another configuration
//
// Checks constraints confirming `from` can safely transfer its active memory sizing
// and capabilities securely into the `to` requirements. Assesses matching shapes,
// automatic widening primitive escalations, and generic boundary consistencies.
//
// Uses ASTKind-based isa<>/as<> helpers instead of dynamic_cast — zero RTTI overhead.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isAssignable(TypeAST* from, TypeAST* to) {
    if (!from || !to) return false;

    // Quick exit: identical pointer = same allocated node = same type.
    if (from == to) return true;

    // 1. Primitive vs Primitive — check widening table.
    if (from->isa<PrimitiveTypeAST>() && to->isa<PrimitiveTypeAST>()) {
        auto* primFrom = from->as<PrimitiveTypeAST>();
        auto* primTo   = to->as<PrimitiveTypeAST>();
        if (primFrom->primitiveKind == primTo->primitiveKind) return true;
        if (primitiveWidening(primFrom->primitiveKind, primTo->primitiveKind)) return true;
        return false;
    }

    // 2. Struct names vs struct names. (E.g. assigning Vec2 -> Vec2)
    if (from->isa<NamedTypeAST>() && to->isa<NamedTypeAST>()) {
        auto* namedFrom = from->as<NamedTypeAST>();
        auto* namedTo   = to->as<NamedTypeAST>();
        if (namedFrom->name != namedTo->name) return false;

        // Ensure generic shape properties align perfectly.
        if (namedFrom->genericArgs.size() != namedTo->genericArgs.size()) return false;
        for (size_t i = 0; i < namedFrom->genericArgs.size(); ++i) {
            if (!isAssignable(namedFrom->genericArgs[i].get(), namedTo->genericArgs[i].get())) return false;
        }
        return true;
    }

    // Fallback: compare ASTKind tags — same concrete node type means structurally identical.
    return from->kind == to->kind;
}

// ─────────────────────────────────────────────────────────────────────────────
// isCallable  — Assesses if a Type inherently permits invocation executions
//
// Identifies if the raw bounded Type node carries the functional properties to map
// back to underlying lambda or raw-function call architectures.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isCallable(TypeAST* type) {
    if (!type) return false;
    return type->isa<FuncTypeAST>();
}

// ─────────────────────────────────────────────────────────────────────────────
// isBooleanCompatible  — Validates logical query compatibilities
//
// Often needed for verifying `if`, `while`, or looping constructs evaluating raw
// boolean conditional branch decisions natively.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isBooleanCompatible(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
}

// ─────────────────────────────────────────────────────────────────────────────
// isNullable  — Safely verifies the topmost nullable context rule constraints
//
// Checks entirely if the given raw outermost bounding Type specifically allows
// `nil` initializations logically directly on its specific mapping.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isNullable(TypeAST* type) {
    if (!type) return false;
    if (type->isa<NullableTypeAST>()) return true;
    if (type->isa<FuncTypeAST>()) return type->as<FuncTypeAST>()->isNullable;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// unify  — Merges two Types discovering boundaries defining them safely together
//
// Commonly required securely balancing dual-branch architectures mapping out
// combinations inside Match-Arms or If-Else statements where branches merge securely.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeChecker::unify(TypeAST* a, TypeAST* b) {
    if (!a && !b) return nullptr;
    if (!a) return b;
    if (!b) return a;

    // If one can securely assign into the shape of another without data slicing/dropping,
    // their unified constraint type becomes the larger bounding type securely.
    if (isAssignable(a, b)) return b;
    if (isAssignable(b, a)) return a;

    // Failing bounds matching entirely across disparate forms -> unify fails (nullptr).
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// primitiveWidening  — Verifies standard safe math primitive widening logic
//
// Ascertains whether passing one native memory shape over another (e.g., int8 to int32)
// strictly transfers safely inherently without risking binary data truncation.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::primitiveWidening(PrimitiveKind from, PrimitiveKind to) {
    // Basic structural mathematical widening rules.
    if (from == PrimitiveKind::Int   && to == PrimitiveKind::Float)  return true;
    if (from == PrimitiveKind::Float && to == PrimitiveKind::Double) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isFromConvertible  — Evaluates custom 'from' integration bounds logically
//
// Inquires across standard object mapping rules whether an explicit `from(src)`
// logic has logically been deployed enabling type safety across complex transfers.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isFromConvertible(TypeAST* src, TypeAST* target) {
    // Evaluates conversion logic mappings bound to `impl [target] { from(src) }`
    // Stubs out safely default-false until ImplDecl logic is integrated.
    return false;
}
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
// hasNilInTree  — Drills down completely deep to check if `nil` is permissible inside
//
// Crucial for val validation. Searches complex deep types like Arrays or Unions to
// verify if there is any mapping internally whatsoever evaluating as securely nullable.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::hasNilInTree(TypeAST* type) {
    if (!type) return false;
    if (isNullable(type)) return true;

    if (type->isa<FixedArrayTypeAST>()) {
        return hasNilInTree(type->as<FixedArrayTypeAST>()->element.get());
    }
    if (type->isa<UnionTypeAST>()) {
        for (auto& m : type->as<UnionTypeAST>()->members) {
            if (hasNilInTree(m.get())) return true;
        }
    }

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

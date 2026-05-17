/**
 * @file TypeChecker.cpp
 * @responsibility Implements the core type compatibility checks for the semantic analyzer.
 *
 * This file contains the implementation of TypeChecker, which answers questions like:
 *   - Can type A be assigned to type B?
 *   - Are two types structurally equal?
 *   - Is a type callable (function type)?
 *   - What is the unified type of two branches?
 *   - Is a type integer, boolean, nullable, or reference‑comparable?
 *   - Is there a custom `from` conversion from source to target?
 *
 * All methods assume that the input TypeAST nodes have already been resolved
 * by TypeResolver (Phase 2a). The checker uses the StringPool for diagnostic
 * messages and the ASTArena for synthesising temporary types (e.g., slice types).
 *
 * @related
 *   - TypeChecker.hpp – class declaration
 *   - TypeResolver.cpp – type resolution (Phase 2a)
 *   - SemanticExpr.cpp, SemanticStmt.cpp – call these helpers during Phase 3
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Constructor
 *   TypeChecker::TypeChecker()           – initialises with StringPool and ASTArena
 *
 * ██ Local Helper
 *   printType()                          – debug logging of a TypeAST tree
 *
 * ██ Type Compatibility Core
 *   isEqual()                            – structural equality (ignores widening)
 *   isAssignable()                       – checks if `from` can be assigned to `to`
 *   isCallable()                         – true for FuncTypeAST
 *   unify()                              – finds a common supertype (for match/if)
 *
 * ██ Primitive Type Helpers
 *   isIntegerType()                      – true for any integer primitive
 *   primitiveWidening()                  – checks safe widening between primitives
 *   isBooleanCompatible()                – true for bool
 *   isNullable()                         – true for T? or ~nullable function
 *   isBoolOrNullable()                   – true for bool or any nullable type
 *
 * ██ Constant Evaluation
 *   getConstantIntValue()                – extracts integer value from compile‑time constant
 *   isValidArrayIndex()                  – validates array index (type & non‑negative constant)
 *   isValidSliceBound()                  – validates slice bound (must be constant & ≥0)
 *
 * ██ Custom Casting (from blocks)
 *   isFromCastable()                     – checks for a registered from‑entry conversion
 *
 * ██ Comparison Helpers
 *   isValueComparable()                  – true for == and != (primitives, enums, nullable)
 *   isReferenceComparable()              – true for === (structs, refs, nullable)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note The checker is instance‑based, holding references to StringPool and
 *       ASTArena. This makes dependencies explicit and avoids hidden global
 *       state. All methods are non‑static.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "ast/ExprAST.hpp"
#include "header/TypeChecker.hpp"
#include "header/NameMangler.hpp"
#include "debug/DebugUtils.hpp"

#include <iostream>
#include <cstdlib> 
#include <cerrno>

TypeChecker::TypeChecker(StringPool& pool, ASTArena& arena)
    : pool_(pool), arena_(arena) {
    LUC_LOG_SEMANTIC("TypeChecker constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// Local helper to print a TypeAST for debug logging
// ─────────────────────────────────────────────────────────────────────────────

static void printType(const std::string& label, TypeAST* t, const StringPool& pool, int indent = 0) {
    if (!t) {
        LUC_LOG_SEMANTIC_EXTREME(std::string(indent, ' ') << label << " = nullptr");
        return;
    } 

    std::string indentStr(indent, ' ');

    switch (t->kind) {
        case ASTKind::PrimitiveType: {
            auto* p = t->as<PrimitiveTypeAST>();
            std::string typeName;
            switch (p->primitiveKind) {
                case PrimitiveKind::Bool:   typeName = "bool"; break;
                case PrimitiveKind::Int:    typeName = "int"; break;
                case PrimitiveKind::Float:  typeName = "float"; break;
                case PrimitiveKind::Double: typeName = "double"; break;
                case PrimitiveKind::String: typeName = "string"; break;
                case PrimitiveKind::Uint8:  typeName = "uint8"; break;
                case PrimitiveKind::Uint64: typeName = "uint64"; break;
                case PrimitiveKind::Any:    typeName = "any"; break;
                default: typeName = "other(" + std::to_string(static_cast<int>(p->primitiveKind)) + ")";
            }
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = PrimitiveType(" << typeName << ")");
            break;
        }
        case ASTKind::NamedType: {
            auto* n = t->as<NamedTypeAST>();
            std::string msg = indentStr + label + " = NamedType(" + std::string(pool.lookup(n->name)) + ")";
            if (n->isGenericParam) msg += " [generic param]";
            LUC_LOG_SEMANTIC_EXTREME(msg);
            break;
        }
        case ASTKind::NullableType: {
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = NullableType");
            printType("  inner", t->as<NullableTypeAST>()->inner.get(), pool, indent + 2);
            break;
        }
        case ASTKind::PtrType: {
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = PtrType");
            printType("  inner", t->as<PtrTypeAST>()->inner.get(), pool, indent + 2);
            break;
        }
        case ASTKind::FuncType: {
            auto* f = t->as<FuncTypeAST>();
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = FuncType");
            LUC_LOG_SEMANTIC_EXTREME(indentStr << "  async=" << f->sig.isAsync()
                                    << ", parallel=" << f->sig.isParallel()
                                    << ", nullable=" << f->sig.isNullable());
            break;
        }
        default:
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = " << LucDebug::kindToString(t->kind));
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Compatibility Core
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// isEqual  —  Structural equality between two types (ignores widening).
//
// Returns true if two types are exactly the same, including all generic
// arguments and nested types. No implicit conversions (e.g., widening,
// nullable wrapping) are applied – this is a pure structural comparison.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used when the language requires exact type matching, such as:
//   - Function type equality (parameter and return types must match exactly).
//   - Generic argument matching (e.g., `Box<int>` vs `Box<int>`).
//   - Type alias resolution (after unwrapping, the aliased type must match).
//   - Pattern type checks (e.g., `s is Circle` – runtime type must be `Circle`).
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   PrimitiveTypeAST      → compares primitiveKind (Bool, Int, Float, etc.).
//   NamedTypeAST          → compares name and all genericArgs recursively.
//   NullableTypeAST       → compares inner type.
//   FixedArrayTypeAST     → compares size and element type.
//   SliceTypeAST          → compares element type.
//   DynamicArrayTypeAST   → compares element type.
//   RefTypeAST            → compares inner type (reference).
//   PtrTypeAST            → compares inner type (raw pointer).
//   FuncTypeAST           → compares:
//        - qualifiers that affect type equality (via equalityMask)
//        - nullability (function itself nullable)
//        - number of parameter groups (curry levels)
//        - each parameter’s type (ignoring parameter names)
//        - number of return types and each return type.
//   All other kinds       → false (unknown type).
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Widening conversions (e.g., int → float). Use isAssignable for that.
//   - Nullable wrapping (T → T?). isAssignable handles it.
//   - from‑block conversions.
//   - Generic parameter substitution (abstract T).
//
// ─── Complexity ──────────────────────────────────────────────────────────────
//   O(N) where N is the total number of nodes in the type trees.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isEqual(TypeAST* a, TypeAST* b) {
    LUC_LOG_SEMANTIC_VERBOSE("isEqual: checking structural equality");
    
    if (a == b) {
        LUC_LOG_SEMANTIC_EXTREME("\tsame pointer -> true");
        return true;
    }
    if (!a || !b) {
        LUC_LOG_SEMANTIC_EXTREME("\tnull pointer -> false");
        return false;
    }
    if (a->kind != b->kind) {
        LUC_LOG_SEMANTIC_VERBOSE("\tkind mismatch: " << static_cast<int>(a->kind) 
                            << " vs " << static_cast<int>(b->kind) << " -> false");
        return false;
    }

    bool result = false;
    
    if (a->isa<PrimitiveTypeAST>()) {
        result = a->as<PrimitiveTypeAST>()->primitiveKind == b->as<PrimitiveTypeAST>()->primitiveKind;
        LUC_LOG_SEMANTIC_VERBOSE("\tPrimitiveType: " << (result ? "true" : "false"));
        return result;
    }
    if (a->isa<NamedTypeAST>()) {
        auto* na = a->as<NamedTypeAST>();
        auto* nb = b->as<NamedTypeAST>();
        if (na->name != nb->name) {
            LUC_LOG_SEMANTIC_VERBOSE("\tNamedType name mismatch: " << na->name.id << " vs " << nb->name.id << " -> false");
            return false;
        }
        if (na->genericArgs.size() != nb->genericArgs.size()) {
            LUC_LOG_SEMANTIC_VERBOSE("\tNamedType generic args count mismatch -> false");
            return false;
        }
        for (size_t i = 0; i < na->genericArgs.size(); ++i) {
            if (!isEqual(na->genericArgs[i].get(), nb->genericArgs[i].get())) {
                LUC_LOG_SEMANTIC_VERBOSE("\tNamedType generic arg " << i << " mismatch -> false");
                return false;
            }
        }
        LUC_LOG_SEMANTIC_VERBOSE("\tNamedType " << na->name.id << " -> true");
        return true;
    }
    if (a->isa<NullableTypeAST>()) {
        result = isEqual(a->as<NullableTypeAST>()->inner.get(), b->as<NullableTypeAST>()->inner.get());
        LUC_LOG_SEMANTIC_VERBOSE("\tNullableType: " << (result ? "true" : "false"));
        return result;
    }
    if (a->isa<FixedArrayTypeAST>()) {
        if (a->as<FixedArrayTypeAST>()->size != b->as<FixedArrayTypeAST>()->size) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFixedArray size mismatch -> false");
            return false;
        }
        result = isEqual(a->as<FixedArrayTypeAST>()->element.get(), b->as<FixedArrayTypeAST>()->element.get());
        LUC_LOG_SEMANTIC_VERBOSE("\tFixedArrayType: " << (result ? "true" : "false"));
        return result;
    }
    if (a->isa<SliceTypeAST>()) {
        result = isEqual(a->as<SliceTypeAST>()->element.get(), b->as<SliceTypeAST>()->element.get());
        LUC_LOG_SEMANTIC_VERBOSE("\tSliceType: " << (result ? "true" : "false"));
        return result;
    }
    if (a->isa<DynamicArrayTypeAST>()) {
        result = isEqual(a->as<DynamicArrayTypeAST>()->element.get(), b->as<DynamicArrayTypeAST>()->element.get());
        LUC_LOG_SEMANTIC_VERBOSE("\tDynamicArrayType: " << (result ? "true" : "false"));
        return result;
    }
    if (a->isa<RefTypeAST>()) {
        result = isEqual(a->as<RefTypeAST>()->inner.get(), b->as<RefTypeAST>()->inner.get());
        LUC_LOG_SEMANTIC_VERBOSE("\tRefType: " << (result ? "true" : "false"));
        return result;
    }
    if (a->isa<PtrTypeAST>()) {
        result = isEqual(a->as<PtrTypeAST>()->inner.get(), b->as<PtrTypeAST>()->inner.get());
        LUC_LOG_SEMANTIC_VERBOSE("\tPtrType: " << (result ? "true" : "false"));
        return result;
    }
    if (a->isa<FuncTypeAST>()) {
        auto* fa = a->as<FuncTypeAST>();
        auto* fb = b->as<FuncTypeAST>();
        
        // Compare qualifiers that affect type equality
        uint32_t equalityMask = QualifierRegistry::instance().equalityMask();
        if ((fa->sig.qualifiers & equalityMask) != (fb->sig.qualifiers & equalityMask)) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFuncType qualifier mismatch -> false");
            return false;
        }
        
        // Compare nullability (function itself nullable via qualifier)
        if (fa->sig.isNullable() != fb->sig.isNullable()) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFuncType nullability mismatch -> false");
            return false;
        }
        
        // Compare parameter group count (curry levels)
        if (fa->sig.paramGroups.size() != fb->sig.paramGroups.size()) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFuncType param group count mismatch -> false");
            return false;
        }
        
        // Compare each parameter group
        for (size_t g = 0; g < fa->sig.paramGroups.size(); ++g) {
            const auto& groupA = fa->sig.paramGroups[g];
            const auto& groupB = fb->sig.paramGroups[g];
            
            if (groupA.size() != groupB.size()) {
                LUC_LOG_SEMANTIC_VERBOSE("\tFuncType param group " << g << " size mismatch -> false");
                return false;
            }
            
            for (size_t i = 0; i < groupA.size(); ++i) {
                // Compare parameter types (ignoring parameter names)
                if (!isEqual(groupA[i]->type.get(), groupB[i]->type.get())) {
                    LUC_LOG_SEMANTIC_VERBOSE("\tFuncType param " << i << " in group " << g << " mismatch -> false");
                    return false;
                }
            }
        }
        
        // Compare return types
        if (fa->sig.returnTypes.size() != fb->sig.returnTypes.size()) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFuncType return count mismatch -> false");
            return false;
        }
        for (size_t i = 0; i < fa->sig.returnTypes.size(); ++i) {
            if (!isEqual(fa->sig.returnTypes[i].get(), fb->sig.returnTypes[i].get())) {
                LUC_LOG_SEMANTIC_VERBOSE("\tFuncType return " << i << " mismatch -> false");
                return false;
            }
        }
        
        LUC_LOG_SEMANTIC_VERBOSE("\tFuncType: true");
        return true;
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("\tUnknown type kind -> false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isAssignable  —  Checks if a value of type `from` can be assigned to a
//                  location of type `to`.
//
// This is the central type compatibility predicate for assignments, argument
// passing, return statements, and binary operator operands. It allows implicit
// conversions that are safe and defined by the language.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in:
//   - Variable initialisation (let x T = expr).
//   - Assignment statements (x = expr).
//   - Function arguments (call f(expr) where f expects T).
//   - Return statements (return expr; expr must match function's return type).
//   - Binary operators (e.g., + requires both operands to be assignable to
//     a common numeric type after widening).
//   - from‑block conversions (checked separately, but called from here).
//
// ─── Cases Covered (in order of checking) ────────────────────────────────────
//   0.1  nil assignment → true iff `to` is nullable.
//   0.2  target is `any` → true (boxing).
//   0.3  implicit nullable wrapping: T → T?  → checks inner type.
//   0.4  nullable to nullable: T? → T?      → checks inner types.
//   1.   same pointer → true.
//   2.   primitive → primitive:
//           - same primitive → true.
//           - primitiveWidening(from, to) → true (int→float, int→long, etc.).
//   3.   NamedType → NamedType:
//           - names match.
//           - generic arg counts match.
//           - each generic arg is assignable (recursive).
//   4.   FuncType → FuncType: uses isEqual (exact structural match).
//   5.   fallback: isEqual (all other types, e.g., arrays, refs, pointers).
//
// ─── What is NOT covered (reported as error by caller) ───────────────────────
//   - Struct types without a from‑block conversion (use === for reference
//     equality or implement Equatable).
//   - Function types with mismatched qualifiers (e.g., async vs sync).
//   - Implicit `from` conversion (handled separately in isFromCastable).
//
// ─── Order Matters ───────────────────────────────────────────────────────────
//   The function applies checks in a specific order to give the most
//   intuitive behaviour:
//     1. nil → nullable (special case)
//     2. any boxing (widest)
//     3. nullable wrapping (T → T?)
//     4. nullable to nullable
//     5. exact pointer equality (fast path)
//     6. primitive widening
//     7. named types (structs, enums, aliases)
//     8. function types
//     9. structural equality for remaining types.
//
// ─── Design Note ─────────────────────────────────────────────────────────────
//   This function does NOT handle from‑block conversions itself. The caller
//   (e.g., assignment checker) should call isFromCastable if isAssignable
//   returns false. That separation keeps the logic clean and allows custom
//   conversion search without polluting the core assignability rules.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isAssignable(TypeAST* from, TypeAST* to) {
    printType("from", from, pool_);
    printType("to", to, pool_);

    // 0.1 Handle nil assignment
    if (!from) {
        bool result = isNullable(to);
        LUC_LOG_SEMANTIC("\tnil assignment: " << (result ? "true" : "false"));
        return result;
    }
    if (!to) {
        LUC_LOG_SEMANTIC("\tto is null -> false");
        return false;
    }

    // 0.2 Handle target 'any' (boxing)
    if (to->isa<PrimitiveTypeAST>() && to->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Any) {
        LUC_LOG_SEMANTIC("\ttarget is 'any' -> true (boxing)");
        return true;
    }

    // 0.3 Handle implicit wrapping into nullable (T -> T?)
    if (to->isa<NullableTypeAST>() && !from->isa<NullableTypeAST>()) {
        LUC_LOG_SEMANTIC_VERBOSE("\timplicit nullable wrapping (T -> T?)");
        bool result = isAssignable(from, to->as<NullableTypeAST>()->inner.get());
        LUC_LOG_SEMANTIC("\tnullable wrapping result: " << (result ? "true" : "false"));
        return result;
    }

    // 0.4 Handle nullable to nullable (T? -> T?)
    if (from->isa<NullableTypeAST>() && to->isa<NullableTypeAST>()) {
        LUC_LOG_SEMANTIC_VERBOSE("\tnullable to nullable (T? -> T?)");
        bool result = isAssignable(from->as<NullableTypeAST>()->inner.get(), to->as<NullableTypeAST>()->inner.get());
        LUC_LOG_SEMANTIC("\tnullable comparison result: " << (result ? "true" : "false"));
        return result;
    }

    // Quick exit: identical pointer = same allocated node = same type.
    if (from == to) {
        LUC_LOG_SEMANTIC("\tsame pointer -> true");
        return true;
    }

    // 1. Primitive vs Primitive — check widening table.
    if (from->isa<PrimitiveTypeAST>() && to->isa<PrimitiveTypeAST>()) {
        auto* primFrom = from->as<PrimitiveTypeAST>();
        auto* primTo   = to->as<PrimitiveTypeAST>();
        
        if (primFrom->primitiveKind == primTo->primitiveKind) {
            LUC_LOG_SEMANTIC("\tsame primitive type -> true");
            return true;
        }
        
        bool widening = primitiveWidening(primFrom->primitiveKind, primTo->primitiveKind);
        LUC_LOG_SEMANTIC("\tprimitive widening check: " << (widening ? "true" : "false"));
        return widening;
    }

    // 2. Struct names vs struct names.
    if (from->isa<NamedTypeAST>() && to->isa<NamedTypeAST>()) {
        auto* namedFrom = from->as<NamedTypeAST>();
        auto* namedTo   = to->as<NamedTypeAST>();
        
        if (namedFrom->name != namedTo->name) {
            LUC_LOG_SEMANTIC("\tNamedType name mismatch: " << namedFrom->name.id << " vs " << namedTo->name.id << " -> false");
            return false;
        }

        if (namedFrom->genericArgs.size() != namedTo->genericArgs.size()) {
            LUC_LOG_SEMANTIC("\tNamedType generic args count mismatch -> false");
            return false;
        }
        
        for (size_t i = 0; i < namedFrom->genericArgs.size(); ++i) {
            if (!isAssignable(namedFrom->genericArgs[i].get(), namedTo->genericArgs[i].get())) {
                LUC_LOG_SEMANTIC("\tNamedType generic arg " << i << " not assignable -> false");
                return false;
            }
        }
        
        LUC_LOG_SEMANTIC("\tNamedType " << namedFrom->name.id << " -> true");
        return true;
    }

    // 3. Function types.
    if (from->isa<FuncTypeAST>() && to->isa<FuncTypeAST>()) {
        bool result = isEqual(from, to);
        LUC_LOG_SEMANTIC("\tFuncType: " << (result ? "true" : "false"));
        return result;
    }

    // Fallback: use structural equality for all other types
    bool result = isEqual(from, to);
    LUC_LOG_SEMANTIC("\tfallback structural equality: " << (result ? "true" : "false"));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// areAssignableMultiple  —  Checks if a list of source types can be assigned
//                           to a list of target types element‑wise.
//
// This is the core helper for multi‑return assignment and multi‑variable
// declaration. It verifies that the number of sources equals the number of
// targets and that each individual source is assignable to its corresponding
// target (using isAssignable). The vectors must be in the same order.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used during checking of:
//   - Multi‑variable declaration (let a int, b string = f()) – where the RHS
//     must return as many values as there are variables.
//   - Multi‑assignment (a, b = g()) – where the RHS must return as many
//     values as there are LHS expressions.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. If the vector sizes differ → return false.
//   2. For each index i:
//        - If fromTypes[i] is not assignable to toTypes[i] → return false.
//   3. Otherwise → return true.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - Same length, each source assignable to corresponding target.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - Different vector lengths.
//   - Any individual source‑target pair fails isAssignable.
//   - RHS not being a function call (caller must enforce that separately).
//
// ─── Dependencies ────────────────────────────────────────────────────────────
//   Uses isAssignable for element‑wise compatibility.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::areAssignableMultiple(const std::vector<TypeAST*>& fromTypes,
                                        const std::vector<TypeAST*>& toTypes) {
    if (fromTypes.size() != toTypes.size()) return false;
    for (size_t i = 0; i < fromTypes.size(); ++i) {
        if (!isAssignable(fromTypes[i], toTypes[i])) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// isCallable  —  Determines whether a type can be invoked as a function.
//
// Returns true if the type is a FuncTypeAST (function type). This includes
// regular functions, anonymous functions, and function references.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in semantic checks for:
//   - Call expressions: the callee must be callable.
//   - Pipeline steps: each step must be callable.
//   - Composition operands: each operand must be a function (callable).
//   - Assignment of function values: e.g., `let f: (int) -> int = someFunc`.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - FuncTypeAST → true (any function type, regardless of qualifiers).
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - PrimitiveTypeAST → false.
//   - NamedTypeAST → false (even if it aliases a function type – the alias
//        is resolved to a FuncTypeAST before this check).
//   - NullableTypeAST → false (nullable function types are still callable
//        after a nil check, but the type itself is not directly callable).
//   - All other types (arrays, structs, refs, ptrs) → false.
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   This function does not check nullability. For a nullable function type,
//   the caller must first check for nil (e.g., `if f != nil { f() }`).
//   The semantic pass enforces that rule.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isCallable(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isCallable: type is null -> false");
        return false;
    }
    
    bool result = type->isa<FuncTypeAST>();
    LUC_LOG_SEMANTIC_EXTREME("isCallable: " << LucDebug::kindToString(type->kind) 
                        << " -> " << (result ? "true" : "false"));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// unify  —  Finds a common supertype between two types for branch merging.
//
// Given two types from different branches of an `if` expression, a `match`
// expression, or a ternary‑like construct, returns a type that is assignable
// from both inputs. If no common type exists, returns nullptr.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used to compute the result type of:
//   - If expressions (`if cond ?? expr1 else expr2`) – the then and else
//     branches must unify to a single type.
//   - Match expressions – all arms must unify to a common type.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. If both types are null → return nullptr.
//   2. If one type is null, return the other.
//   3. Check if `a` is assignable to `b` → return `b`.
//   4. Check if `b` is assignable to `a` → return `a`.
//   5. Otherwise → cannot unify, return nullptr.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Same type → unifies to that type.
//   - Primitive widening (int → float) → returns float (the wider type).
//   - Nullable wrapping (T → T?) → returns T? (the nullable type).
//   - Nullable to non‑nullable (T? → T) – only if T? is assignable to T,
//     which is generally false unless the nullable is known non‑nil (not
//     allowed in unification). Actually unification never returns the
//     non‑nullable side because assignability is one‑way.
//   - Struct type with common ancestor (not currently supported; returns
//     nullptr unless one is assignable to the other).
//   - Function types with identical signatures → returns that type.
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Union types – Luc does not have them due to the danger of 
//     'union of function type', unification fails for unrelated
//     types (e.g., `int` and `string`).
//   - `any` type – `any` is assignable from any type, but unification does
//     not automatically produce `any` unless one branch is already `any`.
//   - Custom `from` conversions – unification does not search for conversions.
//     It only uses direct assignability.
//   - Generic type parameter substitution – unification assumes concrete types.
//
// ─── Return Value ───────────────────────────────────────────────────────────
//   Returns a pointer to either `a` or `b` (no new type is allocated). The
//   caller must not assume ownership; the returned pointer is one of the inputs.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeChecker::unify(TypeAST* a, TypeAST* b) {
    LUC_LOG_SEMANTIC_VERBOSE("unify: trying to unify types");
    printType("  a", a, pool_);
    printType("  b", b, pool_);
    
    if (!a && !b) {
        LUC_LOG_SEMANTIC_VERBOSE("unify: both null -> nullptr");
        return nullptr;
    }
    if (!a) {
        LUC_LOG_SEMANTIC_VERBOSE("unify: a is null, returning b");
        return b;
    }
    if (!b) {
        LUC_LOG_SEMANTIC_VERBOSE("unify: b is null, returning a");
        return a;
    }

    if (isAssignable(a, b)) {
        LUC_LOG_SEMANTIC_VERBOSE("unify: a assignable to b, returning b");
        return b;
    }
    if (isAssignable(b, a)) {
        LUC_LOG_SEMANTIC_VERBOSE("unify: b assignable to a, returning a");
        return a;
    }

    LUC_LOG_SEMANTIC_VERBOSE("unify: cannot unify, returning nullptr");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Primitive Type Helpers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// isIntegerType  —  Returns true for any integer primitive type.
//
// Identifies signed and unsigned integer types, including both generic
// (int, byte, short, long) and fixed‑width (int8, uint64, etc.) variants.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used to validate operands for:
//   - Array indexing: the index expression must be integer.
//   - Slice bounds: start and end must be integer.
//   - Bitwise operators (&&, ||, ~^, <<, >>): operands must be integers.
//   - Shift operators: both operands must be integers.
//   - Modulo operator (%): both operands must be integers.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   Signed:   byte, short, int, long, int8, int16, int32, int64.
//   Unsigned: ubyte, ushort, uint, ulong, uint8, uint16, uint32, uint64.
//
// ─── Cases Covered (returns false) ───────────────────────────────────────────
//   float, double, decimal, bool, string, char, any.
//   All non‑primitive types (structs, enums, arrays, functions, references, etc.).
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   Enum types are NOT considered integer types, even though they are backed
//   by integers. Enum‑to‑integer conversion requires an explicit cast
//   (e.g., `int(direction)`).
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isIntegerType(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isIntegerType: type is null -> false");
        return false;
    }
    
    if (!type->isa<PrimitiveTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isIntegerType: not primitive -> false");
        return false;
    }
    
    auto* prim = type->as<PrimitiveTypeAST>();
    switch (prim->primitiveKind) {
        case PrimitiveKind::Byte:
        case PrimitiveKind::Short:
        case PrimitiveKind::Int:
        case PrimitiveKind::Long:
        case PrimitiveKind::Int8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int64:
        case PrimitiveKind::Ubyte:
        case PrimitiveKind::Ushort:
        case PrimitiveKind::Uint:
        case PrimitiveKind::Ulong:
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint64:
            LUC_LOG_SEMANTIC_EXTREME("isIntegerType: true");
            return true;
        default:
            LUC_LOG_SEMANTIC_EXTREME("isIntegerType: false");
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// primitiveWidening  —  Determines if a primitive type can be implicitly
//                       widened to another primitive type.
//
// Returns true if values of `from` can be safely converted to `to` without
// loss of magnitude (though precision may be lost in integer→float conversions).
// This is the core rule for implicit numeric conversions in Luc.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in:
//   - Assignment of primitive values (e.g., `let x float = 5` where 5 is int).
//   - Binary arithmetic operators (`+`, `-`, `*`, `/`, `%`, `^`) – operands
//     are widened to a common type before the operation.
//   - Return statements where the returned expression is a narrower integer
//     than the declared return type (e.g., `-> int64` and `return 42`).
//
// ─── Widening Rules (non‑exhaustive) ─────────────────────────────────────────
//   Signed integer chain:
//     byte/int8 → short/int16 → int/int32 → long/int64 → float → double → decimal
//   Unsigned integer chain:
//     ubyte/uint8 → ushort/uint16 → uint/uint32 → ulong/uint64 → float → double → decimal
//   Signed to unsigned (allowed for positive literals, but not enforced here):
//     int* → uint* (the semantic pass may restrict based on sign of value)
//   Floating‑point widening:
//     float → double → decimal
//   Integer to floating‑point:
//     any integer (signed or unsigned) → float/double/decimal
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - Same type → always true (trivial).
//   - Any widening along the chains described above → true.
//   - Signed integer to larger signed integer → true.
//   - Unsigned integer to larger unsigned integer → true.
//   - Signed or unsigned integer to any floating‑point type → true.
//   - float to double or decimal → true.
//   - double to decimal → true.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - Narrowing conversions (e.g., long → int, double → float) → false.
//   - Unsigned to signed (except via integer→float) → generally false because
//     the full range of unsigned may not fit in signed. The semantic pass may
//     allow it for constants but primitiveWidening returns false.
//   - Floating‑point to integer of any kind → false.
//   - Boolean to/from any numeric type → false.
//   - String or char to any numeric type → false.
//   - Enum to underlying integer → false (requires explicit cast).
//   - Non‑primitive types (structs, arrays, etc.) → not applicable.
//
// ─── Design Note ────────────────────────────────────────────────────────────
//   This function only checks type compatibility, not the actual runtime
//   value. For example, `int` to `uint8` is never considered widening,
//   even if the value is 5. The programmer must use an explicit cast
//   (`uint8(x)`) to acknowledge potential overflow.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::primitiveWidening(PrimitiveKind from, PrimitiveKind to) {
    LUC_LOG_SEMANTIC_VERBOSE("primitiveWidening: from=" << static_cast<int>(from) 
                           << " to=" << static_cast<int>(to));
    
    if (from == to) {
        LUC_LOG_SEMANTIC_VERBOSE("\tidentical types -> true");
        return true;
    }

    // Signed integer widening
    if (from == PrimitiveKind::Byte || from == PrimitiveKind::Int8) {
        switch (to) {
            case PrimitiveKind::Short:
            case PrimitiveKind::Int16:
            case PrimitiveKind::Int:
            case PrimitiveKind::Int32:
            case PrimitiveKind::Long:
            case PrimitiveKind::Int64:
                LUC_LOG_SEMANTIC_VERBOSE("\tint8 widening -> true");
                return true;
            default: break;
        }
    }

    if (from == PrimitiveKind::Short || from == PrimitiveKind::Int16) {
        switch (to) {
            case PrimitiveKind::Int:
            case PrimitiveKind::Int32:
            case PrimitiveKind::Long:
            case PrimitiveKind::Int64:
                LUC_LOG_SEMANTIC_VERBOSE("\tint16 widening -> true");
                return true;
            default: break;
        }
    }

    if (from == PrimitiveKind::Int || from == PrimitiveKind::Int32) {
        switch (to) {
            case PrimitiveKind::Long:
            case PrimitiveKind::Int64:
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                LUC_LOG_SEMANTIC_VERBOSE("\tint32 widening -> true");
                return true;
            default: break;
        }
    }

    if (from == PrimitiveKind::Long || from == PrimitiveKind::Int64) {
        switch (to) {
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                LUC_LOG_SEMANTIC_VERBOSE("\tint64 widening -> true");
                return true;
            default: break;
        }
    }

    // Unsigned integer widening
    if (from == PrimitiveKind::Ubyte || from == PrimitiveKind::Uint8) {
        switch (to) {
            case PrimitiveKind::Ushort:
            case PrimitiveKind::Uint16:
            case PrimitiveKind::Uint:
            case PrimitiveKind::Uint32:
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint64:
                LUC_LOG_SEMANTIC_VERBOSE("\tuint8 widening -> true");
                return true;
            default: break;
        }
    }

    if (from == PrimitiveKind::Ushort || from == PrimitiveKind::Uint16) {
        switch (to) {
            case PrimitiveKind::Uint:
            case PrimitiveKind::Uint32:
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint64:
                LUC_LOG_SEMANTIC_VERBOSE("\tuint16 widening -> true");
                return true;
            default: break;
        }
    }

    if (from == PrimitiveKind::Uint || from == PrimitiveKind::Uint32) {
        switch (to) {
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint64:
                LUC_LOG_SEMANTIC_VERBOSE("\tuint32 widening -> true");
                return true;
            default: break;
        }
    }

    // Signed integer to unsigned integer (for positive literals)
    if (from == PrimitiveKind::Byte || from == PrimitiveKind::Int8 ||
        from == PrimitiveKind::Short || from == PrimitiveKind::Int16 ||
        from == PrimitiveKind::Int || from == PrimitiveKind::Int32 ||
        from == PrimitiveKind::Long || from == PrimitiveKind::Int64) {
        switch (to) {
            case PrimitiveKind::Ubyte:
            case PrimitiveKind::Uint8:
            case PrimitiveKind::Ushort:
            case PrimitiveKind::Uint16:
            case PrimitiveKind::Uint:
            case PrimitiveKind::Uint32:
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint64:
                LUC_LOG_SEMANTIC_VERBOSE("\tsigned int to unsigned widening -> true");
                return true;
            default: break;
        }
    }

    // Floating-point widening
    if (from == PrimitiveKind::Float) {
        switch (to) {
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                LUC_LOG_SEMANTIC_VERBOSE("\tfloat widening -> true");
                return true;
            default: break;
        }
    }

    if (from == PrimitiveKind::Double) {
        if (to == PrimitiveKind::Decimal) {
            LUC_LOG_SEMANTIC_VERBOSE("\tdouble widening -> true");
            return true;
        }
    }

    // Signed integer to floating-point
    if (from == PrimitiveKind::Byte || from == PrimitiveKind::Int8 ||
        from == PrimitiveKind::Short || from == PrimitiveKind::Int16 ||
        from == PrimitiveKind::Int || from == PrimitiveKind::Int32 ||
        from == PrimitiveKind::Long || from == PrimitiveKind::Int64) {
        switch (to) {
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                LUC_LOG_SEMANTIC_VERBOSE("\tsigned int to float widening -> true");
                return true;
            default: break;
        }
    }

    // Unsigned integer to floating-point
    if (from == PrimitiveKind::Ubyte || from == PrimitiveKind::Uint8 ||
        from == PrimitiveKind::Ushort || from == PrimitiveKind::Uint16 ||
        from == PrimitiveKind::Uint || from == PrimitiveKind::Uint32 ||
        from == PrimitiveKind::Ulong || from == PrimitiveKind::Uint64) {
        switch (to) {
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                LUC_LOG_SEMANTIC_VERBOSE("\tunsigned int to float widening -> true");
                return true;
            default: break;
        }
    }

    LUC_LOG_SEMANTIC_VERBOSE("\tno widening path found -> false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isBooleanCompatible  —  Checks if a type is valid in a boolean context.
//
// Returns true only for the primitive `bool` type. This is used in conditions
// for `if`, `while`, and the logical operators `not`, `and`, `or` (though
// those also accept nullable types – see isBoolOrNullable).
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Enforces that condition expressions in control‑flow statements evaluate
// to a boolean value. The language does not implicitly convert integers,
// pointers, or other types to bool (unlike C). Explicit comparison is required.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - PrimitiveTypeAST with primitiveKind == Bool.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - Any nullable type (T?) – use isBoolOrNullable for logical operators.
//   - Integer types (int, byte, etc.).
//   - Floating‑point types (float, double, decimal).
//   - String, char, any.
//   - Structs, enums, arrays, functions, references, pointers.
//   - Generic type parameters (even if constrained to bool).
//
// ─── Distinction from isBoolOrNullable ───────────────────────────────────────
//   - isBooleanCompatible: strict `bool` only – for if/while conditions.
//   - isBoolOrNullable: `bool` or any nullable type – for `and`/`or`/`not`
//     where `nil` is treated as false and non‑nil as true.
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   Enum variants are not implicitly convertible to bool. Use `x != 0` or
//   an explicit comparison.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isBooleanCompatible(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isBooleanCompatible: type is null -> false");
        return false;
    }
    
    if (!type->isa<PrimitiveTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isBooleanCompatible: not primitive -> false");
        return false;
    }
    
    bool result = type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
    LUC_LOG_SEMANTIC_EXTREME("isBooleanCompatible: " << (result ? "true" : "false"));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// isNullable  —  Checks whether a type can hold a `nil` value.
//
// Returns true if the type is explicitly nullable (marked with `?`) or is a
// function type with the `~nullable` qualifier. Used to validate assignments
// of `nil` and operations that require nullability.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in:
//   - Assignment checking: `nil` can only be assigned to nullable types.
//   - Nullable chain operations (`?.`): the left‑hand side must be nullable.
//   - Null coalescing (`??`): the left‑hand side must be nullable.
//   - Equality comparisons: `==` is allowed on nullable types.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - NullableTypeAST (any inner type) → true.
//   - FuncTypeAST where the `~nullable` qualifier is present → true.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - Primitive types (int, float, bool, etc.) – they are never nullable.
//   - Named types (structs, enums) without `?`.
//   - Array types (FixedArray, Slice, DynamicArray) – slices can be empty,
//     but `nil` is not a valid value; use empty slice literal `[]T`.
//   - Reference types (&T) without `?` – they are non‑nullable by default.
//   - Pointer types (*T) – raw pointers can be `nil`, but the semantic pass
//     treats them specially; this function returns false for PtrTypeAST
//     because raw pointers are a separate sealed conduit model.
//   - Function types without `~nullable`.
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   Nullability is a property of the type wrapper, not of the inner type.
//   For example, `int?` is nullable, but `int` is not. This function only
//   examines the outermost type node.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isNullable(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isNullable: type is null -> false");
        return false;
    }
    
    if (type->isa<NullableTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isNullable: NullableType -> true");
        return true;
    }
    if (type->isa<FuncTypeAST>()) {
        bool result = type->as<FuncTypeAST>()->sig.isNullable();
        LUC_LOG_SEMANTIC_EXTREME("isNullable: FuncType nullable=" << result);
        return result;
    }
    
    LUC_LOG_SEMANTIC_EXTREME("isNullable: false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isBoolOrNullable  —  Returns true when a type is valid for the logical
//                      operators `not`, `and`, `or`.
//
// The logical operators operate on `bool` values directly. However, Luc also
// allows nullable types (T?) in logical contexts: `nil` is treated as false,
// and any non‑nil value is treated as true. This function centralises that rule.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in semantic checking of:
//   - Unary `not` operator – operand must be bool or nullable.
//   - Binary `and` and `or` operators – both operands must be bool or nullable.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - PrimitiveTypeAST with primitiveKind == Bool.
//   - Any NullableTypeAST (T?, regardless of inner type).
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - Non‑nullable integer, float, string, char, any.
//   - Structs, enums, arrays, functions, references, pointers.
//   - Generic type parameters (even if instantiated with bool or nullable).
//   - NamedTypeAST aliasing bool (the alias is resolved to PrimitiveTypeAST
//     before this function is called).
//
// ─── Distinction from isBooleanCompatible ────────────────────────────────────
//   - isBooleanCompatible: strict `bool` only – used for `if`/`while`
//     conditions where implicit treatment of nil would be error‑prone.
//   - isBoolOrNullable: allows nullable types – used for logical operators
//     where `nil` is a natural false value (e.g., `if maybeValue and isValid`).
//
// ─── Semantics ───────────────────────────────────────────────────────────────
//   For a nullable type `T?`, the operators behave as if the value were
//   implicitly converted to bool: `nil` → false, any non‑nil → true.
//   The expression does not unwrap the nullable; it just tests presence.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isBoolOrNullable(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: type is null -> false");
        return false;
    }

    if (type->isa<PrimitiveTypeAST>()) {
        bool result = type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
        LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: bool -> " << (result ? "true" : "false"));
        return result;
    }

    if (type->isa<NullableTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: NullableType -> true");
        return true;
    }

    LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constant Evaluation
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// getConstantIntValue  —  Extracts an integer value from a compile‑time constant
//                         expression.
//
// Evaluates an expression and, if it is a constant integer literal or a
// constant arithmetic expression (unary negation or binary +,-,*,/,%),
// returns the computed value. Supports decimal, hexadecimal, and binary
// literals with optional underscore separators.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used to validate compile‑time constraints such as:
//   - Array index bounds: negative indices are rejected at compile time.
//   - Slice bounds: must be constant (non‑negative) for slice expressions.
//   - Fixed array sizes (in type annotations) – handled by parser/type resolver.
//   - Enum variant explicit values (parse time, not semantic).
//   - Constant folding for optimisations (future use).
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Literal integers (decimal, hex, binary) → returns the integer value.
//   - Literals with underscores (e.g., 1_000_000) → underscores stripped.
//   - Unary negation of a constant (e.g., -5) → returns negative value.
//   - Binary arithmetic on constants (+, -, *, /, %) → evaluates and returns.
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Non‑constant expressions (variables, function calls) → false.
//   - Non‑integer literals (float, string, bool, nil) → false (returns false).
//   - Division by zero → false (no value returned, caller may report error).
//   - Binary operators other than +, -, *, /, % → false.
//
// ─── Supported Literal Formats ───────────────────────────────────────────────
//   Decimal:     "123", "1_000_000"
//   Hexadecimal: "0xFF", "0xdead_beef"
//   Binary:      "0b1010", "0b1111_0000"
//   Negative:    "-42" (via unary negation handling, not a single token)
//
// ─── Side Effects ────────────────────────────────────────────────────────────
//   None – pure evaluation. Does not modify AST or emit diagnostics.
//   The caller is responsible for reporting errors (e.g., division by zero,
//   overflow – though strtoll handles overflow gracefully).
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::getConstantIntValue(ExprAST* expr, int64_t& outValue) {
    if (!expr) return false;
    
    // Handle literal integers
    if (expr->isa<LiteralExprAST>()) {
        auto* lit = expr->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Int || lit->kind == LiteralKind::Hex || 
            lit->kind == LiteralKind::Binary) {
            
            std::string_view val = pool_.lookup(lit->value);
            const char* str = val.data();
            char* endptr = nullptr;
            int64_t result = 0;
            
            // Hex literal: 0x... or 0X...
            if (val.find("0x") == 0 || val.find("0X") == 0) {
                result = std::strtoll(str, &endptr, 16);
            }
            // Binary literal: 0b... or 0B...
            else if (val.find("0b") == 0 || val.find("0B") == 0) {
                result = 0;
                for (size_t i = 2; i < val.length(); ++i) {
                    char c = val[i];
                    if (c == '_') continue;
                    if (c == '0') {
                        result = (result << 1);
                    } else if (c == '1') {
                        result = (result << 1) | 1;
                    } else {
                        return false; // invalid binary digit
                    }
                }
                endptr = const_cast<char*>(str + val.length());
            }
            // Decimal literal
            else {
                result = std::strtoll(str, &endptr, 10);
            }
            
            // If parsing stopped early (e.g., due to underscores), strip underscores and retry
            if (endptr != str + val.length()) {
                std::string cleaned;
                cleaned.reserve(val.length());
                for (char c : val) {
                    if (c != '_') cleaned += c;
                }
                const char* cleanedStr = cleaned.c_str();
                char* cleanedEndptr = nullptr;
                result = std::strtoll(cleanedStr, &cleanedEndptr, 10);
                if (cleanedEndptr != cleanedStr + cleaned.length()) {
                    return false; // still has non-numeric characters
                }
            }
            
            outValue = result;
            return true;
        }
    }
    
    // Handle unary negation of a constant: -5
    if (expr->isa<UnaryExprAST>()) {
        auto* unary = expr->as<UnaryExprAST>();
        if (unary->op == UnaryOp::Neg) {
            int64_t innerValue;
            if (getConstantIntValue(unary->operand.get(), innerValue)) {
                outValue = -innerValue;
                return true;
            }
        }
    }
    
    // Handle binary arithmetic of constants: 5 + 3, 10 - 2, etc.
    if (expr->isa<BinaryExprAST>()) {
        auto* binary = expr->as<BinaryExprAST>();
        int64_t leftVal, rightVal;
        if (getConstantIntValue(binary->left.get(), leftVal) &&
            getConstantIntValue(binary->right.get(), rightVal)) {
            switch (binary->op) {
                case BinaryOp::Add: outValue = leftVal + rightVal; return true;
                case BinaryOp::Sub: outValue = leftVal - rightVal; return true;
                case BinaryOp::Mul: outValue = leftVal * rightVal; return true;
                case BinaryOp::Div:
                    if (rightVal != 0) { outValue = leftVal / rightVal; return true; }
                    return false;
                case BinaryOp::Mod:
                    if (rightVal != 0) { outValue = leftVal % rightVal; return true; }
                    return false;
                default: return false;
            }
        }
    }
    
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isValidArrayIndex  —  Validates an array index expression.
//
// Checks that the index expression is an integer type and, if the value is
// a compile‑time constant, that it is non‑negative. Runtime bounds checking
// is deferred to the code generator.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in semantic checking of array element access (e.g., `arr[i]`). Ensures
// that the index is syntactically valid before code generation. Compile‑time
// errors are reported for constant negative indices; runtime panics are
// emitted for out‑of‑bounds or negative variable indices.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Index expression resolves to any integer type (signed or unsigned,
//     any width) → passes type check.
//   - Index expression is a compile‑time constant integer literal (decimal,
//     hex, binary) and value >= 0 → passes.
//   - Index expression is a variable of integer type → passes (runtime check).
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Non‑integer index types (float, string, bool, struct, etc.) → error.
//   - Compile‑time constant index < 0 → error (reported here).
//   - Array bounds checking (value < array length) → deferred to runtime.
//   - Negative constant indices expressed via unary negation (e.g., `-5`) are
//     caught because getConstantIntValue handles negation.
//   - The target expression being indexed (the array) is not validated here.
//     Caller must ensure the target is an array, slice, or dynamic array.
//   - Slice operations (arr[i..j]) – use isValidSliceBound for bounds.
//
// ─── Error Reporting ─────────────────────────────────────────────────────────
//   Emits diagnostic via DiagnosticEngine for type mismatches and negative
//   constant indices. Returns false on error; caller should propagate.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isValidArrayIndex(ExprAST* indexExpr, DiagnosticEngine& dc, 
                                     const SourceLocation& loc) {
    if (!indexExpr) {
        dc.error(DiagnosticCategory::Semantic, loc, DiagCode::E3002,
                 "array index expression is null");
        return false;
    }
    
    TypeAST* indexType = static_cast<TypeAST*>(indexExpr->resolvedType);
    if (!isIntegerType(indexType)) {
        dc.error(DiagnosticCategory::Semantic, indexExpr->loc, DiagCode::E3002,
                 "array index must be an integer type (got '" +
                 LucDebug::kindToString(indexType ? indexType->kind : ASTKind::Unknown) + "')");
        return false;
    }
    
    int64_t constValue;
    if (getConstantIntValue(indexExpr, constValue)) {
        if (constValue < 0) {
            dc.error(DiagnosticCategory::Semantic, indexExpr->loc, DiagCode::E3002,
                     "array index cannot be negative (got " + std::to_string(constValue) + ")");
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// isValidSliceBound  —  Validates a slice bound expression (start or end).
//
// For slice expressions like `arr[start..end]` or `arr[start..<end]`, both
// bounds must be compile‑time constants of integer type and non‑negative.
// Slice bounds are part of the syntactic slice operation and are required
// to be constants (unlike array indices which can be variables).
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Enforces the Luc language rule that slice bounds must be known at compile
// time. This simplifies code generation and guarantees that slice operations
// are statically bounded. Used in semantic checking of index expressions
// where kind == IndexKind::Slice.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Bound expression is an integer literal (decimal, hex, binary) with
//     value >= 0 → passes.
//   - Bound expression is a compile‑time constant integer (via constant folding)
//     with value >= 0 → passes.
//   - Bound expression is a variable of integer type → fails (must be constant).
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Non‑integer bound types → error.
//   - Non‑constant bound expressions (variables, function calls, etc.) → error.
//   - Negative constant bounds → error.
//   - Relationship between start and end (start <= end) – this check is
//     performed separately by the caller (usually during slice validation).
//   - Out‑of‑bounds relative to the array length – runtime panic.
//   - The target expression being sliced – caller must validate it is sliceable.
//
// ─── Error Reporting ─────────────────────────────────────────────────────────
//   Emits diagnostic for type errors, non‑constant expressions, or negative
//   constant values. Returns false on error.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isValidSliceBound(ExprAST* boundExpr, const std::string& boundName,
                                     DiagnosticEngine& dc, const SourceLocation& loc) {
    if (!boundExpr) {
        dc.error(DiagnosticCategory::Semantic, loc, DiagCode::E3002,
                 "slice " + boundName + " bound expression is null");
        return false;
    }
    
    TypeAST* boundType = static_cast<TypeAST*>(boundExpr->resolvedType);
    if (!isIntegerType(boundType)) {
        dc.error(DiagnosticCategory::Semantic, boundExpr->loc, DiagCode::E3002,
                 "slice " + boundName + " bound must be an integer type");
        return false;
    }
    
    int64_t constValue;
    if (!getConstantIntValue(boundExpr, constValue)) {
        dc.error(DiagnosticCategory::Semantic, boundExpr->loc, DiagCode::E3002,
                 "slice " + boundName + " bound must be a constant expression");
        return false;
    }
    
    if (constValue < 0) {
        dc.error(DiagnosticCategory::Semantic, boundExpr->loc, DiagCode::E3002,
                 "slice " + boundName + " bound cannot be negative (got " + 
                 std::to_string(constValue) + ")");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Custom Casting (from blocks)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// isFromCastable  —  Checks if a custom `from` block conversion exists from
//                    `src` to `target`.
//
// Searches the symbol table for a registered `from` conversion entry that
// converts values of type `src` to the target named struct type. These
// conversions are defined using `from` blocks in source code and are used
// for implicit casts in assignments, argument passing, and return statements.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Allows the language to support user‑defined implicit conversions between
// types, similar to `From` traits in Rust or conversion constructors in C++.
// Used by assignment and function call checking when direct assignment fails.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. Ensure `target` is a NamedTypeAST (conversions only target structs).
//   2. Construct a prefix string like "TargetType::from" using NameMangler.
//   3. Query the symbol table for all symbols with that prefix.
//   4. For each candidate:
//        - Must be SymbolKind::Casting.
//        - Must point to a FromEntryAST.
//        - Extract the type of the first parameter in the first parameter group.
//        - If `src` is assignable to that parameter type, conversion exists.
//   5. Return true if any candidate matches.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - There is a `from` block entry whose first parameter type is assignable
//     from `src` and whose return type matches `target`.
//   - Example: `from Fahrenheit { (c Celsius) -> Fahrenheit = ... }` allows
//     assignment `let f Fahrenheit = celsiusValue`.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - `target` is not a NamedTypeAST (e.g., primitive, array, function type).
//   - No registered from‑entry with a matching signature.
//   - The source type is not assignable to the entry's parameter type.
//   - The return type of the entry does not match `target` (should be checked
//     during registration; not re‑checked here).
//   - Multiple parameter groups (curried from‑entries) – only the first
//     parameter of the first group is considered; curried forms may be
//     unsupported or require additional rules.
//   - Generic `from` blocks (e.g., `from Wrapper<T>`); the mangled name must
//     encode the concrete type parameters, but the lookup uses a simple prefix
//     which may not distinguish generic instantiations.
//
// ─── Dependencies ────────────────────────────────────────────────────────────
//   - NameMangler::getFromPrefix to generate the search prefix.
//   - SymbolTable::findSymbolsByPrefix to retrieve candidates.
//   - isAssignable to compare source type with entry's parameter type.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isFromCastable(TypeAST* src, TypeAST* target, SymbolTable* symbols) {
    LUC_LOG_SEMANTIC("isFromCastable: checking if " << (src ? "src" : "null") 
                << " can be cast to " << (target ? "target" : "null"));
    
    if (!src || !target) {
        LUC_LOG_SEMANTIC("\tnull pointer -> false");
        return false;
    }
    if (!target->isa<NamedTypeAST>()) {
        LUC_LOG_SEMANTIC("\ttarget not NamedType -> false");
        return false;
    }
    if (!symbols) {
        LUC_LOG_SEMANTIC("\tno symbol table -> false");
        return false;
    }

    std::string_view targetName = pool_.lookup(target->as<NamedTypeAST>()->name);
    std::string prefix = NameMangler::getFromPrefix(targetName);
    LUC_LOG_SEMANTIC_VERBOSE("\tlooking for from-entries with prefix: " << prefix);

    std::vector<Symbol*> candidates = symbols->findSymbolsByPrefix(prefix, pool_);
    LUC_LOG_SEMANTIC_VERBOSE("\tfound " << candidates.size() << " candidate(s)");

    for (Symbol* sym : candidates) {
        if (!sym || sym->kind != SymbolKind::Casting) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tskipping non-casting symbol");
            continue;
        }
        if (!sym->decl || !sym->decl->isa<FromEntryAST>()) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tskipping non-FromEntryAST");
            continue;
        }

        auto* entry = sym->decl->as<FromEntryAST>();
        
        // Use sig for param groups
        if (entry->sig.paramGroups.empty()) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tentry has no param groups");
            continue;
        }
        if (entry->sig.paramGroups[0].empty()) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tentry param group has no params");
            continue;
        }

        TypeAST* firstParamType = entry->sig.paramGroups[0][0]->type.get();
        if (!firstParamType) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tfirst param type is null");
            continue;
        }

        LUC_LOG_SEMANTIC_VERBOSE("\t\tchecking candidate: " << targetName << ".from");
        
        if (isAssignable(src, firstParamType)) {
            LUC_LOG_SEMANTIC("\t-> found matching from-entry, returning true");
            return true;
        }
    }

    LUC_LOG_SEMANTIC("\t-> no matching from-entry found, returning false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison Helpers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// isValueComparable  —  Returns true when the `==` and `!=` operators are valid
//                       on this type.
//
// Value equality compares the content (values) of two instances, not their
// memory addresses. This is the default equality for primitive types, enums,
// and nullable types. Structs and arrays are not value‑comparable by default.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in semantic checking of equality expressions (`==`, `!=`) to determine
// if the operator is allowed. Prevents accidental misuse of `==` on complex
// types where value equality is ambiguous or expensive.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - PrimitiveTypeAST (int, float, bool, string, char, any) – all primitives.
//   - NullableTypeAST (T?) – compare the inner type (nil is also comparable).
//   - NamedTypeAST that resolves to an enum – enum variants are comparable.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - Struct types (NamedTypeAST for struct) – use `===` for reference equality
//     or implement the `Equatable` trait and use `:equals()`.
//   - Function types – function bodies are not comparable.
//   - Array types (fixed, slice, dynamic) – use collection library comparison.
//   - Reference types (&T) – use `===` for reference equality.
//   - Pointer types (*T) – raw pointers are not comparable; use `===` or
//     `==` after converting to reference?
//   - Generic type parameters (even if they may be instantiated with a
//     comparable type) – the flag is set on the NamedTypeAST itself.
//
// ─── Design Note ────────────────────────────────────────────────────────────
//   The function optionally takes a SymbolTable to resolve named types to
//   their declaration (struct vs enum). Without a symbol table, it returns
//   false for all NamedTypeAST to be conservative.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isValueComparable(TypeAST* type, SymbolTable* symbols) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: type is null -> false");
        return false;
    }

    if (type->isa<PrimitiveTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: primitive -> true");
        return true;
    }

    if (type->isa<NullableTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: NullableType -> true");
        return true;
    }

    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        if (symbols) {
            Symbol* sym = symbols->lookup(named->name);
            if (sym && sym->kind == SymbolKind::Enum) {
                LUC_LOG_SEMANTIC_EXTREME("isValueComparable: enum -> true");
                return true;
            }
            if (sym && sym->kind == SymbolKind::Struct) {
                LUC_LOG_SEMANTIC_EXTREME("isValueComparable: struct -> false (use === or implement Equatable)");
                return false;
            }
        }
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: NamedType (unknown) -> false");
        return false;
    }

    if (type->isa<FuncTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: FuncType -> false");
        return false;
    }

    if (type->isa<FixedArrayTypeAST>() || type->isa<SliceTypeAST>() || type->isa<DynamicArrayTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: array type -> false");
        return false;
    }

    if (type->isa<RefTypeAST>() || type->isa<PtrTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: ref/ptr type -> false (use ===)");
        return false;
    }

    LUC_LOG_SEMANTIC_EXTREME("isValueComparable: unknown type -> false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isReferenceComparable  —  Returns true when the `===` operator is valid on
//                           this type.
//
// Reference equality compares memory addresses, not content. It is valid for
// structs (where two instances are equal if they occupy the same memory
// location) and reference types (&T). Primitive values do not have stable
// addresses and cannot be compared with `===`.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used in semantic checking of reference equality expressions (`===`, `!==`).
// Enforces that `===` is used only on types that have a stable identity.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - RefTypeAST (&T) – references have stable addresses.
//   - NamedTypeAST that resolves to a struct – structs have identity; two
//     struct variables are the same if they point to the same memory.
//   - NullableTypeAST containing any of the above (e.g., &T?, struct?) –
//     nullability does not affect reference comparability.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - PrimitiveTypeAST – primitives are value types with no stable address.
//   - Function types – function bodies are not address‑comparable.
//   - Array types – arrays are value types in Luc; use value equality or
//     convert to reference if needed.
//   - Pointer types (*T) – raw pointers are not directly comparable with `===`;
//     they are sealed conduits; use `==` after converting to reference.
//   - Enum types – enums are value types (integral constants); use `==`.
//   - Generic type parameters (even if instantiated with a struct).
//
// ─── Relationship with isValueComparable ─────────────────────────────────────
//   - Primitive types: isValueComparable = true, isReferenceComparable = false.
//   - Struct types:   isValueComparable = false, isReferenceComparable = true.
//   - Nullable:       inherits comparability of inner type.
//   - Enums:          isValueComparable = true, isReferenceComparable = false.
//   - Functions:      both false.
//   - Arrays:         both false.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isReferenceComparable(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: type is null -> false");
        return false;
    }

    if (type->isa<RefTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: RefType -> true");
        return true;
    }

    if (type->isa<NamedTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: NamedType -> true");
        return true;
    }

    if (type->isa<NullableTypeAST>()) {
        bool result = isReferenceComparable(type->as<NullableTypeAST>()->inner.get());
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: NullableType -> " << (result ? "true" : "false"));
        return result;
    }

    if (type->isa<PrimitiveTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: primitive -> false");
        return false;
    }

    if (type->isa<FuncTypeAST>() || type->isa<FixedArrayTypeAST>() ||
        type->isa<SliceTypeAST>() || type->isa<DynamicArrayTypeAST>() ||
        type->isa<PtrTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: function/array/ptr -> false");
        return false;
    }

    LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: unknown type -> false");
    return false;
}

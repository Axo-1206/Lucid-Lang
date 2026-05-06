/**
 * @file TypeChecker.cpp
 *
 * @nutshell Provides explicit logical comparisons between two separate Types.
 *
 * @reason Because comparing complex generic structures, arrays, and primitive widenings involves heavy branching logic isolated from the actual AST traversal.
 *
 * @responsibility Implementation of Phase 2b semantic pass (type compatibility).
 *
 * @logic Includes checks for isAssignable, isCallable, unification patterns, primitive widening, and 'from' castable type properties.
 *
 * @related TypeChecker.hpp
 */

#include "SemanticHelpers.hpp"

#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// isEqual  — Verifies structural equality precisely, ignoring widening rules
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
            LUC_LOG_SEMANTIC_VERBOSE("\tNamedType name mismatch: " << na->name << " vs " << nb->name << " -> false");
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
        LUC_LOG_SEMANTIC_VERBOSE("\tNamedType " << na->name << " -> true");
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
        if ((fa->qualifiers & equalityMask) != (fb->qualifiers & equalityMask)) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFuncType qualifier mismatch -> false");
            return false;
        }
        
        // Compare nullability (function itself nullable)
        if (fa->isNullable != fb->isNullable) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFuncType nullability mismatch -> false");
            return false;
        }
        
        // Compare parameter group count (curry levels)
        if (fa->paramGroups.size() != fb->paramGroups.size()) {
            LUC_LOG_SEMANTIC_VERBOSE("\tFuncType param group count mismatch -> false");
            return false;
        }
        
        // Compare each parameter group
        for (size_t g = 0; g < fa->paramGroups.size(); ++g) {
            const auto& groupA = fa->paramGroups[g];
            const auto& groupB = fb->paramGroups[g];
            
            if (groupA.size() != groupB.size()) {
                LUC_LOG_SEMANTIC_VERBOSE("\tFuncType param group " << g << " size mismatch -> false");
                return false;
            }
            
            for (size_t i = 0; i < groupA.size(); ++i) {
                // Compare parameter types (ignoring parameter names)
                if (!isEqual(groupA[i].type.get(), groupB[i].type.get())) {
                    LUC_LOG_SEMANTIC_VERBOSE("\tFuncType param " << i << " in group " << g << " mismatch -> false");
                    return false;
                }
            }
        }
        
        // Compare return types
        bool result = isEqual(fa->returnType.get(), fb->returnType.get());
        LUC_LOG_SEMANTIC_VERBOSE("\tFuncType: " << (result ? "true" : "false"));
        return result;
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("\tUnknown type kind -> false");
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
    SemanticHelpers::printTypeAST("from", from);
    SemanticHelpers::printTypeAST("to", to);

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
            LUC_LOG_SEMANTIC("\tNamedType name mismatch: " << namedFrom->name << " vs " << namedTo->name << " -> false");
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
        
        LUC_LOG_SEMANTIC("\tNamedType " << namedFrom->name << " -> true");
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
// isCallable  — Assesses if a Type inherently permits invocation executions
//
// Identifies if the raw bounded Type node carries the functional properties to map
// back to underlying lambda or raw-function call architectures.
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
// isBooleanCompatible  — Validates logical query compatibilities
//
// Often needed for verifying `if`, `while`, or looping constructs evaluating raw
// boolean conditional branch decisions natively.
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
// isNullable  — Safely verifies the topmost nullable context rule constraints
//
// Checks entirely if the given raw outermost bounding Type specifically allows
// `nil` initializations logically directly on its specific mapping.
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
        bool result = type->as<FuncTypeAST>()->isNullable;
        LUC_LOG_SEMANTIC_EXTREME("isNullable: FuncType nullable=" << result);
        return result;
    }
    
    LUC_LOG_SEMANTIC_EXTREME("isNullable: false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// unify  — Merges two Types discovering boundaries defining them safely together
//
// Commonly required securely balancing dual-branch architectures mapping out
// combinations inside Match-Arms or If-Else statements where branches merge securely.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeChecker::unify(TypeAST* a, TypeAST* b) {
    LUC_LOG_SEMANTIC_VERBOSE("unify: trying to unify types");
    SemanticHelpers::printTypeAST("  a", a);
    SemanticHelpers::printTypeAST("  b", b);
    
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
// primitiveWidening  — Verifies standard safe primitive implicit conversion
//
// Determines whether an explicit type cast from one primitive to another is safe
// (widening, not narrowing). Widening conversions maintain or increase precision
// without loss of information. Examples:
//   int → float      (int fits in float's exponent range, though precision may vary)
//   int → double     (int fits completely in double)
//   float → double   (all single-precision values are valid doubles)
//   int8 → int16     (smaller signed fits in larger signed)
//   uint8 → uint16   (smaller unsigned fits in larger unsigned)
//   int8 → int32     (chain of widening: byte → short → int → long)
//
// Non-widening (blocked):
//   int → int8       (narrowing — data loss)
//   double → float   (narrowing — precision loss)
//   unsigned → signed (unsigned range may not fit)
//
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::primitiveWidening(PrimitiveKind from, PrimitiveKind to) {
    LUC_LOG_SEMANTIC_VERBOSE("primitiveWidening: from=" << static_cast<int>(from) 
                           << " to=" << static_cast<int>(to));
    
    // Quick check: identical types are always acceptable.
    if (from == to) {
        LUC_LOG_SEMANTIC_VERBOSE("\tidentical types -> true");
        return true;
    }

    // ── Signed integer widening ───────────────────────────────────────────────
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

    // ── Unsigned integer widening ───────────────────────────────────────────
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

    // ── Signed integer to unsigned integer (ADD THIS SECTION) ─────────────────
    // Allow int → uint64, int → uint32, etc. for positive integer literals
    // The literal 1024 is positive, so it's safe to convert to unsigned
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

    // ── Floating-point widening ───────────────────────────────────────────────
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

    // ── Signed integer to floating-point ───────────────────────────────────────
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

    // ── Unsigned integer to floating-point ────────────────────────────────────
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
// isFromCastable  — Checks if a custom casting (from block) is available
//
// Searches the symbol table for a `from` entry that converts src -> target.
//
// How from-entries are registered (SemanticCollector::visit(FromDeclAST)):
//   Each FromEntryAST gets a mangled symbol name:
//     "TargetType.from.<pointer_address>"
//   So for  from Minutes { (s Seconds) Minutes = { ... } }
//   the symbol "Minutes.from.0x1234abcd" is registered with:
//     sym->kind = SymbolKind::Casting
//     sym->decl = FromEntryAST*
//
// Algorithm:
//   1. Extract target type name (only NamedTypeAST can have from blocks).
//   2. Build the prefix  "TargetType.from."
//   3. Find all symbols with that prefix via findSymbolsByPrefix.
//   4. For each, downcast decl to FromEntryAST and inspect its first param
//      group's first parameter type.
//   5. Return true if any entry's first param type is assignable from src.
//
// When integrated with codegen, a conversion like:
//   let m Minutes = s              (s is Seconds, Minutes ≠ Seconds)
// is desugared by the semantic pass into:
//   let m Minutes = Minutes(s)     (TypeConvExprAST wrapping s)
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

    const std::string targetName = target->as<NamedTypeAST>()->name;
    const std::string prefix = targetName + ".from.";
    LUC_LOG_SEMANTIC_VERBOSE("\tlooking for from-entries with prefix: " << prefix);

    // Find all registered from-entry symbols for this target type.
    std::vector<Symbol*> candidates = symbols->findSymbolsByPrefix(prefix);
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
        
        if (entry->paramGroups.empty()) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tentry has no param groups");
            continue;
        }
        if (entry->paramGroups[0].empty()) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tentry param group has no params");
            continue;
        }

        TypeAST* firstParamType = entry->paramGroups[0][0].type.get();
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
// isValueComparable — returns true when == and != are valid on this type.
//
// Valid:    primitives, enums, nullable types
// Invalid:  structs (use Equatable<T>), functions, arrays
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isValueComparable(TypeAST* type, SymbolTable* symbols) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: type is null -> false");
        return false;
    }

    // Primitives are always value-comparable
    if (type->isa<PrimitiveTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: primitive -> true");
        return true;
    }

    // Nullable types: valid (compare the inner type)
    if (type->isa<NullableTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: NullableType -> true");
        return true;
    }

    // Named types: only enums are comparable via ==, structs are NOT
    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        
        // If we have a symbol table, check if this is an enum
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
        
        // Without symbol table, be conservative: assume not comparable
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: NamedType (unknown) -> false");
        return false;
    }

    // Function types: NOT comparable
    if (type->isa<FuncTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: FuncType -> false");
        return false;
    }

    // Array types: NOT comparable
    if (type->isa<FixedArrayTypeAST>() || type->isa<SliceTypeAST>() || type->isa<DynamicArrayTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: array type -> false");
        return false;
    }

    // Reference and pointer types: use ===, not ==
    if (type->isa<RefTypeAST>() || type->isa<PtrTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isValueComparable: ref/ptr type -> false (use ===)");
        return false;
    }

    LUC_LOG_SEMANTIC_EXTREME("isValueComparable: unknown type -> false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isReferenceComparable — returns true when === is valid on this type.
//
// Valid:    &T references, structs (address comparison), nullable of above
// Invalid:  primitives (value types, no stable address), functions, arrays
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isReferenceComparable(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: type is null -> false");
        return false;
    }

    // References are always reference-comparable
    if (type->isa<RefTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: RefType -> true");
        return true;
    }

    // Structs can be compared by address via ===
    if (type->isa<NamedTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: NamedType -> true");
        return true;
    }

    // Nullable types containing reference-comparable types
    if (type->isa<NullableTypeAST>()) {
        bool result = isReferenceComparable(type->as<NullableTypeAST>()->inner.get());
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: NullableType -> " << (result ? "true" : "false"));
        return result;
    }

    // Primitives: not reference-comparable
    if (type->isa<PrimitiveTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: primitive -> false");
        return false;
    }

    // Functions, arrays, pointers: not reference-comparable
    if (type->isa<FuncTypeAST>() || type->isa<FixedArrayTypeAST>() ||
        type->isa<SliceTypeAST>() || type->isa<DynamicArrayTypeAST>() ||
        type->isa<PtrTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: function/array/ptr -> false");
        return false;
    }

    LUC_LOG_SEMANTIC_EXTREME("isReferenceComparable: unknown type -> false");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isBoolOrNullable — returns true when a type is valid for 'not', 'and', 'or'.
//
// Valid:    bool, any nullable type T?
// Invalid:  int, float, string, struct, function, array
//
// Nullable semantics:
//   nil     → treated as false
//   non-nil → treated as true
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isBoolOrNullable(TypeAST* type) {
    if (!type) {
        LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: type is null -> false");
        return false;
    }

    // Direct bool
    if (type->isa<PrimitiveTypeAST>()) {
        bool result = type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
        LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: bool -> " << (result ? "true" : "false"));
        return result;
    }

    // Any nullable type
    if (type->isa<NullableTypeAST>()) {
        LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: NullableType -> true");
        return true;
    }

    LUC_LOG_SEMANTIC_EXTREME("isBoolOrNullable: false");
    return false;
}

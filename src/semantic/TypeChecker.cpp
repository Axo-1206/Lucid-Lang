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

#include "TypeChecker.hpp"
#include <iostream>

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
    return false;
}

#include <cstring>  // for typeid names (optional)

static void printTypeAST(const std::string& label, TypeAST* t, int indent = 0) {
    if (!t) {
        std::cout << std::string(indent, ' ') << label << " = nullptr" << std::endl;
        return;
    }
    
    std::string indentStr(indent, ' ');
    std::cout << indentStr << label << " = " << t << " kind=" << static_cast<int>(t->kind);
    
    // Print kind name for readability
    switch (t->kind) {
        case ASTKind::PrimitiveType: {
            auto* p = t->as<PrimitiveTypeAST>();
            std::cout << " (PrimitiveType: ";
            switch (p->primitiveKind) {
                case PrimitiveKind::Bool:   std::cout << "bool"; break;
                case PrimitiveKind::Int:    std::cout << "int"; break;
                case PrimitiveKind::Float:  std::cout << "float"; break;
                case PrimitiveKind::Double: std::cout << "double"; break;
                case PrimitiveKind::String: std::cout << "string"; break;
                case PrimitiveKind::Uint8:  std::cout << "uint8"; break;
                case PrimitiveKind::Uint64: std::cout << "uint64"; break;
                case PrimitiveKind::Any:    std::cout << "any"; break;
                // Add other primitives as needed
                default: std::cout << "other(" << static_cast<int>(p->primitiveKind) << ")";
            }
            std::cout << ")";
            break;
        }
        case ASTKind::NamedType: {
            auto* n = t->as<NamedTypeAST>();
            std::cout << " (NamedType: name='" << n->name << "'";
            if (!n->genericArgs.empty()) {
                std::cout << " generics=[";
                for (size_t i = 0; i < n->genericArgs.size(); ++i) {
                    if (i) std::cout << ",";
                    printTypeAST("", n->genericArgs[i].get(), 0);
                }
                std::cout << "]";
            }
            if (n->isGenericParam) std::cout << " isGenericParam=true";
            std::cout << ")";
            break;
        }
        case ASTKind::NullableType: {
            auto* nl = t->as<NullableTypeAST>();
            std::cout << " (NullableType)" << std::endl;
            printTypeAST("  inner", nl->inner.get(), indent + 2);
            return; // Already printed inner on newline
        }
        case ASTKind::PtrType: {
            auto* p = t->as<PtrTypeAST>();
            std::cout << " (PtrType)" << std::endl;
            printTypeAST("  inner", p->inner.get(), indent + 2);
            return;
        }
        case ASTKind::RefType: {
            auto* r = t->as<RefTypeAST>();
            std::cout << " (RefType)" << std::endl;
            printTypeAST("  inner", r->inner.get(), indent + 2);
            return;
        }
        case ASTKind::FixedArrayType: {
            auto* a = t->as<FixedArrayTypeAST>();
            std::cout << " (FixedArrayType: size=" << a->size << ")" << std::endl;
            printTypeAST("  element", a->element.get(), indent + 2);
            return;
        }
        case ASTKind::SliceType: {
            auto* s = t->as<SliceTypeAST>();
            std::cout << " (SliceType)" << std::endl;
            printTypeAST("  element", s->element.get(), indent + 2);
            return;
        }
        case ASTKind::DynamicArrayType: {
            auto* d = t->as<DynamicArrayTypeAST>();
            std::cout << " (DynamicArrayType)" << std::endl;
            printTypeAST("  element", d->element.get(), indent + 2);
            return;
        }
        case ASTKind::FuncType: {
            auto* f = t->as<FuncTypeAST>();
            std::cout << " (FuncType: isNullable=" << f->isNullable << " params=[";
            for (size_t i = 0; i < f->params.size(); ++i) {
                if (i) std::cout << ",";
                printTypeAST("", f->params[i].get(), 0);
            }
            std::cout << "]";
            if (f->returnType) {
                std::cout << " return=";
                printTypeAST("", f->returnType.get(), 0);
            }
            std::cout << ")";
            break;
        }
        default:
            std::cout << " (Unknown kind)";
            break;
    }
    std::cout << std::endl;
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

    // Validate pointers before anything else
    if (from && (reinterpret_cast<uintptr_t>(from) & 0xFFFF) == 0x4F27) {
        std::cout << "CORRUPTED 'from' pointer detected: " << from << std::endl;
        std::cout << "Backtrace:" << std::endl;
        // Print stack trace here (platform-specific)
        return false;
    }
    if (to && (reinterpret_cast<uintptr_t>(to) & 0xFFFF) == 0x4F27) {
        std::cout << "CORRUPTED 'to' pointer detected: " << to << std::endl;
        return false;
    }


    
    std::cout << "\n========== isAssignable called ==========" << std::endl;
    printTypeAST("from", from);
    printTypeAST("to", to);
    std::cout << "=========================================" << std::endl;



    std::cout << "\n--- TEST5.4 ---" << std::endl;
    // 0.1 Handle nil assignment
    if (!from) {
        return isNullable(to);
    }
    if (!to) return false;
std::cout << "\n--- TEST5.5 ---" << std::endl;
    // 0.2 Handle target 'any' (boxing)
    if (to->isa<PrimitiveTypeAST>() && to->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Any) {
        return true;
    }
std::cout << "\n--- TEST5.6 ---" << std::endl;
    // 0.3 Handle implicit wrapping into nullable (T -> T?)
    if (to->isa<NullableTypeAST>() && !from->isa<NullableTypeAST>()) {
        return isAssignable(from, to->as<NullableTypeAST>()->inner.get());
    }
std::cout << "\n--- TEST5.7 ---" << std::endl;
    // 0.4 Handle nullable to nullable (T? -> T?)
    if (from->isa<NullableTypeAST>() && to->isa<NullableTypeAST>()) {
        return isAssignable(from->as<NullableTypeAST>()->inner.get(), to->as<NullableTypeAST>()->inner.get());
    }

    // Quick exit: identical pointer = same allocated node = same type.
    if (from == to) return true;
std::cout << "\n--- TEST5.75 ---" << std::endl;
    // 1. Primitive vs Primitive — check widening table.
    if (from->isa<PrimitiveTypeAST>() && to->isa<PrimitiveTypeAST>()) {
        auto* primFrom = from->as<PrimitiveTypeAST>();
        auto* primTo   = to->as<PrimitiveTypeAST>();
        if (primFrom->primitiveKind == primTo->primitiveKind) return true;
        if (primitiveWidening(primFrom->primitiveKind, primTo->primitiveKind)) return true;
        return false;
    }
std::cout << "\n--- TEST5.8 ---" << std::endl;
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

std::cout << "\n--- TEST5.9 ---" << std::endl;

    // 3. Function types.
    if (from->isa<FuncTypeAST>() && to->isa<FuncTypeAST>()) {
        return isEqual(from, to);
    }

    // Fallback: use structural equality for all other types (Arrays, Ptrs, Refs, etc.)
    return isEqual(from, to);
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
    // Quick check: identical types are always acceptable.
    if (from == to) return true;

    // ── Signed integer widening ───────────────────────────────────────────────
    // Widening within signed integers: smaller signed → larger signed
    if (from == PrimitiveKind::Byte || from == PrimitiveKind::Int8) {
        switch (to) {
            case PrimitiveKind::Short:
            case PrimitiveKind::Int16:
            case PrimitiveKind::Int:
            case PrimitiveKind::Int32:
            case PrimitiveKind::Long:
            case PrimitiveKind::Int64:
                return true;
            default:
                break;
        }
    }

    if (from == PrimitiveKind::Short || from == PrimitiveKind::Int16) {
        switch (to) {
            case PrimitiveKind::Int:
            case PrimitiveKind::Int32:
            case PrimitiveKind::Long:
            case PrimitiveKind::Int64:
                return true;
            default:
                break;
        }
    }

    if (from == PrimitiveKind::Int || from == PrimitiveKind::Int32) {
        switch (to) {
            case PrimitiveKind::Long:
            case PrimitiveKind::Int64:
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                return true;
            default:
                break;
        }
    }

    if (from == PrimitiveKind::Long || from == PrimitiveKind::Int64) {
        switch (to) {
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                return true;
            default:
                break;
        }
    }

    // ── Unsigned integer widening ───────────────────────────────────────────
    // Widening within unsigned integers: smaller unsigned → larger unsigned
    if (from == PrimitiveKind::Ubyte || from == PrimitiveKind::Uint8) {
        switch (to) {
            case PrimitiveKind::Ushort:
            case PrimitiveKind::Uint16:
            case PrimitiveKind::Uint:
            case PrimitiveKind::Uint32:
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint64:
                return true;
            default:
                break;
        }
    }

    if (from == PrimitiveKind::Ushort || from == PrimitiveKind::Uint16) {
        switch (to) {
            case PrimitiveKind::Uint:
            case PrimitiveKind::Uint32:
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint64:
                return true;
            default:
                break;
        }
    }

    if (from == PrimitiveKind::Uint || from == PrimitiveKind::Uint32) {
        switch (to) {
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint64:
                return true;
            default:
                break;
        }
    }

    // ── Floating-point widening ───────────────────────────────────────────────
    // Float (32-bit) can widen to Double (64-bit) or Decimal (128-bit)
    if (from == PrimitiveKind::Float) {
        switch (to) {
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                return true;
            default:
                break;
        }
    }

    // Double (64-bit) can widen to Decimal (128-bit)
    if (from == PrimitiveKind::Double) {
        if (to == PrimitiveKind::Decimal) return true;
    }

    // ── Signed integer to floating-point ───────────────────────────────────────
    // Any signed integer can cast to floating-point (though precision may be lost)
    if (from == PrimitiveKind::Byte || from == PrimitiveKind::Int8 ||
        from == PrimitiveKind::Short || from == PrimitiveKind::Int16 ||
        from == PrimitiveKind::Int || from == PrimitiveKind::Int32 ||
        from == PrimitiveKind::Long || from == PrimitiveKind::Int64) {
        switch (to) {
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                return true;
            default:
                break;
        }
    }

    // ── Unsigned integer to floating-point ────────────────────────────────────
    // Any unsigned integer can cast to floating-point
    if (from == PrimitiveKind::Ubyte || from == PrimitiveKind::Uint8 ||
        from == PrimitiveKind::Ushort || from == PrimitiveKind::Uint16 ||
        from == PrimitiveKind::Uint || from == PrimitiveKind::Uint32 ||
        from == PrimitiveKind::Ulong || from == PrimitiveKind::Uint64) {
        switch (to) {
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                return true;
            default:
                break;
        }
    }

    // No other widening paths exist (e.g., char/string/bool do not widen).
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
    if (!src || !target) return false;
    if (!target->isa<NamedTypeAST>()) return false;
    if (!symbols) return false;

    const std::string targetName = target->as<NamedTypeAST>()->name;
    const std::string prefix = targetName + ".from.";

    // Find all registered from-entry symbols for this target type.
    std::vector<Symbol*> candidates = symbols->findSymbolsByPrefix(prefix);

    for (Symbol* sym : candidates) {
        if (!sym || sym->kind != SymbolKind::Casting) continue;
        if (!sym->decl || !sym->decl->isa<FromEntryAST>()) continue;

        auto* entry = sym->decl->as<FromEntryAST>();

        // A from entry must have at least one parameter group with at least one param.
        if (entry->paramGroups.empty()) continue;
        if (entry->paramGroups[0].empty()) continue;

        // The first parameter's type is the source type this entry accepts.
        TypeAST* firstParamType = entry->paramGroups[0][0]->type.get();
        if (!firstParamType) continue;

        // Check if the source type is assignable to this entry's parameter type.
        if (isAssignable(src, firstParamType)) return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isValueComparable — returns true when == and != are valid on this type.
//
// Valid:    primitives, enums, nullable types
// Invalid:  structs (use Equatable<T>), functions, arrays
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isValueComparable(TypeAST* type) {
    if (!type) return false;

    // Primitives are always value-comparable
    if (type->isa<PrimitiveTypeAST>()) return true;

    // Named types: only enums are comparable via ==
    // Structs are NOT comparable via == — developer must implement Equatable<T>
    // This check is deferred to the semantic pass which has symbol table access
    // to distinguish struct from enum. Here we conservatively return true for
    // NamedTypeAST and let SemanticExpr.cpp perform the struct/enum distinction.
    if (type->isa<NamedTypeAST>()) return true;

    // Nullable types: valid — nil == nil, nil != value, value == value
    if (type->isa<NullableTypeAST>()) return true;

    // Function types: NOT comparable — bodies are incomparable
    if (type->isa<FuncTypeAST>()) return false;

    // Array types: NOT comparable via == — use collection library
    if (type->isa<FixedArrayTypeAST>())   return false;
    if (type->isa<SliceTypeAST>())        return false;
    if (type->isa<DynamicArrayTypeAST>()) return false;



    // Reference types: use === for reference equality, not ==
    if (type->isa<RefTypeAST>()) return false;
    if (type->isa<PtrTypeAST>()) return false;

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isReferenceComparable — returns true when === is valid on this type.
//
// Valid:    &T references, structs (address comparison), nullable of above
// Invalid:  primitives (value types, no stable address), functions, arrays
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isReferenceComparable(TypeAST* type) {
    if (!type) return false;

    // References are always reference-comparable
    if (type->isa<RefTypeAST>()) return true;

    // Structs can be compared by address via ===
    // (NamedTypeAST — semantic pass distinguishes struct from enum;
    //  enums are value types and should not use ===, but we allow it here
    //  and let the semantic pass emit a warning if needed)
    if (type->isa<NamedTypeAST>()) return true;

    // Nullable types containing reference-comparable types
    if (type->isa<NullableTypeAST>()) {
        return isReferenceComparable(type->as<NullableTypeAST>()->inner.get());
    }

    // Primitives are NOT reference-comparable: they are value types
    // copied on assignment — === has no meaningful semantics for them
    if (type->isa<PrimitiveTypeAST>()) return false;

    // Functions, arrays, pointers: not reference-comparable via ===
    if (type->isa<FuncTypeAST>())         return false;
    if (type->isa<FixedArrayTypeAST>())   return false;
    if (type->isa<SliceTypeAST>())        return false;
    if (type->isa<DynamicArrayTypeAST>()) return false;
    if (type->isa<PtrTypeAST>())          return false;

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
    if (!type) return false;

    // Direct bool
    if (type->isa<PrimitiveTypeAST>()) {
        return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
    }

    // Any nullable type: int?, Vec2?, string?, etc.
    if (type->isa<NullableTypeAST>()) return true;

    return false;
}
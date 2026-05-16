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
// Local helper to print a TypeAST for debug logging (replaces SemanticHelpers::printTypeAST)
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
// isAssignable  — Validates if one type can securely pipe into another configuration
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
// isCallable  — Assesses if a Type inherently permits invocation executions
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
// isIntegerType — returns true when a type is any integer primitive.
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
// getConstantIntValue — extracts integer value from compile-time constant
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
// isValidArrayIndex — validates array index expression
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
// isValidSliceBound — validates slice bound expression (start or end)
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
// isBooleanCompatible  — Validates logical query compatibilities
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
// unify  — Merges two Types discovering boundaries defining them safely together
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
// primitiveWidening  — Verifies standard safe primitive implicit conversion
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
// isFromCastable  — Checks if a custom casting (from block) is available
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
// isValueComparable — returns true when == and != are valid on this type.
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
// isReferenceComparable — returns true when === is valid on this type.
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

// ─────────────────────────────────────────────────────────────────────────────
// isBoolOrNullable — returns true when a type is valid for 'not', 'and', 'or'.
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
/**
 * @file TypeChecker.cpp
 * @brief Type compatibility utilities – namespace implementation.
 */

#include "TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "debug/DebugUtils.hpp"

#include <iostream>
#include <cstdlib>
#include <cerrno>

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
        case ASTKind::ResultType: {
            auto* r = t->as<ResultTypeAST>();
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = ResultType");
            printType("  inner", r->inner.get(), pool, indent + 2);
            if (r->errorType) {
                printType("  error", r->errorType.get(), pool, indent + 2);
            } else {
                LUC_LOG_SEMANTIC_EXTREME(indentStr << "  error = nil");
            }
            break;
        }
        case ASTKind::ArrayType: {
            auto* a = t->as<ArrayTypeAST>();
            const char* kindName = a->isFixed() ? "Fixed" : (a->isSlice() ? "Slice" : "Dynamic");
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = ArrayType(" << kindName << ")");
            if (a->isFixed()) {
                LUC_LOG_SEMANTIC_EXTREME(indentStr << "  size = " << a->size);
            }
            printType("  element", a->element.get(), pool, indent + 2);
            break;
        }
        case ASTKind::RefType: {
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = RefType");
            printType("  inner", t->as<RefTypeAST>()->inner.get(), pool, indent + 2);
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
            LUC_LOG_SEMANTIC_EXTREME(indentStr << "  async=" << f->isAsync()
                                    << ", parallel=" << f->isParallel()
                                    << ", nullable=" << f->isNullable());
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

bool TypeChecker::isEqual(TypeAST* a, TypeAST* b, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("isEqual: checking structural equality");

    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case ASTKind::PrimitiveType:
            return a->as<PrimitiveTypeAST>()->primitiveKind == b->as<PrimitiveTypeAST>()->primitiveKind;

        case ASTKind::NamedType: {
            auto* na = a->as<NamedTypeAST>();
            auto* nb = b->as<NamedTypeAST>();
            if (na->name != nb->name) return false;
            if (na->genericArgs.size() != nb->genericArgs.size()) return false;
            for (size_t i = 0; i < na->genericArgs.size(); ++i) {
                if (!isEqual(na->genericArgs[i].get(), nb->genericArgs[i].get(), ctx)) return false;
            }
            return true;
        }

        case ASTKind::NullableType:
            return isEqual(a->as<NullableTypeAST>()->inner.get(),
                           b->as<NullableTypeAST>()->inner.get(), ctx);

        case ASTKind::ResultType: {
            auto* ra = a->as<ResultTypeAST>();
            auto* rb = b->as<ResultTypeAST>();
            if (!isEqual(ra->inner.get(), rb->inner.get(), ctx)) return false;
            if (!isEqual(ra->errorType.get(), rb->errorType.get(), ctx)) return false;
            return true;
        }

        case ASTKind::ArrayType: {
            auto* aa = a->as<ArrayTypeAST>();
            auto* ab = b->as<ArrayTypeAST>();
            if (aa->arrayKind != ab->arrayKind) return false;
            if (aa->arrayKind == ArrayKind::Fixed && aa->size != ab->size) return false;
            return isEqual(aa->element.get(), ab->element.get(), ctx);
        }

        case ASTKind::RefType:
            return isEqual(a->as<RefTypeAST>()->inner.get(),
                           b->as<RefTypeAST>()->inner.get(), ctx);

        case ASTKind::PtrType:
            return isEqual(a->as<PtrTypeAST>()->inner.get(),
                           b->as<PtrTypeAST>()->inner.get(), ctx);

        case ASTKind::FuncType: {
            auto* fa = a->as<FuncTypeAST>();
            auto* fb = b->as<FuncTypeAST>();

            uint32_t equalityMask = QualifierBits::Async | QualifierBits::Nullable;
            if ((fa->qualifiers & equalityMask) != (fb->qualifiers & equalityMask)) return false;

            if (fa->sig.groupCount() != fb->sig.groupCount()) return false;

            for (size_t g = 0; g < fa->sig.groupCount(); ++g) {
                auto groupA = fa->sig.getGroup(g);
                auto groupB = fb->sig.getGroup(g);
                if (groupA.size() != groupB.size()) return false;
                for (size_t i = 0; i < groupA.size(); ++i) {
                    if (!isEqual(groupA[i]->type.get(), groupB[i]->type.get(), ctx)) return false;
                }
            }

            if (fa->sig.returnTypes.size() != fb->sig.returnTypes.size()) return false;
            for (size_t i = 0; i < fa->sig.returnTypes.size(); ++i) {
                if (!isEqual(fa->sig.returnTypes[i].get(), fb->sig.returnTypes[i].get(), ctx)) return false;
            }
            return true;
        }

        default:
            return false;
    }
}

bool TypeChecker::isAssignable(TypeAST* from, TypeAST* to, SemanticContext& ctx) {
    printType("from", from, ctx.pool);
    printType("to", to, ctx.pool);

    if (!from) return isNullable(to, ctx);
    if (!to) return false;

    // Boxing to 'any'
    if (to->isa<PrimitiveTypeAST>() && to->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Any) {
        return true;
    }

    // Implicit nullable wrapping: T -> T?
    if (to->isa<NullableTypeAST>() && !from->isa<NullableTypeAST>()) {
        return isAssignable(from, to->as<NullableTypeAST>()->inner.get(), ctx);
    }

    // Nullable to nullable: T? -> T?
    if (from->isa<NullableTypeAST>() && to->isa<NullableTypeAST>()) {
        return isAssignable(from->as<NullableTypeAST>()->inner.get(),
                            to->as<NullableTypeAST>()->inner.get(), ctx);
    }

    if (from == to) return true;

    // Primitive widening
    if (from->isa<PrimitiveTypeAST>() && to->isa<PrimitiveTypeAST>()) {
        auto* primFrom = from->as<PrimitiveTypeAST>();
        auto* primTo = to->as<PrimitiveTypeAST>();
        if (primFrom->primitiveKind == primTo->primitiveKind) return true;
        return primitiveWidening(primFrom->primitiveKind, primTo->primitiveKind);
    }

    // Named types (structs, enums, type aliases)
    if (from->isa<NamedTypeAST>() && to->isa<NamedTypeAST>()) {
        auto* namedFrom = from->as<NamedTypeAST>();
        auto* namedTo = to->as<NamedTypeAST>();
        if (namedFrom->name != namedTo->name) return false;
        if (namedFrom->genericArgs.size() != namedTo->genericArgs.size()) return false;
        for (size_t i = 0; i < namedFrom->genericArgs.size(); ++i) {
            if (!isAssignable(namedFrom->genericArgs[i].get(), namedTo->genericArgs[i].get(), ctx)) {
                return false;
            }
        }
        return true;
    }

    // Function types
    if (from->isa<FuncTypeAST>() && to->isa<FuncTypeAST>()) {
        auto* fFrom = from->as<FuncTypeAST>();
        auto* fTo = to->as<FuncTypeAST>();

        // Compare async qualifier specially:
        // - Plain → async is allowed (the binding carries the qualifier)
        // - Async → plain is forbidden
        bool fromAsync = (fFrom->qualifiers & QualifierBits::Async) != 0;
        bool toAsync = (fTo->qualifiers & QualifierBits::Async) != 0;
        if (fromAsync && !toAsync) return false;  // async → plain forbidden
        // Plain → async allowed, all other combinations allowed

        // Compare parameter groups and return types (structural equality)
        if (fFrom->sig.groupCount() != fTo->sig.groupCount()) return false;
        for (size_t g = 0; g < fFrom->sig.groupCount(); ++g) {
            auto groupFrom = fFrom->sig.getGroup(g);
            auto groupTo = fTo->sig.getGroup(g);
            if (groupFrom.size() != groupTo.size()) return false;
            for (size_t i = 0; i < groupFrom.size(); ++i) {
                if (!isEqual(groupFrom[i]->type.get(), groupTo[i]->type.get(), ctx)) return false;
            }
        }
        if (fFrom->sig.returnTypes.size() != fTo->sig.returnTypes.size()) return false;
        for (size_t i = 0; i < fFrom->sig.returnTypes.size(); ++i) {
            if (!isEqual(fFrom->sig.returnTypes[i].get(), fTo->sig.returnTypes[i].get(), ctx)) return false;
        }

        // Compare nullability: non‑nullable → nullable allowed; reverse forbidden
        bool fromNullable = fFrom->isNullable();
        bool toNullable = fTo->isNullable();
        if (!fromNullable && toNullable) return true;   // non‑nullable → nullable allowed
        if (fromNullable && !toNullable) return false;  // nullable → non‑nullable forbidden

        return true;
    }

    return isEqual(from, to, ctx);
}

bool TypeChecker::areAssignableMultiple(const std::vector<TypeAST*>& fromTypes,
                                        const std::vector<TypeAST*>& toTypes,
                                        SemanticContext& ctx) {
    if (fromTypes.size() != toTypes.size()) return false;
    for (size_t i = 0; i < fromTypes.size(); ++i) {
        if (!isAssignable(fromTypes[i], toTypes[i], ctx)) return false;
    }
    return true;
}

bool TypeChecker::isCallable(TypeAST* type, SemanticContext& ctx) {
    if (!type) return false;
    return type->isa<FuncTypeAST>();
}

TypeAST* TypeChecker::unify(TypeAST* a, TypeAST* b, SemanticContext& ctx) {
    if (!a && !b) return nullptr;
    if (!a) return b;
    if (!b) return a;

    if (isAssignable(a, b, ctx)) return b;
    if (isAssignable(b, a, ctx)) return a;
    return nullptr;
}

bool TypeChecker::primitiveWidening(PrimitiveKind from, PrimitiveKind to) {
    if (from == to) return true;

    // Signed integer widening chain
    if (from == PrimitiveKind::Byte || from == PrimitiveKind::Int8) {
        switch (to) {
            case PrimitiveKind::Short: case PrimitiveKind::Int16:
            case PrimitiveKind::Int: case PrimitiveKind::Int32:
            case PrimitiveKind::Long: case PrimitiveKind::Int64:
                return true;
            default: break;
        }
    }
    if (from == PrimitiveKind::Short || from == PrimitiveKind::Int16) {
        switch (to) {
            case PrimitiveKind::Int: case PrimitiveKind::Int32:
            case PrimitiveKind::Long: case PrimitiveKind::Int64:
                return true;
            default: break;
        }
    }
    if (from == PrimitiveKind::Int || from == PrimitiveKind::Int32) {
        switch (to) {
            case PrimitiveKind::Long: case PrimitiveKind::Int64:
            case PrimitiveKind::Float: case PrimitiveKind::Double: case PrimitiveKind::Decimal:
                return true;
            default: break;
        }
    }
    if (from == PrimitiveKind::Long || from == PrimitiveKind::Int64) {
        switch (to) {
            case PrimitiveKind::Float: case PrimitiveKind::Double: case PrimitiveKind::Decimal:
                return true;
            default: break;
        }
    }

    // Unsigned integer widening chain
    if (from == PrimitiveKind::Ubyte || from == PrimitiveKind::Uint8) {
        switch (to) {
            case PrimitiveKind::Ushort: case PrimitiveKind::Uint16:
            case PrimitiveKind::Uint: case PrimitiveKind::Uint32:
            case PrimitiveKind::Ulong: case PrimitiveKind::Uint64:
                return true;
            default: break;
        }
    }
    if (from == PrimitiveKind::Ushort || from == PrimitiveKind::Uint16) {
        switch (to) {
            case PrimitiveKind::Uint: case PrimitiveKind::Uint32:
            case PrimitiveKind::Ulong: case PrimitiveKind::Uint64:
                return true;
            default: break;
        }
    }
    if (from == PrimitiveKind::Uint || from == PrimitiveKind::Uint32) {
        switch (to) {
            case PrimitiveKind::Ulong: case PrimitiveKind::Uint64:
                return true;
            default: break;
        }
    }

    // NOTE: Signed → unsigned widening is NOT allowed as implicit conversion.
    // The programmer must use an explicit cast or a `from` conversion.
    // The branch that previously allowed this has been removed.

    // Floating-point widening
    if (from == PrimitiveKind::Float) {
        if (to == PrimitiveKind::Double || to == PrimitiveKind::Decimal) return true;
    }
    if (from == PrimitiveKind::Double) {
        if (to == PrimitiveKind::Decimal) return true;
    }

    // Integer to floating-point (widening)
    if ((from == PrimitiveKind::Byte || from == PrimitiveKind::Int8 ||
         from == PrimitiveKind::Short || from == PrimitiveKind::Int16 ||
         from == PrimitiveKind::Int || from == PrimitiveKind::Int32 ||
         from == PrimitiveKind::Long || from == PrimitiveKind::Int64 ||
         from == PrimitiveKind::Ubyte || from == PrimitiveKind::Uint8 ||
         from == PrimitiveKind::Ushort || from == PrimitiveKind::Uint16 ||
         from == PrimitiveKind::Uint || from == PrimitiveKind::Uint32 ||
         from == PrimitiveKind::Ulong || from == PrimitiveKind::Uint64) &&
        (to == PrimitiveKind::Float || to == PrimitiveKind::Double || to == PrimitiveKind::Decimal)) {
        return true;
    }

    return false;
}


// ─────────────────────────────────────────────────────────────────────────────
// Type Queries
// ─────────────────────────────────────────────────────────────────────────────

bool TypeChecker::isIntegerType(TypeAST* type, SemanticContext& ctx) {
    if (!type) return false;
    if (!type->isa<PrimitiveTypeAST>()) return false;
    auto* prim = type->as<PrimitiveTypeAST>();
    switch (prim->primitiveKind) {
        case PrimitiveKind::Byte: case PrimitiveKind::Int8:
        case PrimitiveKind::Short: case PrimitiveKind::Int16:
        case PrimitiveKind::Int: case PrimitiveKind::Int32:
        case PrimitiveKind::Long: case PrimitiveKind::Int64:
        case PrimitiveKind::Ubyte: case PrimitiveKind::Uint8:
        case PrimitiveKind::Ushort: case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint: case PrimitiveKind::Uint32:
        case PrimitiveKind::Ulong: case PrimitiveKind::Uint64:
            return true;
        default:
            return false;
    }
}

bool TypeChecker::isBooleanCompatible(TypeAST* type, SemanticContext& ctx) {
    if (!type) return false;
    if (!type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
}

bool TypeChecker::isNullable(TypeAST* type, SemanticContext& ctx) {
    if (!type) return false;
    
    if (type->isa<NullableTypeAST>()) return true;
    if (type->isa<FuncTypeAST>()) return type->as<FuncTypeAST>()->isNullable();
    if (type->isa<PtrTypeAST>()) return true;   // raw pointers are always nullable
    
    return false;
}

bool TypeChecker::isBoolOrNullable(TypeAST* type, SemanticContext& ctx) {
    if (!type) return false;
    if (type->isa<PrimitiveTypeAST>()) {
        return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
    }
    if (type->isa<NullableTypeAST>()) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constant Evaluation
// ─────────────────────────────────────────────────────────────────────────────

bool TypeChecker::getConstantIntValue(ExprAST* expr, int64_t& outValue, SemanticContext& ctx) {
    if (!expr) return false;

    if (expr->isa<LiteralExprAST>()) {
        auto* lit = expr->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Int || lit->kind == LiteralKind::Hex ||
            lit->kind == LiteralKind::Binary) {

            std::string_view val = ctx.pool.lookup(lit->value);
            const char* str = val.data();
            char* endptr = nullptr;
            int64_t result = 0;

            if (val.find("0x") == 0 || val.find("0X") == 0) {
                result = std::strtoll(str, &endptr, 16);
            } else if (val.find("0b") == 0 || val.find("0B") == 0) {
                result = 0;
                for (size_t i = 2; i < val.length(); ++i) {
                    char c = val[i];
                    if (c == '_') continue;
                    if (c == '0') result = (result << 1);
                    else if (c == '1') result = (result << 1) | 1;
                    else return false;
                }
                endptr = const_cast<char*>(str + val.length());
            } else {
                result = std::strtoll(str, &endptr, 10);
            }

            if (endptr != str + val.length()) {
                std::string cleaned;
                cleaned.reserve(val.length());
                for (char c : val) if (c != '_') cleaned += c;
                const char* cleanedStr = cleaned.c_str();
                char* cleanedEndptr = nullptr;
                result = std::strtoll(cleanedStr, &cleanedEndptr, 10);
                if (cleanedEndptr != cleanedStr + cleaned.length()) return false;
            }

            outValue = result;
            return true;
        }
    }

    if (expr->isa<UnaryExprAST>()) {
        auto* unary = expr->as<UnaryExprAST>();
        if (unary->op == UnaryOp::Neg) {
            int64_t innerValue;
            if (getConstantIntValue(unary->operand.get(), innerValue, ctx)) {
                outValue = -innerValue;
                return true;
            }
        }
    }

    if (expr->isa<BinaryExprAST>()) {
        auto* binary = expr->as<BinaryExprAST>();
        int64_t leftVal, rightVal;
        if (getConstantIntValue(binary->left.get(), leftVal, ctx) &&
            getConstantIntValue(binary->right.get(), rightVal, ctx)) {
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

bool TypeChecker::isValidArrayIndex(ExprAST* indexExpr, const SourceLocation& loc, SemanticContext& ctx) {
    if (!indexExpr) {
        ctx.error(loc, DiagCode::E2034, "array index expression is null");
        return false;
    }

    TypeAST* indexType = static_cast<TypeAST*>(indexExpr->resolvedType);
    if (!isIntegerType(indexType, ctx)) {
        ctx.error(indexExpr->loc, DiagCode::E2002,
                  "array index must be an integer type (got '", 
                  LucDebug::kindToString(indexType ? indexType->kind : ASTKind::Unknown), "')");
        return false;
    }

    int64_t constValue;
    if (getConstantIntValue(indexExpr, constValue, ctx)) {
        if (constValue < 0) {
            ctx.error(indexExpr->loc, DiagCode::E2034,
                      "array index cannot be negative (got ", std::to_string(constValue), ")");
            return false;
        }
    }
    return true;
}

bool TypeChecker::isValidSliceBound(ExprAST* boundExpr, const std::string& boundName,
                                     const SourceLocation& loc, SemanticContext& ctx) {
    if (!boundExpr) {
        ctx.error(loc, DiagCode::E2034, "slice ", boundName, " bound expression is null");
        return false;
    }

    TypeAST* boundType = static_cast<TypeAST*>(boundExpr->resolvedType);
    if (!isIntegerType(boundType, ctx)) {
        ctx.error(boundExpr->loc, DiagCode::E2002,
                  "slice ", boundName, " bound must be an integer type");
        return false;
    }

    int64_t constValue;
    if (!getConstantIntValue(boundExpr, constValue, ctx)) {
        ctx.error(boundExpr->loc, DiagCode::E2034,
                  "slice ", boundName, " bound must be a constant expression");
        return false;
    }

    if (constValue < 0) {
        ctx.error(boundExpr->loc, DiagCode::E2034,
                  "slice ", boundName, " bound cannot be negative (got ", std::to_string(constValue), ")");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Custom Casting (from blocks)
// ─────────────────────────────────────────────────────────────────────────────

Symbol* TypeChecker::isFromCastable(TypeAST* src, TypeAST* target, SemanticContext& ctx) {
    if (!src || !target) return nullptr;
    if (!ctx.symbols) return nullptr;

    std::string targetMangled = NameMangler::mangleType(target, ctx.pool, ctx.symbols);
    std::string prefix = NameMangler::getFromPrefix(targetMangled);

    std::vector<Symbol*> candidates = ctx.symbols->findSymbolsByPrefix(prefix, ctx.pool);

    for (Symbol* sym : candidates) {
        if (!sym || sym->kind != SymbolKind::Casting) continue;
        if (!sym->decl || !sym->decl->isa<FromEntryAST>()) continue;

        auto* entry = sym->decl->as<FromEntryAST>();

        if (entry->sig.groupCount() == 0) continue;
        auto firstGroup = entry->sig.getGroup(0);
        if (firstGroup.empty()) continue;
        
        // Resolve parameter type for comparison
        TypeAST* firstParamType = ctx.dispatcher ? ctx.dispatcher->resolveType(firstGroup[0]->type.get()) : firstGroup[0]->type.get();
        if (!firstParamType) continue;

        if (isAssignable(src, firstParamType, ctx)) {
            return sym;
        }
    }

    return nullptr;
}

Symbol* TypeChecker::isFromCastableMulti(const std::vector<TypeAST*>& srcTypes, TypeAST* target, SemanticContext& ctx) {
    if (!target || !ctx.symbols) return nullptr;
    std::string targetMangled = NameMangler::mangleType(target, ctx.pool, ctx.symbols);
    std::string prefix = NameMangler::getFromPrefix(targetMangled);
    std::vector<Symbol*> candidates = ctx.symbols->findSymbolsByPrefix(prefix, ctx.pool);
    for (Symbol* sym : candidates) {
        if (!sym || sym->kind != SymbolKind::Casting) continue;
        if (!sym->decl || !sym->decl->isa<FromEntryAST>()) continue;
        auto* entry = sym->decl->as<FromEntryAST>();
        if (entry->sig.groupCount() == 0) continue;
        auto firstGroup = entry->sig.getGroup(0);
        if (firstGroup.size() != srcTypes.size()) continue;
        bool match = true;
        for (size_t i = 0; i < firstGroup.size(); ++i) {
            TypeAST* paramType = firstGroup[i]->type.get();
            if (!paramType) { match = false; break; }
            if (!isAssignable(srcTypes[i], paramType, ctx)) {
                match = false;
                break;
            }
        }
        if (match) return sym;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool TypeChecker::isValueComparable(TypeAST* type, SemanticContext& ctx) {
    if (!type) return false;

    if (type->isa<PrimitiveTypeAST>()) return true;
    if (type->isa<NullableTypeAST>()) return true;

    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        if (ctx.symbols) {
            Symbol* sym = ctx.symbols->lookup(named->name);
            if (sym && sym->kind == SymbolKind::Enum) return true;
            if (sym && sym->kind == SymbolKind::Struct) return false;
        }
        return false;
    }

    if (type->isa<FuncTypeAST>()) return false;
    if (type->isa<ArrayTypeAST>()) return false;
    if (type->isa<RefTypeAST>() || type->isa<PtrTypeAST>()) return false;

    return false;
}

bool TypeChecker::isReferenceComparable(TypeAST* type, SemanticContext& ctx) {
    if (!type) return false;

    if (type->isa<RefTypeAST>()) return true;

    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        if (ctx.symbols) {
            Symbol* sym = ctx.symbols->lookup(named->name);
            if (sym && sym->kind == SymbolKind::Struct) return true;
        }
        return false;
    }

    if (type->isa<NullableTypeAST>()) {
        return isReferenceComparable(type->as<NullableTypeAST>()->inner.get(), ctx);
    }

    return false;
}
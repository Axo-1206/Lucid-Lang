/**
 * @file SemanticExpr.cpp
 *
 * @nutshell Validates exactly how expressions interact with each other in code.
 *
 * @responsibility Phase 3b of semantic analysis: walks every expression node, resolves
 *   its type, writes resolvedType onto the node, and enforces all operator and
 *   language-level expression rules.
 *
 * @logic
 *   checkExpr dispatches to a handler per ASTKind. Each handler returns the TypeAST*
 *   that the expression evaluates to, or nullptr if an irrecoverable error occurred.
 *   The returned pointer is also written onto node->resolvedType for the Annotator.
 *
 * @related SemanticAnalyzer.cpp, SemanticDecl.cpp, SemanticStmt.cpp
 */

#include "registry/IntrinsicRegistry.hpp"
#include "registry/BuiltinMethodRegistry.hpp"
#include "ast/ExprAST.hpp"
#include "ast/BaseAST.hpp"
#include "ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "header/SymbolTable.hpp"
#include "header/TypeResolver.hpp"
#include "header/SemanticContext.hpp"
#include "header/SemanticChecker.hpp"
#include "header/NameMangler.hpp"

#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations (statement checker, needed for expression bodies)
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SemanticContext& ctx, TypeAST* expectedReturn);

// ─────────────────────────────────────────────────────────────────────────────
// Local helper: get the resolved type of an expression (or nullptr)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* getExprType(ExprAST* expr) {
    return expr ? static_cast<TypeAST*>(expr->resolvedType) : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// LiteralExprAST – scalar literals (integers, floats, strings, bools, nil)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkLiteralExpr(LiteralExprAST& node, SemanticContext& ctx) {
    TypeAST* result = nullptr;
    switch (node.kind) {
        case LiteralKind::Int:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int).get();
            break;
        case LiteralKind::Float:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Float).get();
            break;
        case LiteralKind::String:
        case LiteralKind::RawString:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::String).get();
            break;
        case LiteralKind::Char:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Char).get();
            break;
        case LiteralKind::Hex:
        case LiteralKind::Binary:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int).get();
            break;
        case LiteralKind::True:
        case LiteralKind::False:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).get();
            break;
        case LiteralKind::Nil:
            // nil has no type by itself; will be unified later
            result = nullptr;
            break;
        default:
            ctx.error(node.loc, DiagCode::E3002, {"invalid literal kind"});
            return nullptr;
    }
    node.resolvedType = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// IdentifierExprAST – variable/function/type name lookup
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIdentifierExpr(IdentifierExprAST& node, SemanticContext& ctx) {
    Symbol* sym = ctx.symbols.lookup(node.name);
    if (!sym) {
        ctx.error(node.loc, DiagCode::E3001, {"undefined identifier '" + std::string(ctx.pool.lookup(node.name)) + "'"});
        return nullptr;
    }

    TypeAST* type = sym->type;
    if (!type) {
        // If the symbol has no type yet, try to resolve it from the declaration
        if (sym->decl) {
            if (auto* varDecl = sym->decl->as<VarDeclAST>()) {
                type = ctx.resolver.resolveType(varDecl->type.get());
                sym->type = type;
            } else if (auto* funcDecl = sym->decl->as<FuncDeclAST>()) {
                auto* ft = ctx.resolver.cloneFuncSignature(funcDecl->sig, funcDecl->loc);
                type = ctx.resolver.resolveType(ft);
                sym->type = type;
            }
        }
        if (!type) {
            ctx.error(node.loc, DiagCode::E3001,
                {"identifier '" + std::string(ctx.pool.lookup(node.name)) +
                         "' has no known type"});
            return nullptr;
        }
    }

    node.resolvedType = type;
    if (sym->declKw == DeclKeyword::Const) {
        node.isConst = true;
    }
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// BinaryExprAST – infix operators (arithmetic, comparison, logical, bitwise)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkBinaryExpr(BinaryExprAST& node, SemanticContext& ctx) {
    TypeAST* leftType = checkExpr(node.left.get(), ctx);
    TypeAST* rightType = checkExpr(node.right.get(), ctx);
    if (!leftType || !rightType) return nullptr;

    auto bothNumeric = [&]() -> bool {
        if (!leftType->isa<PrimitiveTypeAST>() || !rightType->isa<PrimitiveTypeAST>())
            return false;
        auto lp = leftType->as<PrimitiveTypeAST>()->primitiveKind;
        auto rp = rightType->as<PrimitiveTypeAST>()->primitiveKind;
        return (lp == PrimitiveKind::Int || lp == PrimitiveKind::Float ||
                lp == PrimitiveKind::Double || lp == PrimitiveKind::Decimal) &&
               (rp == PrimitiveKind::Int || rp == PrimitiveKind::Float ||
                rp == PrimitiveKind::Double || rp == PrimitiveKind::Decimal);
    };

    // Arithmetic operators
    if (node.op == BinaryOp::Add || node.op == BinaryOp::Sub ||
        node.op == BinaryOp::Mul || node.op == BinaryOp::Div ||
        node.op == BinaryOp::Pow || node.op == BinaryOp::Mod) {
        if (!bothNumeric()) {
            ctx.error(node.loc, DiagCode::E3002, {"arithmetic operator requires numeric operands"});
            return nullptr;
        }
        TypeAST* unified = ctx.checker.unify(leftType, rightType);
        if (!unified) {
            ctx.error(node.loc, DiagCode::E3002, {"incompatible numeric types for arithmetic operation"});
            return nullptr;
        }
        node.resolvedType = unified;
        return unified;
    }

    // Comparison operators (==, !=, <, >, <=, >=)
    if (node.op == BinaryOp::Eq || node.op == BinaryOp::Ne ||
        node.op == BinaryOp::Lt || node.op == BinaryOp::Gt ||
        node.op == BinaryOp::Le || node.op == BinaryOp::Ge) {
        if (!ctx.checker.isValueComparable(leftType, &ctx.symbols) ||
            !ctx.checker.isValueComparable(rightType, &ctx.symbols)) {
            ctx.error(node.loc, DiagCode::E3011, {"value comparison not allowed on this type"});
            return nullptr;
        }
        if (!ctx.checker.isAssignable(leftType, rightType) &&
            !ctx.checker.isAssignable(rightType, leftType)) {
            ctx.error(node.loc, DiagCode::E3002, {"operands of comparison must be compatible types"});
            return nullptr;
        }
        TypeAST* boolType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).get();
        node.resolvedType = boolType;
        return boolType;
    }

    // Reference equality (===, !==)
    if (node.op == BinaryOp::RefEq) {
        if (!ctx.checker.isReferenceComparable(leftType) ||
            !ctx.checker.isReferenceComparable(rightType)) {
            ctx.error(node.loc, DiagCode::E3002, {"reference equality (===) only allowed on structs and references"});
            return nullptr;
        }
        TypeAST* boolType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).get();
        node.resolvedType = boolType;
        return boolType;
    }

    // Logical operators (
    if (node.op == BinaryOp::And || node.op == BinaryOp::Or) {
        if (!ctx.checker.isBoolOrNullable(leftType) ||
            !ctx.checker.isBoolOrNullable(rightType)) {
            ctx.error(node.loc, DiagCode::E3002, {"logical operators require bool or nullable operands"});
            return nullptr;
        }
        TypeAST* boolType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).get();
        node.resolvedType = boolType;
        return boolType;
    }

    // Bitwise operators (&&, ||, ~^, <<, >>)
    if (node.op == BinaryOp::BitAnd || node.op == BinaryOp::BitOr ||
        node.op == BinaryOp::BitXor || node.op == BinaryOp::Shl ||
        node.op == BinaryOp::Shr) {
        if (!ctx.checker.isIntegerType(leftType) || !ctx.checker.isIntegerType(rightType)) {
            ctx.error(node.loc, DiagCode::E3002, {"bitwise operators require integer operands"});
            return nullptr;
        }
        node.resolvedType = leftType;
        return leftType;
    }

    ctx.error(node.loc, DiagCode::E3002, {"unsupported binary operator"});
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// UnaryExprAST – prefix operators (-, not, ~, &)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkUnaryExpr(UnaryExprAST& node, SemanticContext& ctx) {
    TypeAST* operandType = checkExpr(node.operand.get(), ctx);
    if (!operandType) return nullptr;

    switch (node.op) {
        case UnaryOp::Neg:
            if (!operandType->isa<PrimitiveTypeAST>() ||
                (operandType->as<PrimitiveTypeAST>()->primitiveKind != PrimitiveKind::Int &&
                 operandType->as<PrimitiveTypeAST>()->primitiveKind != PrimitiveKind::Float &&
                 operandType->as<PrimitiveTypeAST>()->primitiveKind != PrimitiveKind::Double &&
                 operandType->as<PrimitiveTypeAST>()->primitiveKind != PrimitiveKind::Decimal)) {
                ctx.error(node.loc, DiagCode::E3002, {"negation (-) requires numeric operand"});
                return nullptr;
            }
            node.resolvedType = operandType;
            return operandType;

        case UnaryOp::Not:
            if (!ctx.checker.isBoolOrNullable(operandType)) {
                ctx.error(node.loc, DiagCode::E3002, {"logical not requires bool or nullable operand"});
                return nullptr;
            }
            {
                TypeAST* boolType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).get();
                node.resolvedType = boolType;
                return boolType;
            }

        case UnaryOp::BitNot:
            if (!ctx.checker.isIntegerType(operandType)) {
                ctx.error(node.loc, DiagCode::E3002, {"bitwise not (~) requires integer operand"});
                return nullptr;
            }
            node.resolvedType = operandType;
            return operandType;

        case UnaryOp::Ref:
            {
                TypeAST* refType = ctx.arena.make<RefTypeAST>(TypePtr(operandType)).get();
                node.resolvedType = refType;
                return refType;
            }

        default:
            ctx.error(node.loc, DiagCode::E3002, {"unsupported unary operator"});
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CallExprAST – function call, constructor call, or method call
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkCallExpr(CallExprAST& node, SemanticContext& ctx) {
    TypeAST* calleeType = checkExpr(node.callee.get(), ctx);
    if (!calleeType) return nullptr;

    if (!ctx.checker.isCallable(calleeType)) {
        ctx.error(node.loc, DiagCode::E3002, {"callee is not a function"});
        return nullptr;
    }

    FuncTypeAST* funcType = nullptr;
    if (calleeType->isa<FuncTypeAST>()) {
        funcType = calleeType->as<FuncTypeAST>();
    } else if (calleeType->isa<NullableTypeAST>()) {
        auto* nullable = calleeType->as<NullableTypeAST>();
        if (nullable->inner->isa<FuncTypeAST>()) {
            funcType = nullable->inner->as<FuncTypeAST>();
            ctx.warning(node.loc, DiagCode::W3003, {"calling nullable function; will panic if nil"});
        } else {
            ctx.error(node.loc, DiagCode::E3002, {"nullable value is not a function"});
            return nullptr;
        }
    } else {
        ctx.error(node.loc, DiagCode::E3002, {"callee is not a function type"});
        return nullptr;
    }

    // Get the first parameter group (Luc functions have at least one group)
    if (funcType->sig.groupCount() == 0) {
        ctx.error(node.loc, DiagCode::E3002, {"function has no parameter groups"});
        return nullptr;
    }
    auto firstGroup = funcType->sig.getGroup(0);
    if (firstGroup.size() != node.args.size()) {
        ctx.error(node.loc, DiagCode::E3003,
                {"argument count mismatch: expected " + std::to_string(firstGroup.size()) +
                 ", got " + std::to_string(node.args.size())});
        return nullptr;
    }

    for (size_t i = 0; i < firstGroup.size(); ++i) {
        TypeAST* argType = checkExpr(node.args[i].get(), ctx);
        if (!argType) return nullptr;
        TypeAST* paramType = firstGroup[i]->type.get();
        if (!ctx.checker.isAssignable(argType, paramType)) {
            ctx.error(node.args[i]->loc, DiagCode::E3002,
                      {"argument " + std::to_string(i + 1) + " type mismatch"});
            return nullptr;
        }
    }

    if (funcType->sig.returnTypes.empty()) {
        node.resolvedType = nullptr;
        return nullptr;
    }
    TypeAST* retType = funcType->sig.returnTypes[0].get();
    node.resolvedType = retType;
    return retType;
}

// ─────────────────────────────────────────────────────────────────────────────
// IndexExprAST – array/slice element access or slice expression
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIndexExpr(IndexExprAST& node, SemanticContext& ctx) {
    TypeAST* targetType = checkExpr(node.target.get(), ctx);
    if (!targetType) return nullptr;

    TypeAST* elementType = nullptr;
    if (targetType->isa<FixedArrayTypeAST>()) {
        elementType = targetType->as<FixedArrayTypeAST>()->element.get();
    } else if (targetType->isa<SliceTypeAST>()) {
        elementType = targetType->as<SliceTypeAST>()->element.get();
    } else if (targetType->isa<DynamicArrayTypeAST>()) {
        elementType = targetType->as<DynamicArrayTypeAST>()->element.get();
    } else if (targetType->isa<NullableTypeAST>()) {
        ctx.error(node.loc, DiagCode::E3002, {"cannot index nullable value"});
        return nullptr;
    } else {
        ctx.error(node.loc, DiagCode::E3002, {"index operator only allowed on array types"});
        return nullptr;
    }

    if (node.kind == IndexKind::Element) {
        if (!ctx.checker.isValidArrayIndex(node.index.get(), ctx.currentFile, ctx.dc, node.loc))
            return nullptr;
        node.resolvedType = elementType;
        return elementType;
    } else if (node.kind == IndexKind::Slice) {
        if (!ctx.checker.isValidSliceBound(node.index.get(), "start", ctx.currentFile, ctx.dc, node.loc) ||
            !ctx.checker.isValidSliceBound(node.sliceEnd.get(), "end", ctx.currentFile, ctx.dc, node.loc))
            return nullptr;
        if (!node.sliceType) {
            node.sliceType = ctx.arena.make<SliceTypeAST>(TypePtr(elementType));
        }
        TypeAST* sliceRes = node.sliceType.get();
        node.resolvedType = sliceRes;
        return sliceRes;
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// FieldAccessExprAST – struct field or enum variant access ('.')
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SemanticContext& ctx) {
    TypeAST* objectType = checkExpr(node.object.get(), ctx);
    if (!objectType) return nullptr;

    if (objectType->isa<NullableTypeAST>()) {
        ctx.error(node.loc, DiagCode::E3002, {"cannot access field on nullable value; use ?. chain"});
        return nullptr;
    }
    if (!objectType->isa<NamedTypeAST>()) {
        ctx.error(node.loc, DiagCode::E3002, {"field access only allowed on structs and enums"});
        return nullptr;
    }

    InternedString typeName = objectType->as<NamedTypeAST>()->name;
    Symbol* sym = ctx.symbols.lookup(typeName);
    if (!sym) {
        ctx.error(node.loc, DiagCode::E3001, {"type '" + std::string(ctx.pool.lookup(typeName)) + "' not found"});
        return nullptr;
    }

    // Enum variant access: EnumName.variantName
    if (sym->kind == SymbolKind::Enum) {
        std::string mangled = NameMangler::mangleEnumVariant(ctx.pool.lookup(typeName),
                                                             ctx.pool.lookup(node.field));
        Symbol* variantSym = ctx.symbols.lookup(ctx.pool.intern(mangled));
        if (!variantSym) {
            ctx.error(node.loc, DiagCode::E3001,
                {"enum '" + std::string(ctx.pool.lookup(typeName)) +
                         "' has no variant '" + std::string(ctx.pool.lookup(node.field)) + "'"});
            return nullptr;
        }
        TypeAST* variantType = variantSym->type;
        if (!variantType) {
            variantType = objectType;
            variantSym->type = variantType;
        }
        node.resolvedType = variantType;
        return variantType;
    }

    // Struct field access
    if (sym->kind == SymbolKind::Struct) {
        auto* structDecl = sym->decl->as<StructDeclAST>();
        for (auto& field : structDecl->fields) {
            if (field->name == node.field) {
                if (!field->type) {
                    ctx.error(node.loc, DiagCode::E3002, {"field has no type"});
                    return nullptr;
                }
                TypeAST* fieldType = field->type.get();
                node.resolvedType = fieldType;
                return fieldType;
            }
        }
        ctx.error(node.loc, DiagCode::E3001,
            {"struct '" + std::string(ctx.pool.lookup(typeName)) +
                    "' has no field '" + std::string(ctx.pool.lookup(node.field)) + "'"});
        return nullptr;
    }

    ctx.error(node.loc, DiagCode::E3002, {"field access only allowed on structs and enums"});
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// StructLiteralExprAST – construction of a struct value
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkStructLiteralExpr(StructLiteralExprAST& node, SemanticContext& ctx) {
    Symbol* sym = ctx.symbols.lookup(node.typeName);
    if (!sym || sym->kind != SymbolKind::Struct) {
        ctx.error(node.loc, DiagCode::E3001,
                {"unknown struct type '" + std::string(ctx.pool.lookup(node.typeName)) + "'"});
        return nullptr;
    }

    auto* structDecl = sym->decl->as<StructDeclAST>();
    if (!node.genericArgs.empty()) {
        auto* instantiated = ctx.arena.make<NamedTypeAST>(node.typeName).get();
        instantiated->genericArgs = std::move(node.genericArgs);
        for (auto& arg : instantiated->genericArgs) {
            ctx.resolver.resolveType(arg.get());
        }
        node.instantiatedType = ASTPtr<NamedTypeAST>(instantiated); // wrap in ASTPtr
    }

    TypeAST* structType = node.instantiatedType ? node.instantiatedType.get()
                         : structDecl->selfType.get();
    if (!structType) {
        ctx.error(node.loc, DiagCode::E3002, {"struct type not resolved"});
        return nullptr;
    }

    std::unordered_map<InternedString, bool> fieldSeen;
    for (auto& init : node.inits) {
        if (!init) continue;
        bool found = false;
        for (auto& field : structDecl->fields) {
            if (field->name == init->name) {
                found = true;
                TypeAST* initType = checkExpr(init->value.get(), ctx);
                if (!initType) return nullptr;
                if (!ctx.checker.isAssignable(initType, field->type.get())) {
                    ctx.error(init->value->loc, DiagCode::E3002,
                        {"initialiser type mismatch for field '" +
                                 std::string(ctx.pool.lookup(init->name)) + "'"});
                    return nullptr;
                }
                fieldSeen[init->name] = true;
                break;
            }
        }
        if (!found) {
            ctx.error(init->loc, DiagCode::E3001,
                {"struct '" + std::string(ctx.pool.lookup(node.typeName)) +
                         "' has no field '" + std::string(ctx.pool.lookup(init->name)) + "'"});
            return nullptr;
        }
    }

    for (auto& field : structDecl->fields) {
        if (!fieldSeen[field->name] && !field->defaultVal) {
            ctx.error(node.loc, DiagCode::E3001,
                {"missing initialiser for field '" +
                         std::string(ctx.pool.lookup(field->name)) + "'"});
            return nullptr;
        }
    }

    node.resolvedType = structType;
    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// ArrayLiteralExprAST – array literal (e.g., [1,2,3])
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST& node, SemanticContext& ctx) {
    if (node.elements.empty()) {
        node.resolvedType = nullptr;
        return nullptr;
    }

    TypeAST* firstType = checkExpr(node.elements[0].get(), ctx);
    if (!firstType) return nullptr;
    for (size_t i = 1; i < node.elements.size(); ++i) {
        TypeAST* elemType = checkExpr(node.elements[i].get(), ctx);
        if (!elemType) return nullptr;
        TypeAST* unified = ctx.checker.unify(firstType, elemType);
        if (!unified) {
            ctx.error(node.loc, DiagCode::E3002, {"array elements have incompatible types"});
            return nullptr;
        }
        firstType = unified;
    }
    node.resolvedType = firstType;
    return firstType;
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeConvExprAST – explicit type conversion (safe cast or unsafe reinterpret)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkTypeConvExpr(TypeConvExprAST& node, SemanticContext& ctx) {
    TypeAST* srcType = checkExpr(node.expr.get(), ctx);
    if (!srcType) return nullptr;

    TypeAST* targetType = ctx.resolver.resolveType(node.targetType.get());
    if (!targetType) {
        ctx.error(node.loc, DiagCode::E3002, {"cannot resolve target type for conversion"});
        return nullptr;
    }

    if (node.isUnsafe) {
        if (!ctx.insideExtern) {
            ctx.error(node.loc, DiagCode::E3002, {"unsafe type conversion (*T(...)) only allowed in @extern functions"});
            return nullptr;
        }
        node.resolvedType = targetType;
        return targetType;
    }

    if (srcType->isa<PrimitiveTypeAST>() && targetType->isa<PrimitiveTypeAST>()) {
        if (ctx.checker.primitiveWidening(srcType->as<PrimitiveTypeAST>()->primitiveKind,
                                          targetType->as<PrimitiveTypeAST>()->primitiveKind) ||
            ctx.checker.isAssignable(srcType, targetType)) {
            node.resolvedType = targetType;
            return targetType;
        }
        ctx.error(node.loc, DiagCode::E3008, {"cannot safely convert between these primitive types"});
        return nullptr;
    }

    if (srcType->isa<NamedTypeAST>() && targetType->isa<PrimitiveTypeAST>()) {
        Symbol* sym = ctx.symbols.lookup(srcType->as<NamedTypeAST>()->name);
        if (sym && sym->kind == SymbolKind::Enum) {
            node.resolvedType = targetType;
            return targetType;
        }
    }

    Symbol* fromEntry = ctx.checker.isFromCastable(srcType, targetType, &ctx.symbols);
    if (fromEntry) {
        node.resolvedType = targetType;
        return targetType;
    }

    ctx.error(node.loc, DiagCode::E3008, {"no valid conversion from source type to target type"});
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// BehaviorAccessExprAST – Type:method reference
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkBehaviorAccessExpr(BehaviorAccessExprAST& node, SemanticContext& ctx) {
    Symbol* sym = ctx.symbols.lookup(node.typeName);
    if (!sym || sym->kind != SymbolKind::Struct) {
        ctx.error(node.loc, DiagCode::E3001,
            {"unknown type '" + std::string(ctx.pool.lookup(node.typeName)) +
                     "' for method access"});
        return nullptr;
    }

    std::string mangled = NameMangler::mangleMethod(ctx.pool.lookup(node.typeName),
                                                    ctx.pool.lookup(node.method));
    Symbol* methodSym = ctx.symbols.lookup(ctx.pool.intern(mangled));
    if (!methodSym || (methodSym->kind != SymbolKind::Method && methodSym->kind != SymbolKind::Func)) {
        ctx.error(node.loc, DiagCode::E3001,
            {"type '" + std::string(ctx.pool.lookup(node.typeName)) +
                     "' has no method '" + std::string(ctx.pool.lookup(node.method)) + "'"});
        return nullptr;
    }

    TypeAST* methodType = methodSym->type;
    if (!methodType) {
        if (auto* methodDecl = methodSym->decl->as<MethodDeclAST>()) {
            auto* ft = ctx.resolver.cloneFuncSignature(methodDecl->sig, methodDecl->loc);
            methodType = ctx.resolver.resolveType(ft);
            methodSym->type = methodType;
        }
    }

    if (!methodType) {
        ctx.error(node.loc, DiagCode::E3002, {"method type could not be resolved"});
        return nullptr;
    }

    node.resolvedType = methodType;
    node.isBehaviorMember = true;
    return methodType;
}

// ─────────────────────────────────────────────────────────────────────────────
// NullableChainExprAST – ?. chain (terminated by ?? elsewhere)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkNullableChainExpr(NullableChainExprAST& node, SemanticContext& ctx) {
    TypeAST* rootType = checkExpr(node.object.get(), ctx);
    if (!rootType) return nullptr;
    if (!ctx.checker.isNullable(rootType)) {
        ctx.error(node.loc, DiagCode::E3002, {"nullable chain (?.) requires nullable left‑hand side"});
        return nullptr;
    }

    TypeAST* current = rootType;
    for (auto& fieldName : node.steps) {
        if (!current->isa<NullableTypeAST>()) {
            ctx.error(node.loc, DiagCode::E3002, {"internal error: nullable chain expected nullable type"});
            return nullptr;
        }
        TypeAST* inner = current->as<NullableTypeAST>()->inner.get();
        if (!inner->isa<NamedTypeAST>()) {
            ctx.error(node.loc, DiagCode::E3002, {"nullable chain only supports struct fields"});
            return nullptr;
        }
        Symbol* structSym = ctx.symbols.lookup(inner->as<NamedTypeAST>()->name);
        if (!structSym || structSym->kind != SymbolKind::Struct) {
            ctx.error(node.loc, DiagCode::E3001, {"type not found for nullable chain"});
            return nullptr;
        }
        auto* structDecl = structSym->decl->as<StructDeclAST>();
        TypeAST* fieldType = nullptr;
        for (auto& field : structDecl->fields) {
            if (field->name == fieldName) {
                fieldType = field->type.get();
                break;
            }
        }
        if (!fieldType) {
            ctx.error(node.loc, DiagCode::E3001, {"field '" + std::string(ctx.pool.lookup(fieldName)) + "' not found"});
            return nullptr;
        }
        current = ctx.arena.make<NullableTypeAST>(TypePtr(fieldType)).get();
    }

    node.resolvedType = current;
    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// NullCoalesceExprAST – left ?? right
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkNullCoalesceExpr(NullCoalesceExprAST& node, SemanticContext& ctx) {
    TypeAST* leftType = checkExpr(node.value.get(), ctx);
    if (!leftType) return nullptr;
    TypeAST* rightType = checkExpr(node.fallback.get(), ctx);
    if (!rightType) return nullptr;

    if (!ctx.checker.isNullable(leftType)) {
        ctx.error(node.loc, DiagCode::E3002, {"left side of ?? must be nullable"});
        return nullptr;
    }

    TypeAST* innerLeft = leftType->isa<NullableTypeAST>()
                         ? leftType->as<NullableTypeAST>()->inner.get()
                         : leftType;
    if (!ctx.checker.isAssignable(innerLeft, rightType) &&
        !ctx.checker.isAssignable(rightType, innerLeft)) {
        ctx.error(node.loc, DiagCode::E3002, {"fallback type incompatible with nullable inner type"});
        return nullptr;
    }

    TypeAST* result = ctx.checker.unify(innerLeft, rightType);
    if (!result) result = rightType;
    node.resolvedType = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// AssignExprAST – assignment (plain or compound)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAssignExpr(AssignExprAST& node, SemanticContext& ctx) {
    TypeAST* lhsType = checkExpr(node.lhs.get(), ctx);
    if (!lhsType) return nullptr;
    TypeAST* rhsType = checkExpr(node.rhs.get(), ctx);
    if (!rhsType) return nullptr;

    if (!ctx.checker.isAssignable(rhsType, lhsType)) {
        ctx.error(node.loc, DiagCode::E3002, {"cannot assign expression to left‑hand side"});
        return nullptr;
    }

    node.resolvedType = rhsType;
    return rhsType;
}

// ─────────────────────────────────────────────────────────────────────────────
// IsExprAST – runtime type check (x is Type)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIsExpr(IsExprAST& node, SemanticContext& ctx) {
    TypeAST* exprType = checkExpr(node.expr.get(), ctx);
    if (!exprType) return nullptr;
    TypeAST* checkType = ctx.resolver.resolveType(node.checkType.get());
    if (!checkType) {
        ctx.error(node.loc, DiagCode::E3001, {"cannot resolve type in 'is' expression"});
        return nullptr;
    }

    TypeAST* boolType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).get();
    node.resolvedType = boolType;
    return boolType;
}

// ─────────────────────────────────────────────────────────────────────────────
// PipelineExprAST – seed |> step1 |> step2 ...
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkPipelineExpr(PipelineExprAST& node, SemanticContext& ctx) {
    TypeAST* currentType = checkExpr(node.seed.get(), ctx);
    if (!currentType) return nullptr;

    for (auto& step : node.steps) {
        if (!step) continue;
        if (step->kind == PipelineStepKind::Ident && step->ident.isValid()) {
            Symbol* sym = ctx.symbols.lookup(step->ident);
            if (!sym || !sym->type || !sym->type->isa<FuncTypeAST>()) {
                ctx.error(step->loc, DiagCode::E3002, {"pipeline step is not a function"});
                return nullptr;
            }
            FuncTypeAST* func = sym->type->as<FuncTypeAST>();
            if (func->sig.groupCount() == 0) {
                ctx.error(step->loc, DiagCode::E3002, {"pipeline step function has no parameters"});
                return nullptr;
            }
            auto firstGroup = func->sig.getGroup(0);
            if (firstGroup.empty()) {
                ctx.error(step->loc, DiagCode::E3002, {"pipeline step function has no parameters"});
                return nullptr;
            }
            TypeAST* firstParamType = firstGroup[0]->type.get();
            if (!ctx.checker.isAssignable(currentType, firstParamType)) {
                ctx.error(step->loc, DiagCode::E3002,
                          {"pipeline seed/result type does not match step's first parameter"});
                return nullptr;
            }
            if (func->sig.returnTypes.empty()) {
                currentType = nullptr;
            } else {
                currentType = func->sig.returnTypes[0].get();
            }
        } else {
            ctx.error(step->loc, DiagCode::E3002, {"unsupported pipeline step kind"});
            return nullptr;
        }
    }

    node.resolvedType = currentType;
    return currentType;
}

// ─────────────────────────────────────────────────────────────────────────────
// ComposeExprAST – f +> g +> h (compile‑time function composition)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkComposeExpr(ComposeExprAST& node, SemanticContext& ctx) {
    TypeAST* leftType = checkExpr(node.left.get(), ctx);
    if (!leftType) return nullptr;
    if (!leftType->isa<FuncTypeAST>()) {
        ctx.error(node.left->loc, DiagCode::E3002, {"left side of composition must be a function"});
        return nullptr;
    }

    TypeAST* current = leftType;
    for (auto& operand : node.operands) {
        if (!operand) continue;
        TypeAST* funcType = nullptr;
        if (operand->kind == ComposeOperandKind::Ident && operand->ident.isValid()) {
            Symbol* sym = ctx.symbols.lookup(operand->ident);
            if (!sym || !sym->type || !sym->type->isa<FuncTypeAST>()) {
                ctx.error(operand->loc, DiagCode::E3002, {"compose operand is not a function"});
                return nullptr;
            }
            funcType = sym->type;
        } else {
            ctx.error(operand->loc, DiagCode::E3002, {"unsupported compose operand"});
            return nullptr;
        }

        FuncTypeAST* curFunc = current->as<FuncTypeAST>();
        FuncTypeAST* nextFunc = funcType->as<FuncTypeAST>();
        if (curFunc->sig.returnTypes.empty()) {
            ctx.error(operand->loc, DiagCode::E3002, {"left function has no return type"});
            return nullptr;
        }
        if (nextFunc->sig.groupCount() == 0) {
            ctx.error(operand->loc, DiagCode::E3002, {"right function has no parameter groups"});
            return nullptr;
        }
        auto nextFirstGroup = nextFunc->sig.getGroup(0);
        if (nextFirstGroup.empty()) {
            ctx.error(operand->loc, DiagCode::E3002, {"right function has no parameters"});
            return nullptr;
        }
        TypeAST* curRet = curFunc->sig.returnTypes[0].get();
        TypeAST* nextFirstParam = nextFirstGroup[0]->type.get();
        if (!ctx.checker.isEqual(curRet, nextFirstParam)) {
            ctx.error(operand->loc, DiagCode::E3002,
                      {"composition type mismatch: output of left does not match input of right"});
            return nullptr;
        }
        current = funcType;
    }

    node.resolvedType = current;
    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// AnonFuncExprAST – anonymous function expression
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAnonFuncExpr(AnonFuncExprAST& node, SemanticContext& ctx) {
    auto* funcType = ctx.resolver.cloneFuncSignature(node.sig, node.loc);
    TypeAST* resolvedType = ctx.resolver.resolveType(funcType);
    if (!resolvedType) {
        ctx.error(node.loc, DiagCode::E3002, {"invalid anonymous function signature"});
        return nullptr;
    }

    ctx.symbols.pushScope();
    // Declare parameters from flattened allParams
    for (const auto& param : node.sig.allParams) {
        if (!param) continue;
        Symbol paramSym;
        paramSym.name = param->name;
        paramSym.kind = SymbolKind::Param;
        paramSym.declKw = DeclKeyword::Let;
        paramSym.visibility = Visibility::Private;
        paramSym.type = param->type.get();
        paramSym.decl = param.get();
        paramSym.loc = param->loc;
        if (!ctx.symbols.declare(paramSym)) {
            ctx.error(param->loc, DiagCode::E3005,
                      {"duplicate parameter name '" + std::string(ctx.pool.lookup(param->name)) + "'"});
        }
    }

    TypeAST* expectedReturn = nullptr;
    if (!funcType->sig.returnTypes.empty())
        expectedReturn = funcType->sig.returnTypes[0].get();
    checkStmt(node.body.get(), ctx, expectedReturn);
    ctx.symbols.popScope();

    node.resolvedType = funcType;
    return funcType;
}

// ─────────────────────────────────────────────────────────────────────────────
// AwaitExprAST – await expression (inside async functions)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAwaitExpr(AwaitExprAST& node, SemanticContext& ctx) {
    TypeAST* innerType = checkExpr(node.inner.get(), ctx);
    if (!innerType) return nullptr;
    // FIXME: verify current function is async (requires FunctionContext)
    // For now, emit warning if not inside async.
    node.resolvedType = innerType;
    return innerType;
}

// ─────────────────────────────────────────────────────────────────────────────
// MatchExprAST – pattern matching expression
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkMatchExpr(MatchExprAST& node, SemanticContext& ctx) {
    TypeAST* subjectType = checkExpr(node.subject.get(), ctx);
    if (!subjectType) return nullptr;

    TypeAST* unifiedType = nullptr;
    for (auto& arm : node.arms) {
        for (auto& expr : arm->exprs) {
            TypeAST* exprType = checkExpr(expr.get(), ctx);
            if (!exprType) return nullptr;
            if (!unifiedType) unifiedType = exprType;
            else unifiedType = ctx.checker.unify(unifiedType, exprType);
            if (!unifiedType) {
                ctx.error(expr->loc, DiagCode::E3002, {"match arms have incompatible types"});
                return nullptr;
            }
        }
    }
    for (auto& expr : node.defaultBody->exprs) {
        TypeAST* exprType = checkExpr(expr.get(), ctx);
        if (!exprType) return nullptr;
        if (!unifiedType) unifiedType = exprType;
        else unifiedType = ctx.checker.unify(unifiedType, exprType);
        if (!unifiedType) {
            ctx.error(expr->loc, DiagCode::E3002, {"default arm type does not match other arms"});
            return nullptr;
        }
    }

    node.resolvedType = unifiedType;
    return unifiedType;
}

// ─────────────────────────────────────────────────────────────────────────────
// IfExprAST – expression form of if (condition ?? then else)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIfExpr(IfExprAST& node, SemanticContext& ctx) {
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (!condType) return nullptr;
    if (!ctx.checker.isBooleanCompatible(condType)) {
        ctx.error(node.condition->loc, DiagCode::E3002, {"if condition must be boolean"});
        return nullptr;
    }

    TypeAST* thenType = checkExpr(node.thenBranch.get(), ctx);
    if (!thenType) return nullptr;
    TypeAST* elseType = checkExpr(node.elseBranch.get(), ctx);
    if (!elseType) return nullptr;

    TypeAST* unified = ctx.checker.unify(thenType, elseType);
    if (!unified) {
        ctx.error(node.loc, DiagCode::E3002, {"then and else branches must have compatible types"});
        return nullptr;
    }

    node.resolvedType = unified;
    return unified;
}

// ─────────────────────────────────────────────────────────────────────────────
// RangeExprAST – inclusive range literal (lo..hi)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkRangeExpr(RangeExprAST& node, SemanticContext& ctx) {
    TypeAST* loType = checkExpr(node.lo.get(), ctx);
    if (!loType) return nullptr;
    TypeAST* hiType = checkExpr(node.hi.get(), ctx);
    if (!hiType) return nullptr;

    if (!ctx.checker.isIntegerType(loType) || !ctx.checker.isIntegerType(hiType)) {
        ctx.error(node.loc, DiagCode::E3002, {"range bounds must be integers"});
        return nullptr;
    }

    TypeAST* anyType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Any).get();
    node.resolvedType = anyType;
    return anyType;
}

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicCallExprAST – #intrinsic(...)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST& node, SemanticContext& ctx) {
    const IntrinsicEntry* entry = intrinsic::lookup(node.intrinsicName);
    if (!entry) {
        ctx.error(node.loc, DiagCode::E3009,
            {"unknown intrinsic '#" + std::string(ctx.pool.lookup(node.intrinsicName)) + "'"});
        return nullptr;
    }

    if (entry->argKinds.size() > 0 && entry->argKinds[0] == IntrinsicArgKind::TypeArg) {
        if (!node.typeArg) {
            ctx.error(node.loc, DiagCode::E3010, {"intrinsic requires a type argument"});
            return nullptr;
        }
        if (!ctx.resolver.resolveType(node.typeArg.get())) {
            ctx.error(node.loc, DiagCode::E3002, {"invalid type argument for intrinsic"});
            return nullptr;
        }
    }

    size_t expectedMin = entry->minArgs;
    size_t expectedMax = entry->maxArgs;
    if (node.args.size() < expectedMin || node.args.size() > expectedMax) {
        ctx.error(node.loc, DiagCode::E3010,
            {"intrinsic expects " + std::to_string(expectedMin) + " to " +
                     std::to_string(expectedMax) + " arguments, got " +
                     std::to_string(node.args.size())});
        return nullptr;
    }

    for (size_t i = 0; i < node.args.size(); ++i) {
        if (!checkExpr(node.args[i].get(), ctx)) return nullptr;
    }

    TypeAST* returnType = nullptr;
    switch (entry->returnKind) {
        case IntrinsicReturnKind::Void:
            returnType = nullptr;
            break;
        case IntrinsicReturnKind::Uint64:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Uint64).get();
            break;
        case IntrinsicReturnKind::Float64:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Double).get();
            break;
        case IntrinsicReturnKind::SameAsArg0:
            if (!node.args.empty())
                returnType = getExprType(node.args[0].get());
            break;
        case IntrinsicReturnKind::SameAsArg1:
            if (node.args.size() >= 2)
                returnType = getExprType(node.args[1].get());
            break;
        case IntrinsicReturnKind::RefOfTypeArg0:
            if (node.typeArg)
                returnType = ctx.arena.make<RefTypeAST>(TypePtr(node.typeArg.get())).get();
            break;
        default:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Any).get();
            break;
    }

    node.resolvedType = returnType;
    return returnType;
}

// ─────────────────────────────────────────────────────────────────────────────
// The main entry point: checkExpr
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SemanticContext& ctx) {
    if (!node) return nullptr;

    if (node->resolvedType) return static_cast<TypeAST*>(node->resolvedType);

    TypeAST* result = nullptr;
    switch (node->kind) {
        case ASTKind::LiteralExpr:
            result = checkLiteralExpr(*node->as<LiteralExprAST>(), ctx);
            break;
        case ASTKind::IdentifierExpr:
            result = checkIdentifierExpr(*node->as<IdentifierExprAST>(), ctx);
            break;
        case ASTKind::BinaryExpr:
            result = checkBinaryExpr(*node->as<BinaryExprAST>(), ctx);
            break;
        case ASTKind::UnaryExpr:
            result = checkUnaryExpr(*node->as<UnaryExprAST>(), ctx);
            break;
        case ASTKind::CallExpr:
            result = checkCallExpr(*node->as<CallExprAST>(), ctx);
            break;
        case ASTKind::IndexExpr:
            result = checkIndexExpr(*node->as<IndexExprAST>(), ctx);
            break;
        case ASTKind::FieldAccessExpr:
            result = checkFieldAccessExpr(*node->as<FieldAccessExprAST>(), ctx);
            break;
        case ASTKind::StructLiteralExpr:
            result = checkStructLiteralExpr(*node->as<StructLiteralExprAST>(), ctx);
            break;
        case ASTKind::ArrayLiteralExpr:
            result = checkArrayLiteralExpr(*node->as<ArrayLiteralExprAST>(), ctx);
            break;
        case ASTKind::TypeConvExpr:
            result = checkTypeConvExpr(*node->as<TypeConvExprAST>(), ctx);
            break;
        case ASTKind::BehaviorAccessExpr:
            result = checkBehaviorAccessExpr(*node->as<BehaviorAccessExprAST>(), ctx);
            break;
        case ASTKind::NullableChainExpr:
            result = checkNullableChainExpr(*node->as<NullableChainExprAST>(), ctx);
            break;
        case ASTKind::NullCoalesceExpr:
            result = checkNullCoalesceExpr(*node->as<NullCoalesceExprAST>(), ctx);
            break;
        case ASTKind::AssignExpr:
            result = checkAssignExpr(*node->as<AssignExprAST>(), ctx);
            break;
        case ASTKind::IsExpr:
            result = checkIsExpr(*node->as<IsExprAST>(), ctx);
            break;
        case ASTKind::PipelineExpr:
            result = checkPipelineExpr(*node->as<PipelineExprAST>(), ctx);
            break;
        case ASTKind::ComposeExpr:
            result = checkComposeExpr(*node->as<ComposeExprAST>(), ctx);
            break;
        case ASTKind::AnonFuncExpr:
            result = checkAnonFuncExpr(*node->as<AnonFuncExprAST>(), ctx);
            break;
        case ASTKind::AwaitExpr:
            result = checkAwaitExpr(*node->as<AwaitExprAST>(), ctx);
            break;
        case ASTKind::MatchExpr:
            result = checkMatchExpr(*node->as<MatchExprAST>(), ctx);
            break;
        case ASTKind::IfExpr:
            result = checkIfExpr(*node->as<IfExprAST>(), ctx);
            break;
        case ASTKind::RangeExpr:
            result = checkRangeExpr(*node->as<RangeExprAST>(), ctx);
            break;
        case ASTKind::IntrinsicCallExpr:
            result = checkIntrinsicCallExpr(*node->as<IntrinsicCallExprAST>(), ctx);
            break;
        default:
            ctx.error(node->loc, DiagCode::E3002,
            {"unsupported expression kind: " + LucDebug::kindToString(node->kind)});
            return nullptr;
    }

    node->resolvedType = result;
    return result;
}
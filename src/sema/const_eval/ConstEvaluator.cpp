/// @file const_eval/ConstEvaluator.cpp
/// @brief Main const evaluation logic - public API only.

#include "ConstEvaluator.hpp"
#include "ConstEvalHelpers.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaType.hpp"
#include "sema/Sema.hpp"
#include "sema/support/Truthiness.hpp"

#include <cmath>

namespace sema {

// ─── Static Member Initialization ────────────────────────────────────────

std::unordered_map<DeclAST*, std::vector<DeclAST*>> ConstEvaluator::m_deps;
std::vector<DeclAST*> ConstEvaluator::m_constDecls;
std::unordered_map<ExprAST*, ConstantValue> ConstEvaluator::m_evalCache;
std::unordered_set<DeclAST*> ConstEvaluator::m_evaluating;
size_t ConstEvaluator::m_recursionDepth = 0;

// ─── Main Entry Points ───────────────────────────────────────────────────

ConstantValue ConstEvaluator::evaluateDecl(SemaContext& ctx, VarDeclAST* decl) {
    if (!decl) return ConstantValue::error();
    if (decl->hasSyntaxError) return ConstantValue::error();

    if (!decl->init) {
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                              "const variable '", ctx.pool.lookup(decl->name),
                              "' has no initializer");
        return ConstantValue::error();
    }

    if (m_recursionDepth >= MAX_RECURSION) {
        return ConstantValue::unknown();
    }

    if (m_evaluating.find(decl) != m_evaluating.end()) {
        ctx.diagnostics.error(DiagCode::Sem_CircularDependency, decl,
                              "circular dependency detected in const declaration '",
                              ctx.pool.lookup(decl->name), "'");
        return ConstantValue::error();
    }

    EvaluationGuard guard(m_evaluating, decl);
    DepthGuard depthGuard(m_recursionDepth);

    ctx.pushScope();
    ctx.insertValue(decl);
    
    ConstantValue result = evaluate(ctx, decl->init, decl->type);
    
    ctx.popScope();

    return result;
}

ConstantValue ConstEvaluator::evaluate(SemaContext& ctx, ExprAST* expr,
                                        TypeAST* targetType) {
    if (!expr) return ConstantValue::error();
    if (expr->hasSyntaxError) return ConstantValue::error();

    if (m_recursionDepth >= MAX_RECURSION) {
        return ConstantValue::unknown();
    }

    // ─── Cache check ──────────────────────────────────────────────────────
    auto it = m_evalCache.find(expr);
    if (it != m_evalCache.end()) {
        return it->second;
    }

    ConstantValue result;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = evalLiteral(ctx, expr->as<LiteralExprAST>());
            break;
        case ASTKind::IdentifierExpr:
            result = evalIdentifier(ctx, expr->as<IdentifierExprAST>());
            break;
        case ASTKind::BinaryExpr:
            result = evalBinary(ctx, expr->as<BinaryExprAST>(), targetType);
            break;
        case ASTKind::UnaryExpr:
            result = evalUnary(ctx, expr->as<UnaryExprAST>(), targetType);
            break;
        case ASTKind::CallExpr:
            result = evalCall(ctx, expr->as<CallExprAST>());
            break;
        case ASTKind::StructLiteralExpr:
            result = evalStructLiteral(ctx, expr->as<StructLiteralExprAST>());
            break;
        case ASTKind::ArrayLiteralExpr:
            result = evalArrayLiteral(ctx, expr->as<ArrayLiteralExprAST>());
            break;
        case ASTKind::FieldAccessExpr:
            result = evalFieldAccess(ctx, expr->as<FieldAccessExprAST>());
            break;
        case ASTKind::NullCoalesceExpr:
            result = evalNullCoalesce(ctx, expr->as<NullCoalesceExprAST>());
            break;
        case ASTKind::IfExpr:
            result = evalIfExpr(ctx, expr->as<IfExprAST>());
            break;
        case ASTKind::RangeExpr:
            result = evalRangeExpr(ctx, expr->as<RangeExprAST>());
            break;
        default:
            return ConstantValue::unknown();
    }

    // ─── Cache the result and update AST metadata ──────────────────────
    if (result.isEvaluated() && !result.isError()) {
        // Store in cache for future lookups
        m_evalCache[expr] = result;
        
        // Update AST metadata (lightweight: just the flag and type)
        expr->isConst = true;
        expr->resolvedType = getConstantType(ctx, result);
        expr->valueState = result.isErr() ? ValueState::Err : ValueState::Definite;
    }

    return result;
}

bool ConstEvaluator::isConstExpr(SemaContext& ctx, ExprAST* expr,
                                  TypeAST* targetType) {
    if (!expr) return false;
    if (expr->isConst) return true;
    
    ConstantValue val = evaluate(ctx, expr, targetType);
    return val.isEvaluated() && !val.isError();
}

ConstantValue ConstEvaluator::getConstValue(SemaContext& ctx, ExprAST* expr,
                                             TypeAST* targetType) {
    if (!expr) return ConstantValue::unknown();
    if (expr->hasSyntaxError) return ConstantValue::error();
    
    // Check cache first
    auto it = m_evalCache.find(expr);
    if (it != m_evalCache.end()) {
        return it->second;
    }
    
    // If the expression is marked const but not in cache, evaluate it
    if (expr->isConst) {
        return evaluate(ctx, expr, targetType);
    }
    
    return ConstantValue::unknown();
}

std::optional<int64_t> ConstEvaluator::evaluateAsInt(SemaContext& ctx, ExprAST* expr) {
    if (!expr) return std::nullopt;
    
    ConstantValue val = getConstValue(ctx, expr);
    if (val.isInt()) {
        return val.asInt();
    }
    return std::nullopt;
}

std::optional<bool> ConstEvaluator::evaluateAsBool(SemaContext& ctx, ExprAST* expr) {
    if (!expr) return std::nullopt;
    
    ConstantValue val = getConstValue(ctx, expr);
    if (val.isBool()) {
        return val.asBool();
    }
    return std::nullopt;
}

// ─── evalLiteral ──────────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalLiteral(SemaContext& ctx, LiteralExprAST* expr) {
    if (!expr) return ConstantValue::error();

    switch (expr->kind) {
        case LiteralKind::True:   return ConstantValue(true);
        case LiteralKind::False:  return ConstantValue(false);
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string str = ctx.pool.lookup(expr->value);
            try {
                return ConstantValue(std::stoll(str, nullptr, 0));
            } catch (const std::exception&) {
                ctx.diagnostics.error(DiagCode::Lex_InvalidNumberLiteral, expr,
                                      "invalid integer literal '", str, "'");
                return ConstantValue::error();
            }
        }
        case LiteralKind::Float: {
            std::string str = ctx.pool.lookup(expr->value);
            try {
                return ConstantValue(std::stod(str));
            } catch (const std::exception&) {
                ctx.diagnostics.error(DiagCode::Lex_InvalidNumberLiteral, expr,
                                      "invalid float literal '", str, "'");
                return ConstantValue::error();
            }
        }
        case LiteralKind::String:
        case LiteralKind::RawString:
            return ConstantValue(expr->value);
        case LiteralKind::Char:
            return ConstantValue(expr->value);
        case LiteralKind::Nil:   return ConstantValue::nil();
        case LiteralKind::Err:   return ConstantValue::err();
        default:                 return ConstantValue::unknown();
    }
}

// ─── evalIdentifier ──────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalIdentifier(SemaContext& ctx, IdentifierExprAST* expr) {
    if (!expr) return ConstantValue::error();

    // ─── `_` is the discard placeholder ──────────────────────────────────
    if (ctx.pool.lookupView(expr->name) == "_") {
        return ConstantValue::unknown();
    }

    ValueDeclAST* decl = ctx.lookupValue(expr->name);
    if (!decl) {
        return ConstantValue::error();
    }

    // ─── Variable ──────────────────────────────────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        VarDeclAST* var = decl->as<VarDeclAST>();
        
        // Check if this variable has a const value already computed (in cache)
        if (var->init && var->init->isConst) {
            auto it = m_evalCache.find(var->init);
            if (it != m_evalCache.end()) {
                return it->second;
            }
            // If marked const but not in cache, evaluate it
            return evaluate(ctx, var->init, var->type);
        }

        // If it's a const variable, evaluate it now
        if (var->keyword == DeclKeyword::Const && var->init) {
            // Check for circular dependency
            if (m_evaluating.find(var) != m_evaluating.end()) {
                ctx.diagnostics.error(DiagCode::Sem_CircularDependency, expr,
                                      "cycle detected in const declaration '",
                                      ctx.pool.lookup(expr->name), "'");
                return ConstantValue::error();
            }
            return evaluate(ctx, var->init, var->type);
        }

        // Non-const variable cannot be evaluated at compile time
        return ConstantValue::unknown();
    }

    // ─── Function ──────────────────────────────────────────────────────────
    if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* func = decl->as<FuncDeclAST>();
        if (func->keyword != DeclKeyword::Const) {
            return ConstantValue::unknown();
        }
        return ConstantValue(func);
    }

    // ─── Enum Variant ──────────────────────────────────────────────────────
    if (decl->isa<EnumVariantAST>()) {
        const EnumVariantAST* variant = decl->as<EnumVariantAST>();
        return ConstantValue(variant->value);
    }

    // ─── Parameter ─────────────────────────────────────────────────────────
    if (decl->isa<ParamAST>()) {
        // Parameters get their values from function arguments during
        // const function execution.
        ParamAST* param = decl->as<ParamAST>();
        if (param->type && param->type->isa<PrimitiveTypeAST>()) {
            return ConstantValue::unknown();
        }
        return ConstantValue::unknown();
    }

    return ConstantValue::unknown();
}

// ─── evalBinary ──────────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalBinary(SemaContext& ctx, BinaryExprAST* expr,
                                          TypeAST* targetType) {
    if (!expr) return ConstantValue::error();

    // ─── If condition context: detect narrowing ──────────────────────────
    if (ctx.stack.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.stack.setPendingNarrowing(info);
            // Return unknown - the condition is const for narrowing purposes
            // but we don't need the actual value here.
            return ConstantValue::unknown();
        }
    }

    // ─── Evaluate left operand ──────────────────────────────────────────
    ConstantValue left = evaluate(ctx, expr->left, targetType);
    if (left.isError()) return left;
    if (left.isUnknown()) return ConstantValue::unknown();

    // ─── Short-circuit for logical operators ────────────────────────────
    if (expr->op == BinaryOp::And) {
        if (left.isBool() && !left.asBool()) {
            return ConstantValue(false);
        }
        if (left.isUnknown()) return ConstantValue::unknown();
    }
    if (expr->op == BinaryOp::Or) {
        if (left.isBool() && left.asBool()) {
            return ConstantValue(true);
        }
        if (left.isUnknown()) return ConstantValue::unknown();
    }

    // ─── Evaluate right operand ──────────────────────────────────────────
    ConstantValue right = evaluate(ctx, expr->right, targetType);
    if (right.isError()) return right;
    if (right.isUnknown()) return ConstantValue::unknown();

    // ─── Perform the operation ────────────────────────────────────────────
    return evalBinaryOp(ctx, expr->op, left, right, expr, targetType);
}

// ─── evalStructLiteral ────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalStructLiteral(SemaContext& ctx, StructLiteralExprAST* expr) {
    if (!expr) return ConstantValue::error();

    // ─── Look up struct type ──────────────────────────────────────────────
    TypeDeclAST* typeDecl = ctx.lookupType(expr->typeName);
    if (!typeDecl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "undefined type '", ctx.pool.lookup(expr->typeName), "'");
        return ConstantValue::error();
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                              "'", ctx.pool.lookup(expr->typeName), "' is not a struct");
        return ConstantValue::error();
    }

    StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // ─── Build field map ──────────────────────────────────────────────────
    std::unordered_map<InternedString, FieldDeclAST*> fieldMap;
    for (FieldDeclAST* field : structDecl->fields) {
        fieldMap[field->name] = field;
    }

    std::unordered_map<InternedString, ConstantValue> fields;

    // ─── Initialize with default values ──────────────────────────────────
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->defaultVal) {
            ConstantValue val = evaluate(ctx, field->defaultVal, field->type);
            if (val.isError()) return val;
            if (val.isUnknown()) return ConstantValue::unknown();
            fields[field->name] = val;
        }
    }

    // ─── Override with explicit initializers ─────────────────────────────
    for (FieldInitAST* init : expr->inits) {
        auto it = fieldMap.find(init->name);
        if (it == fieldMap.end()) {
            ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, init,
                                  "struct '", ctx.pool.lookup(structDecl->name),
                                  "' has no field named '", ctx.pool.lookup(init->name), "'");
            return ConstantValue::error();
        }

        FieldDeclAST* field = it->second;

        // Check const field cannot be assigned nil/err
        if (field->isConst()) {
            if (init->value->isa<LiteralExprAST>()) {
                LiteralExprAST* lit = init->value->as<LiteralExprAST>();
                if (lit->kind == LiteralKind::Nil || lit->kind == LiteralKind::Err) {
                    ctx.diagnostics.error(DiagCode::Sem_ConstNullable, init,
                                          "const field '", ctx.pool.lookup(field->name),
                                          "' cannot be assigned '",
                                          (lit->kind == LiteralKind::Nil ? "nil" : "err"), "'");
                    return ConstantValue::error();
                }
            }
        }

        ConstantValue val = evaluate(ctx, init->value, field->type);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        fields[init->name] = val;
    }

    // ─── Check missing required fields ──────────────────────────────────
    for (FieldDeclAST* field : structDecl->fields) {
        if (fields.find(field->name) == fields.end()) {
            if (field->defaultVal) continue;
            if (isNullableType(field->type) || isFallibleType(field->type)) continue;
            
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, expr,
                                  "missing initializer for struct field '",
                                  ctx.pool.lookup(field->name), "'");
            return ConstantValue::error();
        }
    }

    ConstantValue result;
    result.kind = ConstantValue::Kind::Struct;
    result.value = fields;
    result.type = ctx.getNamedType(structDecl->name);
    return result;
}

// ─── evalArrayLiteral ────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalArrayLiteral(SemaContext& ctx, ArrayLiteralExprAST* expr) {
    if (!expr) return ConstantValue::error();

    std::vector<ConstantValue> elements;

    for (ExprAST* elem : expr->elements) {
        ConstantValue val = evaluate(ctx, elem);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        elements.push_back(val);
    }

    // ─── Check all elements have the same type ──────────────────────────
    if (!elements.empty()) {
        TypeAST* firstType = elements[0].type;
        for (size_t i = 1; i < elements.size(); ++i) {
            if (!typesEqual(elements[i].type, firstType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr,
                                      "array elements must have the same type");
                return ConstantValue::error();
            }
        }
    }

    ConstantValue result;
    result.kind = ConstantValue::Kind::Array;
    result.value = elements;
    if (!elements.empty()) {
        result.type = ctx.getArrayType(ArrayKind::Fixed, elements.size(), elements[0].type);
    }
    return result;
}

// ─── evalFieldAccess ─────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalFieldAccess(SemaContext& ctx, FieldAccessExprAST* expr) {
    if (!expr) return ConstantValue::error();

    ConstantValue obj = evaluate(ctx, expr->object);
    if (obj.isError()) return obj;
    if (obj.isUnknown()) return ConstantValue::unknown();

    if (!obj.isStruct()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, expr->object,
                              "field access on non-struct value");
        return ConstantValue::error();
    }

    const auto& structFields = obj.asStruct();
    auto it = structFields.find(expr->fieldName);
    if (it == structFields.end()) {
        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                              "struct has no field '",
                              ctx.pool.lookup(expr->fieldName), "'");
        return ConstantValue::error();
    }

    return it->second;
}

// ─── evalNullCoalesce ────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalNullCoalesce(SemaContext& ctx, NullCoalesceExprAST* expr) {
    if (!expr) return ConstantValue::error();

    ConstantValue val = evaluate(ctx, expr->value);
    if (val.isError()) return val;

    if (val.isNil() || val.isErr()) {
        return evaluate(ctx, expr->fallback);
    }

    if (val.isUnknown()) return ConstantValue::unknown();
    return val;
}

// ─── evalIfExpr ──────────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalIfExpr(SemaContext& ctx, IfExprAST* expr) {
    if (!expr) return ConstantValue::error();

    ConstantValue cond = evaluate(ctx, expr->condition);
    if (cond.isError()) return cond;
    if (cond.isUnknown()) return ConstantValue::unknown();

    if (constantTruthiness(cond, ctx)) {
        return evaluate(ctx, expr->thenBranch);
    } else {
        return evaluate(ctx, expr->elseBranch);
    }
}

// ─── evalRangeExpr ──────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalRangeExpr(SemaContext& ctx, RangeExprAST* expr) {
    if (!expr) return ConstantValue::error();

    // ─── Evaluate both bounds ───────────────────────────────────────────
    ConstantValue loVal = evaluate(ctx, expr->lo);
    if (loVal.isError()) return loVal;
    if (loVal.isUnknown()) return ConstantValue::unknown();

    ConstantValue hiVal = evaluate(ctx, expr->hi);
    if (hiVal.isError()) return hiVal;
    if (hiVal.isUnknown()) return ConstantValue::unknown();

    // ─── Both bounds must be integers ────────────────────────────────────
    if (!loVal.isInt() || !hiVal.isInt()) {
        return ConstantValue::unknown();
    }

    int64_t lo = loVal.asInt();
    int64_t hi = hiVal.asInt();
    bool isInclusive = !expr->isExclusive;
    
    // ─── Validate range order ────────────────────────────────────────────
    if (isInclusive && lo > hi) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidRange, expr,
                              "inclusive range start (", lo, 
                              ") must be less than or equal to end (", hi, ")");
        return ConstantValue::error();
    }
    if (!isInclusive && lo >= hi) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidRange, expr,
                              "exclusive range start (", lo, 
                              ") must be less than end (", hi, ")");
        return ConstantValue::error();
    }

    // ─── Return the lower bound (range expressions are only used for loops) ──
    // The actual range evaluation for loops is handled in executeFor, which
    // calls evaluateAsInt on lo and hi separately.
    return ConstantValue::unknown();
}

// ─── evalCall ────────────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalCall(SemaContext& ctx, CallExprAST* expr) {
    FuncDeclAST* func = resolveCalleeOrError(expr->callee, ctx);
    if (!func) {
        return ConstantValue::error();
    }

    if (func->keyword != DeclKeyword::Const) {
        return ConstantValue::unknown();
    }

    if (!func->genericParams.empty() && expr->genericArgs.empty()) {
        return ConstantValue::unknown();
    }

    std::vector<ConstantValue> args;
    for (ExprAST* arg : expr->args) {
        ConstantValue val = evaluate(ctx, arg);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        args.push_back(val);
    }

    return executeFunction(ctx, func, args);
}

// ─── Report Cycle ────────────────────────────────────────────────────────

void ConstEvaluator::reportCycle(SemaContext& ctx, const std::vector<DeclAST*>& cycle) {
    if (cycle.empty()) return;
    
    std::string msg = "circular dependency in const declarations: ";
    for (size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) msg += " → ";
        msg += ctx.pool.lookup(cycle[i]->name);
    }
    ctx.diagnostics.error(DiagCode::Sem_CircularDependency, cycle[0], msg);
}

ConstantValue ConstEvaluator::getConstValue(VarDeclAST* decl) {
    if (!decl || decl->keyword != DeclKeyword::Const || !decl->init) {
        return ConstantValue::unknown();
    }
    if (decl->init->isConst) {
        auto it = m_evalCache.find(decl->init);
        if (it != m_evalCache.end()) {
            return it->second;
        }
    }
    return ConstantValue::unknown();
}

void ConstEvaluator::buildDependencyGraph(SemaContext& ctx) {
    m_constDecls.clear();
    m_deps.clear();

    for (ModuleAST* module : ctx.modules) {
        for (DeclAST* decl : module->decls) {
            if (decl && decl->isa<VarDeclAST>()) {
                VarDeclAST* var = decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const) {
                    m_constDecls.push_back(var);
                }
            }
            if (decl && decl->isa<FuncDeclAST>()) {
                FuncDeclAST* func = decl->as<FuncDeclAST>();
                if (func->keyword == DeclKeyword::Const) {
                    m_constDecls.push_back(func);
                }
            }
        }
    }

    for (DeclAST* decl : m_constDecls) {
        std::vector<DeclAST*> deps;
        if (decl->isa<VarDeclAST>()) {
            VarDeclAST* var = decl->as<VarDeclAST>();
            if (var->init) {
                collectDeps(ctx, var->init, deps);
            }
        } else if (decl->isa<FuncDeclAST>()) {
            FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->body) {
                collectDepsFromStmt(ctx, func->body, deps);
            }
        }
        m_deps[decl] = deps;
    }

    topologicalSort(ctx, m_deps);
}

} // namespace sema
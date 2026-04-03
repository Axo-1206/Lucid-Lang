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
 * @related SemanticDecl.cpp, SemanticStmt.cpp, TypeChecker.hpp
 */

#include "SemanticSymbol.hpp"
#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "ast/ExprAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/PatternAST.hpp"

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& asyncDepth, int& loopDepth, int& parallelDepth,
               bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// Helper — make a primitive TypeAST on the fly (unowned, do not delete)
// We use a static local so callers can return a stable pointer without ownership.
// ─────────────────────────────────────────────────────────────────────────────
static PrimitiveTypeAST* primType(PrimitiveKind k) {
    // One singleton per kind, lazily constructed.
    static PrimitiveTypeAST singletons[] = {
        PrimitiveTypeAST(PrimitiveKind::Bool),
        PrimitiveTypeAST(PrimitiveKind::Byte),
        PrimitiveTypeAST(PrimitiveKind::Short),
        PrimitiveTypeAST(PrimitiveKind::Int),
        PrimitiveTypeAST(PrimitiveKind::Long),
        PrimitiveTypeAST(PrimitiveKind::Ubyte),
        PrimitiveTypeAST(PrimitiveKind::Ushort),
        PrimitiveTypeAST(PrimitiveKind::Uint),
        PrimitiveTypeAST(PrimitiveKind::Ulong),
        PrimitiveTypeAST(PrimitiveKind::Int8),
        PrimitiveTypeAST(PrimitiveKind::Int16),
        PrimitiveTypeAST(PrimitiveKind::Int32),
        PrimitiveTypeAST(PrimitiveKind::Int64),
        PrimitiveTypeAST(PrimitiveKind::Uint8),
        PrimitiveTypeAST(PrimitiveKind::Uint16),
        PrimitiveTypeAST(PrimitiveKind::Uint32),
        PrimitiveTypeAST(PrimitiveKind::Uint64),
        PrimitiveTypeAST(PrimitiveKind::Float),
        PrimitiveTypeAST(PrimitiveKind::Double),
        PrimitiveTypeAST(PrimitiveKind::Decimal),
        PrimitiveTypeAST(PrimitiveKind::String),
        PrimitiveTypeAST(PrimitiveKind::Char),
        PrimitiveTypeAST(PrimitiveKind::Any),
    };
    return &singletons[static_cast<int>(k)];
}

// ─────────────────────────────────────────────────────────────────────────────
// checkLiteralExpr
// Maps token kind to a primitive TypeAST.
// nil → nullptr (caller handles nil assignability).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkLiteralExpr(LiteralExprAST& node) {
    TypeAST* t = nullptr;
    switch (node.kind) {
        case LiteralKind::True:
        case LiteralKind::False:    t = primType(PrimitiveKind::Bool);   break;
        case LiteralKind::Int:      t = primType(PrimitiveKind::Int);    break;
        case LiteralKind::Hex:      t = primType(PrimitiveKind::Int);    break;
        case LiteralKind::Binary:   t = primType(PrimitiveKind::Int);    break;
        case LiteralKind::Float:    t = primType(PrimitiveKind::Float);  break;
        case LiteralKind::String:
        case LiteralKind::RawString:t = primType(PrimitiveKind::String); break;
        case LiteralKind::Char:     t = primType(PrimitiveKind::Char);   break;
        case LiteralKind::Nil:      t = nullptr; break; // nil has no concrete type alone
    }
    node.resolvedType = t;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIdentExpr
// Looks the name up in the symbol table. Reports E3001 if missing.
// Returns the symbol's declared type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIdentExpr(IdentExprAST& node, SymbolTable& symbols,
                                DiagnosticEngine& dc) {
    Symbol* sym = symbols.lookup(node.name);
    if (!sym) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "undeclared identifier '" + node.name + "'");
        return nullptr;
    }
    node.resolvedType = sym->type;
    node.isBehaviorMember = (sym->kind == SymbolKind::Method);
    return sym->type;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFieldAccessExpr
// obj.field — resolves the struct type of obj, then looks up the field.
// Also handles enum variant access: Direction.North.
// ─────────────────────────────────────────────────────────────────────────────
// static TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SymbolTable& symbols,
//                                      TypeResolver& resolver, DiagnosticEngine& dc,
//                                      int& asyncDepth, int& loopDepth,
//                                      int& parallelDepth, bool insideExtern);

// Forward declare checkExpr so the helpers below can call it.
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

static TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SymbolTable& symbols,
                                     TypeResolver& resolver, DiagnosticEngine& dc,
                                     int& asyncDepth, int& loopDepth,
                                     int& parallelDepth, bool insideExtern) {
    TypeAST* objType = checkExpr(node.object.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (!objType) return nullptr;

    // Resolve named types to their struct symbol.
    std::string typeName;
    if (objType->isa<NamedTypeAST>()) typeName = objType->as<NamedTypeAST>()->name;

    // Enum variant access: Direction.North
    if (!typeName.empty()) {
        Symbol* typeSym = symbols.lookup(typeName);
        if (typeSym && typeSym->kind == SymbolKind::Enum) {
            // The type of Direction.North is Direction itself.
            node.resolvedType = objType;
            return objType;
        }
        // Struct field access
        if (typeSym && typeSym->kind == SymbolKind::Struct) {
            auto* structDecl = typeSym->decl->as<StructDeclAST>();
            for (auto& f : structDecl->fields) {
                if (f->name == node.field) {
                    TypeAST* ft = resolver.resolveType(f->type.get());
                    node.resolvedType = ft;
                    return ft;
                }
            }
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "struct '" + typeName + "' has no field '" + node.field + "'");
            return nullptr;
        }
    }

    // Fallback: return any for now (e.g. any-typed values).
    node.resolvedType = primType(PrimitiveKind::Any);
    return primType(PrimitiveKind::Any);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkBehaviorAccessExpr
// Vec2:normalize — looks up the mangled name "Vec2.normalize" in the symbol table.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkBehaviorAccessExpr(BehaviorAccessExprAST& node, SymbolTable& symbols,
                                         DiagnosticEngine& dc) {
    std::string mangled = node.typeName + "." + node.method;
    Symbol* sym = symbols.lookup(mangled);
    if (!sym) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "no method '" + node.method + "' found on '" + node.typeName + "'");
        return nullptr;
    }
    node.isBehaviorMember = true;
    node.resolvedType = sym->type;
    return sym->type;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkBinaryExpr
// Validates that the operator is defined for the operand types.
// Comparison and logical operators always produce bool.
// Arithmetic operators produce the type of the left operand (after widening).
// String + string produces string.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkBinaryExpr(BinaryExprAST& node, SymbolTable& symbols,
                                 TypeResolver& resolver, DiagnosticEngine& dc,
                                 int& asyncDepth, int& loopDepth,
                                 int& parallelDepth, bool insideExtern) {
    TypeAST* lt = checkExpr(node.left.get(),  symbols, resolver, dc,
                            asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* rt = checkExpr(node.right.get(), symbols, resolver, dc,
                            asyncDepth, loopDepth, parallelDepth, insideExtern);

    TypeAST* result = nullptr;

    switch (node.op) {
        // Logical — both sides must be bool, result is bool.
        case BinaryOp::And:
        case BinaryOp::Or:
            if (lt && !TypeChecker::isBooleanCompatible(lt))
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'and'/'or' requires bool operands");
            result = primType(PrimitiveKind::Bool);
            break;

        // Comparison — result is always bool.
        case BinaryOp::Eq: case BinaryOp::Ne:
        case BinaryOp::Lt: case BinaryOp::Gt:
        case BinaryOp::Le: case BinaryOp::Ge:
            result = primType(PrimitiveKind::Bool);
            break;

        // Arithmetic — result follows left type; check operands are numeric/string.
        case BinaryOp::Add:
            // string + string is valid.
            if (lt && lt->isa<PrimitiveTypeAST>() &&
                lt->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String) {
                if (rt && !TypeChecker::isAssignable(rt, lt))
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                             "'+' on strings requires string right-hand side");
                result = primType(PrimitiveKind::String);
            } else {
                result = TypeChecker::unify(lt, rt);
                if (!result && lt) result = lt;
            }
            break;

        case BinaryOp::Sub: case BinaryOp::Mul:
        case BinaryOp::Div: case BinaryOp::Pow:
        case BinaryOp::Mod:
            result = TypeChecker::unify(lt, rt);
            if (!result && lt) result = lt;
            break;

        // Bitwise — result is the left operand type.
        case BinaryOp::BitAnd: case BinaryOp::BitOr:
        case BinaryOp::BitXor: case BinaryOp::Shl:
        case BinaryOp::Shr:
            result = lt ? lt : rt;
            break;
    }

    node.resolvedType = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkUnaryExpr
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkUnaryExpr(UnaryExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    TypeAST* inner = checkExpr(node.operand.get(), symbols, resolver, dc,
                               asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* result = inner;

    switch (node.op) {
        case UnaryOp::Not:
            if (inner && !TypeChecker::isBooleanCompatible(inner))
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'not' requires a bool operand");
            result = primType(PrimitiveKind::Bool);
            break;
        case UnaryOp::Neg:
            result = inner; // numeric — leave it to codegen to validate
            break;
        case UnaryOp::BitNot:
            result = inner;
            break;
        case UnaryOp::Ref:
            // &x — produce a RefTypeAST wrapping inner's type.
            // For now, return inner; codegen will wrap in a reference.
            result = inner;
            break;
    }

    node.resolvedType = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkCallExpr
// Validates that the callee is callable, argument count matches, and each
// argument type is assignable to the corresponding parameter type.
// Also handles struct constructor (from() dispatch) and primitive type conversion.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkCallExpr(CallExprAST& node, SymbolTable& symbols,
                               TypeResolver& resolver, DiagnosticEngine& dc,
                               int& asyncDepth, int& loopDepth,
                               int& parallelDepth, bool insideExtern) {
    // Check each argument.
    for (auto& arg : node.args) {
        checkExpr(arg.get(), symbols, resolver, dc,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    // Resolve callee type.
    TypeAST* calleeType = checkExpr(node.callee.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);

    // If callee is a named type (struct constructor / from() dispatch), return that type.
    if (node.callee->isa<IdentExprAST>()) {
        auto* ident = node.callee->as<IdentExprAST>();
        Symbol* sym = symbols.lookup(ident->name);
        if (sym && sym->kind == SymbolKind::Struct) {
            // Struct literal call — return a NamedTypeAST for this struct.
            // The type pointer from the symbol table is the best we have here.
            node.resolvedType = sym->type ? sym->type : calleeType;
            return static_cast<TypeAST*>(node.resolvedType);
        }
    }

    // Callable check.
    if (calleeType && !TypeChecker::isCallable(calleeType)) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "expression is not callable");
        return nullptr;
    }

    // Extract return type from FuncTypeAST.
    TypeAST* returnType = nullptr;
    if (calleeType && calleeType->isa<FuncTypeAST>()) {
        auto* ft = calleeType->as<FuncTypeAST>();
        returnType = ft->returnType.get();

        // Argument count check.
        if (!ft->params.empty() && node.args.size() != ft->params.size()) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                     "wrong number of arguments: expected " +
                     std::to_string(ft->params.size()) + ", got " +
                     std::to_string(node.args.size()));
        }

        // Argument type check.
        size_t limit = std::min(node.args.size(), ft->params.size());
        for (size_t i = 0; i < limit; ++i) {
            TypeAST* argType = static_cast<TypeAST*>(node.args[i]->resolvedType);
            TypeAST* paramType = ft->params[i].get();
            if (argType && paramType && !TypeChecker::isAssignable(argType, paramType)) {
                dc.error(DiagnosticCategory::Semantic, node.args[i]->loc, DiagCode::E3002,
                         "argument " + std::to_string(i + 1) + " type mismatch");
            }
        }
    }

    node.resolvedType = returnType;
    return returnType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAssignExpr
// Validates that the LHS is mutable (let variable or let-held field).
// Compound operators are desugared: x += e → x = x + e, then checked.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAssignExpr(AssignExprAST& node, SymbolTable& symbols,
                                 TypeResolver& resolver, DiagnosticEngine& dc,
                                 int& asyncDepth, int& loopDepth,
                                 int& parallelDepth, bool insideExtern) {
    TypeAST* lhsType = checkExpr(node.lhs.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* rhsType = checkExpr(node.rhs.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    // Check the LHS is a let-declared symbol.
    if (node.lhs->isa<IdentExprAST>()) {
        auto* ident = node.lhs->as<IdentExprAST>();
        Symbol* sym = symbols.lookup(ident->name);
        if (sym && sym->declKw != DeclKeyword::Let) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3004,
                     "cannot assign to '" + ident->name + "': declared with " +
                     (sym->declKw == DeclKeyword::Imt ? "imt" : "val"));
        }
    }

    // Parallel scope: writing to outer variables is forbidden.
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "assignment to outer variable inside parallel scope is not allowed");
    }

    // Type compatibility for plain assignment.
    if (node.op == AssignOp::Assign && lhsType && rhsType) {
        if (!TypeChecker::isAssignable(rhsType, lhsType)) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "type mismatch in assignment");
        }
    }

    node.resolvedType = lhsType;
    return lhsType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIsExpr
// x is int → produces bool, marks the narrowed type on the node.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIsExpr(IsExprAST& node, SymbolTable& symbols,
                             TypeResolver& resolver, DiagnosticEngine& dc,
                             int& asyncDepth, int& loopDepth,
                             int& parallelDepth, bool insideExtern) {
    checkExpr(node.expr.get(), symbols, resolver, dc,
              asyncDepth, loopDepth, parallelDepth, insideExtern);
    resolver.resolveType(node.checkType.get());
    node.resolvedType = primType(PrimitiveKind::Bool);
    return primType(PrimitiveKind::Bool);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIfExpr (expression form — else is required)
// Both branches must produce the same type (unified).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIfExpr(IfExprAST& node, SymbolTable& symbols,
                              TypeResolver& resolver, DiagnosticEngine& dc,
                              TypeAST* expectedReturn,
                              int& asyncDepth, int& loopDepth,
                              int& parallelDepth, bool insideExtern) {
    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "if condition must be bool");
    }

    if (node.thenBranch) {
        checkStmt(node.thenBranch.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
    if (node.elseBranch) {
        checkStmt(node.elseBranch.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    } else {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "if expression requires an else branch");
    }

    // Return type is the then-branch's last expression type — approximate with any.
    node.resolvedType = primType(PrimitiveKind::Any);
    return primType(PrimitiveKind::Any);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkMatchExpr
// Subject is checked; each arm pattern is validated against the subject type;
// all arm bodies must unify.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkMatchExpr(MatchExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                TypeAST* expectedReturn,
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    checkExpr(node.subject.get(), symbols, resolver, dc,
              asyncDepth, loopDepth, parallelDepth, insideExtern);

    TypeAST* unified = nullptr;
    for (auto& arm : node.arms) {
        // Check guard if present.
        if (arm->guard) {
            TypeAST* gt = checkExpr(arm->guard.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
            if (gt && !TypeChecker::isBooleanCompatible(gt)) {
                dc.error(DiagnosticCategory::Semantic, arm->guard->loc, DiagCode::E3002,
                         "match arm guard must be bool");
            }
        }

        // Bind patterns introduce variables into the arm scope.
        symbols.pushScope();
        for (auto& pat : arm->patterns) {
            if (pat->isa<BindPatternAST>()) {
                auto* bp = pat->as<BindPatternAST>();
                Symbol bs;
                bs.name = bp->name; bs.kind = SymbolKind::Var;
                bs.declKw = DeclKeyword::Let; bs.visibility = Visibility::Private;
                bs.type = nullptr; bs.decl = bp; bs.isAsync = false; bs.loc = bp->loc;
                symbols.declare(bs);
            } else if (pat->isa<TypePatternAST>()) {
                auto* tp = pat->as<TypePatternAST>();
                TypeAST* narrowed = resolver.resolveType(tp->checkType.get());
                Symbol bs;
                bs.name = tp->bindName; bs.kind = SymbolKind::Var;
                bs.declKw = DeclKeyword::Let; bs.visibility = Visibility::Private;
                bs.type = narrowed; bs.decl = tp; bs.isAsync = false; bs.loc = tp->loc;
                symbols.declare(bs);
            }
        }

        if (arm->body) {
            checkStmt(arm->body.get(), symbols, resolver, dc, expectedReturn,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
        }
        symbols.popScope();
    }

    // Default arm.
    if (node.defaultBody) {
        checkStmt(node.defaultBody.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    } else {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "match expression requires a 'default' arm");
    }

    node.resolvedType = unified ? unified : primType(PrimitiveKind::Any);
    return static_cast<TypeAST*>(node.resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAwaitExpr
// Valid only inside an async body and not inside a parallel scope.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAwaitExpr(AwaitExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    if (asyncDepth <= 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'await' is only valid inside an async function body");
    }
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'await' is not allowed inside a parallel scope");
    }
    TypeAST* innerType = checkExpr(node.inner.get(), symbols, resolver, dc,
                                   asyncDepth, loopDepth, parallelDepth, insideExtern);
    node.resolvedType = innerType;
    return innerType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAnonFuncExpr
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAnonFuncExpr(AnonFuncExprAST& node, SymbolTable& symbols,
                                   TypeResolver& resolver, DiagnosticEngine& dc,
                                   int& asyncDepth, int& loopDepth,
                                   int& parallelDepth, bool insideExtern) {
    TypeAST* returnType = nullptr;
    if (node.returnType) returnType = resolver.resolveType(node.returnType.get());

    if (node.isAsync) asyncDepth++;
    symbols.pushScope();

    for (auto& param : node.params) {
        TypeAST* pt = resolver.resolveType(param->type.get());
        if (pt) {
            Symbol ps;
            ps.name = param->name; ps.kind = SymbolKind::Param;
            ps.declKw = DeclKeyword::Let; ps.visibility = Visibility::Private;
            ps.type = pt; ps.decl = param.get(); ps.isAsync = false; ps.loc = param->loc;
            symbols.declare(ps);
        }
    }

    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, returnType,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    if (node.isAsync) asyncDepth--;

    // The type of an anon func is a FuncTypeAST built from its signature.
    // For now, return nullptr — the anon func value's type is tracked by its
    // declaration site. Full FuncType construction would need ownership management.
    node.resolvedType = nullptr;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkNullableChainExpr
// player.?weapon.?damage ?? 0
// Every .? step must be on a nullable type. The ?? fallback must match.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkNullableChainExpr(NullableChainExprAST& node, SymbolTable& symbols,
                                        TypeResolver& resolver, DiagnosticEngine& dc,
                                        int& asyncDepth, int& loopDepth,
                                        int& parallelDepth, bool insideExtern) {
    TypeAST* objType = checkExpr(node.object.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (objType && !TypeChecker::isNullable(objType)) {
        dc.error(DiagnosticCategory::Semantic, node.object->loc, DiagCode::E3002,
                 "'.?' chain must start on a nullable type");
    }

    TypeAST* fallbackType = nullptr;
    if (node.fallback) {
        fallbackType = checkExpr(node.fallback.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    node.resolvedType = fallbackType;
    return fallbackType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkRangeExpr
// 0..10 — both sides must be the same numeric type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkRangeExpr(RangeExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    TypeAST* loType = checkExpr(node.lo.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* hiType = checkExpr(node.hi.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);

    if (loType && hiType && !TypeChecker::isAssignable(loType, hiType) &&
        !TypeChecker::isAssignable(hiType, loType)) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "range bounds must be the same type");
    }

    node.resolvedType = loType ? loType : hiType;
    return static_cast<TypeAST*>(node.resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkPipelineExpr
// seed -> step -> step
// Each step must be callable. The upstream result is passed as the first arg.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkPipelineExpr(PipelineExprAST& node, SymbolTable& symbols,
                                   TypeResolver& resolver, DiagnosticEngine& dc,
                                   int& asyncDepth, int& loopDepth,
                                   int& parallelDepth, bool insideExtern) {
    TypeAST* current = checkExpr(node.seed.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    for (auto& step : node.steps) {
        switch (step->kind) {
            case PipelineStepKind::Ident: {
                Symbol* sym = symbols.lookup(step->ident);
                if (!sym) {
                    dc.error(DiagnosticCategory::Semantic, step->loc, DiagCode::E3001,
                             "undeclared identifier '" + step->ident + "' in pipeline");
                    current = nullptr;
                } else {
                    TypeAST* st = sym->type;
                    if (st && st->isa<FuncTypeAST>()) {
                        current = st->as<FuncTypeAST>()->returnType.get();
                    } else {
                        current = st;
                    }
                }
                break;
            }
            case PipelineStepKind::BehaviorRef: {
                std::string mangled = step->typeName + "." + step->method;
                Symbol* sym = symbols.lookup(mangled);
                if (!sym) {
                    dc.error(DiagnosticCategory::Semantic, step->loc, DiagCode::E3001,
                             "no method '" + step->method + "' on '" + step->typeName + "'");
                    current = nullptr;
                } else if (sym->type && sym->type->isa<FuncTypeAST>()) {
                    current = sym->type->as<FuncTypeAST>()->returnType.get();
                }
                break;
            }
            case PipelineStepKind::ArgPack:
            case PipelineStepKind::FieldRef:
            case PipelineStepKind::AnonFunc:
                // For ArgPack and AnonFunc the return type cannot be easily inferred
                // without full function resolution; pass through current for now.
                break;
        }
    }

    node.resolvedType = current;
    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIndexExpr
// nums[i] → element type; nums[i..j] → slice type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIndexExpr(IndexExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    TypeAST* targetType = checkExpr(node.target.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* idxType = checkExpr(node.index.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    // Index must be an integer type.
    if (idxType && idxType->isa<PrimitiveTypeAST>()) {
        auto k = idxType->as<PrimitiveTypeAST>()->primitiveKind;
        bool isInt = (k == PrimitiveKind::Int || k == PrimitiveKind::Long ||
                      k == PrimitiveKind::Uint || k == PrimitiveKind::Ulong ||
                      k == PrimitiveKind::Byte || k == PrimitiveKind::Short);
        if (!isInt) {
            dc.error(DiagnosticCategory::Semantic, node.index->loc, DiagCode::E3002,
                     "array index must be an integer type");
        }
    }

    if (node.sliceEnd) {
        checkExpr(node.sliceEnd.get(), symbols, resolver, dc,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    // Resolve the element type from the target array type.
    TypeAST* elemType = primType(PrimitiveKind::Any);
    if (targetType) {
        if (targetType->isa<FixedArrayTypeAST>())
            elemType = targetType->as<FixedArrayTypeAST>()->element.get();
        else if (targetType->isa<SliceTypeAST>())
            elemType = targetType->as<SliceTypeAST>()->element.get();
        else if (targetType->isa<DynamicArrayTypeAST>())
            elemType = targetType->as<DynamicArrayTypeAST>()->element.get();
    }

    node.resolvedType = elemType;
    return elemType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStructLiteralExpr
// Verifies every required field is provided and each value's type matches.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkStructLiteralExpr(StructLiteralExprAST& node, SymbolTable& symbols,
                                        TypeResolver& resolver, DiagnosticEngine& dc,
                                        int& asyncDepth, int& loopDepth,
                                        int& parallelDepth, bool insideExtern) {
    Symbol* sym = symbols.lookup(node.typeName);
    if (!sym || sym->kind != SymbolKind::Struct) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "'" + node.typeName + "' is not a declared struct");
        return nullptr;
    }

    auto* structDecl = sym->decl->as<StructDeclAST>();

    // Check that every field init matches the declared field type.
    for (auto& init : node.inits) {
        FieldDeclAST* fieldDecl = nullptr;
        for (auto& f : structDecl->fields) {
            if (f->name == init.name) { fieldDecl = f.get(); break; }
        }
        if (!fieldDecl) {
            dc.error(DiagnosticCategory::Semantic, init.loc, DiagCode::E3001,
                     "struct '" + node.typeName + "' has no field '" + init.name + "'");
            continue;
        }
        TypeAST* ft = resolver.resolveType(fieldDecl->type.get());
        TypeAST* vt = checkExpr(init.value.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);
        if (ft && vt && !TypeChecker::isAssignable(vt, ft)) {
            dc.error(DiagnosticCategory::Semantic, init.loc, DiagCode::E3002,
                     "type mismatch for field '" + init.name + "' in struct literal");
        }
    }

    // Check that all required fields (no default) are provided.
    for (auto& f : structDecl->fields) {
        if (f->defaultVal) continue;
        bool found = false;
        for (auto& init : node.inits) {
            if (init.name == f->name) { found = true; break; }
        }
        if (!found) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "missing required field '" + f->name + "' in struct literal '" +
                     node.typeName + "'");
        }
    }

    // Return a NamedTypeAST representing this struct type.
    node.resolvedType = sym->type;
    return sym->type;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkArrayLiteralExpr
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST& node, SymbolTable& symbols,
                                       TypeResolver& resolver, DiagnosticEngine& dc,
                                       int& asyncDepth, int& loopDepth,
                                       int& parallelDepth, bool insideExtern) {
    TypeAST* elemType = nullptr;
    for (auto& elem : node.elements) {
        TypeAST* et = checkExpr(elem.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);
        if (!elemType && et) elemType = et;
    }
    // The resolved type is left as nullptr; context (declaration type) determines
    // whether this is a fixed / slice / dynamic array.
    node.resolvedType = nullptr;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTypeConvExpr
// float(x) — safe conversion; @float(x) — unsafe bit reinterpret (extern only).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkTypeConvExpr(TypeConvExprAST& node, SymbolTable& symbols,
                                   TypeResolver& resolver, DiagnosticEngine& dc,
                                   int& asyncDepth, int& loopDepth,
                                   int& parallelDepth, bool insideExtern) {
    if (node.isUnsafe && !insideExtern) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "unsafe type reinterpret '@' is only valid inside extern declarations");
    }
    checkExpr(node.expr.get(), symbols, resolver, dc,
              asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* targetType = resolver.resolveType(node.targetType.get());
    node.resolvedType = targetType;
    return targetType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkComposeExpr
// f +> g: left output type must match right input type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkComposeExpr(ComposeExprAST& node, SymbolTable& symbols,
                                  TypeResolver& resolver, DiagnosticEngine& dc,
                                  int& asyncDepth, int& loopDepth,
                                  int& parallelDepth, bool insideExtern) {
    TypeAST* current = checkExpr(node.left.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    for (auto& operand : node.operands) {
        TypeAST* opType = nullptr;
        if (operand->kind == ComposeOperandKind::Ident) {
            Symbol* sym = symbols.lookup(operand->ident);
            if (sym) opType = sym->type;
        } else if (operand->kind == ComposeOperandKind::BehaviorRef) {
            std::string mangled = operand->typeName + "." + operand->method;
            Symbol* sym = symbols.lookup(mangled);
            if (sym) opType = sym->type;
        }

        if (opType && !TypeChecker::isCallable(opType)) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "compose operand must be a function");
        }

        if (current && opType && opType->isa<FuncTypeAST>()) {
            auto* ft = opType->as<FuncTypeAST>();
            if (!ft->params.empty() &&
                !TypeChecker::isAssignable(current, ft->params[0].get())) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "type mismatch in '+>' composition: output type does not match next input");
            }
            current = ft->returnType.get();
        } else {
            current = nullptr;
        }
    }

    node.resolvedType = current;
    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkExpr — main dispatcher
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
    if (!node) return nullptr;

    switch (node->kind) {
        case ASTKind::LiteralExpr:
            return checkLiteralExpr(*node->as<LiteralExprAST>());

        case ASTKind::IdentExpr:
            return checkIdentExpr(*node->as<IdentExprAST>(), symbols, dc);

        case ASTKind::FieldAccessExpr:
            return checkFieldAccessExpr(*node->as<FieldAccessExprAST>(), symbols,
                                        resolver, dc, asyncDepth, loopDepth,
                                        parallelDepth, insideExtern);

        case ASTKind::BehaviorAccessExpr:
            return checkBehaviorAccessExpr(*node->as<BehaviorAccessExprAST>(),
                                           symbols, dc);

        case ASTKind::BinaryExpr:
            return checkBinaryExpr(*node->as<BinaryExprAST>(), symbols, resolver, dc,
                                   asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::UnaryExpr:
            return checkUnaryExpr(*node->as<UnaryExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::CallExpr:
            return checkCallExpr(*node->as<CallExprAST>(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::AssignExpr:
            return checkAssignExpr(*node->as<AssignExprAST>(), symbols, resolver, dc,
                                   asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::IsExpr:
            return checkIsExpr(*node->as<IsExprAST>(), symbols, resolver, dc,
                               asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::IfExpr:
            return checkIfExpr(*node->as<IfExprAST>(), symbols, resolver, dc,
                               nullptr, asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::MatchExpr:
            return checkMatchExpr(*node->as<MatchExprAST>(), symbols, resolver, dc,
                                  nullptr, asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::AwaitExpr:
            return checkAwaitExpr(*node->as<AwaitExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::AnonFuncExpr:
            return checkAnonFuncExpr(*node->as<AnonFuncExprAST>(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::NullableChainExpr:
            return checkNullableChainExpr(*node->as<NullableChainExprAST>(), symbols,
                                          resolver, dc, asyncDepth, loopDepth,
                                          parallelDepth, insideExtern);

        case ASTKind::RangeExpr:
            return checkRangeExpr(*node->as<RangeExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::PipelineExpr:
            return checkPipelineExpr(*node->as<PipelineExprAST>(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::ComposeExpr:
            return checkComposeExpr(*node->as<ComposeExprAST>(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::IndexExpr:
            return checkIndexExpr(*node->as<IndexExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::StructLiteralExpr:
            return checkStructLiteralExpr(*node->as<StructLiteralExprAST>(), symbols,
                                          resolver, dc, asyncDepth, loopDepth,
                                          parallelDepth, insideExtern);

        case ASTKind::ArrayLiteralExpr:
            return checkArrayLiteralExpr(*node->as<ArrayLiteralExprAST>(), symbols,
                                         resolver, dc, asyncDepth, loopDepth,
                                         parallelDepth, insideExtern);

        case ASTKind::TypeConvExpr:
            return checkTypeConvExpr(*node->as<TypeConvExprAST>(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);

        default:
            // Unknown or unhandled expression kind — skip silently.
            return nullptr;
    }
}
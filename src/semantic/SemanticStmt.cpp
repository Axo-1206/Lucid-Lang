/**
 * @file SemanticStmt.cpp
 *
 * @nutshell Maintains structural depth and controls block-level logic (flow and loops).
 *
 * @responsibility Phase 3c of semantic analysis: walks statement AST nodes and
 *   manages scope depth, loop/async/parallel context flags, and return-type tracking.
 *
 * @logic
 *   checkBlock          — pushScope, check each stmt, popScope
 *   checkExprStmt       — checkExpr; warn if Result<T> discarded
 *   checkDeclStmt       — dispatch to checkVarDecl or checkFuncDecl
 *   checkIfStmt         — check condition is bool; check branches
 *   checkSwitchStmt     — check subject; check each case
 *   checkForStmt        — check iterable; declare loop var; check body
 *   checkWhileStmt      — check condition is bool; loop depth
 *   checkDoWhileStmt    — body first, then condition
 *   checkReturnStmt     — validate return type matches expected
 *   checkBreakStmt      — error if not inside a loop or inside parallel
 *   checkContinueStmt   — same as break
 *   checkMultiVarDecl   — multiple variable declaration
 *   checkMultiAssignStmt— multiple assignment
 *
 * @related SemanticAnalyzer.cpp, SemanticDecl.cpp, SemanticExpr.cpp
 */

#include "ast/BaseAST.hpp"
#include "header/SymbolTable.hpp"
#include "header/TypeResolver.hpp"
#include "header/SemanticContext.hpp"
#include "header/SemanticChecker.hpp"
#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/DeclAST.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <iostream>
#include <iterator>
#include <vector>
#include <string>

// Forward declarations from SemanticExpr.cpp and SemanticDecl.cpp
TypeAST* checkExpr(ExprAST* node, SemanticContext& ctx);
void checkVarDecl(VarDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkFuncDecl(FuncDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx); // for local decls inside blocks

// ─────────────────────────────────────────────────────────────────────────────
// BlockStmt – opens a new scope, checks statements, then pops scope
// ─────────────────────────────────────────────────────────────────────────────
static void checkBlockStmt(BlockStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    ctx.symbols.pushScope();
    for (auto& stmt : node.stmts) {
        checkStmt(stmt.get(), ctx, expectedReturn);
        // If a fatal error occurred, we might stop, but we continue for better diagnostics
    }
    ctx.symbols.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// ExprStmt – expression used as a statement; warn if non-void result discarded
// ─────────────────────────────────────────────────────────────────────────────
static void checkExprStmt(ExprStmtAST& node, SemanticContext& ctx) {
    TypeAST* exprType = checkExpr(node.expr.get(), ctx);
    if (exprType && !exprType->isa<PrimitiveTypeAST>()) {
        // If it's a function call that returns a non-void type, warn about unused result
        // Only warn if the expression is a call to a function that returns something.
        // For simplicity, we check if the expression is a CallExprAST or has side effects.
        // We'll emit a warning for any non-void expression that isn't a call to a void function.
        // Actually, the language rule: discarding a non-void result is a warning.
        ctx.dc.warning(DiagnosticCategory::Semantic, node.loc, DiagCode::W3003,
                       "unused result of expression; value discarded");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DeclStmt – local declaration (var, func, type alias, etc.)
// ─────────────────────────────────────────────────────────────────────────────
static void checkDeclStmt(DeclStmtAST& node, SemanticContext& ctx) {
    if (!node.decl) return;

    // Dispatch to the appropriate declaration checker with isLocal = true
    if (auto* varDecl = node.decl->as<VarDeclAST>()) {
        checkVarDecl(*varDecl, ctx, true);
    } else if (auto* funcDecl = node.decl->as<FuncDeclAST>()) {
        checkFuncDecl(*funcDecl, ctx, true);
    } else {
        // For other declarations (type alias, struct, enum, etc.) we use the top-level
        // checker but with local context. However, those may not be allowed locally.
        // The parser should already reject them, but we handle gracefully.
        checkTopLevelDecl(node.decl.get(), ctx);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IfStmt – conditional branch; condition must be boolean
// ─────────────────────────────────────────────────────────────────────────────
static void checkIfStmt(IfStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (!condType) return;

    if (!ctx.checker.isBooleanCompatible(condType)) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                     "if condition must be boolean");
        return;
    }

    // Check then branch
    if (node.thenBranch) {
        checkStmt(node.thenBranch.get(), ctx, expectedReturn);
    }

    // Check else branch (if present)
    if (node.elseBranch) {
        // Else branch can be a BlockStmtAST or another IfStmtAST
        checkStmt(node.elseBranch.get(), ctx, expectedReturn);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WhileStmt – condition loop; condition must be boolean
// ─────────────────────────────────────────────────────────────────────────────
static void checkWhileStmt(WhileStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (!condType) return;

    if (!ctx.checker.isBooleanCompatible(condType)) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                     "while condition must be boolean");
        return;
    }

    ctx.enterLoop();
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    }
    ctx.exitLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// ForStmt – iteration over a collection or range
// ─────────────────────────────────────────────────────────────────────────────
static void checkForStmt(ForStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    // First, check the iterable expression
    TypeAST* iterableType = checkExpr(node.iterable.get(), ctx);
    if (!iterableType) return;

    // Determine the element type of the iterable
    TypeAST* elementType = nullptr;
    if (iterableType->isa<FixedArrayTypeAST>()) {
        elementType = iterableType->as<FixedArrayTypeAST>()->element.get();
    } else if (iterableType->isa<SliceTypeAST>()) {
        elementType = iterableType->as<SliceTypeAST>()->element.get();
    } else if (iterableType->isa<DynamicArrayTypeAST>()) {
        elementType = iterableType->as<DynamicArrayTypeAST>()->element.get();
    } else if (iterableType->isa<RangeExprAST>() || (iterableType && iterableType->kind == ASTKind::RangeExpr)) {
        // Range expression yields integer
        elementType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int).get();
    } else if (iterableType->isa<NullableTypeAST>()) {
        // Nullable iterable not allowed
        ctx.dc.error(DiagnosticCategory::Semantic, node.iterable->loc, DiagCode::E3002,
                     "for loop iterable cannot be nullable");
        return;
    } else {
        ctx.dc.error(DiagnosticCategory::Semantic, node.iterable->loc, DiagCode::E3002,
                     "for loop iterable must be an array, slice, dynamic array, or range");
        return;
    }

    // Determine the iteration variable type (may be explicitly annotated)
    TypeAST* varType = nullptr;
    if (node.iterVar && node.iterVar->type) {
        varType = ctx.resolver.resolveType(node.iterVar->type.get());
        if (!varType) return;
        if (!ctx.checker.isAssignable(elementType, varType)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.iterVar->loc, DiagCode::E3002,
                         "iteration variable type mismatch: expected " +
                         LucDebug::kindToString(elementType->kind) + ", got " +
                         LucDebug::kindToString(varType->kind));
            return;
        }
    } else {
        varType = elementType;
    }

    // Push a new scope for the loop variable
    ctx.symbols.pushScope();

    // Declare the iteration variable
    if (node.iterVar) {
        Symbol varSym;
        varSym.name = node.iterVar->name;
        varSym.kind = SymbolKind::Var;
        varSym.declKw = DeclKeyword::Let; // iteration variable is mutable inside loop
        varSym.visibility = Visibility::Private;
        varSym.type = varType;
        varSym.decl = node.iterVar.get();
        varSym.loc = node.iterVar->loc;
        if (!ctx.symbols.declare(varSym)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.iterVar->loc, DiagCode::E3005,
                         "duplicate declaration of loop variable '" +
                         std::string(ctx.pool.lookup(node.iterVar->name)) + "'");
        }
    }

    // Optional step expression (only for range loops)
    if (node.step) {
        TypeAST* stepType = checkExpr(node.step.get(), ctx);
        if (!stepType) {
            // error already reported
        } else if (!ctx.checker.isIntegerType(stepType)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.step->loc, DiagCode::E3002,
                         "step expression must be integer");
        }
    }

    ctx.enterLoop();
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    }
    ctx.exitLoop();

    ctx.symbols.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// ReturnStmt – exits function with optional value(s)
// ─────────────────────────────────────────────────────────────────────────────
static void checkReturnStmt(ReturnStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    // expectedReturn may be nullptr for void functions
    if (node.values.empty()) {
        // Bare return
        if (expectedReturn != nullptr) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "bare return in non-void function");
        }
        return;
    }

    // Currently we only support single return value (simplified)
    // Multi-return would require checking each value against multiple return types
    if (node.values.size() == 1) {
        TypeAST* retType = checkExpr(node.values[0].get(), ctx);
        if (!retType) return;
        if (expectedReturn == nullptr) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "return value in void function");
            return;
        }
        if (!ctx.checker.isAssignable(retType, expectedReturn)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.values[0]->loc, DiagCode::E3002,
                         "return type mismatch: expected " +
                         LucDebug::kindToString(expectedReturn->kind) + ", got " +
                         LucDebug::kindToString(retType->kind));
        }
    } else {
        // Multi-return – need to compare with function's multiple return types
        // For simplicity, we'll just check that the number matches, but proper
        // implementation would require the function signature's returnTypes vector.
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "multiple return values not yet fully supported");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BreakStmt – exit nearest enclosing loop
// ─────────────────────────────────────────────────────────────────────────────
static void checkBreakStmt(BreakStmtAST& node, SemanticContext& ctx) {
    if (ctx.loopDepth == 0) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "break statement outside loop");
    }
    if (ctx.parallelDepth > 0) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "break not allowed inside parallel block or parallel for");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ContinueStmt – skip to next iteration of nearest enclosing loop
// ─────────────────────────────────────────────────────────────────────────────
static void checkContinueStmt(ContinueStmtAST& node, SemanticContext& ctx) {
    if (ctx.loopDepth == 0) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "continue statement outside loop");
    }
    if (ctx.parallelDepth > 0) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "continue not allowed inside parallel block or parallel for");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SwitchStmt – value dispatch (statement version, no fallthrough)
// ─────────────────────────────────────────────────────────────────────────────
static void checkSwitchStmt(SwitchStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    TypeAST* subjectType = checkExpr(node.subject.get(), ctx);
    if (!subjectType) return;

    // Value comparability required for case values
    if (!ctx.checker.isValueComparable(subjectType, &ctx.symbols)) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.subject->loc, DiagCode::E3002,
                     "switch subject type does not support value equality");
        return;
    }

    // Check each case
    for (auto& caseNode : node.cases) {
        if (!caseNode) continue;
        // Check each case value (must be constant and comparable)
        for (auto& valExpr : caseNode->values) {
            TypeAST* valType = checkExpr(valExpr.get(), ctx);
            if (!valType) continue;
            if (!ctx.checker.isAssignable(valType, subjectType)) {
                ctx.dc.error(DiagnosticCategory::Semantic, valExpr->loc, DiagCode::E3002,
                             "case value type mismatch");
            }
            // Verify that the case value is a constant (if it's a literal, it's fine)
            // We could also check for duplicate values, but that's more complex.
        }
        // Check the case body
        if (caseNode->body) {
            checkStmt(caseNode->body.get(), ctx, expectedReturn);
        }
    }

    // Check default body if present
    if (node.defaultBody) {
        checkStmt(node.defaultBody.get(), ctx, expectedReturn);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DoWhileStmt – body-first loop
// ─────────────────────────────────────────────────────────────────────────────
static void checkDoWhileStmt(DoWhileStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    ctx.enterLoop();
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    }
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (condType && !ctx.checker.isBooleanCompatible(condType)) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                     "do-while condition must be boolean");
    }
    ctx.exitLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// MultiVarDeclAST – multiple variable declaration (let a int, b int = f())
// ─────────────────────────────────────────────────────────────────────────────
static void checkMultiVarDecl(MultiVarDeclAST& node, SemanticContext& ctx) {
    // Check the RHS expression
    TypeAST* rhsType = checkExpr(node.rhs.get(), ctx);
    if (!rhsType) return;

    // For multi-return, the RHS must be a call that returns as many values as variables
    // We'll assume the RHS is a call expression and we can get its return types.
    // For simplicity, we'll just check that the number of variables matches the number
    // of return values (if we can determine that). Here we'll only check type compatibility
    // for single return case.
    if (node.vars.size() == 1) {
        // Single variable: check assignability from RHS type to the declared type
        TypeAST* varType = ctx.resolver.resolveType(node.vars[0].second.get());
        if (!varType) return;
        if (!ctx.checker.isAssignable(rhsType, varType)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.rhs->loc, DiagCode::E3002,
                         "initializer type mismatch for variable '" +
                         std::string(ctx.pool.lookup(node.vars[0].first)) + "'");
            return;
        }
        // Declare the variable
        Symbol varSym;
        varSym.name = node.vars[0].first;
        varSym.kind = SymbolKind::Var;
        varSym.declKw = node.keyword;
        varSym.visibility = Visibility::Private;
        varSym.type = varType;
        varSym.decl = &node;
        varSym.loc = node.loc;
        if (!ctx.symbols.declare(varSym)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                         "duplicate declaration of variable '" +
                         std::string(ctx.pool.lookup(node.vars[0].first)) + "'");
        }
    } else {
        // Multi-variable declaration: need to get multiple return types from RHS
        // For now, we report not fully supported
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "multi-variable declaration with more than one variable not fully supported");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MultiAssignStmtAST – multiple assignment (a, b = g())
// ─────────────────────────────────────────────────────────────────────────────
static void checkMultiAssignStmt(MultiAssignStmtAST& node, SemanticContext& ctx) {
    // Similar to multi-var decl but without declaration
    if (node.lhs.empty()) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "multi-assignment with no left-hand side");
        return;
    }

    TypeAST* rhsType = checkExpr(node.rhs.get(), ctx);
    if (!rhsType) return;

    if (node.lhs.size() == 1) {
        // Single assignment: check assignability from RHS type to LHS type
        TypeAST* lhsType = checkExpr(node.lhs[0].get(), ctx);
        if (!lhsType) return;
        if (!ctx.checker.isAssignable(rhsType, lhsType)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.rhs->loc, DiagCode::E3002,
                         "assignment type mismatch");
        }
    } else {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "multi-assignment with more than one left-hand side not fully supported");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The main entry point: checkStmt (dispatcher)
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SemanticContext& ctx, TypeAST* expectedReturn) {
    if (!node) return;

    switch (node->kind) {
        case ASTKind::BlockStmt:
            checkBlockStmt(*node->as<BlockStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ExprStmt:
            checkExprStmt(*node->as<ExprStmtAST>(), ctx);
            break;
        case ASTKind::DeclStmt:
            checkDeclStmt(*node->as<DeclStmtAST>(), ctx);
            break;
        case ASTKind::IfStmt:
            checkIfStmt(*node->as<IfStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::WhileStmt:
            checkWhileStmt(*node->as<WhileStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ForStmt:
            checkForStmt(*node->as<ForStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ReturnStmt:
            checkReturnStmt(*node->as<ReturnStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::BreakStmt:
            checkBreakStmt(*node->as<BreakStmtAST>(), ctx);
            break;
        case ASTKind::ContinueStmt:
            checkContinueStmt(*node->as<ContinueStmtAST>(), ctx);
            break;
        case ASTKind::SwitchStmt:
            checkSwitchStmt(*node->as<SwitchStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::DoWhileStmt:
            checkDoWhileStmt(*node->as<DoWhileStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::MultiVarDecl:
            checkMultiVarDecl(*node->as<MultiVarDeclAST>(), ctx);
            break;
        case ASTKind::MultiAssignStmt:
            checkMultiAssignStmt(*node->as<MultiAssignStmtAST>(), ctx);
            break;
        default:
            ctx.dc.error(DiagnosticCategory::Semantic, node->loc, DiagCode::E3002,
                         "unsupported statement kind: " + LucDebug::kindToString(node->kind));
            break;
    }
}
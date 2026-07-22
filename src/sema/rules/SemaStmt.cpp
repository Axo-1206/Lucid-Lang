/**
 * @file SemaStmt.cpp
 * @brief Implements Sema.hpp's "Statements" section — analyzeStmt() and every
 *        specific analyze*Stmt()/analyze*() function for statements.
 *
 * @architectural_note Control-flow return facts
 *   Every analyze*Stmt() function in this file returns a bool indicating
 *   whether the statement is guaranteed to transfer control out of the
 *   enclosing block on every path (i.e., it "diverges"). This is used by:
 *     - analyzeFuncDecl() to detect missing returns in non-void functions
 *     - analyzeIfStmt()/analyzeSwitchStmt() to propagate divergence through
 *       branches
 *     - analyzeBlock() to know whether the block's last statement diverges
 *
 *   The rules mirror the grammar's control-flow constructs:
 *     - return/break/continue always diverge (return true)
 *     - if/switch diverge if all branches diverge
 *     - loops do NOT diverge (they can exit normally)
 *     - blocks diverge if their last statement diverges
 *     - expression/declaration statements never diverge
 *
 * @architectural_note Scoping and semantic contexts
 *   Blocks open a `ScopedScope` (for local declarations) and, when part of
 *   a loop/function/switch, are wrapped in the appropriate
 *   `ScopedSemanticContext` by their parent statement's analyze function.
 *   This means analyzeBlock() itself does NOT push a SemanticContext — that's
 *   the caller's responsibility (e.g., analyzeFuncDecl() pushes FuncBody,
 *   analyzeWhileStmt() pushes LoopBody, etc.).
 */

#include "../Sema.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <unordered_set>

namespace sema {

// =============================================================================
// analyzeStmt — Dispatch
// =============================================================================

/**
 * @brief Dispatch a statement to its specific analyze*Stmt() function.
 *
 * Returns the divergence fact (true = guaranteed to transfer control out)
 * from the specific function, or false for unknown/error-recovery nodes.
 *
 * @param stmt The statement to analyze (may be nullptr — returns false).
 * @param ctx  The semantic context.
 * @return true if the statement guarantees control flow exits the block.
 */
bool analyzeStmt(StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            return analyzeBlock(stmt->as<BlockStmtAST>(), ctx);
        case ASTKind::ExprStmt:         return analyzeExprStmt(stmt->as<ExprStmtAST>(), ctx);
        case ASTKind::DeclStmt:         return analyzeDeclStmt(stmt->as<DeclStmtAST>(), ctx);
        case ASTKind::IfStmt:           return analyzeIfStmt(stmt->as<IfStmtAST>(), ctx);
        case ASTKind::SwitchStmt:       return analyzeSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
        case ASTKind::ForStmt:          return analyzeForStmt(stmt->as<ForStmtAST>(), ctx);
        case ASTKind::WhileStmt:        return analyzeWhileStmt(stmt->as<WhileStmtAST>(), ctx);
        case ASTKind::DoWhileStmt:      return analyzeDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
        case ASTKind::ReturnStmt:       return analyzeReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
        case ASTKind::BreakStmt:        return analyzeBreakStmt(stmt->as<BreakStmtAST>(), ctx);
        case ASTKind::ContinueStmt:     return analyzeContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
        case ASTKind::MultiVarDecl:     return analyzeMultiVarDecl(stmt->as<MultiVarDeclAST>(), ctx);
        case ASTKind::MultiAssignStmt:  return analyzeMultiAssignStmt(stmt->as<MultiAssignStmtAST>(), ctx);
        case ASTKind::AsyncExpr:        return analyzeAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
        case ASTKind::AwaitExpr:        return analyzeAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
        case ASTKind::SpawnExpr:        return analyzeSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
        case ASTKind::JoinExpr:         return analyzeJoinStmt(stmt->as<JoinStmtAST>(), ctx);
        default:
            // Unknown/error-recovery statement — doesn't diverge
            return false;
    }
}

// =============================================================================
// analyzeBlock
// =============================================================================

/**
 * @brief Analyze a block: open a scope, analyze each statement in order,
 *        then close the scope.
 *
 * The block diverges if its last statement diverges. This matches the
 * grammar: a block's control flow is the flow of its last statement.
 *
 * @param block The block statement.
 * @param ctx   The semantic context.
 * @return true if the block's last statement diverges.
 */
bool analyzeBlock(BlockStmtAST* block, SemaContext& ctx) {
    if (!block) return false;

    ScopedScope scope(ctx);
    bool lastDiverges = false;

    for (StmtAST* stmt : block->stmts) {
        lastDiverges = analyzeStmt(stmt, ctx);
        // If we hit a diverging statement, any statements after it are
        // unreachable. We still process them for diagnostics (unreachable
        // code warning), but the block's divergence is determined by the
        // last diverging statement.
        if (lastDiverges) {
            // TODO: Warn about unreachable code after a diverging statement
            // break; // Uncomment to stop processing after divergence
        }
    }

    return lastDiverges;
}

// =============================================================================
// analyzeExprStmt
// =============================================================================

/**
 * @brief Analyze an expression statement: type-check the expression,
 *        discard its result.
 *
 * Expression statements never diverge (they're just side effects).
 *
 * @param stmt The expression statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeExprStmt(ExprStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->expr) return false;

    TypeAST* type = checkExpr(stmt->expr, ctx);

    // TODO: Warn if the expression's result is ignored and it has a
    //       non-void, non-nullable, non-fallible type (i.e., the user
    //       might have intended to use the result).

    return false;
}

// =============================================================================
// analyzeDeclStmt
// =============================================================================

/**
 * @brief Analyze a declaration statement: dispatch to analyzeDecl().
 *
 * Declaration statements never diverge.
 *
 * @param stmt The declaration statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeDeclStmt(DeclStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->decl) return false;

    analyzeDecl(stmt->decl, ctx);
    return false;
}

// =============================================================================
// analyzeIfStmt
// =============================================================================

/**
 * @brief Analyze an if statement: type-check the condition, then analyze
 *        both branches.
 *
 * An if statement diverges if both branches diverge (i.e., there's no path
 * that falls through to the next statement). If there's no else branch, the
 * statement never diverges (the condition can be false and fall through).
 *
 * @param stmt The if statement.
 * @param ctx  The semantic context.
 * @return true if both branches diverge and there's an else branch.
 */
bool analyzeIfStmt(IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Type-check the condition (must be bool, or coercible to bool)
    TypeAST* condType = checkExpr(stmt->condition, ctx);

    // TODO: Verify condition type is bool or coercible to bool
    //       (nullable/fallible types are coercible to bool via truthiness)

    // Analyze the then branch
    bool thenDiverges = false;
    if (stmt->thenBranch) {
        // If the then branch is an IfStmtAST (chained else-if), analyze it
        // directly; otherwise, it should be a BlockStmtAST.
        thenDiverges = analyzeStmt(stmt->thenBranch, ctx);
    }

    // Analyze the else branch (if present)
    bool elseDiverges = false;
    if (stmt->elseBranch) {
        elseDiverges = analyzeStmt(stmt->elseBranch, ctx);
    } else {
        // No else branch means there's a path that falls through
        return false;
    }

    // Diverges only if both branches diverge
    return thenDiverges && elseDiverges;
}

// =============================================================================
// analyzeSwitchStmt
// =============================================================================

/**
 * @brief Analyze a switch statement: type-check the subject, analyze each
 *        case, check exhaustiveness for enum types, and determine divergence.
 *
 * A switch statement diverges if:
 *   - The subject is an enum type, all variants are covered, and there's no
 *     default clause (exhaustive), OR
 *   - There's a default clause AND it diverges, OR
 *   - All cases diverge AND there's a default clause that diverges, OR
 *   - All cases diverge and the subject type is an enum with all variants
 *     covered.
 *
 * @param stmt The switch statement.
 * @param ctx  The semantic context.
 * @return true if the switch is exhaustive and all branches diverge.
 */
bool analyzeSwitchStmt(SwitchStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Type-check the subject
    TypeAST* subjectType = checkExpr(stmt->subject, ctx);

    // TODO: Verify subject type is valid for switch:
    //       - integer types
    //       - bool
    //       - char
    //       - string
    //       - enum types
    //       Reject: structs, arrays, floats, function types

    // Analyze each case
    bool allCasesDiverge = true;
    bool anyCaseDiverge = false;

    for (SwitchCaseAST* caseNode : stmt->cases) {
        bool caseDiverges = analyzeSwitchCase(caseNode, ctx);
        if (caseDiverges) {
            anyCaseDiverge = true;
        } else {
            allCasesDiverge = false;
        }
    }

    // Analyze default body (if present)
    bool defaultDiverges = false;
    if (stmt->defaultBody) {
        defaultDiverges = analyzeBlock(stmt->defaultBody, ctx);
    }

    // Determine if the switch diverges
    if (stmt->defaultBody) {
        // With a default clause: diverges if the default diverges AND
        // all non-default cases diverge (so there's no fall-through path)
        return defaultDiverges && allCasesDiverge;
    } else {
        // Without a default clause: only diverges if the subject is an
        // enum type AND all variants are covered (exhaustive) AND all cases
        // diverge.
        // TODO: Check if subjectType is an enum and all variants are covered
        //       (requires resolving subjectType to an EnumDeclAST and
        //        comparing variants against case values)
        // For now, assume it doesn't diverge unless we can prove otherwise.
        return false;
    }
}

// =============================================================================
// analyzeSwitchCase
// =============================================================================

/**
 * @brief Analyze a single switch case: type-check its values and body.
 *
 * A case diverges if its body diverges.
 *
 * @param switchCase The switch case node.
 * @param ctx        The semantic context.
 * @return true if the case's body diverges.
 */
bool analyzeSwitchCase(SwitchCaseAST* switchCase, SemaContext& ctx) {
    if (!switchCase) return false;

    // Type-check each case value
    for (ExprAST* value : switchCase->values) {
        // TODO: Verify each value is a literal, enum variant, or literal range
        //       (see Grammar.md's "Rejected Case Values" section)
        checkExpr(value, ctx);
    }

    // Analyze the body (should be a BlockStmtAST)
    if (switchCase->body) {
        return analyzeBlock(switchCase->body, ctx);
    }

    return false;
}

// =============================================================================
// analyzeForStmt
// =============================================================================

/**
 * @brief Analyze a for statement: type-check the iterable and loop variables,
 *        analyze the body.
 *
 * For statements never diverge (the loop can exit normally).
 *
 * @param stmt The for statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeForStmt(ForStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Open a scope for the loop variables
    ScopedScope scope(ctx);

    // Type-check the iterable
    TypeAST* iterableType = checkExpr(stmt->iterable, ctx);

    // Analyze the index variable (if present)
    if (stmt->indexVar) {
        analyzeParam(stmt->indexVar, ctx);
        // TODO: Verify indexVar type is int (always)
    }

    // Analyze the value variable (if present)
    if (stmt->valueVar) {
        analyzeParam(stmt->valueVar, ctx);
        // TODO: Verify valueVar type matches the iterable's element type
    }

    // Analyze the step (if present, for range loops)
    if (stmt->step) {
        TypeAST* stepType = checkExpr(stmt->step, ctx);
        // TODO: Verify stepType is numeric
    }

    // Determine if this is a range loop or collection loop
    if (stmt->iterable && stmt->iterable->isa<RangeExprAST>()) {
        // Range loop: indexVar is the loop variable, valueVar must be nullptr
        // (range loops have only one variable)
        // TODO: Verify stmt->valueVar == nullptr for range loops
    } else {
        // Collection loop: both indexVar and valueVar are required
        // (though they may be discarded with '_' -> ParamAST with name = "")
        // TODO: Verify both indexVar and valueVar are present for collection
    }

    // Analyze the body inside a LoopBody semantic context
    ScopedSemanticContext loopCtx(ctx, SemanticContext::LoopBody,
                                   stmt, stmt->loc);
    if (stmt->body) {
        // The body should be a BlockStmtAST
        if (stmt->body->isa<BlockStmtAST>()) {
            analyzeBlock(stmt->body->as<BlockStmtAST>(), ctx);
        } else {
            analyzeStmt(stmt->body, ctx);
        }
    }

    // For loops never diverge (they can exit normally)
    return false;
}

// =============================================================================
// analyzeWhileStmt
// =============================================================================

/**
 * @brief Analyze a while statement: type-check the condition, analyze the body.
 *
 * While statements never diverge (the loop can exit normally).
 *
 * @param stmt The while statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeWhileStmt(WhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Type-check the condition
    TypeAST* condType = checkExpr(stmt->condition, ctx);
    // TODO: Verify condition type is bool or coercible to bool

    // Analyze the body inside a LoopBody semantic context
    ScopedSemanticContext loopCtx(ctx, SemanticContext::LoopBody,
                                   stmt, stmt->loc);
    if (stmt->body) {
        if (stmt->body->isa<BlockStmtAST>()) {
            analyzeBlock(stmt->body->as<BlockStmtAST>(), ctx);
        } else {
            analyzeStmt(stmt->body, ctx);
        }
    }

    return false;
}

// =============================================================================
// analyzeDoWhileStmt
// =============================================================================

/**
 * @brief Analyze a do-while statement: analyze the body, type-check the condition.
 *
 * Do-while statements never diverge (the loop can exit normally).
 *
 * @param stmt The do-while statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeDoWhileStmt(DoWhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Analyze the body inside a LoopBody semantic context
    ScopedSemanticContext loopCtx(ctx, SemanticContext::LoopBody,
                                   stmt, stmt->loc);
    if (stmt->body) {
        if (stmt->body->isa<BlockStmtAST>()) {
            analyzeBlock(stmt->body->as<BlockStmtAST>(), ctx);
        } else {
            analyzeStmt(stmt->body, ctx);
        }
    }

    // Type-check the condition
    TypeAST* condType = checkExpr(stmt->condition, ctx);
    // TODO: Verify condition type is bool or coercible to bool

    return false;
}

// =============================================================================
// analyzeReturnStmt
// =============================================================================

/**
 * @brief Analyze a return statement: type-check the return values against
 *        the enclosing function's return type.
 *
 * Returns always diverge (they transfer control out of the function).
 *
 * @param stmt The return statement.
 * @param ctx  The semantic context.
 * @return true (always diverges).
 */
bool analyzeReturnStmt(ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // Verify we're inside a function
    if (!ctx.insideFunction()) {
        ctx.error(stmt, DiagCode::E1010, "return outside function");
        return true; // Still diverges syntactically
    }

    // Get the current function's return type
    FuncDeclAST* func = ctx.currentFunction();
    if (!func) {
        // Shouldn't happen if insideFunction() is true, but be safe
        return true;
    }

    // Get the resolved return type (set during analyzeFuncDecl)
    TypeAST* returnType = func->resolvedReturnType;

    // Check if this is a void function
    bool isVoid = !returnType;

    if (stmt->values.empty()) {
        // Bare return
        if (!isVoid) {
            ctx.error(stmt, DiagCode::E3003,
                       "return statement missing value in non-void function '",
                       ctx.toString(func->name), "'");
        }
        return true;
    }

    // Check return values against the function's return type
    if (isVoid) {
        ctx.error(stmt, DiagCode::E3003,
                   "void function '", ctx.toString(func->name),
                   "' cannot return a value");
        return true;
    }

    // For single return value, check type compatibility
    if (stmt->values.size() == 1) {
        TypeAST* valueType = checkExpr(stmt->values[0], ctx);
        if (valueType && !isAssignable(returnType, valueType, ctx)) {
            ctx.error(stmt->values[0], DiagCode::E3003,
                       "return type mismatch in function '",
                       ctx.toString(func->name), "'");
        }
    } else {
        // Multiple return values: the function must have a multi-return type
        // TODO: Check that returnType is a CombinedTypeAST or similar
        //       representing multiple return values, and that each value's
        //       type matches the corresponding return type.
        // For now, just type-check each expression individually.
        for (ExprAST* value : stmt->values) {
            checkExpr(value, ctx);
        }
        // TODO: Add diagnostic if the function doesn't support multiple returns
    }

    return true;
}

// =============================================================================
// analyzeBreakStmt
// =============================================================================

/**
 * @brief Analyze a break statement: verify it's inside a loop.
 *
 * Break always diverges (it transfers control out of the loop).
 *
 * @param stmt The break statement.
 * @param ctx  The semantic context.
 * @return true (always diverges).
 */
bool analyzeBreakStmt(BreakStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    if (!ctx.insideLoop()) {
        ctx.error(stmt, DiagCode::E1010, "break outside loop");
    }

    // TODO: Verify we're not inside a parallel body (break not allowed there)

    return true;
}

// =============================================================================
// analyzeContinueStmt
// =============================================================================

/**
 * @brief Analyze a continue statement: verify it's inside a loop.
 *
 * Continue always diverges (it transfers control out of the loop body).
 *
 * @param stmt The continue statement.
 * @param ctx  The semantic context.
 * @return true (always diverges).
 */
bool analyzeContinueStmt(ContinueStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    if (!ctx.insideLoop()) {
        ctx.error(stmt, DiagCode::E1010, "continue outside loop");
    }

    // TODO: Verify we're not inside a parallel body (continue not allowed there)

    return true;
}

// =============================================================================
// analyzeMultiVarDecl
// =============================================================================

/**
 * @brief Analyze a multi-variable declaration: type-check the RHS and
 *        each variable's type.
 *
 * Multi-var declarations never diverge.
 *
 * @param stmt The multi-var declaration statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeMultiVarDecl(MultiVarDeclAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Type-check the RHS expression
    TypeAST* rhsType = checkExpr(stmt->rhs, ctx);

    // Check that the number of variables matches the number of return values
    // from the RHS. For now, this is a placeholder; proper multi-return
    // type checking requires knowing the RHS's return arity.
    size_t varCount = stmt->vars.size();

    // TODO: Determine how many values the RHS returns (if it's a function
    //       call with multiple returns, or a multi-return expression).
    //       This requires extending TypeAST to support multi-return types.

    // Type-check each variable's type
    for (auto& var : stmt->vars) {
        InternedString name = var.first;
        TypePtr type = var.second;

        // Check for redeclaration
        if (ctx.lookupValue(name) != nullptr) {
            ctx.error(nullptr, DiagCode::E2101,
                       "redeclaration of '", ctx.toString(name), "'");
        }

        // Resolve the type
        TypeAST* resolvedType = resolveType(type, ctx);

        // Insert the variable into the current scope
        // For multi-var declarations, we need to create VarDeclAST nodes
        // or store the variables differently.
        // TODO: Create VarDeclAST nodes for each variable and insert them.
    }

    // TODO: For const multi-var declarations, verify all variables have
    //       initializers (the RHS provides the initializer).

    return false;
}

// =============================================================================
// analyzeMultiAssignStmt
// =============================================================================

/**
 * @brief Analyze a multi-assignment statement: verify LHS targets are
 *        assignable lvalues, type-check the RHS, and check type compatibility.
 *
 * Multi-assignment statements never diverge.
 *
 * @param stmt The multi-assignment statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeMultiAssignStmt(MultiAssignStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Type-check each LHS expression (must be an assignable lvalue)
    for (ExprAST* lhs : stmt->lhs) {
        TypeAST* lhsType = checkExpr(lhs, ctx);

        // TODO: Verify lhs is an assignable lvalue:
        //       - IdentifierExprAST (variable)
        //       - FieldAccessExprAST (struct field)
        //       - IndexExprAST (array element)
        //       Reject: literals, function calls, module access, etc.

        // TODO: Check if the LHS is const (variable declared const, or
        //       const field) — if so, reject the assignment.
    }

    // Type-check the RHS expression
    TypeAST* rhsType = checkExpr(stmt->rhs, ctx);

    // TODO: Check that the number of LHS targets matches the number of
    //       values the RHS returns, and that each target's type is
    //       assignable from the corresponding RHS value type.

    return false;
}

// =============================================================================
// Concurrency Statements
// =============================================================================

/**
 * @brief Analyze an async statement: type-check the call and verify
 *        variables are declared.
 *
 * Async statements never diverge.
 *
 * @param stmt The async statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeAsyncStmt(AsyncStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // TODO: Verify we're inside a function (not at top-level)

    // Type-check each target variable (should be an identifier lvalue)
    for (ExprAST* target : stmt->target) {
        TypeAST* targetType = checkExpr(target, ctx);
        // TODO: Verify target is an assignable lvalue
        // TODO: Check that the target is already declared (let variable)
    }

    // Type-check the call expression
    TypeAST* callType = checkExpr(stmt->call, ctx);

    // TODO: Verify the call returns a Future<T> type
    // TODO: Check that the number of targets matches the number of
    //       values the call returns
    // TODO: Verify each target's type matches the corresponding Future<T>
    //       element type

    return false;
}

/**
 * @brief Analyze an await statement: verify variables hold Future<T> values.
 *
 * Await statements never diverge.
 *
 * @param stmt The await statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeAwaitStmt(AwaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // TODO: Verify we're inside a function (not at top-level)
    // TODO: Verify we're inside an async context? (Grammar.md says await
    //       can be used anywhere, but it's most useful inside async)

    // Type-check each target variable (should be an identifier)
    for (ExprAST* target : stmt->targets) {
        TypeAST* targetType = checkExpr(target, ctx);
        // TODO: Verify target is an identifier
        // TODO: Verify target's type is Future<T>
        // TODO: After await, the variable becomes plain T (type narrowing)
        //       This is a semantic effect that needs to be recorded.
    }

    return false;
}

/**
 * @brief Analyze a spawn statement: type-check the call and verify
 *        variables are declared.
 *
 * Spawn statements never diverge.
 *
 * @param stmt The spawn statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeSpawnStmt(SpawnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // TODO: Verify we're inside a function (not at top-level)

    // Type-check each target variable
    bool hasNamedTarget = false;
    for (ExprAST* target : stmt->targets) {
        // If the target is an identifier with name "_", it's discarded
        bool isDiscard = false;
        if (target->isa<IdentifierExprAST>()) {
            InternedString name = target->as<IdentifierExprAST>()->name;
            // TODO: Check if name is "_" (discard pattern)
            //       The parser currently represents '_' as a normal identifier
            //       with a special name. Need to handle this.
        }

        if (!isDiscard) {
            hasNamedTarget = true;
            TypeAST* targetType = checkExpr(target, ctx);
            // TODO: Verify target is an assignable lvalue
            // TODO: Check that the target is already declared (let variable)
        }
    }

    // Type-check the call expression
    TypeAST* callType = checkExpr(stmt->call, ctx);

    // TODO: Verify the call returns a Future<T> type (or void for discard)
    // TODO: Check that the number of targets matches the number of
    //       values the call returns
    // TODO: Verify each target's type matches the corresponding Future<T>
    //       element type

    // TODO: If hasNamedTarget is true, warn if the spawn result is never joined

    return false;
}

/**
 * @brief Analyze a join statement: verify variables hold Future<T> values
 *        from spawn operations.
 *
 * Join statements never diverge.
 *
 * @param stmt The join statement.
 * @param ctx  The semantic context.
 * @return false (never diverges).
 */
bool analyzeJoinStmt(JoinStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // TODO: Verify we're inside a function (not at top-level)

    // Type-check each target variable
    for (ExprAST* target : stmt->targets) {
        TypeAST* targetType = checkExpr(target, ctx);
        // TODO: Verify target is an identifier
        // TODO: Verify target's type is Future<T> from a spawn operation
        //       (not from async)
        // TODO: After join, the variable becomes plain T (type narrowing)
        //       This is a semantic effect that needs to be recorded.
    }

    return false;   

} // namespace sema
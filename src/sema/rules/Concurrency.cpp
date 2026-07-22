/**
 * @file Concurrency.cpp
 * @brief Implements Sema.hpp's concurrency statement analysis.
 *
 * Implements:
 *   - analyzeAsyncStmt() - Schedule async operation on event loop
 *   - analyzeAwaitStmt() - Wait for async operations to complete
 *   - analyzeSpawnStmt() - Launch function on OS thread
 *   - analyzeJoinStmt() - Wait for spawned threads to complete
 *
 * @architectural_note Async vs Spawn
 *   - async: Cooperative concurrency on single-threaded event loop
 *   - spawn: Parallelism on OS threads (preemptive)
 *
 * @architectural_note Future Types
 *   Async and spawn operations return Future<T> values that must be
 *   awaited/joined before use. The compiler tracks these types and
 *   warns about unawaited/unjoined futures.
 */

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// =============================================================================
// analyzeAsyncStmt
// =============================================================================

bool analyzeAsyncStmt(AsyncStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Verify we're inside a function (not at top-level)
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E1010, "async statement outside function");
        return false;
    }

    // Type-check each target variable (should be an identifier lvalue)
    for (ExprAST* target : stmt->target) {
        TypeAST* targetType = checkExpr(target, ctx);
        if (!targetType) continue;

        // Verify target is an assignable lvalue
        // TODO: Check that target is an identifier that's already declared
        // TODO: Verify target is not const (can't assign to const variable)

        // The target's type should be Future<T> after async assignment
        // TODO: Implement Future<T> type checking
    }

    // Type-check the call expression
    TypeAST* callType = checkExpr(stmt->call, ctx);
    if (!callType) return false;

    // TODO: Verify the call returns a type that can be awaited
    // TODO: Check that the number of targets matches the number of return values
    // TODO: Verify each target's type matches the corresponding Future<T> element type
    // TODO: If the call returns void, targets should be empty

    // Async statements never diverge
    return false;
}

// =============================================================================
// analyzeAwaitStmt
// =============================================================================

bool analyzeAwaitStmt(AwaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Verify we're inside a function (not at top-level)
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E1010, "await statement outside function");
        return false;
    }

    // Type-check each target variable (should be an identifier)
    for (ExprAST* target : stmt->targets) {
        TypeAST* targetType = checkExpr(target, ctx);
        if (!targetType) continue;

        // TODO: Verify target is an identifier
        // TODO: Verify target's type is Future<T> (from async, not spawn)
        // TODO: After await, the variable becomes plain T (type narrowing)
        //       This is a semantic effect that needs to be recorded.

        // Check that the target is not const (await modifies the variable's type)
        // TODO: Check if target is const and reject if so
    }

    // Await statements never diverge
    return false;
}

// =============================================================================
// analyzeSpawnStmt
// =============================================================================

bool analyzeSpawnStmt(SpawnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Verify we're inside a function (not at top-level)
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E1010, "spawn statement outside function");
        return false;
    }

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
            if (name.id == 0) { // Placeholder: actual check would compare to "_"
                isDiscard = true;
            }
        }

        if (!isDiscard) {
            hasNamedTarget = true;
            TypeAST* targetType = checkExpr(target, ctx);
            if (!targetType) continue;

            // TODO: Verify target is an assignable lvalue
            // TODO: Check that the target is already declared (let variable)
            // TODO: Verify target is not const (can't assign to const variable)
        }
    }

    // Type-check the call expression
    TypeAST* callType = checkExpr(stmt->call, ctx);
    if (!callType) return false;

    // TODO: Verify the call returns a Future<T> type (or void for discard)
    // TODO: Check that the number of targets matches the number of return values
    // TODO: Verify each target's type matches the corresponding Future<T> element type

    // TODO: If hasNamedTarget is true, warn if the spawn result is never joined
    //       (This would be tracked at the end of the scope)

    // Spawn statements never diverge
    return false;
}

// =============================================================================
// analyzeJoinStmt
// =============================================================================

bool analyzeJoinStmt(JoinStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Verify we're inside a function (not at top-level)
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E1010, "join statement outside function");
        return false;
    }

    // Type-check each target variable
    for (ExprAST* target : stmt->targets) {
        TypeAST* targetType = checkExpr(target, ctx);
        if (!targetType) continue;

        // TODO: Verify target is an identifier
        // TODO: Verify target's type is Future<T> from a spawn operation
        //       (not from async)
        // TODO: After join, the variable becomes plain T (type narrowing)
        //       This is a semantic effect that needs to be recorded.

        // Check that the target is not const (join modifies the variable's type)
        // TODO: Check if target is const and reject if so
    }

    // Join statements never diverge
    return false;
}

} // namespace sema
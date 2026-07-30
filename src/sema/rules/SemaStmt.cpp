/// @file SemaStmt.cpp
/// @brief Implements Sema.hpp's "STATEMENTS - Control flow analysis" section.
/// 
/// This file handles Phase 2: Type resolution and validation for statements.
/// All names are already registered from Phase 1, so lookups will succeed.
/// 
/// @architectural_note Control Flow Analysis
///   Each statement resolver returns a boolean indicating whether the statement
///   guarantees control transfer out of the enclosing block (return, break,
///   continue, or a block whose last statement guarantees it).
/// 
/// @architectural_note Statement Structure
///   Statements are read-only AST nodes. We validate them and determine
///   control flow behavior without modifying the AST.
/// 
/// @architectural_note Error Recovery
///   Even if a statement has errors, we continue analysis to find more errors.
///   The return value should reflect the statement's control flow behavior
///   regardless of errors (if the statement is syntactically a return, it
///   still transfers control).

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// =============================================================================
// resolveStmt - Dispatch
// =============================================================================

/// @brief Dispatch a statement to its specific resolver function.
///
/// @param stmt The statement to resolve.
/// @param ctx The semantic context.
/// @return true if this statement guarantees control transfer out of the block.
bool resolveStmt(const StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:        return resolveBlock(stmt->as<BlockStmtAST>(), ctx);
        case ASTKind::IfStmt:           return resolveIfStmt(stmt->as<IfStmtAST>(), ctx);
        case ASTKind::SwitchStmt:       return resolveSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
        case ASTKind::SwitchCase:       return resolveSwitchCase(stmt->as<SwitchCaseAST>(), ctx);
        case ASTKind::ForStmt:          return resolveForStmt(stmt->as<ForStmtAST>(), ctx);
        case ASTKind::WhileStmt:        return resolveWhileStmt(stmt->as<WhileStmtAST>(), ctx);
        case ASTKind::DoWhileStmt:      return resolveDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
        case ASTKind::ReturnStmt:       return resolveReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
        case ASTKind::BreakStmt:        return resolveBreakStmt(stmt->as<BreakStmtAST>(), ctx);
        case ASTKind::ContinueStmt:     return resolveContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
        case ASTKind::ExprStmt:         return resolveExprStmt(stmt->as<ExprStmtAST>(), ctx);
        case ASTKind::DeclStmt:         return resolveDeclStmt(stmt->as<DeclStmtAST>(), ctx);
        case ASTKind::AsyncExpr:        return resolveAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
        case ASTKind::AwaitExpr:        return resolveAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
        case ASTKind::SpawnExpr:        return resolveSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
        case ASTKind::JoinExpr:         return resolveJoinStmt(stmt->as<JoinStmtAST>(), ctx);
        default:
            // Unknown/error-recovery statement
            return false;
    }
}

// =============================================================================
// resolveBlock
// =============================================================================

/// @brief Resolve a block statement.
///
/// A block is a sequence of statements in a new scope.
/// The block returns true if its last statement guarantees control transfer.
///
/// @param block The block statement.
/// @param ctx The semantic context.
/// @return true if the block guarantees control transfer out of the block.
///
/// ─── Control Flow Analysis ──────────────────────────────────────────────────
///
/// The function tracks whether any statement in the block transfers control
/// (return, break, continue). Once a transfer is detected:
///   1. The block is considered to transfer control
///   2. Subsequent statements are unreachable
///   3. The function should warn about unreachable code
///
/// ─── Example ──────────────────────────────────────────────────────────────
///
/// ```lucid
/// {
///     let x int = 5
///     return x      // ← transfers = true
///     let y int = 10 // ← UNREACHABLE - should warn
/// }
/// ```
bool resolveBlock(const BlockStmtAST* block, SemaContext& ctx) {
    // ─── 1. Push block context for pending inverse narrowing ────────────
    ctx.contexts.pushBlock(const_cast<BlockStmtAST*>(block), block->loc);

    bool transfers = false;
    bool hasAppliedPendingNarrowing = false;

    // ─── 2. Apply pending inverse narrowing from previous statements ────
    // If there's pending inverse narrowing from a standalone if with early exit,
    // apply it before resolving the rest of the block
    if (ctx.contexts.hasPendingInverseNarrowing()) {
        const NarrowingInfo& pendingInfo = ctx.contexts.getPendingInverseNarrowing();
        if (pendingInfo.hasNarrowing) {
            ctx.contexts.pushNarrowingLevel(true);
            for (const auto& [varName, narrowedType] : pendingInfo.narrowings) {
                ctx.contexts.narrowVariable(varName, narrowedType);
            }
            ctx.contexts.clearPendingInverseNarrowing();
            hasAppliedPendingNarrowing = true;
        }
    }

    // ─── 3. Resolve each statement in the block ──────────────────────────
    for (const StmtAST* stmt : block->stmts) {
        // ── 3a. Check if previous statement transferred control ──────────
        // If the previous statement transferred control, this statement is
        // unreachable. We should warn about it.
        if (transfers) {
            // TODO: Emit unreachable code warning
            // ctx.warning(stmt, DiagCode::W1001,
            //             "unreachable statement after control transfer");
            continue;
        }

        // ── 3b. Resolve the statement ──────────────────────────────────────
        transfers = resolveStmt(stmt, ctx);
        
        // If the statement transfers control, subsequent statements are unreachable
        if (transfers) {
            break;
        }
    }

    // ─── 4. Pop pending narrowing level if we applied one ───────────────
    if (hasAppliedPendingNarrowing) {
        ctx.contexts.popNarrowingLevel();
    }

    // ─── 5. Pop block context ─────────────────────────────────────────────
    ctx.contexts.pop();

    // ─── 6. Final Check: Return Requirements ─────────────────────────────
    // If we're inside a function that has requirements, check they're satisfied
    if (ctx.contexts.hasReturnRequirements() && !ctx.contexts.returnRequirementsSatisfied()) {
        // Only report error if the block doesn't transfer control via return
        if (!transfers) {
            ctx.error(block, DiagCode::E3005,
                      "function is missing a return statement");
        }
    }

    return transfers;
}

// =============================================================================
// resolveIfStmt
// =============================================================================

/// @brief Resolve an if statement.
///
/// The condition must be a boolean expression.
/// The then branch is always executed if the condition is true.
/// The else branch is optional.
///
/// ─── Type Narrowing Rules ─────────────────────────────────────────────────
///
/// The compiler applies type narrowing inside branches based on the condition.
/// See TypeNarrowHelpers.hpp for detailed documentation.
bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push if context for type narrowing ──────────────────────────
    ctx.contexts.push(ContextKind::IfStmt, const_cast<IfStmtAST*>(stmt), stmt->loc);
    ctx.contexts.setHasElse(stmt->elseBranch != nullptr);

    // ─── 2. Create a bool type for the condition ──────────────────────────
    PrimitiveTypeAST* boolType = ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);

    // ─── 3. Resolve condition with if context ────────────────────────────
    ctx.contexts.setIfConditionCtx(true);
    
    if (!checkExpr(stmt->condition, boolType, ctx)) {
        ctx.contexts.setIfConditionCtx(false);
        ctx.contexts.pop();
        return false;
    }
    
    ctx.contexts.setIfConditionCtx(false);

    // ─── 4. Extract narrowing info from the condition ────────────────────
    NarrowingInfo info = extractNarrowingsFromCondition(stmt->condition, ctx);
    bool hasNarrowing = info.hasNarrowing;

    // ─── 5. Resolve then branch with narrowing ──────────────────────────
    bool thenReturns = false;

    if (hasNarrowing) {
        // Push a single narrowing level with all narrowings
        ctx.contexts.pushNarrowingLevel(false);
        
        // Apply all narrowings to the then branch
        for (const auto& [varName, narrowedType] : info.narrowings) {
            // For equality (x == nil), no narrowing in then branch
            // (x is nil, but we don't track nil types)
            // For inequality (x != nil, x != err), apply normal narrowing
            if (!info.isEquality) {
                ctx.contexts.narrowVariable(varName, narrowedType);
            }
        }
        
        if (stmt->thenBranch && stmt->thenBranch->isa<BlockStmtAST>()) {
            thenReturns = resolveBlock(stmt->thenBranch->as<BlockStmtAST>(), ctx);
        } else {
            thenReturns = resolveStmt(stmt->thenBranch, ctx);
        }
        ctx.contexts.popNarrowingLevel();
    } else {
        // No narrowing - just resolve
        if (stmt->thenBranch && stmt->thenBranch->isa<BlockStmtAST>()) {
            thenReturns = resolveBlock(stmt->thenBranch->as<BlockStmtAST>(), ctx);
        } else {
            thenReturns = resolveStmt(stmt->thenBranch, ctx);
        }
    }

    // ─── 6. Resolve else branch (if present) ────────────────────────────
    if (stmt->elseBranch) {
        bool elseReturns = false;

        // ─── 6a. Check if else branch is an else-if ──────────────────────
        if (stmt->elseBranch->isa<IfStmtAST>()) {
            elseReturns = resolveIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
        } else {
            // ─── 6b. Regular else branch with inverse narrowing ──────────
            if (hasNarrowing) {
                ctx.contexts.pushNarrowingLevel(true); // Inverse narrowing
                
                for (const auto& [varName, narrowedType] : info.narrowings) {
                    // For equality (x == nil, x == err):
                    //   x is non-nullable/non-fallible in else branch
                    // For inequality (x != nil, x != err):
                    //   x is nullable/fallible (no change), so skip
                    if (info.isEquality) {
                        ctx.contexts.narrowVariable(varName, narrowedType);
                    }
                }
                
                if (stmt->elseBranch->isa<BlockStmtAST>()) {
                    elseReturns = resolveBlock(stmt->elseBranch->as<BlockStmtAST>(), ctx);
                } else {
                    elseReturns = resolveStmt(stmt->elseBranch, ctx);
                }
                ctx.contexts.popNarrowingLevel();
            } else {
                // No narrowing
                if (stmt->elseBranch->isa<BlockStmtAST>()) {
                    elseReturns = resolveBlock(stmt->elseBranch->as<BlockStmtAST>(), ctx);
                } else {
                    elseReturns = resolveStmt(stmt->elseBranch, ctx);
                }
            }
        }

        // If both branches return, the if transfers control
        if (thenReturns && elseReturns) {
            ctx.contexts.pop();
            return true;
        }
    }

    // ─── 7. Handle inverse narrowing for standalone if ───────────────────
    // Rule: standalone if with early exit applies inverse narrowing to rest of scope
    // ONLY when: no else, then branch transfers control, and condition is equality
    if (!stmt->elseBranch && thenReturns && hasNarrowing && info.isEquality) {
        // Set pending inverse narrowing on the current block
        // This will be applied when resolveBlock continues
        ctx.contexts.setPendingInverseNarrowing(info);
    }

    // ─── 8. Pop if context ────────────────────────────────────────────────
    ctx.contexts.pop();

    // If then branch doesn't transfer control, the if doesn't transfer
    return false;
}

// =============================================================================
// resolveSwitchStmt
// =============================================================================

/// @brief Resolve a switch statement.
///
/// The subject must be an integer, enum, bool, char, or string type.
/// Each case must have a body that is a block.
/// The default clause is optional.
///
/// @param stmt The switch statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push switch context ──────────────────────────────────────────
    ctx.contexts.push(ContextKind::SwitchBody, const_cast<SwitchStmtAST*>(stmt), stmt->loc);

    // ─── 2. Resolve the subject expression ──────────────────────────────
    // The subject must be an integer, enum, bool, char, or string type
    // We check this by resolving the expression and validating the type
    if (!checkExpr(stmt->subject, nullptr, ctx)) {
        ctx.contexts.pop();
        return false;
    }

    // ─── 3. Validate subject type ────────────────────────────────────────
    const TypeAST* subjectType = stmt->subject->resolvedType;
    if (!subjectType) {
        ctx.error(stmt->subject, DiagCode::E2002, "switch subject has no type");
        ctx.contexts.pop();
        return false;
    }

    // Check if subject type is valid for switch
    if (!isValidSwitchType(subjectType, ctx)) {
        ctx.error(stmt->subject, DiagCode::E3003,
                  "switch subject must be integer, enum, bool, char, or string");
        ctx.contexts.pop();
        return false;
    }

    // ─── 4. Resolve each case statement ──────────────────────────────────
    bool allCasesReturn = true;
    bool hasDefault = stmt->defaultBody != nullptr;

    for (const SwitchCasePtr caseStmt : stmt->cases) {
        if (!resolveSwitchCase(caseStmt, ctx)) {
            allCasesReturn = false;
        }
    }

    // ─── 5. Resolve default clause (if present) ──────────────────────────
    if (stmt->defaultBody) {
        if (!resolveBlock(stmt->defaultBody, ctx)) {
            allCasesReturn = false;
        }
    }

    // ─── 6. Check exhaustiveness for enum types ──────────────────────────
    // If the subject is an enum and no default is present, check all variants are covered
    if (!hasDefault && isEnumType(subjectType, ctx)) {
        checkSwitchExhaustiveness(stmt, subjectType, ctx);
    }

    // ─── 7. Pop switch context ────────────────────────────────────────────
    ctx.contexts.pop();

    // A switch transfers control only if ALL cases transfer control
    // AND (there is a default that transfers control OR the enum is exhaustive)
    return allCasesReturn && (hasDefault || isEnumType(subjectType, ctx));
}

/// @brief Check if a type is valid for switch statements.
bool isValidSwitchType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return false;

    // Integer types are valid
    if (isIntegerType(type)) return true;

    // Bool is valid
    if (isBoolType(type)) return true;

    // Char is valid
    if (isCharType(type)) return true;

    // String is valid
    if (isStringType(type)) return true;

    // Enum types are valid
    if (type->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = type->as<NamedTypeAST>();
        const TypeDeclAST* decl = lookupType(named->name, ctx);
        if (decl && decl->isa<EnumDeclAST>()) {
            return true;
        }
    }

    return false;
}

/// @brief Check if a type is an enum type.
bool isEnumType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    const TypeDeclAST* decl = lookupType(named->name, ctx);
    return decl && decl->isa<EnumDeclAST>();
}

/// @brief Check exhaustiveness of enum switch cases.
void checkSwitchExhaustiveness(const SwitchStmtAST* stmt, const TypeAST* subjectType, SemaContext& ctx) {
    // Collect all enum variants covered by cases
    std::unordered_set<InternedString> coveredVariants;

    for (const SwitchCasePtr caseStmt : stmt->cases) {
        for (const ExprAST* value : caseStmt->values) {
            if (value->isa<FieldAccessExprAST>()) {
                const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
                // Check if this is an enum variant access
                if (field->object->isa<IdentifierExprAST>()) {
                    const IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
                    if (isEnum(id->name, ctx)) {
                        coveredVariants.insert(field->fieldName);
                    }
                }
            }
        }
    }

    // Get all variants from the enum declaration
    const NamedTypeAST* named = subjectType->as<NamedTypeAST>();
    const TypeDeclAST* decl = lookupType(named->name, ctx);
    if (!decl || !decl->isa<EnumDeclAST>()) return;

    const EnumDeclAST* enumDecl = decl->as<EnumDeclAST>();

    // Check for missing variants
    for (const EnumVariantAST* variant : enumDecl->variants) {
        if (coveredVariants.find(variant->name) == coveredVariants.end()) {
            ctx.error(stmt, DiagCode::E3003,
                      "switch on enum '", ctx.pool().lookup(enumDecl->name),
                      "' is missing case for variant '", ctx.pool().lookup(variant->name), "'");
        }
    }
}

// =============================================================================
// resolveSwitchCase
// =============================================================================

/// @brief Resolve a switch case.
///
/// A case has one or more match values (literals, enum variants, or ranges)
/// and a body block.
///
/// @param switchCase The switch case.
/// @param ctx The semantic context.
/// @return true if the case body guarantees control transfer out of the block.
bool resolveSwitchCase(const SwitchCaseAST* switchCase, SemaContext& ctx) {
    if (!switchCase) return false;

    // ─── 1. Validate each case value ──────────────────────────────────────
    for (const ExprAST* value : switchCase->values) {
        // Case values must be literals, enum variants, or ranges
        // The parser already enforces this, but we check again
        if (!value->isa<LiteralExprAST>() && 
            !value->isa<FieldAccessExprAST>() && 
            !value->isa<RangeExprAST>()) {
            ctx.error(value, DiagCode::E3003,
                      "case value must be a literal, enum variant, or range");
            continue;
        }

        // Resolve the case value
        if (!checkExpr(value, nullptr, ctx)) {
            // Error already reported
            continue;
        }
    }

    // ─── 2. Resolve the case body ────────────────────────────────────────
    if (switchCase->body) {
        return resolveBlock(switchCase->body, ctx);
    }

    return false;
}

// =============================================================================
// resolveForStmt
// =============================================================================

/// @brief Resolve a for loop statement.
///
/// A for loop iterates over a range or collection.
/// The index and value bindings are optional (can be ignored with `_`).
///
/// @param stmt The for statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveForStmt(const ForStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push loop context ────────────────────────────────────────────
    ctx.contexts.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Push a scope for loop variables ──────────────────────────────
    ctx.symbols.pushScope();

    // ─── 3. Resolve the index binding (if present) ──────────────────────
    if (stmt->indexVar) {
        // The index variable was already registered in Phase 1
        // Now we resolve its type
        TypeAST* indexType = resolveType(stmt->indexVar->type, ctx);
        if (!indexType) {
            // Error already reported
        }
        // Check that index type is integer
        if (indexType && !isIntegerType(indexType)) {
            ctx.error(stmt->indexVar, DiagCode::E3003,
                      "index variable must be an integer type");
        }
    }

    // ─── 4. Resolve the value binding (if present) ──────────────────────
    if (stmt->valueVar) {
        // The value variable was already registered in Phase 1
        // Now we resolve its type
        TypeAST* valueType = resolveType(stmt->valueVar->type, ctx);
        if (!valueType) {
            // Error already reported
        }
    }

    // ─── 5. Resolve the iterable expression ──────────────────────────────
    if (stmt->iterable) {
        if (!checkExpr(stmt->iterable, nullptr, ctx)) {
            // Error already reported
        }
    }

    // ─── 6. Resolve the step expression (if present) ──────────────────────
    if (stmt->step) {
        if (!checkExpr(stmt->step, nullptr, ctx)) {
            // Error already reported
        }
        // Check that step is numeric
        if (stmt->step->resolvedType && !isNumericType(stmt->step->resolvedType)) {
            ctx.error(stmt->step, DiagCode::E3003,
                      "step must be a numeric type");
        }
    }

    // ─── 7. Resolve the loop body ─────────────────────────────────────────
    bool bodyTransfers = false;
    if (stmt->body) {
        bodyTransfers = resolveStmt(stmt->body, ctx);
    }

    // ─── 8. Pop the scope ──────────────────────────────────────────────────
    ctx.symbols.popScope();

    // ─── 9. Pop loop context ──────────────────────────────────────────────
    ctx.contexts.pop();

    // For loops do NOT guarantee control transfer (unless the body always returns)
    // But even then, the loop might not execute, so we return false
    // Note: A for loop with break/continue doesn't guarantee transfer
    return false;
}

// =============================================================================
// resolveWhileStmt
// =============================================================================

/// @brief Resolve a while loop statement.
///
/// The condition is tested before each iteration.
/// The loop exits when the condition is false or a break is reached.
///
/// @param stmt The while statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push loop context ────────────────────────────────────────────
    ctx.contexts.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Resolve the condition ──────────────────────────────────────────
    PrimitiveTypeAST* boolType = ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
    if (!checkExpr(stmt->condition, boolType, ctx)) {
        ctx.contexts.pop();
        return false;
    }

    // ─── 3. Resolve the loop body ─────────────────────────────────────────
    bool bodyTransfers = false;
    if (stmt->body) {
        bodyTransfers = resolveStmt(stmt->body, ctx);
    }

    // ─── 4. Pop loop context ──────────────────────────────────────────────
    ctx.contexts.pop();

    // While loops do NOT guarantee control transfer
    return false;
}

// =============================================================================
// resolveDoWhileStmt
// =============================================================================

/// @brief Resolve a do-while loop statement.
///
/// The body executes at least once before the condition is checked.
///
/// @param stmt The do-while statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push loop context ────────────────────────────────────────────
    ctx.contexts.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Resolve the loop body ─────────────────────────────────────────
    bool bodyTransfers = false;
    if (stmt->body) {
        bodyTransfers = resolveStmt(stmt->body, ctx);
    }

    // ─── 3. Resolve the condition ──────────────────────────────────────────
    PrimitiveTypeAST* boolType = ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
    if (!checkExpr(stmt->condition, boolType, ctx)) {
        ctx.contexts.pop();
        return false;
    }

    // ─── 4. Pop loop context ──────────────────────────────────────────────
    ctx.contexts.pop();

    // Do-while loops do NOT guarantee control transfer
    return false;
}

// =============================================================================
// resolveReturnStmt
// =============================================================================

/// @brief Resolve a return statement.
///
/// The return statement exits the enclosing function.
///
/// @param stmt The return statement.
/// @param ctx The semantic context.
/// @return true (return always transfers control out of the block).
bool resolveReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;  // Return always transfers control

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "return statement outside of function body");
        return true;  // Still transfers control (error recovery)
    }

    // ─── 2. Get the current function's return requirements ──────────────────
    const ReturnRequirements* reqs = ctx.contexts.currentReturnReqs();
    if (!reqs) {
        ctx.error(stmt, DiagCode::E3006,
                  "return statement with no return requirements");
        return true;
    }

    // ─── 3. Get the current return group ────────────────────────────────────
    const ReturnRequirements::Group* currentGroup = ctx.contexts.currentReturnGroup();

    // ─── 4. Check return value against current group ────────────────────────
    if (stmt->value) {
        // ── 4a. Function has a return value ─────────────────────────────────
        if (!currentGroup) {
            ctx.error(stmt, DiagCode::E3005,
                      "return value provided but function has no pending return group");
            return true;
        }

        const TypeAST* expectedType = currentGroup->returnType;
        if (!expectedType) {
            ctx.error(stmt, DiagCode::E3005,
                      "return value provided but function expects void return");
            return true;
        }

        if (!checkExpr(stmt->value, expectedType, ctx)) {
            return true;
        }

        // Validate fallible/nullable propagation
        if (stmt->value->valueState == ValueState::Err) {
            if (!isFallibleType(expectedType)) {
                ctx.error(stmt->value, DiagCode::E3003,
                          "cannot return err to non-fallible return type");
                return true;
            }
        }

        if (stmt->value->valueState == ValueState::Nil) {
            if (!isNullableType(expectedType)) {
                ctx.error(stmt->value, DiagCode::E3003,
                          "cannot return nil to non-nullable return type");
                return true;
            }
        }

    } else {
        // ── 4b. Void return (no value) ──────────────────────────────────────
        if (currentGroup && currentGroup->requiresReturn) {
            ctx.error(stmt, DiagCode::E3005,
                      "void return statement but function expects a return value");
            return true;
        }

        if (!reqs->allowsOptionalReturn) {
            ctx.error(stmt, DiagCode::E3005,
                      "void return statement not allowed in this function");
            return true;
        }
    }

    // ─── 5. Advance the return group ────────────────────────────────────────
    if (currentGroup) {
        ctx.contexts.advanceReturnGroup();
        const_cast<ReturnRequirements::Group*>(currentGroup)->satisfiedAt = stmt->loc;
    }

    // ─── 6. Return true (return always transfers control) ───────────────────
    return true;
}

// =============================================================================
// resolveBreakStmt
// =============================================================================

/// @brief Resolve a break statement.
///
/// The break statement exits the nearest enclosing loop or switch.
///
/// @param stmt The break statement.
/// @param ctx The semantic context.
/// @return true (break always transfers control out of the block).
bool resolveBreakStmt(const BreakStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // ─── 1. Check: Must be inside a loop or switch ────────────────────────
    if (!ctx.contexts.insideLoop() && !ctx.contexts.insideSwitch()) {
        ctx.error(stmt, DiagCode::E3006,
                  "break statement outside of loop or switch");
        return true;  // Still transfers control (error recovery)
    }

    // ─── 2. Return true (break always transfers control) ───────────────────
    return true;
}

// =============================================================================
// resolveContinueStmt
// =============================================================================

/// @brief Resolve a continue statement.
///
/// The continue statement skips the rest of the current loop iteration
/// and jumps to the next iteration.
///
/// @param stmt The continue statement.
/// @param ctx The semantic context.
/// @return true (continue always transfers control out of the block).
bool resolveContinueStmt(const ContinueStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // ─── 1. Check: Must be inside a loop ────────────────────────────────────
    if (!ctx.contexts.insideLoop()) {
        ctx.error(stmt, DiagCode::E3006,
                  "continue statement outside of loop");
        return true;  // Still transfers control (error recovery)
    }

    // ─── 2. Return true (continue always transfers control) ─────────────────
    return true;
}

// =============================================================================
// resolveExprStmt
// =============================================================================

/// @brief Resolve an expression statement.
///
/// An expression statement evaluates an expression for its side effects.
/// The expression's value is discarded.
///
/// @param stmt The expression statement.
/// @param ctx The semantic context.
/// @return false (expression statements do NOT transfer control).
bool resolveExprStmt(const ExprStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->expr) return false;

    // ─── 1. Resolve the expression ──────────────────────────────────────────
    // The expression type is determined by its context
    if (!checkExpr(stmt->expr, nullptr, ctx)) {
        return false;
    }

    // ─── 2. Check for discarded non-void value ─────────────────────────────
    // If the expression has a non-void type and no side effects, warn
    if (stmt->expr->resolvedType && 
        !stmt->expr->resolvedType->isa<PrimitiveTypeAST>() &&
        stmt->expr->resolvedType->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Void) {
        // Void expression is fine
    } else if (stmt->expr->resolvedType) {
        // Check if the expression has side effects (function calls, assignments, etc.)
        bool hasSideEffects = hasSideEffects(stmt->expr, ctx);
        if (!hasSideEffects) {
            // TODO: Emit warning about discarded pure expression
            // ctx.warning(stmt, DiagCode::W1002,
            //             "expression result is discarded (no side effects)");
        }
    }

    // ─── 3. Return false (expression statements do not transfer control) ────
    return false;
}

/// @brief Check if an expression has side effects.
bool hasSideEffects(const ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        case ASTKind::CallExpr:
            return true;  // Function calls may have side effects
        case ASTKind::AssignExpr:
            return true;  // Assignment has side effects
        case ASTKind::PipelineExpr: {
            const PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
            for (const PipelineStepPtr step : pipeline->steps) {
                if (step->callable && hasSideEffects(step->callable, ctx)) {
                    return true;
                }
            }
            return false;
        }
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            return hasSideEffects(bin->left, ctx) || hasSideEffects(bin->right, ctx);
        }
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            return hasSideEffects(unary->operand, ctx);
        }
        default:
            return false;
    }
}

// =============================================================================
// resolveDeclStmt
// =============================================================================

/// @brief Resolve a declaration statement.
///
/// A declaration statement introduces one or more local declarations
/// (variables, functions, structs, enums, traits).
///
/// @param stmt The declaration statement.
/// @param ctx The semantic context.
/// @return false (declaration statements do NOT transfer control).
bool resolveDeclStmt(const DeclStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->decl) return false;

    // ─── 1. Dispatch to resolveDecl() for the inner declaration ────────────
    // The declaration was already registered in Phase 1.
    // Now we resolve its type and validate it.
    resolveDecl(stmt->decl, ctx);

    // ─── 2. Return false (declarations do not transfer control) ────────────
    return false;
}

// =============================================================================
// Concurrency Statements
// =============================================================================

// ─── resolveAsyncStmt ──────────────────────────────────────────────────────

/// @brief Resolve an async statement.
///
/// Grammar: `async IDENTIFIER { ',' IDENTIFIER } '=' call_expr`
///
/// Schedules a function call on the event loop.
/// The result is a Future<T> that must be awaited.
///
/// @param stmt The async statement.
/// @param ctx The semantic context.
/// @return false (async does NOT transfer control).
bool resolveAsyncStmt(const AsyncStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "async statement outside of function body");
        return false;
    }

    // ─── 2. Check the target variable ──────────────────────────────────────
    if (!stmt->target) {
        ctx.error(stmt, DiagCode::E3003, "async statement requires a target variable");
        return false;
    }

    // The target must be an assignable lvalue
    if (!stmt->target->isa<IdentifierExprAST>()) {
        ctx.error(stmt->target, DiagCode::E3003,
                  "async target must be a variable (not an expression)");
        return false;
    }

    // ─── 3. Resolve the call expression ─────────────────────────────────────
    if (!stmt->call) {
        ctx.error(stmt, DiagCode::E3003, "async statement requires a call expression");
        return false;
    }

    if (!checkExpr(stmt->call, nullptr, ctx)) {
        return false;
    }

    // ─── 4. Verify the call returns a Future type ──────────────────────────
    // The checkExpr should have set the resolved type
    // TODO: Verify the call returns a Future<T> type

    // ─── 5. Return false (async does not transfer control) ──────────────────
    return false;
}

// ─── resolveAwaitStmt ──────────────────────────────────────────────────────

/// @brief Resolve an await statement.
///
/// Grammar: `await IDENTIFIER { ',' IDENTIFIER }`
///
/// Waits for async operations to complete.
/// After await, variables become plain T (no longer Future<T>).
///
/// @param stmt The await statement.
/// @param ctx The semantic context.
/// @return false (await does NOT transfer control).
bool resolveAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "await statement outside of function body");
        return false;
    }

    // ─── 2. Check each target variable ─────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.error(target, DiagCode::E3003,
                      "await target must be a variable (not an expression)");
            continue;
        }

        // TODO: Verify the variable is a Future<T> type
        // TODO: Change variable type from Future<T> to T
    }

    // ─── 3. Return false (await does not transfer control) ──────────────────
    return false;
}

// ─── resolveSpawnStmt ──────────────────────────────────────────────────────

/// @brief Resolve a spawn statement.
///
/// Grammar: `spawn IDENTIFIER { ',' IDENTIFIER } '=' call_expr`
///
/// Launches a function call on a separate OS thread.
/// The result is a Future<T> that must be joined.
///
/// @param stmt The spawn statement.
/// @param ctx The semantic context.
/// @return false (spawn does NOT transfer control).
bool resolveSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "spawn statement outside of function body");
        return false;
    }

    // ─── 2. Check the target variable ──────────────────────────────────────
    if (!stmt->target) {
        // _ is allowed (discard)
        // No validation needed
    } else if (!stmt->target->isa<IdentifierExprAST>()) {
        ctx.error(stmt->target, DiagCode::E3003,
                  "spawn target must be a variable (not an expression)");
        return false;
    }

    // ─── 3. Resolve the call expression ─────────────────────────────────────
    if (!stmt->call) {
        ctx.error(stmt, DiagCode::E3003, "spawn statement requires a call expression");
        return false;
    }

    if (!checkExpr(stmt->call, nullptr, ctx)) {
        return false;
    }

    // ─── 4. Return false (spawn does not transfer control) ──────────────────
    return false;
}

// ─── resolveJoinStmt ───────────────────────────────────────────────────────

/// @brief Resolve a join statement.
///
/// Grammar: `join IDENTIFIER { ',' IDENTIFIER }`
///
/// Waits for spawned threads to complete.
/// After join, variables become plain T (no longer Future<T>).
///
/// @param stmt The join statement.
/// @param ctx The semantic context.
/// @return false (join does NOT transfer control).
bool resolveJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "join statement outside of function body");
        return false;
    }

    // ─── 2. Check each target variable ─────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.error(target, DiagCode::E3003,
                      "join target must be a variable (not an expression)");
            continue;
        }

        // TODO: Verify the variable is a Future<T> from spawn
        // TODO: Change variable type from Future<T> to T
    }

    // ─── 3. Return false (join does not transfer control) ──────────────────
    return false;
}

} // namespace sema
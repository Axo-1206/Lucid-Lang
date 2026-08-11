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
/// @architectural_note RAII Guards
///   All scope and context management is done via RAII guards to ensure
///   proper cleanup even when errors occur.
/// 
/// @architectural_note Const Evaluation Integration
///   Const evaluation is used for constant folding and dead code elimination.
///   - If conditions with constant booleans: only resolve the taken branch
///   - While conditions with constant false: warn about unreachable body
///   - For loop ranges with constant bounds: validate range at compile time

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../const_eval/ConstEvaluator.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../support/CaptureAnalysis.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// =============================================================================
// resolveStmt - Dispatch
// =============================================================================

bool resolveStmt(const StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:        return resolveBlock(stmt->as<BlockStmtAST>(), ctx);
        case ASTKind::IfStmt:           return resolveIfStmt(stmt->as<IfStmtAST>(), ctx);
        case ASTKind::SwitchStmt:       return resolveSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
        case ASTKind::ForStmt:          return resolveForStmt(stmt->as<ForStmtAST>(), ctx);
        case ASTKind::WhileStmt:        return resolveWhileStmt(stmt->as<WhileStmtAST>(), ctx);
        case ASTKind::DoWhileStmt:      return resolveDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
        case ASTKind::ReturnStmt:       return resolveReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
        case ASTKind::BreakStmt:        return resolveBreakStmt(stmt->as<BreakStmtAST>(), ctx);
        case ASTKind::ContinueStmt:     return resolveContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
        case ASTKind::ExprStmt:         return resolveExprStmt(stmt->as<ExprStmtAST>(), ctx);
        case ASTKind::DeclStmt:         return resolveDeclStmt(stmt->as<DeclStmtAST>(), ctx);
        case ASTKind::AsyncStmt:        return resolveAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
        case ASTKind::AwaitStmt:        return resolveAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
        case ASTKind::SpawnStmt:        return resolveSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
        case ASTKind::JoinStmt:         return resolveJoinStmt(stmt->as<JoinStmtAST>(), ctx);
        default:
            return false;
    }
}

// =============================================================================
// resolveBlock
// =============================================================================

bool resolveBlock(const BlockStmtAST* block, SemaContext& ctx) {
    if (!block) return false;

    // ─── RAII: Push block context ──────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::Block, 
                                   const_cast<BlockStmtAST*>(block));

    bool transfers = false;
    bool hasAppliedPendingNarrowing = false;

    // Apply pending inverse narrowing
    if (ctx.stack.hasPendingInverseNarrowing()) {
        const NarrowingInfo& pendingInfo = ctx.stack.getPendingInverseNarrowing();
        if (pendingInfo.hasNarrowing) {
            ctx.stack.pushNarrowingLevel(true);
            for (const auto& [varName, narrowedType] : pendingInfo.narrowings) {
                ctx.stack.narrowVariable(varName, narrowedType);
            }
            ctx.stack.clearPendingInverseNarrowing();
            hasAppliedPendingNarrowing = true;
        }
    }

    // ─── RAII: Push a new scope for the block ──────────────────────────────
    SymbolScope scope(ctx);

    // ─── Resolve each statement ─────────────────────────────────────────
    for (const StmtAST* stmt : block->stmts) {
        if (transfers) {
            ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt, "unreachable code");
            continue;
        }
        transfers = resolveStmt(stmt, ctx);
        if (transfers) {
            break;
        }
    }

    // Check for unresolved async/spawn operations
    for (const InternedString& name : ctx.getPendingAsyncNames()) {
        ctx.diagnostics.warning(DiagCode::Warn_UnawaitedAsync, block,
                                "async '", ctx.pool.lookup(name), "' was never awaited");
    }

    for (const InternedString& name : ctx.getPendingSpawnNames()) {
        ctx.diagnostics.warning(DiagCode::Warn_UnjoinedSpawn, block,
                                "spawn '", ctx.pool.lookup(name), "' was never joined");
    }

    // ─── Pop pending narrowing level ──────────────────────────────────────
    if (hasAppliedPendingNarrowing) {
        ctx.stack.popNarrowingLevel();
    }

    return transfers;
}

// =============================================================================
// resolveIfStmt
// =============================================================================

bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push if context ────────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::IfStmt,
                                   const_cast<IfStmtAST*>(stmt));
    ctx.stack.setHasElse(stmt->elseBranch != nullptr);

    // ─── RAII: ScopedIfCondition for narrowing detection ──────────────────
    ScopedIfCondition ifContext(ctx, stmt->elseBranch != nullptr);

    // ─── Resolve condition with target type = bool ─────────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);

    if (!condType || condType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── CONST EVALUATION: Try to evaluate the condition at compile time ──
    ConstantValue condVal = ConstEvaluator::evaluate(ctx, stmt->condition, boolType);
    bool condIsConst = condVal.isBool();
    bool condValue = condIsConst ? condVal.asBool() : false;

    // ─── Extract narrowing info from the condition ─────────────────────────
    NarrowingInfo info = extractNarrowingsFromCondition(stmt->condition, ctx);
    bool hasNarrowing = info.hasNarrowing;

    // ─── If condition is compile-time constant, only resolve the taken branch ──
    if (condIsConst) {
        if (condValue) {
            // ─── Condition is always true: only resolve then branch ──────
            if (stmt->thenBranch) {
                if (hasNarrowing && !info.isEquality) {
                    ScopedNarrowing narrowing(ctx, info.narrowings, false);
                    return resolveStmt(stmt->thenBranch, ctx);
                }
                return resolveStmt(stmt->thenBranch, ctx);
            }
            return false;
        } else {
            // ─── Condition is always false: only resolve else branch ──────
            if (stmt->elseBranch) {
                if (stmt->elseBranch->isa<IfStmtAST>()) {
                    return resolveIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
                }
                if (hasNarrowing && info.isEquality) {
                    ScopedNarrowing narrowing(ctx, info.narrowings, true);
                    return resolveStmt(stmt->elseBranch, ctx);
                }
                return resolveStmt(stmt->elseBranch, ctx);
            }
            return false;
        }
    }

    // ─── Condition is runtime: resolve both branches ──────────────────────
    bool thenReturns = false;

    if (hasNarrowing && !info.isEquality) {
        ScopedNarrowing narrowing(ctx, info.narrowings, false);
        thenReturns = stmt->thenBranch ? resolveStmt(stmt->thenBranch, ctx) : false;
    } else {
        thenReturns = stmt->thenBranch ? resolveStmt(stmt->thenBranch, ctx) : false;
    }

    if (stmt->elseBranch) {
        bool elseReturns = false;

        if (stmt->elseBranch->isa<IfStmtAST>()) {
            elseReturns = resolveIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
        } else {
            if (hasNarrowing && info.isEquality) {
                ScopedNarrowing narrowing(ctx, info.narrowings, true);
                elseReturns = resolveStmt(stmt->elseBranch, ctx);
            } else {
                elseReturns = resolveStmt(stmt->elseBranch, ctx);
            }
        }

        if (thenReturns && elseReturns) {
            return true;
        }
    }

    // ─── Handle inverse narrowing for standalone if ───────────────────────
    if (!stmt->elseBranch && thenReturns && hasNarrowing && info.isEquality) {
        ctx.stack.setPendingInverseNarrowing(info);
    }

    return false;
}

// =============================================================================
// resolveSwitchStmt
// =============================================================================

bool resolveSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Resolve subject expression ────────────────────────────────────────
    TypeAST* subjectType = resolveExpr(stmt->subject, ctx);
    if (!subjectType || subjectType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, stmt->subject,
                              "switch subject has unknown type");
        return false;
    }
    
    // ─── Validate subject type ─────────────────────────────────────────────
    if (!isValidSwitchType(subjectType, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, stmt->subject,
                              "switch subject must be integer, enum, bool, char, or string");
        return false;
    }
    
    // ─── RAII: Push switch context ─────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::SwitchBody,
                                   const_cast<SwitchStmtAST*>(stmt));
    
    // ─── CONST EVALUATION: Try to evaluate subject at compile time ────────
    ConstantValue subjectVal = ConstEvaluator::evaluate(ctx, stmt->subject);
    bool subjectConst = subjectVal.isInt() || subjectVal.isBool() || subjectVal.isString();
    
    // ─── Validate cases ─────────────────────────────────────────────────────
    bool allCasesReturn = true;
    bool foundMatch = false;
    
    // ─── Track seen values for duplicate detection ─────────────────────────
    // For literal values, store the raw value as key
    std::unordered_map<InternedString, SourceLocation> seenLiterals;
    // For enum variants, store the variant name
    std::unordered_map<InternedString, SourceLocation> seenVariants;
    // For ranges, store the range bounds
    struct RangeKey {
        int64_t lo;
        int64_t hi;
        bool isInclusive;
        bool operator==(const RangeKey& other) const {
            return lo == other.lo && hi == other.hi && isInclusive == other.isInclusive;
        }
    };
    struct RangeKeyHash {
        size_t operator()(const RangeKey& key) const {
            return std::hash<int64_t>{}(key.lo) ^ 
                   std::hash<int64_t>{}(key.hi) ^ 
                   std::hash<bool>{}(key.isInclusive);
        }
    };
    std::unordered_map<RangeKey, SourceLocation, RangeKeyHash> seenRanges;
    
    for (const SwitchCasePtr caseStmt : stmt->cases) {
        // Validate each case value against the subject type
        for (ExprAST* value : caseStmt->values) {
            TypeAST* valueType = resolveExprWithTarget(value, subjectType, ctx);
            if (!valueType || valueType->isa<UnknownTypeAST>()) {
                continue;
            }
            
            if (!isSwitchCaseCompatible(value, subjectType, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, value,
                                      "case value is not compatible with switch subject type");
            }
            
            // ─── DUPLICATE DETECTION ─────────────────────────────────────────
            
            // ─── Check literal values ──────────────────────────────────────
            if (value->isa<LiteralExprAST>()) {
                const LiteralExprAST* lit = value->as<LiteralExprAST>();
                InternedString key = lit->value;
                
                // For integer literals, also check if they fall within any range
                auto it = seenLiterals.find(key);
                if (it != seenLiterals.end()) {
                    ctx.diagnostics.error(DiagCode::Sem_DuplicateCase, value,
                                          "duplicate case value '", ctx.pool.lookup(key), "'");
                    ctx.diagnostics.noteAt(it->second, "previous definition here");
                    return false;
                }
                seenLiterals[key] = value->loc;
                continue;
            }
            
            // ─── Check enum variants ──────────────────────────────────────
            if (switch_helpers::isEnumVariantAccess(value, ctx)) {
                InternedString variantName = switch_helpers::getEnumVariantName(value, ctx);
                if (variantName.isValid()) {
                    auto it = seenVariants.find(variantName);
                    if (it != seenVariants.end()) {
                        ctx.diagnostics.error(DiagCode::Sem_DuplicateCase, value,
                                              "duplicate case value '", ctx.pool.lookup(variantName), "'");
                        ctx.diagnostics.noteAt(it->second, "previous definition here");
                        return false;
                    }
                    seenVariants[variantName] = value->loc;
                }
                continue;
            }
            
            // ─── Check ranges ──────────────────────────────────────────────
            if (value->isa<RangeExprAST>()) {
                const RangeExprAST* range = value->as<RangeExprAST>();
                auto loOpt = ConstEvaluator::evaluateAsInt(ctx, range->lo);
                auto hiOpt = ConstEvaluator::evaluateAsInt(ctx, range->hi);
                
                if (loOpt.has_value() && hiOpt.has_value()) {
                    RangeKey key{loOpt.value(), hiOpt.value(), !range->isExclusive};
                    
                    // Check for duplicate range
                    auto it = seenRanges.find(key);
                    if (it != seenRanges.end()) {
                        ctx.diagnostics.error(DiagCode::Sem_DuplicateCase, value,
                                              "duplicate range case");
                        ctx.diagnostics.noteAt(it->second, "previous definition here");
                        return false;
                    }
                    seenRanges[key] = value->loc;
                    
                    // ─── Check if this range overlaps with any literal ──────
                    for (const auto& [litKey, loc] : seenLiterals) {
                        // Try to parse the literal as an integer
                        // This is a simplified check - only works for int literals
                        std::string litStr = ctx.pool.lookup(litKey);
                        try {
                            int64_t litVal = std::stoll(litStr, nullptr, 0);
                            bool overlaps = key.isInclusive 
                                ? (litVal >= key.lo && litVal <= key.hi)
                                : (litVal >= key.lo && litVal < key.hi);
                            if (overlaps) {
                                ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, value,
                                                        "range case overlaps with literal case '", litStr, 
                                                        "' - literal case is unreachable");
                                break;
                            }
                        } catch (const std::exception&) {
                            // Not an integer literal - skip
                        }
                    }
                }
                continue;
            }
            
            // ─── CONST EVALUATION: Check if this case matches the subject ──
            if (subjectConst && !foundMatch) {
                bool matches = false;
                
                if (value->isa<RangeExprAST>()) {
                    const RangeExprAST* range = value->as<RangeExprAST>();
                    auto loOpt = ConstEvaluator::evaluateAsInt(ctx, range->lo);
                    auto hiOpt = ConstEvaluator::evaluateAsInt(ctx, range->hi);
                    if (loOpt.has_value() && hiOpt.has_value() && subjectVal.isInt()) {
                        int64_t subj = subjectVal.asInt();
                        bool isInclusive = !range->isExclusive;
                        matches = isInclusive ? (subj >= loOpt.value() && subj <= hiOpt.value())
                                              : (subj >= loOpt.value() && subj < hiOpt.value());
                    }
                } else if (subjectVal.isInt() && value->isa<LiteralExprAST>() && 
                           value->as<LiteralExprAST>()->kind == LiteralKind::Int) {
                    auto caseInt = ConstEvaluator::evaluateAsInt(ctx, value);
                    if (caseInt.has_value() && caseInt.value() == subjectVal.asInt()) {
                        matches = true;
                    }
                } else if (subjectVal.isBool() && value->isa<LiteralExprAST>()) {
                    auto caseBool = ConstEvaluator::evaluateAsBool(ctx, value);
                    if (caseBool.has_value() && caseBool.value() == subjectVal.asBool()) {
                        matches = true;
                    }
                } else if (subjectVal.isString() && value->isa<LiteralExprAST>() &&
                           value->as<LiteralExprAST>()->kind == LiteralKind::String) {
                    auto caseStr = ConstEvaluator::evaluate(ctx, value);
                    if (caseStr.isString() && 
                        ctx.pool.lookup(caseStr.asString()) == ctx.pool.lookup(subjectVal.asString())) {
                        matches = true;
                    }
                } else if (value->isa<FieldAccessExprAST>() && subjectVal.isEnum()) {
                    // Enum variant case - check by comparing names
                    // For now, skip enum const evaluation as it's more complex
                }
                
                if (matches) {
                    foundMatch = true;
                }
            }
        }
        
        // Resolve case body
        if (caseStmt->body) {
            if (!resolveBlock(caseStmt->body, ctx)) {
                allCasesReturn = false;
            }
        }
    }
    
    // ─── Check exhaustiveness ──────────────────────────────────────────────
    if (!stmt->defaultBody && isEnumType(subjectType, ctx)) {
        switch_helpers::checkExhaustiveness(stmt, subjectType, ctx);
    }
    
    // ─── Resolve default clause ────────────────────────────────────────────
    if (stmt->defaultBody) {
        if (!resolveBlock(stmt->defaultBody, ctx)) {
            allCasesReturn = false;
        }
    }
    
    return allCasesReturn && (stmt->defaultBody || !isEnumType(subjectType, ctx));
}

// =============================================================================
// resolveForStmt
// =============================================================================

bool resolveForStmt(const ForStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push loop context ───────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::LoopBody,
                                   const_cast<StmtAST*>(stmt->body));

    // ─── RAII: Push a scope for loop variables ─────────────────────────────
    SymbolScope scope(ctx);

    // ─── Determine if this is a range loop or collection loop ─────────────
    bool isRangeLoop = (stmt->valueVar == nullptr);
    
    if (isRangeLoop) {
        // ─── Form 1: Range loop ─────────────────────────────────────────────
        // for i int in 0..10 [..step]
        
        if (!stmt->iterable || !stmt->iterable->isa<RangeExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidIterator, stmt->iterable,
                                  "range loop requires a range expression (start..end)");
            return false;
        }
        
        const RangeExprAST* range = stmt->iterable->as<RangeExprAST>();
        
        // ─── Resolve AND REGISTER the index binding ──────────────────────
        if (stmt->indexVar) {
            TypeAST* indexType = resolveType(stmt->indexVar->type, ctx);
            ctx.insertValue(stmt->indexVar);
            if (indexType && !isNumericType(indexType)) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->indexVar,
                                      "index variable in range loop must be numeric, got ",
                                      debug::typeToString(indexType, ctx.pool));
            }
        }
        
        // ─── CONST EVALUATION: Validate range bounds at compile time ──────
        auto loOpt = ConstEvaluator::evaluateAsInt(ctx, range->lo);
        auto hiOpt = ConstEvaluator::evaluateAsInt(ctx, range->hi);
        
        if (loOpt.has_value() && hiOpt.has_value()) {
            int64_t lo = loOpt.value();
            int64_t hi = hiOpt.value();
            bool isInclusive = !range->isExclusive;
            
            // Validate range order
            if (isInclusive && lo > hi) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidRange, range,
                                      "inclusive range start (", lo, 
                                      ") must be less than or equal to end (", hi, ")");
                return false;
            }
            if (!isInclusive && lo >= hi) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidRange, range,
                                      "exclusive range start (", lo, 
                                      ") must be less than end (", hi, ")");
                return false;
            }
            
            // Warn about empty ranges
            int64_t count = hi - lo + (isInclusive ? 1 : 0);
            if (count <= 0) {
                ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, range,
                                        "range is empty - loop body will never execute");
            }
        }
        
        // ─── Resolve step ──────────────────────────────────────────────────
        if (stmt->step) {
            PrimitiveTypeAST* numericType = ctx.getIntType();
            TypeAST* stepType = resolveExprWithTarget(stmt->step, numericType, ctx);
            if (!stepType || stepType->isa<UnknownTypeAST>()) {
                // Error already reported
            }
            
            // ─── CONST EVALUATION: Validate step at compile time ──────────
            auto stepOpt = ConstEvaluator::evaluateAsInt(ctx, stmt->step);
            if (stepOpt.has_value()) {
                int64_t step = stepOpt.value();
                if (step <= 0) {
                    ctx.diagnostics.error(DiagCode::Sem_InvalidRange, stmt->step,
                                          "step must be positive, got ", step);
                    return false;
                }
            }
        }
        
    } else {
        // ─── Form 2: Collection loop ───────────────────────────────────────
        // for i int, v V in collection
        
        if (stmt->step) {
            // Step should have been rejected by the parser
            ctx.diagnostics.error(DiagCode::Sem_InvalidIterator, stmt->step,
                                  "step ('..') is not allowed in collection iteration");
            return false;
        }
        
        // ─── Resolve AND REGISTER the index binding ──────────────────────
        if (stmt->indexVar) {
            TypeAST* indexType = resolveType(stmt->indexVar->type, ctx);
            ctx.insertValue(stmt->indexVar);
            if (indexType && !isIntegerType(indexType)) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->indexVar,
                                      "index variable in collection loop must be integer, got ",
                                      debug::typeToString(indexType, ctx.pool));
            }
        }
        
        // ─── Resolve AND REGISTER the value binding ──────────────────────
        if (stmt->valueVar) {
            TypeAST* valueType = resolveType(stmt->valueVar->type, ctx);
            ctx.insertValue(stmt->valueVar);
        }
        
        // ─── Resolve the iterable expression ──────────────────────────────
        TypeAST* iterableType = resolveExpr(stmt->iterable, ctx);
        if (!iterableType || iterableType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidIterator, stmt->iterable,
                                  "iterable has unknown type");
            return false;
        }
        
        // ─── Validate value type against iterable element type ──────────
        if (stmt->valueVar && iterableType->isa<ArrayTypeAST>()) {
            const ArrayTypeAST* arrayType = iterableType->as<ArrayTypeAST>();
            const TypeAST* elementType = arrayType->element;
            
            if (stmt->valueVar->type) {
                TypeAST* valueType = resolveType(stmt->valueVar->type, ctx);
                if (valueType && !typesEqual(valueType, elementType)) {
                    ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->valueVar,
                                          "value type '", debug::typeToString(valueType, ctx.pool),
                                          "' does not match iterable element type '",
                                          debug::typeToString(elementType, ctx.pool), "'");
                }
            }
        } else if (stmt->valueVar && !iterableType->isa<ArrayTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidIterator, stmt->iterable,
                                  "collection loop requires an array type, got ",
                                  debug::typeToString(iterableType, ctx.pool));
            return false;
        }
    }

    // ─── Resolve the loop body ─────────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    return false;
}

// =============================================================================
// resolveWhileStmt
// =============================================================================

bool resolveWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push loop context ───────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::LoopBody,
                                   const_cast<StmtAST*>(stmt->body));

    // ─── Resolve the condition against bool type ──────────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── CONST EVALUATION: Check if condition is compile-time constant ────
    ConstantValue condVal = ConstEvaluator::evaluate(ctx, stmt->condition, boolType);
    bool condIsConst = condVal.isBool();
    bool condValue = condIsConst ? condVal.asBool() : false;

    // ─── If condition is compile-time false, body is unreachable ──────────
    if (condIsConst && !condValue) {
        ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt->body,
                                "while loop condition is always false - body will never execute");
        return false;
    }

    // ─── If condition is compile-time true, it's an infinite loop ─────────
    if (condIsConst && condValue) {
        ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt,
                                "while loop condition is always true - infinite loop (no break)");
        // Still resolve the body (it may have break/return)
    }

    // ─── Resolve the loop body ─────────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    return false;
}

// =============================================================================
// resolveDoWhileStmt
// =============================================================================

bool resolveDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push loop context ───────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::LoopBody,
                                   const_cast<StmtAST*>(stmt->body));

    // ─── Resolve the loop body ─────────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // ─── Resolve the condition against bool type ──────────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── CONST EVALUATION: Check if condition is compile-time constant ────
    ConstantValue condVal = ConstEvaluator::evaluate(ctx, stmt->condition, boolType);
    bool condIsConst = condVal.isBool();
    bool condValue = condIsConst ? condVal.asBool() : false;

    // ─── If condition is compile-time false, loop executes once ────────────
    if (condIsConst && !condValue) {
        // Body already resolved above
        return false;
    }

    // ─── If condition is compile-time true, it's an infinite loop ─────────
    if (condIsConst && condValue) {
        ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt,
                                "do-while loop condition is always true - infinite loop (no break)");
    }

    return false;
}

// =============================================================================
// resolveReturnStmt
// =============================================================================

bool resolveReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBreak, stmt,
                              "return statement outside of function body");
        return true;
    }

    // ─── Get the current expected return type from the stack ──────────────
    const TypeAST* expectedType = ctx.stack.currentReturnType();

    // ─── Validate return value against expected type ───────────────────────
    if (stmt->value) {
        // ─── Non-void return ──────────────────────────────────────────────
        if (!expectedType) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, stmt,
                                  "return value provided but function has no return type (expected void)");
            return true;
        }

        // Resolve the return value against the expected type
        TypeAST* valueType = resolveExprWithTarget(stmt->value, expectedType, ctx);
        if (!valueType || valueType->isa<UnknownTypeAST>()) {
            return true;
        }

        // ─── DETECT CLOSURE RETURN ───────────────────────────────────────────
        // Check if the returned expression is a closure that needs to be
        // marked as escaping (heap-allocated).
        markClosureIfEscaping(stmt->value, ctx);

        // Validate fallible/nullable propagation
        if (stmt->value->valueState == ValueState::Err) {
            if (!isFallibleType(expectedType)) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, stmt->value,
                                      "cannot return err to non-fallible return type");
                return true;
            }
        }

        if (stmt->value->valueState == ValueState::Nil) {
            if (!isNullableType(expectedType)) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, stmt->value,
                                      "cannot return nil to non-nullable return type");
                return true;
            }
        }

    } else {
        // ─── Void return (no value) ──────────────────────────────────────
        if (expectedType) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, stmt,
                                  "void return statement but function expects a return value (", 
                                  debug::typeToString(expectedType, ctx.pool), ")");
            return true;
        }
    }

    return true;
}

// =============================================================================
// resolveBreakStmt
// =============================================================================

bool resolveBreakStmt(const BreakStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    if (!ctx.stack.insideLoop() && !ctx.stack.insideSwitch()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBreak, stmt,
                              "break statement outside of loop or switch");
        return true;
    }

    return true;
}

// =============================================================================
// resolveContinueStmt
// =============================================================================

bool resolveContinueStmt(const ContinueStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    if (!ctx.stack.insideLoop()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidContinue, stmt,
                              "continue statement outside of loop");
        return true;
    }

    return true;
}

// =============================================================================
// resolveExprStmt
// =============================================================================

/// @brief Check if an expression has side effects.
static bool hasSideEffects(const ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        case ASTKind::CallExpr:
            return true;

        case ASTKind::IntrinsicCallExpr: {
            const IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
            std::string nameStr = ctx.pool.lookup(intrinsic->intrinsicName);
            if (nameStr == "memcpy" || nameStr == "memmove" || nameStr == "memset" ||
                nameStr == "alloc" || nameStr == "free" ||
                nameStr == "arena_create" || nameStr == "arena_alloc" || 
                nameStr == "arena_free" || nameStr == "arena_reset" ||
                nameStr == "atomic_store" || nameStr == "atomic_add" ||
                nameStr == "atomic_sub" || nameStr == "atomic_and" ||
                nameStr == "atomic_or" || nameStr == "atomic_xor" ||
                nameStr == "atomic_cas") {
                return true;
            }
            return false;
        }

        case ASTKind::AssignExpr:
            return true;

        case ASTKind::PipelineExpr: {
            const PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
            if (hasSideEffects(pipeline->seed, ctx)) return true;
            for (const PipelineStepAST* step : pipeline->steps) {
                if (hasSideEffects(step->callable, ctx)) return true;
                for (const ExprAST* arg : step->packArgs) {
                    if (hasSideEffects(arg, ctx)) return true;
                }
            }
            return false;
        }

        case ASTKind::PipelineStep: {
            const PipelineStepAST* step = expr->as<PipelineStepAST>();
            if (hasSideEffects(step->callable, ctx)) return true;
            for (const ExprAST* arg : step->packArgs) {
                if (hasSideEffects(arg, ctx)) return true;
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

        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            return hasSideEffects(field->object, ctx);
        }

        case ASTKind::IndexExpr: {
            const IndexExprAST* index = expr->as<IndexExprAST>();
            return hasSideEffects(index->target, ctx) || 
                   hasSideEffects(index->index, ctx);
        }

        case ASTKind::SliceExpr: {
            const SliceExprAST* slice = expr->as<SliceExprAST>();
            if (hasSideEffects(slice->target, ctx)) return true;
            if (slice->start && hasSideEffects(slice->start, ctx)) return true;
            if (slice->end && hasSideEffects(slice->end, ctx)) return true;
            return false;
        }

        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* sl = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : sl->inits) {
                if (hasSideEffects(init->value, ctx)) return true;
            }
            return false;
        }

        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* al = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : al->elements) {
                if (hasSideEffects(elem, ctx)) return true;
            }
            return false;
        }

        case ASTKind::IfExpr: {
            const IfExprAST* ifExpr = expr->as<IfExprAST>();
            if (hasSideEffects(ifExpr->condition, ctx)) return true;
            if (hasSideEffects(ifExpr->thenBranch, ctx)) return true;
            if (hasSideEffects(ifExpr->elseBranch, ctx)) return true;
            return false;
        }

        case ASTKind::NullCoalesceExpr: {
            const NullCoalesceExprAST* nc = expr->as<NullCoalesceExprAST>();
            return hasSideEffects(nc->value, ctx) || 
                   hasSideEffects(nc->fallback, ctx);
        }

        case ASTKind::LiteralExpr:
        case ASTKind::IdentifierExpr:
        case ASTKind::ModuleAccessExpr:
        case ASTKind::RangeExpr:
            return false;

        default:
            return false;
    }
}

bool resolveExprStmt(const ExprStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->expr) return false;

    // ─── Resolve the expression ────────────────────────────────────────────
    TypeAST* exprType = resolveExpr(stmt->expr, ctx);
    if (!exprType || exprType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, stmt->expr,
                              "expression has unknown type");
        return false;
    }

    // ─── Check for discarded non-void value ──────────────────────────────
    if (exprType && !exprType->isa<UnknownTypeAST>()) {
        if (!hasSideEffects(stmt->expr, ctx)) {
            ctx.diagnostics.warning(DiagCode::Warn_DiscardedResult, stmt,
                                    "expression result is discarded (no side effects)");
        }
    }

    return false;
}

// =============================================================================
// resolveDeclStmt
// =============================================================================

bool resolveDeclStmt(const DeclStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->decl) return false;

    resolveDecl(stmt->decl, ctx);
    return false;
}

// =============================================================================
// Concurrency Statements
// =============================================================================

// ─── resolveAsyncStmt ──────────────────────────────────────────────────────

bool resolveAsyncStmt(const AsyncStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement outside of function body");
        return false;
    }

    // ─── Check: Must have a binding ─────────────────────────────────────────
    if (!stmt->binding) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement requires a binding variable");
        return false;
    }

    // ─── Resolve the binding's type ────────────────────────────────────────
    // The type is already stored in stmt->binding->type (which was set by the parser).
    // We just need to validate it resolves to FutureTypeAST.
    TypeAST* resolvedType = resolveType(stmt->binding->type, ctx);
    if (!resolvedType || resolvedType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt->binding,
                              "async binding has invalid type");
        return false;
    }

    // ─── Verify it's a FutureTypeAST ───────────────────────────────────────
    if (!resolvedType->isa<FutureTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt->binding,
                              "async binding type must be Future<T>, got ",
                              debug::typeToString(resolvedType, ctx.pool));
        return false;
    }

    // ─── Get the inner type for call validation ────────────────────────────
    const FutureTypeAST* futureType = resolvedType->as<FutureTypeAST>();
    TypeAST* innerType = futureType->inner;

    // ─── Register the binding in the current scope ──────────────────────────
    // The binding already has its type in `binding->type` (the FutureTypeAST).
    // We don't need to store `semanticType` anywhere.
    if (!ctx.insertValue(stmt->binding)) {
        return false;
    }

    // ─── Resolve the call expression ───────────────────────────────────────
    if (!stmt->call) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExprWithTarget(stmt->call, innerType, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── Check: The call's return type must match the Future's inner type ──
    if (!typesEqual(callType, innerType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->call,
                              "async call return type '", 
                              debug::typeToString(callType, ctx.pool),
                              "' does not match binding type '",
                              debug::typeToString(innerType, ctx.pool), "'");
        return false;
    }

    // ─── Store in pending list for later await ─────────────────────────────
    ctx.addPendingAsync(stmt->binding->name, stmt->call, stmt->loc);

    LOG_SEMA("resolveAsyncStmt: registered async '", 
             ctx.pool.lookup(stmt->binding->name), "'");
    return false;
}

// ─── resolveAwaitStmt ──────────────────────────────────────────────────────

bool resolveAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AwaitOutsideFunction, stmt,
                              "await statement outside of function body");
        return false;
    }

    // ─── Check each target variable ────────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "await target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── Look up the variable ──────────────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, target,
                                  "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        if (!decl->isa<VarDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "'", ctx.pool.lookup(targetName), "' is not a variable");
            return false;
        }

        // ─── Check if this is a pending async operation ────────────────────
        if (ctx.hasPendingAsync(targetName)) {
            // ─── Validate the variable's type is FutureTypeAST ────────────
            // Use decl->type directly (the parser-stored type)
            const TypeAST* varType = decl->type;
            if (!varType || !varType->isa<FutureTypeAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                      "'", ctx.pool.lookup(targetName), 
                                      "' is not a Future<T> (type: ", 
                                      debug::typeToString(varType, ctx.pool), ")");
                return false;
            }

            // ─── NARROW THE TYPE: Unwrap FutureTypeAST to its inner type ──
            const FutureTypeAST* futureType = varType->as<FutureTypeAST>();
            const TypeAST* innerType = futureType->inner;
            
            if (!innerType) {
                ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                      "'", ctx.pool.lookup(targetName), 
                                      "' has no inner type");
                return false;
            }

            // Apply narrowing to the variable
            ctx.stack.narrowVariable(targetName, innerType);

            // Mark the async as resolved
            ctx.resolveAsync(targetName);
            
            LOG_SEMA("resolveAwaitStmt: narrowed '", ctx.pool.lookup(targetName),
                     "' from Future<", debug::typeToString(innerType, ctx.pool),
                     "> to ", debug::typeToString(innerType, ctx.pool));
        } else if (ctx.hasPendingSpawn(targetName)) {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "'", ctx.pool.lookup(targetName), 
                                  "' was declared with spawn, not async. Use 'join' instead.");
            return false;
        } else {
            // ─── Check if already narrowed (double await) ──────────────────
            const TypeAST* narrowedType = ctx.stack.getNarrowedType(targetName);
            if (narrowedType) {
                // Check if the original type was FutureTypeAST
                const TypeAST* originalType = decl->type;
                if (originalType && originalType->isa<FutureTypeAST>()) {
                    ctx.diagnostics.error(DiagCode::Sem_DoubleAwait, target,
                                          "'", ctx.pool.lookup(targetName), 
                                          "' has already been awaited");
                    return false;
                }
            }
            
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "'", ctx.pool.lookup(targetName), 
                                  "' is not a pending async operation");
            return false;
        }
    }

    return false;
}

// ─── resolveSpawnStmt ──────────────────────────────────────────────────────

bool resolveSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt,
                              "spawn statement outside of function body");
        return false;
    }

    // ─── Handle discard pattern ────────────────────────────────────────────
    if (!stmt->binding) {
        if (!stmt->call) {
            ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt,
                                  "spawn statement requires a call expression");
            return false;
        }

        TypeAST* callType = resolveExpr(stmt->call, ctx);
        if (!callType || callType->isa<UnknownTypeAST>()) {
            return false;
        }

        LOG_SEMA("resolveSpawnStmt: parsed spawn discard (fire-and-forget)");
        return false;
    }

    // ─── Resolve the binding's type ────────────────────────────────────────
    TypeAST* resolvedType = resolveType(stmt->binding->type, ctx);
    if (!resolvedType || resolvedType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt->binding,
                              "spawn binding has invalid type");
        return false;
    }

    // ─── Verify it's a ThreadTypeAST ───────────────────────────────────────
    if (!resolvedType->isa<ThreadTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt->binding,
                              "spawn binding type must be Thread<T>, got ",
                              debug::typeToString(resolvedType, ctx.pool));
        return false;
    }

    // ─── Get the inner type for call validation ────────────────────────────
    const ThreadTypeAST* threadType = resolvedType->as<ThreadTypeAST>();
    TypeAST* innerType = threadType->inner;

    // ─── Register the binding in the current scope ──────────────────────────
    // The binding already has its type in `binding->type` (the ThreadTypeAST).
    if (!ctx.insertValue(stmt->binding)) {
        return false;
    }

    // ─── Resolve the call expression ───────────────────────────────────────
    if (!stmt->call) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt,
                              "spawn statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExprWithTarget(stmt->call, innerType, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── Check: The call's return type must match the Thread's inner type ──
    if (!typesEqual(callType, innerType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->call,
                              "spawn call return type '", 
                              debug::typeToString(callType, ctx.pool),
                              "' does not match binding type '",
                              debug::typeToString(innerType, ctx.pool), "'");
        return false;
    }

    // ─── Store in pending list for later join ──────────────────────────────
    ctx.addPendingSpawn(stmt->binding->name, stmt->call, stmt->loc);

    LOG_SEMA("resolveSpawnStmt: registered spawn '", 
             ctx.pool.lookup(stmt->binding->name), "'");
    return false;
}

// ─── resolveJoinStmt ───────────────────────────────────────────────────────

bool resolveJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_JoinOutsideFunction, stmt,
                              "join statement outside of function body");
        return false;
    }

    // ─── Check each target variable ────────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "join target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── Look up the variable ──────────────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, target,
                                  "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        if (!decl->isa<VarDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "'", ctx.pool.lookup(targetName), "' is not a variable");
            return false;
        }

        // ─── Check if this is a pending spawn operation ────────────────────
        if (ctx.hasPendingSpawn(targetName)) {
            // ─── Validate the variable's type is ThreadTypeAST ────────────
            // Use decl->type directly (the parser-stored type)
            const TypeAST* varType = decl->type;
            if (!varType || !varType->isa<ThreadTypeAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                      "'", ctx.pool.lookup(targetName), 
                                      "' is not a Thread<T> (type: ", 
                                      debug::typeToString(varType, ctx.pool), ")");
                return false;
            }

            // ─── NARROW THE TYPE: Unwrap ThreadTypeAST to its inner type ──
            const ThreadTypeAST* threadType = varType->as<ThreadTypeAST>();
            const TypeAST* innerType = threadType->inner;
            
            if (!innerType) {
                ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                      "'", ctx.pool.lookup(targetName), 
                                      "' has no inner type");
                return false;
            }

            // Apply narrowing to the variable
            ctx.stack.narrowVariable(targetName, innerType);

            // Mark the spawn as resolved
            ctx.resolveSpawn(targetName);
            
            LOG_SEMA("resolveJoinStmt: narrowed '", ctx.pool.lookup(targetName),
                     "' from Thread<", debug::typeToString(innerType, ctx.pool),
                     "> to ", debug::typeToString(innerType, ctx.pool));
        } else if (ctx.hasPendingAsync(targetName)) {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "'", ctx.pool.lookup(targetName), 
                                  "' was declared with async, not spawn. Use 'await' instead.");
            return false;
        } else {
            // ─── Check if already narrowed (double join) ──────────────────
            const TypeAST* narrowedType = ctx.stack.getNarrowedType(targetName);
            if (narrowedType) {
                const TypeAST* originalType = decl->type;
                if (originalType && originalType->isa<ThreadTypeAST>()) {
                    ctx.diagnostics.error(DiagCode::Sem_DoubleJoin, target,
                                          "'", ctx.pool.lookup(targetName), 
                                          "' has already been joined");
                    return false;
                }
            }
            
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "'", ctx.pool.lookup(targetName), 
                                  "' is not a pending spawn operation");
            return false;
        }
    }

    return false;
}

} // namespace sema
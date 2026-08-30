/// @file CaptureAnalysis.cpp
/// @brief Implementation of closure capture and escape analysis.
///
/// # Sema vs CodeGen Responsibilities
///
/// ## Sema (This File)
/// - Detect captures and mark them with `byReference` and `isClosureValue`
/// - `isClosureValue = true` is conservative for unknown function-typed values
/// - CodeGen must handle runtime checking for these conservative cases
///
/// ## CodeGen
/// - For `isClosureValue = true` where the actual value might be a plain function,
///   emit a runtime check to determine the value's shape
/// - Use a runtime API (e.g., `__lucid_is_closure(value)`) to check
/// - Store 1 word for plain function, 2 words for closure
/// - Handle refcounting for closure environments

#include "CaptureAnalysis.hpp"
#include "../types/SemaType.hpp"
#include "core/ASTStrings.hpp"
#include "core/trace/Trace.hpp"
#include "core/ast/TypeAST.hpp"

#include <unordered_map>
#include <functional>

namespace sema {

// ─── isClosureValue Implementation ──────────────────────────────────────────

bool isClosureValue(ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        case ASTKind::AnonFuncExpr: {
            auto* anon = expr->as<AnonFuncExprAST>();
            return anon->hasClosure;
        }

        case ASTKind::IdentifierExpr: {
            auto* id = expr->as<IdentifierExprAST>();
            if (!id->resolvedDecl) return false;

            // ─── Case 1: Function declaration ──────────────────────────────
            // We know at compile time if this function captures variables.
            if (id->resolvedDecl->isa<FuncDeclAST>()) {
                auto* funcDecl = id->resolvedDecl->as<FuncDeclAST>();
                return funcDecl->hasClosure;
            }

            // ─── Case 2: Struct field ──────────────────────────────────────
            // Fields can hold function values. Sema cannot know at compile time
            // if the field will hold a plain function or a closure when the
            // struct literal is created. Conservative: return `true` so CodeGen
            // can emit a runtime check.
            if (id->resolvedDecl->isa<FieldDeclAST>()) {
                auto* fieldDecl = id->resolvedDecl->as<FieldDeclAST>();
                if (fieldDecl->type && fieldDecl->type->isa<FuncTypeAST>()) {
                    // If the field has a default value, check if we can determine
                    // at compile time whether it's a closure.
                    if (fieldDecl->defaultVal) {
                        // Recursively check the default value.
                        return isClosureValue(fieldDecl->defaultVal, ctx);
                    }
                    if (fieldDecl->defaultBody) {
                        // Function field with block body - we could analyze
                        // the body, but for simplicity we're conservative.
                        // CodeGen will handle the runtime check.
                        return true;
                    }
                    // No default value - will be initialized at struct literal site.
                    // Sema cannot know the actual value. Conservative: `true`.
                    return true;
                }
                // Non-function fields are not closures.
                return false;
            }

            // ─── Case 3: Function parameter ───────────────────────────────
            // Parameters are passed by the caller. Sema cannot know at compile
            // time if the caller will pass a plain function or a closure.
            // Conservative: `true` so CodeGen can emit a runtime check.
            if (id->resolvedDecl->isa<ParamAST>()) {
                auto* param = id->resolvedDecl->as<ParamAST>();
                if (param->type && param->type->isa<FuncTypeAST>()) {
                    return true;  // Conservative: CodeGen must do runtime check
                }
                return false;
            }

            // ─── Case 4: Variable declaration ──────────────────────────────
            // VarDeclAST cannot hold function values in Lucid.
            // (No type inference, no function-typed variables.)
            return false;
        }

        case ASTKind::ModuleAccessExpr: {
            auto* mod = expr->as<ModuleAccessExprAST>();
            if (!mod->resolvedDecl) return false;

            // ─── Module member function declaration ────────────────────────
            if (mod->resolvedDecl->isa<FuncDeclAST>()) {
                auto* funcDecl = mod->resolvedDecl->as<FuncDeclAST>();
                return funcDecl->hasClosure;
            }

            // ─── Module member field ────────────────────────────────────────
            // Same conservative logic as FieldDeclAST above.
            if (mod->resolvedDecl->isa<FieldDeclAST>()) {
                auto* fieldDecl = mod->resolvedDecl->as<FieldDeclAST>();
                if (fieldDecl->type && fieldDecl->type->isa<FuncTypeAST>()) {
                    if (fieldDecl->defaultVal) {
                        return isClosureValue(fieldDecl->defaultVal, ctx);
                    }
                    if (fieldDecl->defaultBody) {
                        return true;  // Conservative: runtime check in CodeGen
                    }
                    return true;  // Conservative: runtime check in CodeGen
                }
                return false;
            }

            // ─── Module member variables cannot hold function values ──────
            return false;
        }

        case ASTKind::FieldAccessExpr: {
            auto* field = expr->as<FieldAccessExprAST>();

            // ─── Field access to a struct field ────────────────────────────
            // Same conservative logic as FieldDeclAST above.
            if (field->resolvedDecl && field->resolvedDecl->isa<FieldDeclAST>()) {
                auto* fieldDecl = field->resolvedDecl->as<FieldDeclAST>();
                if (fieldDecl->type && fieldDecl->type->isa<FuncTypeAST>()) {
                    if (fieldDecl->defaultVal) {
                        return isClosureValue(fieldDecl->defaultVal, ctx);
                    }
                    if (fieldDecl->defaultBody) {
                        return true;  // Conservative: runtime check in CodeGen
                    }
                    return true;  // Conservative: runtime check in CodeGen
                }
                return false;
            }

            // ─── Field access to a function field (resolved to FuncDeclAST) ──
            if (field->resolvedDecl && field->resolvedDecl->isa<FuncDeclAST>()) {
                return field->resolvedDecl->as<FuncDeclAST>()->hasClosure;
            }

            return false;
        }

        case ASTKind::CallExpr: {
            // ─── Call expression returning a function value ──────────────
            // A call expression returns a value. If the return type is a function
            // type, we don't know at compile time if the callee returns a plain
            // function or a closure. Conservative: `true` so CodeGen can emit a
            // runtime check.
            auto* call = expr->as<CallExprAST>();
            if (call->resolvedType && call->resolvedType->isa<FuncTypeAST>()) {
                // Conservative: CodeGen must do runtime check
                return true;
            }
            return false;
        }

        default:
            return false;
    }
}

// ─── Internal CaptureAnalyzer ──────────────────────────────────────────────

namespace {

/// @brief Internal state for capture analysis.
///
/// This analyzer walks the AST of a function/closure body and detects
/// which variables from outer scopes are captured.
///
/// # Key Design Decisions
///
/// 1. **Two-pass approach**: First we walk the AST to detect mutations
///    (`mutatedVariables`), then we add captures. This ensures mutation
///    detection happens before capture decisions are made.
///
/// 2. **Conservative `isClosureValue`**: For function-typed parameters and
///    fields, Sema sets `isClosureValue = true` conservatively. CodeGen
///    must emit a runtime check to determine the actual value's shape.
///
/// 3. **By-reference vs by-value**: Uses mutation analysis to decide.
///    Read-only captures are by-value (snapshot copy), mutated captures
///    are by-reference.
struct CaptureAnalyzer {
    SemaContext& ctx;

    /// The closure being analyzed (if analyzing an anonymous function).
    AnonFuncExprAST* closure = nullptr;

    /// The function being analyzed (if analyzing a named function).
    FuncDeclAST* function = nullptr;

    /// The innermost function node (FuncDeclAST or AnonFuncExprAST).
    BaseAST* innermostFunction = nullptr;

    /// Current closure depth (from ContextStack).
    size_t currentClosureDepth = 0;

    /// Variables declared in the closure's own parameter list.
    /// These are NOT captures.
    std::unordered_set<InternedString> ownParams;

    /// Variables declared *inside* the body currently being walked — via
    /// `let`/`const` or a `for` loop's index/value binders — tracked as a
    /// stack of block-scoped frames.
    std::vector<std::unordered_set<InternedString>> localScopes;

    /// Variables that have been marked as captures.
    std::vector<CapturedVariable> captures;

    /// Variables that have been seen to avoid duplicates.
    std::unordered_set<InternedString> seenCaptures;

    /// Variables that are assigned to inside the closure body.
    /// Used to decide by-reference vs by-value capture.
    ///
    /// @note This set is populated during the first pass (walkExpr) and
    ///       then used during the second pass (validateAndAddCapture).
    std::unordered_set<InternedString> mutatedVariables;

    // ─── Constructors ──────────────────────────────────────────────────────

    /// Constructor for anonymous function analysis.
    CaptureAnalyzer(SemaContext& c, AnonFuncExprAST* e)
        : ctx(c)
        , closure(e)
        , function(nullptr)
        , innermostFunction(e)
        , currentClosureDepth(ctx.getClosureDepth()) {
        localScopes.emplace_back();   // top-level frame for the closure's own body
    }

    /// Constructor for named function analysis.
    CaptureAnalyzer(SemaContext& c, FuncDeclAST* f)
        : ctx(c)
        , closure(nullptr)
        , function(f)
        , innermostFunction(f)
        , currentClosureDepth(ctx.getClosureDepth()) {
        localScopes.emplace_back();   // top-level frame for the function's own body
    }

    // ─── Capture Detection ──────────────────────────────────────────────────

    /// @brief Check if a name is a parameter of this function/closure.
    bool isOwnParam(InternedString name) const {
        return ownParams.find(name) != ownParams.end();
    }

    /// @brief Push a new local-scope frame.
    void pushLocalScope() {
        localScopes.emplace_back();
    }

    /// @brief Pop the innermost local-scope frame.
    void popLocalScope() {
        if (!localScopes.empty()) localScopes.pop_back();
    }

    /// @brief Register `name` as declared in the innermost currently-open local scope.
    void declareLocal(InternedString name) {
        if (!localScopes.empty()) localScopes.back().insert(name);
    }

    /// @brief Check if `name` was declared anywhere within the body being walked.
    bool isLocallyDeclared(InternedString name) const {
        for (auto it = localScopes.rbegin(); it != localScopes.rend(); ++it) {
            if (it->find(name) != it->end()) return true;
        }
        return false;
    }

    /// @brief Check if a name is from an outer scope (i.e., a capture).
    bool isCapture(InternedString name) const {
        // Module members are global - not captures
        if (ctx.isModuleMember(name)) {
            return false;
        }

        // Declared inside this body (own params, or localScopes) — not a capture
        if (isOwnParam(name) || isLocallyDeclared(name)) {
            return false;
        }

        // Generic parameters are not captures
        if (ctx.isGenericParam(name)) {
            return false;
        }

        // Check if the name exists in any outer scope
        ValueDeclAST* decl = ctx.lookupValue(name);
        if (!decl) {
            return false;
        }

        return true;
    }

    ValueDeclAST* getDeclaration(InternedString name) const {
        return ctx.lookupValue(name);
    }

    // ─── Validate + Add Capture ──────────────────────────────────────────────

    /// @brief Validate capture rules for `decl` and add it to the capture list.
    ///
    /// # Conservative `isClosureValue` Handling
    ///
    /// For `ParamAST` and `FieldDeclAST` where the actual value is unknown
    /// at compile time, this function sets `isClosureValue = true`. CodeGen
    /// must emit a runtime check to determine the actual value's shape.
    ///
    /// The runtime check should:
    /// 1. Inspect the function value to determine if it's a closure
    ///    (e.g., using `__lucid_is_closure(value)`)
    /// 2. Store the appropriate representation:
    ///    - Plain function: 1 word (function pointer)
    ///    - Closure: 2 words (function pointer + environment pointer)
    ///
    /// This approach is safe but may result in a small memory overhead
    /// (2 words allocated instead of 1) for plain functions.
    void validateAndAddCapture(ValueDeclAST* decl, BaseAST* diagLoc) {
        if (!decl) return;
        InternedString name = decl->name;

        // Skip if already seen
        if (seenCaptures.find(name) != seenCaptures.end()) {
            return;
        }

        // ─── Validate capture rules ─────────────────────────────────────────
        TypeAST* varType = decl->type;

        // Rule 3: Borrowed types (&T, [_]T) cannot be captured
        if (varType && isBorrowedType(varType)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, diagLoc,
                                  "closure cannot capture borrowed type '",
                                  ctx.pool.lookup(name),
                                  "' (", typeToString(varType, ctx.pool),
                                  ") — closures cannot capture &T or [_]T");
            return;
        }

        // Rule 4: Linear types (Future<T>, Thread<T>) cannot be captured
        if (varType && (varType->isa<FutureTypeAST>() || varType->isa<ThreadTypeAST>())) {
            const char* typeName = varType->isa<FutureTypeAST>() ? "Future<T>" : "Thread<T>";
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, diagLoc,
                                  "closure cannot capture linear type '",
                                  ctx.pool.lookup(name),
                                  "' (", typeName, ") — linear values can only be consumed once");
            return;
        }

        // ─── Determine if this captured value itself is a closure ──────────
        bool isClosureVal = false;

        // Case 1: It's an AnonFuncExprAST (closure literal)
        // We know at compile time if this anonymous function captures variables.
        if (decl->isa<AnonFuncExprAST>()) {
            isClosureVal = decl->as<AnonFuncExprAST>()->hasClosure;
        }

        // Case 2: It's a FuncDeclAST (named function)
        // We know at compile time if this function captures variables.
        else if (decl->isa<FuncDeclAST>()) {
            isClosureVal = decl->as<FuncDeclAST>()->hasClosure;
        }

        // Case 3: It's a FieldDeclAST (struct field)
        // Fields can hold function values. Sema cannot know at compile time
        // if the field will hold a plain function or a closure when the
        // struct literal is created. Conservative: `true` so CodeGen can
        // emit a runtime check.
        else if (decl->isa<FieldDeclAST>()) {
            auto* fieldDecl = decl->as<FieldDeclAST>();
            if (fieldDecl->type && fieldDecl->type->isa<FuncTypeAST>()) {
                // If the field has a default value, check if we can determine
                // at compile time whether it's a closure.
                if (fieldDecl->defaultVal) {
                    isClosureVal = isClosureValue(fieldDecl->defaultVal, ctx);
                } else if (fieldDecl->defaultBody) {
                    // Function field with block body - conservative: `true`.
                    // CodeGen will handle the runtime check.
                    isClosureVal = true;
                } else {
                    // No default value - will be initialized at struct literal site.
                    // Sema cannot know the actual value. Conservative: `true`.
                    isClosureVal = true;
                }
            }
            // Non-function fields are not closures.
        }

        // Case 4: It's a ParamAST (function parameter)
        // Parameters are passed by the caller. Sema cannot know at compile time
        // if the caller will pass a plain function or a closure.
        // Conservative: `true` so CodeGen can emit a runtime check.
        else if (decl->isa<ParamAST>()) {
            auto* param = decl->as<ParamAST>();
            if (param->type && param->type->isa<FuncTypeAST>()) {
                // Conservative: CodeGen must do runtime check
                isClosureVal = true;
            }
        }

        // Case 5: VarDeclAST - cannot hold function values in Lucid.
        // Case 6: EnumVariantAST - constants, not functions.
        // All other declaration types are not function values.

        // ─── Determine capture by reference vs by value ────────────────────
        // Lucid grammar: read‑only captures may be snapshot‑copied (by‑value).
        // If the variable is mutated anywhere in the closure body, it must
        // be captured by reference to reflect those changes.
        bool mutated = (mutatedVariables.find(name) != mutatedVariables.end());
        bool byRef = mutated;

        // ─── Create the capture entry ──────────────────────────────────────
        CapturedVariable capture;
        capture.decl = decl;
        capture.byReference = byRef;
        capture.isClosureValue = isClosureVal;
        capture.index = captures.size();

        captures.push_back(capture);
        seenCaptures.insert(name);

        Trace::info("CaptureAnalysis: captured '", ctx.pool.lookup(name),
                 "' by ", byRef ? "reference" : "value",
                 " (closure value: ", isClosureVal ? "yes (conservative)" : "no",
                 ") at depth ", currentClosureDepth);
    }

    // ─── Propagate Capture ────────────────────────────────────────────────────

    /// @brief Pull a capture from a nested closure up into our own capture list.
    ///
    /// Why this is needed: If a variable is referenced only inside a nested
    /// closure's own nested closure, and the immediately-enclosing closure
    /// never captures it itself, CodeGen would end up reusing a stale value
    /// from a different function. Propagating the capture upward closes this gap.
    ///
    /// @param childCapture The capture from the nested closure.
    /// @param diagLoc AST node to anchor diagnostics on.
    void propagateCapture(const CapturedVariable& childCapture, BaseAST* diagLoc) {
        if (!childCapture.decl) return;
        InternedString name = childCapture.decl->name;

        // Already ours - own param or locally declared
        if (isOwnParam(name) || isLocallyDeclared(name)) return;

        // Module members and generic params are never captured/propagated
        if (ctx.isModuleMember(name)) return;
        if (ctx.isGenericParam(name)) return;

        validateAndAddCapture(childCapture.decl, diagLoc);
    }

    // ─── Process Identifier ──────────────────────────────────────────────────

    void processIdentifier(IdentifierExprAST* id) {
        if (!id) return;

        InternedString name = id->name;

        // Skip '_' (discard placeholder)
        if (ctx.pool.lookupView(name) == "_") {
            return;
        }

        // Skip if it's our own parameter
        if (isOwnParam(name)) {
            return;
        }

        // Check if this is a capture from an outer scope
        if (!isCapture(name)) {
            return;
        }

        // Skip if already seen
        if (seenCaptures.find(name) != seenCaptures.end()) {
            return;
        }

        ValueDeclAST* decl = getDeclaration(name);
        if (!decl) {
            return;
        }

        validateAndAddCapture(decl, id);
    }

    // ─── Mutation Detection ──────────────────────────────────────────────────

    /// @brief Detect if an expression mutates a variable.
    ///
    /// A variable is considered mutated if it appears on the LHS of:
    ///   - Plain assignment (x = ...)
    ///   - Compound assignment (x += ...)
    ///   - Field assignment (x.field = ...)
    ///   - Index assignment (x[i] = ...)
    ///
    /// @note This is a **first pass** that runs before capture decisions are made.
    ///       The `mutatedVariables` set is populated first, then used by
    ///       `validateAndAddCapture` to decide by-reference vs by-value.
    void detectMutation(ExprAST* expr) {
        if (!expr) return;

        // Check for assignments
        if (expr->isa<AssignExprAST>()) {
            auto* assign = expr->as<AssignExprAST>();
            if (assign->lhs) {
                // Check for plain identifier
                if (assign->lhs->isa<IdentifierExprAST>()) {
                    auto* id = assign->lhs->as<IdentifierExprAST>();
                    if (id->resolvedDecl) {
                        mutatedVariables.insert(id->name);
                    }
                }
                // Check for field access (obj.field = ...)
                else if (assign->lhs->isa<FieldAccessExprAST>()) {
                    auto* field = assign->lhs->as<FieldAccessExprAST>();
                    if (field->object && field->object->isa<IdentifierExprAST>()) {
                        auto* objId = field->object->as<IdentifierExprAST>();
                        if (objId->resolvedDecl) {
                            mutatedVariables.insert(objId->name);
                        }
                    }
                }
                // Check for index access (arr[i] = ...)
                else if (assign->lhs->isa<IndexExprAST>()) {
                    auto* index = assign->lhs->as<IndexExprAST>();
                    if (index->target && index->target->isa<IdentifierExprAST>()) {
                        auto* objId = index->target->as<IdentifierExprAST>();
                        if (objId->resolvedDecl) {
                            mutatedVariables.insert(objId->name);
                        }
                    }
                }
            }
        }

        // Recurse into sub-expressions based on kind
        switch (expr->kind) {
            case ASTKind::BinaryExpr: {
                auto* bin = expr->as<BinaryExprAST>();
                detectMutation(bin->left);
                detectMutation(bin->right);
                break;
            }
            case ASTKind::UnaryExpr: {
                auto* unary = expr->as<UnaryExprAST>();
                detectMutation(unary->operand);
                break;
            }
            case ASTKind::CallExpr: {
                auto* call = expr->as<CallExprAST>();
                detectMutation(call->callee);
                for (ExprAST* arg : call->args) {
                    detectMutation(arg);
                }
                break;
            }
            case ASTKind::PipelineExpr: {
                auto* pipeline = expr->as<PipelineExprAST>();
                detectMutation(pipeline->seed);
                for (const PipelineStepAST* step : pipeline->steps) {
                    detectMutation(step->callable);
                    for (ExprAST* arg : step->packArgs) {
                        detectMutation(arg);
                    }
                }
                break;
            }
            // ... other expression kinds that contain sub-expressions
            default:
                break;
        }
    }

    // ─── AST Walking ──────────────────────────────────────────────────────────

    void walkExpr(ExprAST* expr) {
        if (!expr) return;

        // First, detect any mutations in this expression
        detectMutation(expr);

        switch (expr->kind) {
            case ASTKind::IdentifierExpr:
                processIdentifier(expr->as<IdentifierExprAST>());
                break;

            case ASTKind::BinaryExpr: {
                BinaryExprAST* bin = expr->as<BinaryExprAST>();
                walkExpr(bin->left);
                walkExpr(bin->right);
                break;
            }

            case ASTKind::UnaryExpr: {
                UnaryExprAST* unary = expr->as<UnaryExprAST>();
                walkExpr(unary->operand);
                break;
            }

            case ASTKind::CallExpr: {
                CallExprAST* call = expr->as<CallExprAST>();
                walkExpr(call->callee);
                for (ExprAST* arg : call->args) {
                    walkExpr(arg);
                }
                break;
            }

            case ASTKind::FieldAccessExpr: {
                FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
                walkExpr(field->object);
                break;
            }

            case ASTKind::IndexExpr: {
                IndexExprAST* index = expr->as<IndexExprAST>();
                walkExpr(index->target);
                walkExpr(index->index);
                break;
            }

            case ASTKind::SliceExpr: {
                SliceExprAST* slice = expr->as<SliceExprAST>();
                walkExpr(slice->target);
                if (slice->start) walkExpr(slice->start);
                if (slice->end) walkExpr(slice->end);
                break;
            }

            case ASTKind::ArrayLiteralExpr: {
                ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
                for (ExprAST* elem : arr->elements) {
                    walkExpr(elem);
                }
                break;
            }

            case ASTKind::StructLiteralExpr: {
                StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
                for (FieldInitAST* init : st->inits) {
                    walkExpr(init->value);
                }
                break;
            }

            case ASTKind::NullCoalesceExpr: {
                NullCoalesceExprAST* nc = expr->as<NullCoalesceExprAST>();
                walkExpr(nc->value);
                walkExpr(nc->fallback);
                break;
            }

            case ASTKind::AssignExpr: {
                AssignExprAST* assign = expr->as<AssignExprAST>();
                // detectMutation already handled this
                walkExpr(assign->lhs);
                walkExpr(assign->rhs);
                break;
            }

            case ASTKind::PipelineExpr: {
                PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
                walkExpr(pipeline->seed);
                for (const PipelineStepAST* step : pipeline->steps) {
                    walkExpr(step->callable);
                    for (ExprAST* arg : step->packArgs) {
                        walkExpr(arg);
                    }
                }
                break;
            }

            case ASTKind::ComposeExpr: {
                ComposeExprAST* compose = expr->as<ComposeExprAST>();
                if (compose->left) {
                    walkExpr(compose->left);
                }
                for (const ComposeOperandAST* operand : compose->operands) {
                    if (operand->callable) {
                        walkExpr(operand->callable);
                    }
                }
                break;
            }

            case ASTKind::AnonFuncExpr: {
                // Nested closure: propagate its captures upward
                AnonFuncExprAST* nested = expr->as<AnonFuncExprAST>();
                if (nested) {
                    for (const CapturedVariable& childCapture : nested->captures) {
                        propagateCapture(childCapture, nested);
                    }
                }
                break;
            }

            case ASTKind::IfExpr: {
                IfExprAST* ifExpr = expr->as<IfExprAST>();
                walkExpr(ifExpr->condition);
                walkExpr(ifExpr->thenBranch);
                walkExpr(ifExpr->elseBranch);
                break;
            }

            case ASTKind::RangeExpr: {
                RangeExprAST* range = expr->as<RangeExprAST>();
                walkExpr(range->lo);
                walkExpr(range->hi);
                break;
            }

            case ASTKind::ModuleAccessExpr:
                // Module members are global - not captures
                break;

            case ASTKind::LiteralExpr:
                break;

            case ASTKind::IntrinsicCallExpr: {
                IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
                for (ExprAST* arg : intrinsic->args) {
                    walkExpr(arg);
                }
                break;
            }

            default:
                break;
        }
    }

    void walkStmt(StmtAST* stmt) {
        if (!stmt) return;

        switch (stmt->kind) {
            case ASTKind::BlockStmt: {
                BlockStmtAST* block = stmt->as<BlockStmtAST>();
                pushLocalScope();
                for (StmtAST* s : block->stmts) {
                    walkStmt(s);
                }
                popLocalScope();
                break;
            }

            case ASTKind::ExprStmt: {
                ExprStmtAST* exprStmt = stmt->as<ExprStmtAST>();
                walkExpr(exprStmt->expr);
                break;
            }

            case ASTKind::DeclStmt: {
                DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
                if (declStmt->decl && declStmt->decl->isa<VarDeclAST>()) {
                    VarDeclAST* var = declStmt->decl->as<VarDeclAST>();
                    if (var->init) {
                        walkExpr(var->init);
                    }
                    // Register AFTER walking the initializer
                    declareLocal(var->name);
                }
                break;
            }

            case ASTKind::IfStmt: {
                IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
                walkExpr(ifStmt->condition);
                walkStmt(ifStmt->thenBranch);
                if (ifStmt->elseBranch) {
                    walkStmt(ifStmt->elseBranch);
                }
                break;
            }

            case ASTKind::SwitchStmt: {
                SwitchStmtAST* switchStmt = stmt->as<SwitchStmtAST>();
                walkExpr(switchStmt->subject);
                for (const SwitchCaseAST* caseStmt : switchStmt->cases) {
                    for (ExprAST* value : caseStmt->values) {
                        walkExpr(value);
                    }
                    if (caseStmt->body) {
                        walkStmt(caseStmt->body);
                    }
                }
                if (switchStmt->defaultBody) {
                    walkStmt(switchStmt->defaultBody);
                }
                break;
            }

            case ASTKind::ForStmt: {
                ForStmtAST* forStmt = stmt->as<ForStmtAST>();
                walkExpr(forStmt->iterable);
                if (forStmt->step) {
                    walkExpr(forStmt->step);
                }
                pushLocalScope();
                if (forStmt->indexVar) declareLocal(forStmt->indexVar->name);
                if (forStmt->valueVar) declareLocal(forStmt->valueVar->name);
                if (forStmt->body) {
                    walkStmt(forStmt->body);
                }
                popLocalScope();
                break;
            }

            case ASTKind::WhileStmt: {
                WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
                walkExpr(whileStmt->condition);
                if (whileStmt->body) {
                    walkStmt(whileStmt->body);
                }
                break;
            }

            case ASTKind::DoWhileStmt: {
                DoWhileStmtAST* doWhileStmt = stmt->as<DoWhileStmtAST>();
                if (doWhileStmt->body) {
                    walkStmt(doWhileStmt->body);
                }
                walkExpr(doWhileStmt->condition);
                break;
            }

            case ASTKind::ReturnStmt: {
                ReturnStmtAST* returnStmt = stmt->as<ReturnStmtAST>();
                if (returnStmt->value) {
                    walkExpr(returnStmt->value);
                }
                break;
            }

            case ASTKind::AsyncStmt: {
                AsyncStmtAST* asyncStmt = stmt->as<AsyncStmtAST>();
                if (asyncStmt->call) {
                    walkExpr(asyncStmt->call);
                }
                if (asyncStmt->binding) {
                    declareLocal(asyncStmt->binding->name);
                }
                break;
            }

            case ASTKind::SpawnStmt: {
                SpawnStmtAST* spawnStmt = stmt->as<SpawnStmtAST>();
                if (spawnStmt->call) {
                    walkExpr(spawnStmt->call);
                }
                if (spawnStmt->binding) {
                    declareLocal(spawnStmt->binding->name);
                }
                break;
            }

            case ASTKind::AwaitStmt: {
                AwaitStmtAST* awaitStmt = stmt->as<AwaitStmtAST>();
                for (ExprAST* target : awaitStmt->targets) {
                    walkExpr(target);
                }
                break;
            }

            case ASTKind::JoinStmt: {
                JoinStmtAST* joinStmt = stmt->as<JoinStmtAST>();
                for (ExprAST* target : joinStmt->targets) {
                    walkExpr(target);
                }
                break;
            }

            default:
                break;
        }
    }

    // ─── Store Captures ──────────────────────────────────────────────────────

    /// @brief Store the captured variables on the appropriate AST node.
    void storeCaptures() {
        if (captures.empty()) {
            return;
        }

        // Build the ArenaSpan
        auto builder = ctx.arena.makeBuilder<CapturedVariable>();
        for (const auto& capture : captures) {
            builder.push_back(capture);
        }
        ArenaSpan<CapturedVariable> captureSpan = builder.build();

        // Store on the appropriate node
        if (closure) {
            closure->captures = captureSpan;
            closure->hasClosure = true;
            Trace::detail("analyzeCaptures: anonymous closure captures ",
                     captures.size(), " variables");
        } else if (function) {
            function->captures = captureSpan;
            function->hasClosure = true;
            Trace::detail("analyzeCaptures: function '",
                     ctx.pool.lookup(function->name),
                     "' captures ", captures.size(), " variables");
        }
    }
};

} // anonymous namespace

// ─── analyzeCaptures (AnonFuncExprAST) ──────────────────────────────────────

void analyzeCaptures(AnonFuncExprAST* expr, SemaContext& ctx) {
    if (!expr || !expr->body) {
        return;
    }

    Trace::detail("analyzeCaptures: analyzing anonymous closure at depth ",
             ctx.getClosureDepth());

    CaptureAnalyzer analyzer(ctx, expr);

    // ─── Step 1: Collect the closure's own parameters ──────────────────────
    if (expr->funcType) {
        for (FuncTypeAST* group = expr->funcType; group; group = group->getNext()) {
            for (ParamAST* param : group->params) {
                analyzer.ownParams.insert(param->name);
            }
        }
    }

    // ─── Step 2: Walk the body to find captures ─────────────────────────────
    analyzer.walkStmt(expr->body);

    // ─── Step 3: Store the captures on the AST node ─────────────────────────
    analyzer.storeCaptures();

    if (!expr->hasClosure) {
        Trace::detail("analyzeCaptures: no captures detected for anonymous closure");
    }
}

// ─── analyzeCaptures (FuncDeclAST) ──────────────────────────────────────────

void analyzeCaptures(FuncDeclAST* func, SemaContext& ctx) {
    if (!func || !func->body) {
        return;
    }

    // ─── Only nested functions can capture variables ──────────────────────
    size_t currentDepth = ctx.getClosureDepth();
    if (currentDepth == 0) {
        // Top-level function - cannot capture anything
        Trace::detail("analyzeCaptures: top-level function '",
                 ctx.pool.lookup(func->name),
                 "' cannot capture variables");
        return;
    }

    Trace::detail("analyzeCaptures: analyzing nested function '",
             ctx.pool.lookup(func->name),
             "' at depth ", currentDepth);

    CaptureAnalyzer analyzer(ctx, func);

    // ─── Step 1: Collect the function's own parameters ──────────────────────
    if (func->funcType) {
        for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
            for (ParamAST* param : group->params) {
                analyzer.ownParams.insert(param->name);
            }
        }
    }

    // ─── Step 2: Walk the body to find captures ─────────────────────────────
    analyzer.walkStmt(func->body);

    // ─── Step 3: Store the captures on the AST node ─────────────────────────
    if (!analyzer.captures.empty()) {
        auto builder = ctx.arena.makeBuilder<CapturedVariable>();
        for (const auto& capture : analyzer.captures) {
            builder.push_back(capture);
        }
        func->captures = builder.build();
        func->hasClosure = true;

        Trace::detail("analyzeCaptures: function '", ctx.pool.lookup(func->name),
                 "' captures ", func->captures.size(), " variables");
    } else {
        func->hasClosure = false;
        Trace::detail("analyzeCaptures: no captures detected for function '",
                 ctx.pool.lookup(func->name), "'");
    }
}

// ─── markClosureIfEscaping ──────────────────────────────────────────────────

void markClosureIfEscaping(ExprAST* expr, SemaContext& ctx) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::AnonFuncExpr: {
            AnonFuncExprAST* closure = expr->as<AnonFuncExprAST>();
            closure->isReturned = true;
            Trace::detail("markClosureIfEscaping: direct anonymous function returned");
            return;
        }

        case ASTKind::IdentifierExpr: {
            IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            ValueDeclAST* decl = ctx.lookupValue(id->name);

            if (!decl) return;

            if (ctx.isModuleMember(id->name)) {
                Trace::detail("markClosureIfEscaping: '", ctx.pool.lookup(id->name),
                         "' is a module member (static) - not marking as escaping");
                return;
            }

            if (decl->isa<FuncDeclAST>()) {
                FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
                if (!ctx.isModuleMember(funcDecl->name)) {
                    funcDecl->isReturned = true;
                    Trace::detail("markClosureIfEscaping: nested function '",
                            ctx.pool.lookup(id->name),
                            "' returned - marking as closure");
                }
                return;
            }
            return;
        }

        case ASTKind::ModuleAccessExpr: {
            Trace::detail("markClosureIfEscaping: module member '",
                     ctx.pool.lookup(expr->as<ModuleAccessExprAST>()->moduleName), ":",
                     ctx.pool.lookup(expr->as<ModuleAccessExprAST>()->memberName),
                     "' is static - not marking as escaping");
            return;
        }

        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();

            if (field->object && field->object->isa<IdentifierExprAST>()) {
                IdentifierExprAST* id = field->object->as<IdentifierExprAST>();

                if (ctx.isModuleMember(id->name)) {
                    Trace::detail("markClosureIfEscaping: static struct field '",
                             ctx.pool.lookup(id->name), ".",
                             ctx.pool.lookup(field->fieldName),
                             "' is static - not marking as escaping");
                    return;
                }
            }

            Trace::detail("markClosureIfEscaping: field access '",
                     ctx.pool.lookup(field->fieldName),
                     "' may be a closure - conservative mark");
            return;
        }

        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            FuncDeclAST* funcDecl = resolveCalleeOrError(call->callee, ctx);
            if (funcDecl) {
                Trace::detail("markClosureIfEscaping: call to '",
                         ctx.pool.lookup(funcDecl->name),
                         "' returns a function - may be a closure");
            }
            return;
        }

        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            markClosureIfEscaping(bin->left, ctx);
            markClosureIfEscaping(bin->right, ctx);
            return;
        }

        case ASTKind::IfExpr: {
            const IfExprAST* ifExpr = expr->as<IfExprAST>();
            markClosureIfEscaping(ifExpr->thenBranch, ctx);
            markClosureIfEscaping(ifExpr->elseBranch, ctx);
            return;
        }

        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (ExprAST* elem : arr->elements) {
                markClosureIfEscaping(elem, ctx);
            }
            return;
        }

        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (FieldInitAST* init : st->inits) {
                markClosureIfEscaping(init->value, ctx);
            }
            return;
        }

        case ASTKind::PipelineExpr: {
            const PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
            markClosureIfEscaping(pipeline->seed, ctx);
            for (const PipelineStepAST* step : pipeline->steps) {
                markClosureIfEscaping(step->callable, ctx);
            }
            return;
        }

        case ASTKind::ComposeExpr: {
            const ComposeExprAST* compose = expr->as<ComposeExprAST>();
            if (compose->left) {
                markClosureIfEscaping(compose->left, ctx);
            }
            for (const ComposeOperandAST* operand : compose->operands) {
                if (operand->callable) {
                    markClosureIfEscaping(operand->callable, ctx);
                }
            }
            return;
        }

        default:
            return;
    }
}

} // namespace sema
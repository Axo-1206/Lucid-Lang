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
/// - Values classified as `ResourceKind::Refcounted` are treated as
///   closure fat pointers. `emitRelease` / `emitRetain` check the LLVM
///   value's shape statically (`isClosureShaped`), not the value's
///   runtime closure-ness.
/// - The conservative `isClosureValue = true` case (parameters, fields
///   without a statically-known shape) classifies as `ResourceKind::None`,
///   so no release or retain is emitted at the declaration. Correctness
///   for values that escape through these slots is the escape sites'
///   responsibility — see Rule 3 in CodeGenOwnership.hpp.
/// - The runtime does not perform a closure-shape probe here; the value's
///   static shape is used instead. Ownership and call dispatch are both
///   driven by the resolved `FuncTypeAST::shape`, not by runtime detection.
///
/// # Design: Capture Identity
///
/// Captured variables are identified by the `ValueDeclAST*` they resolve
/// to, in the specialized context. Sema runs capture analysis *after*
/// substitution (see Generic.cpp's substituteExpr and SemaExpr.cpp's
/// resolveAnonFuncExpr), so the declaration pointer Sema resolves is a
/// node in the specialized tree — the same node CodeGen will see. The
/// pointer never goes stale because there is no template-vs-specialization
/// distinction left at capture-analysis time.
///
/// `name` is kept alongside the pointer for diagnostics and for LLVM IR
/// labels, not as part of the identity.

#include "CaptureAnalysis.hpp"
#include "../types/SemaType.hpp"
#include "core/ASTStrings.hpp"
#include "core/trace/Trace.hpp"
#include "core/ast/TypeAST.hpp"

#include <unordered_map>
#include <functional>

namespace sema {

// ─── isClosureValue Implementation ──────────────────────────────────────────

/// NOTE:
/// resolvedType must already be set when isClosureValue is called. 
/// In CaptureAnalysis.cpp, isClosureValue is called from validateAndAddCapture 
/// (on fieldDecl->defaultVal), and from the old processIdentifier path 
/// (which is going away — see below). In the new shape, isClosureValue is called 
/// on a captured variable's declaration's default value, and the default 
/// value has already been resolved (that's how we know its type). 
/// So resolvedType is populated. If a future caller invokes it before resolution,
/// the answer is wrong. Add an assert if you want to catch that.
///
/// The anon case is a slight redundancy. If resolvedType is set on the anon, 
/// resolvedType->as<FuncTypeAST>()->isCls() and hasClosure should agree — 
/// that's what validateFuncShapeAgainstBody enforces. But checking hasClosure 
/// first is defensive: it works even if the anon's resolvedType is somehow 
/// not yet set, and it's a direct read of the field capture analysis actually 
/// populated. I'd keep both checks in that order.
bool isClosureValue(ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    // ─── Read the shape from the expression's resolved type ────────────
    //
    // Under the `fn`/`cls` design, every function-typed expression has a
    // statically-known shape. The shape is either `fn` (bare pointer, no
    // environment, never a closure value) or `cls` (fat pointer, may or
    // may not have a non-null environment).
    //
    // The function's name is a slight misnomer under the new model: it
    // answers "does this expression have the *shape* of a closure value?"
    // which is `cls`, not "does this expression's value definitely have
    // a non-null environment?" — the latter depends on the value, not
    // the type. For a `cls`-typed slot, the value may be a null-env
    // coercion of a bare function; the *slot's* shape is still `cls`,
    // and any code that handles it must handle the fat pointer.
    //
    // The conservative logic that used to live here — "a ParamAST might
    // receive a bare function or a closure, we can't tell, assume yes" —
    // is gone. The type tells us. A `cls`-typed param is a closure
    // *value slot*; a `fn`-typed param is not.
    if (expr->resolvedType && expr->resolvedType->isa<FuncTypeAST>()) {
        return expr->resolvedType->as<FuncTypeAST>()->isCls();
    }

    // ─── AnonFuncExprAST: hasClosure is the authoritative answer ───────
    //
    // For a closure literal, the shape is determined by whether it
    // captured anything, not by the declared shape of the slot it's
    // being assigned to. A capturing anon is `cls`-shaped; a
    // non-capturing anon is `fn`-shaped (and can coerce to `cls`).
    //
    // This matters because an anon may be resolved *before* its target
    // slot is known — e.g. an anon passed as an argument, where
    // `resolvedType` is set from the anon's own `funcType` (which was
    // set by the parser from the declared shape, and by capture
    // analysis from the body). So `resolvedType` and `hasClosure` agree
    // here by construction, but `hasClosure` is the more direct answer.
    if (expr->isa<AnonFuncExprAST>()) {
        return expr->as<AnonFuncExprAST>()->hasClosure;
    }

    // Everything else: not a function value.
    return false;
}

// ─── Internal CaptureAnalyzer ──────────────────────────────────────────────

namespace {

/// @brief Internal state for capture analysis.
///
/// This analyzer walks the AST of an AnonFuncExprAST body and detects
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
///
/// 4. **Declaration pointer**: Each captured variable is recorded with
///    the `ValueDeclAST*` it resolved to, in the specialized context.
///
/// # Only one node kind is analyzed
///
/// Under the new design, a FuncDeclAST is never itself a closure — the
/// closure lives on the AnonFuncExprAST stored in the function's `init`.
/// So this analyzer only ever operates on AnonFuncExprAST. There is no
/// FuncDeclAST overload.
struct CaptureAnalyzer {
    SemaContext& ctx;

    /// The closure being analyzed.
    AnonFuncExprAST* closure = nullptr;

    /// Current closure depth (from ContextStack). Used for tracing.
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

    // ─── Constructor ───────────────────────────────────────────────────────

    CaptureAnalyzer(SemaContext& c, AnonFuncExprAST* e)
        : ctx(c)
        , closure(e)
        , currentClosureDepth(ctx.getClosureDepth()) {
        localScopes.emplace_back();   // top-level frame for the closure's own body
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
    /// @param decl         The captured variable's declaration.
    /// @param diagLoc      AST node to anchor diagnostics on.
    void validateAndAddCapture(ValueDeclAST* decl, BaseAST* diagLoc) {
        if (!decl) return;
        InternedString name = decl->name;

        // Skip if already seen
        if (seenCaptures.find(name) != seenCaptures.end()) {
            return;
        }

        // ─── Validate capture rules ─────────────────────────────────────────
        TypeAST* varType = decl->type;

        // ─── Rule: Arena cannot be captured by value ──────────────────────
        // Arena is Owned, scope-confined - it has no copy operation and
        // cannot cross function boundaries by value.
        if (varType && isArenaType(varType)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, diagLoc,
                                  "closure cannot capture Arena by value");
            ctx.diagnostics.note(diagLoc,
                                 "Arena is scope-confined and cannot be captured by closures. "
                                 "Use &Arena to reference an arena from a closure.");
            return;
        }

        // Borrowed types (&T, [_]T) cannot be captured
        if (varType && isBorrowedType(varType)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, diagLoc,
                                  "closure cannot capture borrowed type '",
                                  ctx.pool.lookup(name),
                                  "' (", typeToString(varType, ctx.pool),
                                  ") — closures cannot capture &T or [_]T");
            return;
        }

        // Linear types (Future<T>, Thread<T>) cannot be captured
        if (varType && (varType->isa<FutureTypeAST>() || varType->isa<ThreadTypeAST>())) {
            const char* typeName = varType->isa<FutureTypeAST>() ? "Future<T>" : "Thread<T>";
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, diagLoc,
                                  "closure cannot capture linear type '",
                                  ctx.pool.lookup(name),
                                  "' (", typeName, ") — linear values can only be consumed once");
            return;
        }

        // ─── Determine if this captured value is itself a closure ──────────
        //
        // Under the new design, the answer for any function-typed declaration
        // is a direct read of `decl->type`. A `fn`-typed binding is a bare
        // pointer and never a closure value. A `cls`-typed binding is a fat
        // pointer and always a closure value, whether its environment is
        // null or not.

        /// NOTE:
        /// VarDeclAST - cannot hold function values in Lucid.
        /// EnumVariantAST - constants, not functions.
        /// All other declaration types are not function values.
        bool isClosureVal = false;
        if (decl->type && decl->type->isa<FuncTypeAST>()) {
            isClosureVal = decl->type->as<FuncTypeAST>()->isCls();
        }

        // ─── Determine capture by reference vs by value ────────────────────
        // Lucid grammar: read‑only captures may be snapshot‑copied (by‑value).
        // If the variable is mutated anywhere in the closure body, it must
        // be captured by reference to reflect those changes.
        bool mutated = (mutatedVariables.find(name) != mutatedVariables.end());

        // ─── Create the capture entry ──────────────────────────────────────
        CapturedVariable capture;
        capture.resolvedDecl = decl;
        capture.name = name;
        capture.byReference = mutated;
        capture.isClosureValue = isClosureVal;
        capture.index = captures.size();

        captures.push_back(capture);
        seenCaptures.insert(name);

        Trace::info("CaptureAnalysis: captured '", ctx.pool.lookup(name),
                "' by ", mutated ? "reference" : "value",
                " (closure value: ", isClosureVal ? "yes (conservative)" : "no",
                ") at closure depth ", currentClosureDepth);
    }

    // ─── Propagate Capture ────────────────────────────────────────────────────

    /// @brief Pull a capture from a nested closure up into our own capture list.
    ///
    /// Why this is needed: If a variable is referenced only inside a nested
    /// closure's own nested closure, and the immediately-enclosing closure
    /// never captures it itself, CodeGen would end up reusing a stale value
    /// from a different function. Propagating the capture upward closes this gap.
    ///
    /// The child resolved its capture against a scope nested inside ours,
    /// so its `resolvedDecl` is valid here too. No depth arithmetic; just
    /// forward the declaration.
    ///
    /// @param childCapture The capture from the nested closure.
    /// @param diagLoc AST node to anchor diagnostics on.
    void propagateCapture(const CapturedVariable& childCapture, BaseAST* diagLoc) {
        InternedString name = childCapture.name;

        // Already ours - own param or locally declared
        if (isOwnParam(name) || isLocallyDeclared(name)) return;

        // Module members and generic params are never captured/propagated
        if (ctx.isModuleMember(name)) return;
        if (ctx.isGenericParam(name)) return;

        // The child resolved this capture against a scope nested inside
        // ours, so its `resolvedDecl` is valid here too.
        ValueDeclAST* decl = childCapture.resolvedDecl;
        if (!decl) return;

        validateAndAddCapture(decl, diagLoc);
    }

    // ─── Process Identifier ──────────────────────────────────────────────────

    void processIdentifier(IdentifierExprAST* id) {
        if (!id) return;
        InternedString name = id->name;

        // Skip '_' (discard placeholder)
        if (ctx.pool.lookupView(name) == "_") return;

        // Skip if it's our own parameter
        if (isOwnParam(name)) return;

        // Check if this is a capture from an outer scope
        if (!isCapture(name)) return;

        // Skip if already seen
        if (seenCaptures.find(name) != seenCaptures.end()) return;

        ValueDeclAST* decl = getDeclaration(name);
        if (!decl) return;

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

    /// @brief Store the captured variables on the closure.
    void storeCaptures() {
        if (captures.empty()) {
            return;
        }

        // Build the ArenaSpan
        auto builder = ctx.arena.makeBuilder<CapturedVariable>();
        for (const auto& capture : captures) {
            builder.push_back(capture);
        }

        closure->captures = builder.build();
        closure->hasClosure = true;

        Trace::detail("analyzeCaptures: anonymous closure captures ",
                 captures.size(), " variables");
    }
};

} // anonymous namespace

// ─── analyzeCaptures (AnonFuncExprAST) ──────────────────────────────────────

void analyzeCaptures(AnonFuncExprAST* expr, SemaContext& ctx) {
    if (!expr || !expr->body) {
        return;
    }

    Trace::detail("analyzeCaptures: closure at depth ", ctx.getClosureDepth());

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

    // ─── Step 3: Store the captures on the closure ─────────────────────────
    analyzer.storeCaptures();

    if (!expr->hasClosure) {
        Trace::detail("analyzeCaptures: no captures detected for anonymous closure");
    }
}

} // namespace sema
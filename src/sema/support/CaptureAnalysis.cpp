/// @file CaptureAnalysis.cpp
/// @brief Implementation of closure capture and escape analysis.

#include "CaptureAnalysis.hpp"
#include "../types/SemaResolve.hpp"
#include "../types/SemaCompare.hpp"
#include "core/trace/Trace.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"

#include <unordered_map>
#include <functional>

namespace sema {

// ─── Forward declarations for internal helpers ──────────────────────────────

namespace {

/// @brief Internal state for capture analysis.
/// 
/// This analyzer walks the AST of a function/closure body and detects
/// which variables from outer scopes are captured.
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
    /// stack of block-scoped frames, pushed/popped in step with `BlockStmt`
    /// entry/exit, mirroring how the first pass's own `SymbolScope` guards
    /// work. This exists because capture analysis is a *second*, separate
    /// walk that runs after the body has already been fully resolved and
    /// every scope the first pass pushed for it has already been popped —
    /// `ctx.scopes` no longer reflects this body's own internal block
    /// structure by the time this walk runs. Without this, a name declared
    /// inside this body that happens to share a name with a genuinely outer
    /// variable would incorrectly resolve to the outer one via
    /// `ctx.lookupValue`, since nothing else records that the name was
    /// ever local. See `isLocallyDeclared`.
    std::vector<std::unordered_set<InternedString>> localScopes;
    
    /// Variables that have been marked as captures.
    std::vector<CapturedVariable> captures;
    
    /// Variables that have been seen to avoid duplicates.
    std::unordered_set<InternedString> seenCaptures;
    
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
    
    /// @brief Push a new local-scope frame — call on entry to any nested
    /// block belonging to the body currently being walked (BlockStmt, a
    /// for-loop's own index/value binder scope).
    void pushLocalScope() {
        localScopes.emplace_back();
    }
    
    /// @brief Pop the innermost local-scope frame — call on exit from the
    /// block `pushLocalScope` was entered for.
    void popLocalScope() {
        if (!localScopes.empty()) localScopes.pop_back();
    }
    
    /// @brief Register `name` as declared in the innermost currently-open
    /// local scope. Call this the moment a `let`/`const`/for-loop binder is
    /// walked, in the same textual order the first pass would have seen it,
    /// so declare-before-use ordering is preserved: a read of `name` before
    /// its own declaration is walked will not yet find it here, and falls
    /// through to `ctx.lookupValue` as a genuine outer reference — same
    /// as the first pass would have resolved it.
    void declareLocal(InternedString name) {
        if (!localScopes.empty()) localScopes.back().insert(name);
    }
    
    /// @brief Check if `name` was declared anywhere within the body
    /// currently being walked (any still-open local-scope frame, innermost
    /// to outermost) — as opposed to a genuinely outer function/closure.
    bool isLocallyDeclared(InternedString name) const {
        for (auto it = localScopes.rbegin(); it != localScopes.rend(); ++it) {
            if (it->find(name) != it->end()) return true;
        }
        return false;
    }
    
    /// @brief Check if a name is from an outer scope (i.e., a capture).
    /// 
    /// This is the core capture detection logic:
    /// 1. If it's a module member → not a capture (global)
    /// 2. If it's declared inside the body being walked (own params, or any
    ///    still-tracked local-scope frame) → not a capture (local)
    /// 3. If it's a generic parameter → not a capture
    /// 4. If it exists in any outer scope → it's a capture
    bool isCapture(InternedString name) const {
        // Module members are global - not captures
        if (ctx.isModuleMember(name)) {
            return false;
        }
        
        // Declared inside this body (own params, or a let/const/for-binder
        // tracked via localScopes) — not a capture. Deliberately NOT using
        // ctx.isInCurrentScope here: that checks only the single innermost
        // frame of ctx.scopes, which by the time this second pass runs no
        // longer reflects this body's own internal block structure at all
        // (the first pass already popped it) — see localScopes, above.
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
        
        // Reaching here means: not a module member, not declared anywhere
        // in this body (own params or localScopes), not a generic param,
        // and found in some still-open outer scope — genuinely a capture.
        return true;
    }
    
    ValueDeclAST* getDeclaration(InternedString name) const {
        return ctx.lookupValue(name);
    }
    
    bool shouldCaptureByReference(ValueDeclAST* decl, IdentifierExprAST* id) const {
        if (!decl) return false;
        // Conservative: capture all variables by reference
        // TODO: Optimize to capture by value when possible (read-only, small types)
        return true;
    }
    
    // ─── Validate + Add Capture ──────────────────────────────────────────────
    
    /// @brief Validate capture rules for `decl` and, if they pass, add it to
    /// this closure's/function's capture list (skipping if already present).
    ///
    /// Shared by processIdentifier() (a direct identifier reference in this
    /// body) and propagateCapture() (a capture pulled up from a nested
    /// closure that needs it but doesn't get it from its own immediate
    /// scope - see propagateCapture() below).
    ///
    /// @param decl The declaration being captured.
    /// @param diagLoc AST node to anchor a diagnostic on if capture rules
    ///        are violated - the identifier itself for a direct reference,
    ///        or the nested closure this capture is being propagated
    ///        through for a transitive one.
    void validateAndAddCapture(ValueDeclAST* decl, BaseAST* diagLoc) {
        if (!decl) return;
        InternedString name = decl->name;

        // Skip if already seen (direct reference already captured it, or
        // this exact variable was already propagated from another nested
        // closure).
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
                                  "' (", debug::typeToString(varType, ctx.pool),
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

        // ─── Create the capture entry ──────────────────────────────────────
        CapturedVariable capture;
        capture.decl = decl;
        capture.byReference = shouldCaptureByReference(decl, nullptr);
        capture.index = captures.size();

        captures.push_back(capture);
        seenCaptures.insert(name);

        Trace::info("CaptureAnalysis: captured '", ctx.pool.lookup(name),
                 "' by ", capture.byReference ? "reference" : "value",
                 " at depth ", currentClosureDepth);
    }

    /// @brief Pull a capture that a nested (grand)closure needs up into our
    /// own capture list, if we don't already provide it locally ourselves.
    ///
    /// Why this is needed: ctx.values (CodeGen's decl -> llvm::Value* map,
    /// see CodeGenContext) is flat and unscoped, not a stack. A closure's
    /// own capture-reload loop (CodeGenClosure.cpp's emitClosureBody) only
    /// re-stores the entries IT captured. If a variable is referenced only
    /// inside a nested closure's own nested closure, and the
    /// immediately-enclosing closure never captures it itself, CodeGen ends
    /// up reusing whatever stale value - typically an alloca belonging to a
    /// completely different llvm::Function - is still sitting in ctx.values
    /// when it later builds the innermost closure's environment while
    /// emitting ours. That's invalid IR (a value from one function used in
    /// another).
    ///
    /// Propagating the capture upward one level at a time - this runs once
    /// per enclosing closure, each time its own walkExpr encounters a
    /// nested AnonFuncExpr - closes that gap for any nesting depth.
    ///
    /// NOTE: assumes `childCapture` comes from an already-fully-analyzed
    /// nested closure. This holds under the current resolution order:
    /// nested closures are resolved (and thus capture-analyzed, via
    /// resolveAnonFuncExpr) inside-out, strictly before the enclosing
    /// body's own analyzeCaptures() call runs - see the AnonFuncExpr case
    /// in walkExpr(). If that ordering ever changes, this would need to
    /// explicitly trigger analysis of `nested` first instead of assuming
    /// its captures are already populated.
    void propagateCapture(const CapturedVariable& childCapture, BaseAST* diagLoc) {
        if (!childCapture.decl) return;
        InternedString name = childCapture.decl->name;

        // Already ours - own param, something declared in our own body
        // (own params / localScopes) - nothing to propagate.
        if (isOwnParam(name) || isLocallyDeclared(name)) return;

        // Module members and generic params are never captured/propagated -
        // they're resolved the same way regardless of nesting depth.
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
    
    // ─── AST Walking ──────────────────────────────────────────────────────────
    
    void walkExpr(ExprAST* expr) {
        if (!expr) return;
        
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
                const UnaryExprAST* unary = expr->as<UnaryExprAST>();
                walkExpr(unary->operand);
                break;
            }
            
            case ASTKind::CallExpr: {
                const CallExprAST* call = expr->as<CallExprAST>();
                walkExpr(call->callee);
                for (ExprAST* arg : call->args) {
                    walkExpr(arg);
                }
                break;
            }
            
            case ASTKind::FieldAccessExpr: {
                const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
                walkExpr(field->object);
                break;
            }
            
            case ASTKind::IndexExpr: {
                const IndexExprAST* index = expr->as<IndexExprAST>();
                walkExpr(index->target);
                walkExpr(index->index);
                break;
            }
            
            case ASTKind::SliceExpr: {
                const SliceExprAST* slice = expr->as<SliceExprAST>();
                walkExpr(slice->target);
                if (slice->start) walkExpr(slice->start);
                if (slice->end) walkExpr(slice->end);
                break;
            }
            
            case ASTKind::ArrayLiteralExpr: {
                const ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
                for (ExprAST* elem : arr->elements) {
                    walkExpr(elem);
                }
                break;
            }
            
            case ASTKind::StructLiteralExpr: {
                const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
                for (FieldInitAST* init : st->inits) {
                    walkExpr(init->value);
                }
                break;
            }
            
            case ASTKind::NullCoalesceExpr: {
                const NullCoalesceExprAST* nc = expr->as<NullCoalesceExprAST>();
                walkExpr(nc->value);
                walkExpr(nc->fallback);
                break;
            }
            
            case ASTKind::AssignExpr: {
                const AssignExprAST* assign = expr->as<AssignExprAST>();
                walkExpr(assign->lhs);
                walkExpr(assign->rhs);
                break;
            }
            
            case ASTKind::PipelineExpr: {
                const PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
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
                const ComposeExprAST* compose = expr->as<ComposeExprAST>();
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
                // Nested closure: we don't re-walk its body here - its own
                // captures were already computed independently, by its own
                // analyzeCaptures() call triggered when IT was resolved.
                // Resolution is inside-out, so by the time OUR walk reaches
                // this node, the nested closure has already been fully
                // resolved AND capture-analyzed (see propagateCapture()'s
                // doc comment for the ordering this relies on).
                //
                // What we DO need to do: propagate up anything the nested
                // closure captured that we don't already provide it
                // locally ourselves. Without this, a variable used only by
                // a nested closure's OWN nested closure would never make
                // it into this (intermediate) closure's capture list -
                // see "transitive capture propagation" in
                // CaptureAnalysis.hpp / propagateCapture() below.
                AnonFuncExprAST* nested = expr->as<AnonFuncExprAST>();
                if (nested) {
                    for (const CapturedVariable& childCapture : nested->captures) {
                        propagateCapture(childCapture, nested);
                    }
                }
                break;
            }
            
            case ASTKind::FuncRefStmt:
                // Function references are not captures - they're just names
                break;
            
            case ASTKind::IfExpr: {
                const IfExprAST* ifExpr = expr->as<IfExprAST>();
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
                // Intrinsics take real value arguments (e.g. #atomic_store(ptr, val),
                // #memcpy(dst, src, len)) that can reference outer variables just
                // like a normal call's arguments do - unlike a literal, this is
                // NOT a leaf node. Previously grouped with LiteralExpr as a no-op,
                // which meant a variable referenced only inside an intrinsic call
                // was never detected as a capture.
                const IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
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
                // Declarations inside the closure body don't create captures
                // But their initializers might reference outer variables
                if (declStmt->decl && declStmt->decl->isa<VarDeclAST>()) {
                    VarDeclAST* var = declStmt->decl->as<VarDeclAST>();
                    if (var->init) {
                        walkExpr(var->init);
                    }
                    // Register AFTER walking the initializer, so a use of
                    // the same name inside the initializer itself (e.g.
                    // shadowing an outer `x` with `let x = x + 1;`) still
                    // correctly resolves the right-hand `x` to the outer
                    // one, matching declare-before-use ordering.
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
                // The index/value binders are scoped to the loop body, not
                // to the loop statement's own enclosing block — push a
                // frame that encloses body's own BlockStmt frame so they're
                // visible inside it without leaking past the loop.
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
                // Same reasoning as DeclStmt, above: 'binding' is a fresh
                // local this statement introduces, registered after
                // walking 'call' (declare-before-use — 'result' isn't in
                // scope while evaluating its own initializing call).
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
                // 'binding' is nullptr for the '_' discard pattern — nothing
                // to register in that case.
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
    // We use the context stack directly - no stored closureDepth needed.
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
    if (func->type) {
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
        // ─── Case 1: Direct anonymous function ────────────────────────────
        // `return (n int) -> int { ... };`
        case ASTKind::AnonFuncExpr: {
            AnonFuncExprAST* closure = expr->as<AnonFuncExprAST>();
            closure->isReturned = true;
            Trace::detail("markClosureIfEscaping: direct anonymous function returned");
            return;
        }

        // ─── Case 2: Identifier expression ─────────────────────────────────
        // `return myFunc;` where myFunc is a function declaration
        case ASTKind::IdentifierExpr: {
            IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            ValueDeclAST* decl = ctx.lookupValue(id->name);
            
            if (!decl) return;
            
            // ─── 2a. Module member (static) ──────────────────────────────
            // Module members live for the entire program - no heap allocation needed.
            if (ctx.isModuleMember(id->name)) {
                Trace::detail("markClosureIfEscaping: '", ctx.pool.lookup(id->name),
                         "' is a module member (static) - not marking as escaping");
                return;
            }
            
            // ─── 2b. Function declaration (nested function) ────────────────
            if (decl->isa<FuncDeclAST>()) {
                FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
                
                // ─── Check if this is a nested function ──────────────────────
                // We need to know if the function is nested. We can check by
                // looking at the current context depth when the function was
                // declared, but we don't store that.
                // 
                // Instead, we can check if the function is a module member.
                // If it's NOT a module member, it's a nested function.
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

        // ─── Case 3: Module access ─────────────────────────────────────────
        // `return module:myFunc;` - static member, no escaping needed.
        case ASTKind::ModuleAccessExpr: {
            const ModuleAccessExprAST* access = expr->as<ModuleAccessExprAST>();
            Trace::detail("markClosureIfEscaping: module member '",
                     ctx.pool.lookup(access->moduleName), ":",
                     ctx.pool.lookup(access->memberName),
                     "' is static - not marking as escaping");
            return;
        }

        // ─── Case 4: Field access ──────────────────────────────────────────
        // `return obj.funcField;` - depends on whether `obj` is local or static.
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

        // ─── Case 5: Call expression returning a function ───────────────────
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

        // ─── Case 6: Binary expression ─────────────────────────────────────
        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            markClosureIfEscaping(bin->left, ctx);
            markClosureIfEscaping(bin->right, ctx);
            return;
        }

        // ─── Case 7: If expression ─────────────────────────────────────────
        case ASTKind::IfExpr: {
            const IfExprAST* ifExpr = expr->as<IfExprAST>();
            markClosureIfEscaping(ifExpr->thenBranch, ctx);
            markClosureIfEscaping(ifExpr->elseBranch, ctx);
            return;
        }

        // ─── Case 8: Array literal containing functions ─────────────────────
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (ExprAST* elem : arr->elements) {
                markClosureIfEscaping(elem, ctx);
            }
            return;
        }

        // ─── Case 9: Struct literal containing functions ────────────────────
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (FieldInitAST* init : st->inits) {
                markClosureIfEscaping(init->value, ctx);
            }
            return;
        }

        // ─── Case 10: Pipeline expression ──────────────────────────────────
        case ASTKind::PipelineExpr: {
            const PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
            markClosureIfEscaping(pipeline->seed, ctx);
            for (const PipelineStepAST* step : pipeline->steps) {
                markClosureIfEscaping(step->callable, ctx);
            }
            return;
        }

        // ─── Case 11: Compose expression ───────────────────────────────────
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

        // ─── Cases that cannot be closures ──────────────────────────────────
        case ASTKind::LiteralExpr:
        case ASTKind::PrimitiveType:
        case ASTKind::NamedType:
        case ASTKind::ArrayType:
        case ASTKind::NullableType:
        case ASTKind::FallibleType:
        case ASTKind::CombinedType:
        case ASTKind::RefType:
        case ASTKind::PtrType:
            return;

        default:
            return;
    }
}

} // namespace sema
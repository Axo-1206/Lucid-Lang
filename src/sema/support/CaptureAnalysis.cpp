/// @file CaptureAnalysis.cpp
/// @brief Implementation of closure capture analysis.

#include "CaptureAnalysis.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/types/SemaResolve.hpp"

#include <unordered_map>
#include <functional>

namespace sema {

// ─── Forward declarations for internal helpers ──────────────────────────────

namespace {

/// @brief Internal state for capture analysis.
struct CaptureAnalyzer {
    SemaContext& ctx;
    AnonFuncExprAST* closure = nullptr;
    
    /// Variables declared in the closure's own parameter list.
    /// These are NOT captures.
    std::unordered_set<InternedString> ownParams;
    
    /// Variables that have been marked as captures.
    std::vector<CapturedVariable> captures;
    
    /// Variables that have been seen to avoid duplicates.
    std::unordered_set<InternedString> seenCaptures;
    
    /// The depth of the closure (used to determine if a variable is from an outer scope).
    size_t closureDepth = 0;
    
    CaptureAnalyzer(SemaContext& c, AnonFuncExprAST* e)
        : ctx(c), closure(e) {}
    
    /// @brief Check if a name is a parameter of this closure.
    bool isOwnParam(InternedString name) const {
        return ownParams.find(name) != ownParams.end();
    }
    
    /// @brief Check if a name is from an outer scope.
    bool isFromOuterScope(InternedString name) const {
        // If it's a module member, it's global - not a capture.
        if (ctx.isModuleMember(name)) {
            return false;
        }
        
        // If it's in the current scope (which is the closure's own scope),
        // it's either a parameter or a local variable declared inside the closure.
        // Neither is a capture.
        if (ctx.isInCurrentScope(name)) {
            return false;
        }
        
        // If it's a generic parameter, it's not a capture.
        if (ctx.isGenericParam(name)) {
            return false;
        }
        
        // Check if the name exists in any outer scope.
        // If ctx.lookupValue returns a declaration and it's NOT in the current scope,
        // then it's a capture.
        const ValueDeclAST* decl = ctx.lookupValueRaw(name);
        if (!decl) {
            return false;
        }
        
        // If the declaration is in the current scope, it's not a capture.
        if (ctx.isInCurrentScope(name)) {
            return false;
        }
        
        return true;
    }
    
    /// @brief Get the declaration for a name.
    const ValueDeclAST* getDeclaration(InternedString name) const {
        return ctx.lookupValueRaw(name);
    }
    
    /// @brief Determine if a variable should be captured by reference or by value.
    bool shouldCaptureByReference(const ValueDeclAST* decl, const IdentifierExprAST* id) const {
        if (!decl) return false;
        
        // If the variable is mutated in the closure body, capture by reference.
        // For now, we conservatively capture by reference if:
        // 1. The variable is mutable (let) AND
        // 2. The identifier is used as an l-value (assigned to) OR
        // 3. The variable is of a type that must be shared (e.g., large structs)
        //
        // In the current implementation, we conservatively capture all variables
        // by reference to be safe. This can be optimized later.
        return true;
    }
    
    /// @brief Process an identifier expression.
    void processIdentifier(const IdentifierExprAST* id) {
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
        
        // Check if this is from an outer scope
        if (!isFromOuterScope(name)) {
            return;
        }
        
        // Skip if already seen
        if (seenCaptures.find(name) != seenCaptures.end()) {
            return;
        }
        
        // Get the declaration
        const ValueDeclAST* decl = getDeclaration(name);
        if (!decl) {
            return;
        }
        
        // ─── Validate capture rules ─────────────────────────────────────────
        const TypeAST* varType = decl->semanticType;
        
        // Rule 3: Borrowed types (&T, [_]T) cannot be captured
        if (varType && isBorrowedType(varType)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, id,
                                  "closure cannot capture borrowed type '",
                                  ctx.pool.lookup(name),
                                  "' (", debug::typeToString(varType, ctx.pool),
                                  ") — closures cannot capture &T or [_]T");
            return;
        }
        
        // Rule 4: Linear types (Future<T>, Thread<T>) cannot be captured
        if (varType && (varType->isa<FutureTypeAST>() || varType->isa<ThreadTypeAST>())) {
            const char* typeName = varType->isa<FutureTypeAST>() ? "Future<T>" : "Thread<T>";
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, id,
                                  "closure cannot capture linear type '",
                                  ctx.pool.lookup(name),
                                  "' (", typeName, ") — linear values can only be consumed once");
            return;
        }
        
        // ─── Create the capture entry ──────────────────────────────────────
        CapturedVariable capture;
        capture.decl = decl;
        capture.byReference = shouldCaptureByReference(decl, id);
        capture.index = captures.size();
        
        captures.push_back(capture);
        seenCaptures.insert(name);
        
        LOG_SEMA("CaptureAnalysis: captured '", ctx.pool.lookup(name),
                 "' by ", capture.byReference ? "reference" : "value");
    }
    
    /// @brief Walk an expression tree to find identifier references.
    void walkExpr(const ExprAST* expr) {
        if (!expr) return;
        
        switch (expr->kind) {
            case ASTKind::IdentifierExpr:
                processIdentifier(expr->as<IdentifierExprAST>());
                break;
                
            case ASTKind::BinaryExpr: {
                const BinaryExprAST* bin = expr->as<BinaryExprAST>();
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
                for (const ExprAST* arg : call->args) {
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
                for (const ExprAST* elem : arr->elements) {
                    walkExpr(elem);
                }
                break;
            }
            
            case ASTKind::StructLiteralExpr: {
                const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
                for (const FieldInitAST* init : st->inits) {
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
                    for (const ExprAST* arg : step->packArgs) {
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
                // Nested closure: don't walk into it - captures are analyzed
                // when the nested closure itself is analyzed.
                // But we need to check if this nested closure captures anything
                // from the outer scope (which would be our closure's scope).
                // This is a separate analysis.
                break;
            }
            
            case ASTKind::IfExpr: {
                const IfExprAST* ifExpr = expr->as<IfExprAST>();
                walkExpr(ifExpr->condition);
                walkExpr(ifExpr->thenBranch);
                walkExpr(ifExpr->elseBranch);
                break;
            }
            
            case ASTKind::RangeExpr: {
                const RangeExprAST* range = expr->as<RangeExprAST>();
                walkExpr(range->lo);
                walkExpr(range->hi);
                break;
            }
            
            case ASTKind::ModuleAccessExpr: {
                // Module members are global - not captures
                // But the module name itself is not a capture
                // We only care about the value being accessed
                // If the module access returns a value, that value might be captured
                // But the module name itself is not a variable
                break;
            }
            
            // These don't contain identifiers to capture
            case ASTKind::LiteralExpr:
            case ASTKind::IntrinsicCallExpr:
                break;
                
            default:
                // Unknown expression type - skip
                break;
        }
    }
    
    /// @brief Walk a statement tree to find identifier references.
    void walkStmt(const StmtAST* stmt) {
        if (!stmt) return;
        
        switch (stmt->kind) {
            case ASTKind::BlockStmt: {
                const BlockStmtAST* block = stmt->as<BlockStmtAST>();
                for (const StmtAST* s : block->stmts) {
                    walkStmt(s);
                }
                break;
            }
            
            case ASTKind::ExprStmt: {
                const ExprStmtAST* exprStmt = stmt->as<ExprStmtAST>();
                walkExpr(exprStmt->expr);
                break;
            }
            
            case ASTKind::DeclStmt: {
                const DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
                // Declarations inside the closure body don't create captures
                // But their initializers might reference outer variables
                if (declStmt->decl && declStmt->decl->isa<VarDeclAST>()) {
                    const VarDeclAST* var = declStmt->decl->as<VarDeclAST>();
                    if (var->init) {
                        walkExpr(var->init);
                    }
                }
                break;
            }
            
            case ASTKind::IfStmt: {
                const IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
                walkExpr(ifStmt->condition);
                walkStmt(ifStmt->thenBranch);
                if (ifStmt->elseBranch) {
                    walkStmt(ifStmt->elseBranch);
                }
                break;
            }
            
            case ASTKind::SwitchStmt: {
                const SwitchStmtAST* switchStmt = stmt->as<SwitchStmtAST>();
                walkExpr(switchStmt->subject);
                for (const SwitchCaseAST* caseStmt : switchStmt->cases) {
                    for (const ExprAST* value : caseStmt->values) {
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
                const ForStmtAST* forStmt = stmt->as<ForStmtAST>();
                // Loop variables are not captures
                // But the iterable might reference outer variables
                walkExpr(forStmt->iterable);
                if (forStmt->step) {
                    walkExpr(forStmt->step);
                }
                if (forStmt->body) {
                    walkStmt(forStmt->body);
                }
                break;
            }
            
            case ASTKind::WhileStmt: {
                const WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
                walkExpr(whileStmt->condition);
                if (whileStmt->body) {
                    walkStmt(whileStmt->body);
                }
                break;
            }
            
            case ASTKind::DoWhileStmt: {
                const DoWhileStmtAST* doWhileStmt = stmt->as<DoWhileStmtAST>();
                if (doWhileStmt->body) {
                    walkStmt(doWhileStmt->body);
                }
                walkExpr(doWhileStmt->condition);
                break;
            }
            
            case ASTKind::ReturnStmt: {
                const ReturnStmtAST* returnStmt = stmt->as<ReturnStmtAST>();
                if (returnStmt->value) {
                    walkExpr(returnStmt->value);
                }
                break;
            }
            
            case ASTKind::AsyncStmt: {
                const AsyncStmtAST* asyncStmt = stmt->as<AsyncStmtAST>();
                // The binding is a new variable - not a capture
                // But the call expression might reference outer variables
                if (asyncStmt->call) {
                    walkExpr(asyncStmt->call);
                }
                break;
            }
            
            case ASTKind::SpawnStmt: {
                const SpawnStmtAST* spawnStmt = stmt->as<SpawnStmtAST>();
                if (spawnStmt->call) {
                    walkExpr(spawnStmt->call);
                }
                break;
            }
            
            case ASTKind::AwaitStmt: {
                const AwaitStmtAST* awaitStmt = stmt->as<AwaitStmtAST>();
                for (const ExprAST* target : awaitStmt->targets) {
                    walkExpr(target);
                }
                break;
            }
            
            case ASTKind::JoinStmt: {
                const JoinStmtAST* joinStmt = stmt->as<JoinStmtAST>();
                for (const ExprAST* target : joinStmt->targets) {
                    walkExpr(target);
                }
                break;
            }
            
            default:
                // Unknown statement type - skip
                break;
        }
    }
};

} // anonymous namespace

// ─── Public API ──────────────────────────────────────────────────────────────

void analyzeCaptures(AnonFuncExprAST* expr, SemaContext& ctx) {
    if (!expr || !expr->body) {
        return;
    }
    
    LOG_SEMA("analyzeCaptures: analyzing closure at ", expr->loc.toString());
    
    CaptureAnalyzer analyzer(ctx, expr);
    
    // ─── Step 1: Collect the closure's own parameters ──────────────────────
    // Parameters are NOT captures - they're freshly bound on each call.
    if (expr->funcType) {
        for (const FuncTypeAST* group = expr->funcType; group; group = group->getNext()) {
            for (const ParamAST* param : group->params) {
                analyzer.ownParams.insert(param->name);
            }
        }
    }
    
    // ─── Step 2: Walk the body to find captures ─────────────────────────────
    analyzer.walkStmt(expr->body);
    
    // ─── Step 3: Store the captures on the AST node ─────────────────────────
    if (!analyzer.captures.empty()) {
        auto builder = ctx.arena.makeBuilder<CapturedVariable>();
        for (const auto& capture : analyzer.captures) {
            builder.push_back(capture);
        }
        expr->captures = builder.build();
        expr->hasClosure = true;
        
        LOG_SEMA("analyzeCaptures: closure captures ", analyzer.captures.size(), " variables");
    } else {
        // No captures - this is a plain function value, not a closure
        expr->hasClosure = false;
        LOG_SEMA("analyzeCaptures: no captures detected");
    }
    
    // ─── Step 4: Mark if this closure is returned ──────────────────────────
    // This is determined by the caller (resolveReturnStmt) when it sees
    // a return statement that returns this closure.
    // We don't set it here - it's set in resolveReturnStmt.
}

} // namespace sema
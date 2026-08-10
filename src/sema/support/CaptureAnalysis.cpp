/// @file CaptureAnalysis.cpp
/// @brief Implementation of closure capture and escape analysis.

#include "CaptureAnalysis.hpp"
#include "../types/SemaResolve.hpp"
#include "debug/DebugUtils.hpp"

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
    
    const ValueDeclAST* getDeclaration(InternedString name) const {
        return ctx.lookupValueRaw(name);
    }
    
    bool shouldCaptureByReference(const ValueDeclAST* decl, const IdentifierExprAST* id) const {
        if (!decl) return false;
        return true;  // Conservative: capture all variables by reference
    }
    
    void processIdentifier(const IdentifierExprAST* id) {
        if (!id) return;
        
        InternedString name = id->name;
        
        // Skip '_' (discard placeholder)
        if (ctx.pool.lookupView(name) == "_") {
            return;
        }
        
        if (isOwnParam(name)) {
            return;
        }
        
        if (!isFromOuterScope(name)) {
            return;
        }
        
        if (seenCaptures.find(name) != seenCaptures.end()) {
            return;
        }
        
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
            
            case ASTKind::ModuleAccessExpr:
                // Module members are global - not captures
                break;
                
            case ASTKind::LiteralExpr:
            case ASTKind::IntrinsicCallExpr:
                break;
                
            default:
                break;
        }
    }
    
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
                break;
        }
    }
};

} // anonymous namespace

// ─── analyzeCaptures ─────────────────────────────────────────────────────────

void analyzeCaptures(AnonFuncExprAST* expr, SemaContext& ctx) {
    if (!expr || !expr->body) {
        return;
    }
    
    LOG_SEMA("analyzeCaptures: analyzing closure at ", expr->loc.toString());
    
    CaptureAnalyzer analyzer(ctx, expr);
    
    // ─── Step 1: Collect the closure's own parameters ──────────────────────
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
        expr->hasClosure = false;
        LOG_SEMA("analyzeCaptures: no captures detected");
    }
}

// ─── markClosureIfEscaping ──────────────────────────────────────────────────

void markClosureIfEscaping(const ExprAST* expr, SemaContext& ctx) {
    if (!expr) return;

    switch (expr->kind) {
        // ─── Case 1: Direct anonymous function ────────────────────────────
        // `return (n int) -> int { ... };`
        // This is valid in reassignment contexts or as return values
        case ASTKind::AnonFuncExpr: {
            AnonFuncExprAST* closure = const_cast<AnonFuncExprAST*>(
                expr->as<AnonFuncExprAST>()
            );
            closure->isReturned = true;
            LOG_SEMA("markClosureIfEscaping: direct anonymous function returned");
            return;
        }

        // ─── Case 2: Identifier expression ─────────────────────────────────
        // `return myFunc;` where myFunc is a function declaration
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            const ValueDeclAST* decl = ctx.lookupValue(id->name);
            
            if (!decl) return;
            
            // ─── 2a. Module member (static) ──────────────────────────────
            // Module members live for the entire program - no heap allocation needed.
            if (ctx.isModuleMember(id->name)) {
                LOG_SEMA("markClosureIfEscaping: '", ctx.pool.lookup(id->name),
                         "' is a module member (static) - not marking as escaping");
                return;
            }
            
            // ─── 2b. Function declaration (nested function) ────────────────
            // This is the common case: returning a nested function.
            if (decl->isa<FuncDeclAST>()) {
                // Nested functions that are returned are closures if they capture
                // variables from the enclosing scope. Mark the function as returned
                // so CodeGen knows it needs heap allocation.
                const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
                
                // Check if this function captures any variables (has a closure)
                // We can check this by looking at the function's body for
                // identifier references to outer scope variables.
                bool hasCaptures = false;
                // TODO: Check if funcDecl captures variables from outer scope.
                // For now, we conservatively assume nested functions are closures.
                // A more precise check would analyze the function body.
                
                // If the function is nested (not top-level), it may have captures
                // Check if the function is at module level
                if (!ctx.isModuleMember(id->name)) {
                    // Nested function - mark as potentially returning a closure
                    // We need to set isReturned on the FuncDeclAST
                    FuncDeclAST* mutableFunc = const_cast<FuncDeclAST*>(funcDecl);
                    mutableFunc->isForeignFunction = true; // TODO: Use proper flag
                    // Actually, FuncDeclAST doesn't have an isReturned flag.
                    // We should add one or use closureDepth > 0 to indicate nesting.
                    
                    // Since FuncDeclAST doesn't have isReturned, we use closureDepth:
                    // If closureDepth > 0, the function is nested and may be a closure.
                    if (mutableFunc->closureDepth > 0) {
                        LOG_SEMA("markClosureIfEscaping: nested function '",
                                 ctx.pool.lookup(id->name),
                                 "' returned - may be a closure");
                    }
                }
                return;
            }
            
            // ─── 2c. Variable (let/const) holding a function value ──────────
            if (decl->isa<VarDeclAST>()) {
                const VarDeclAST* varDecl = decl->as<VarDeclAST>();
                
                // Check if the variable's initializer is a function reference
                if (varDecl->init && varDecl->init->isa<IdentifierExprAST>()) {
                    const IdentifierExprAST* initId = varDecl->init->as<IdentifierExprAST>();
                    const ValueDeclAST* initDecl = ctx.lookupValue(initId->name);
                    
                    if (initDecl && initDecl->isa<FuncDeclAST>()) {
                        // The variable holds a reference to a function.
                        // If the function is nested, it's a closure.
                        const FuncDeclAST* funcDecl = initDecl->as<FuncDeclAST>();
                        if (funcDecl->closureDepth > 0) {
                            LOG_SEMA("markClosureIfEscaping: variable '",
                                     ctx.pool.lookup(id->name),
                                     "' holds nested function - may be a closure");
                        }
                    }
                }
                
                // If the variable's type is a function type, it might hold a closure
                if (varDecl->semanticType && varDecl->semanticType->isa<FuncTypeAST>()) {
                    // Conservative: treat as potentially a closure
                    LOG_SEMA("markClosureIfEscaping: function-typed variable '",
                             ctx.pool.lookup(id->name), "' returned - conservative mark");
                }
            }
            return;
        }

        // ─── Case 3: Module access ─────────────────────────────────────────
        // `return module:myFunc;` - static member, no escaping needed.
        case ASTKind::ModuleAccessExpr: {
            const ModuleAccessExprAST* access = expr->as<ModuleAccessExprAST>();
            LOG_SEMA("markClosureIfEscaping: module member '",
                     ctx.pool.lookup(access->moduleName), ":",
                     ctx.pool.lookup(access->memberName),
                     "' is static - not marking as escaping");
            return;
        }

        // ─── Case 4: Field access ──────────────────────────────────────────
        // `return obj.funcField;` - depends on whether `obj` is local or static.
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            
            // If the object is an identifier, check if it's a module member
            if (field->object && field->object->isa<IdentifierExprAST>()) {
                const IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
                
                // If the object is a module member (static), the field is static
                if (ctx.isModuleMember(id->name)) {
                    LOG_SEMA("markClosureIfEscaping: static struct field '",
                             ctx.pool.lookup(id->name), ".", 
                             ctx.pool.lookup(field->fieldName),
                             "' is static - not marking as escaping");
                    return;
                }
            }
            
            // Otherwise, the field is from a local object - it could be escaping.
            LOG_SEMA("markClosureIfEscaping: field access '",
                     ctx.pool.lookup(field->fieldName),
                     "' may be a closure - conservative mark");
            return;
        }

        // ─── Case 5: Call expression returning a function ───────────────────
        // `return makeFunc();` - depends on the callee.
        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            
            // Try to resolve the callee
            const FuncDeclAST* funcDecl = resolveCalleeOrError(call->callee, ctx);
            if (funcDecl) {
                LOG_SEMA("markClosureIfEscaping: call to '",
                         ctx.pool.lookup(funcDecl->name),
                         "' returns a function - may be a closure");
            }
            return;
        }

        // ─── Case 6: Binary expression (e.g., condition ? func1 : func2) ──
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
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
            for (const ExprAST* elem : arr->elements) {
                markClosureIfEscaping(elem, ctx);
            }
            return;
        }

        // ─── Case 9: Struct literal containing functions ────────────────────
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : st->inits) {
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
/// @file CodeGen.cpp
/// @brief Implementation of the main code generation orchestrator.

#include "CodeGen.hpp"
#include "debug/DebugUtils.hpp"
#include "core/memory/StringPool.hpp"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace codegen {

// =============================================================================
// Public API
// =============================================================================

std::vector<std::unique_ptr<llvm::Module>> generate(
    const std::vector<ModuleAST*>& modules,
    llvm::LLVMContext& context
) {
    std::vector<std::unique_ptr<llvm::Module>> result;
    result.reserve(modules.size());

    for (ModuleAST* module : modules) {
        if (!module) continue;

        CodeGenContext ctx(context);
        
        // ─── Create LLVM module ──────────────────────────────────────────
        std::string name = StringPool::instance().lookup(module->filePath);
        ctx.module = new llvm::Module(name, context);
        
        // ─── Generate IR for the module ────────────────────────────────
        auto irModule = generateModule(module, ctx);
        if (irModule) {
            result.push_back(std::move(irModule));
        }
    }

    return result;
}

std::unique_ptr<llvm::Module> generateModule(ModuleAST* module, CodeGenContext& ctx) {
    if (!module || !ctx.module) {
        return nullptr;
    }

    LOG_CODEGEN("Generating IR for module: ", 
                StringPool::instance().lookup(module->filePath));

    // ─── Phase 1: Lower all declarations ──────────────────────────────────
    // This creates LLVM types, function prototypes, globals, and structs.
    // Function bodies are NOT lowered yet.
    lowerModuleDeclarations(module, ctx);

    // ─── Phase 2: Lower all function bodies ──────────────────────────────
    // Now that all declarations exist, we can lower function bodies
    // with full symbol resolution (forward references work).
    lowerModuleBodies(module, ctx);

    // ─── Phase 3: Verify the module ──────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyModule(*ctx.module, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, module->loc,
                                "LLVM IR verification failed: ", error);
        return nullptr;
    }

    LOG_CODEGEN("Generated IR successfully for module");
    return std::unique_ptr<llvm::Module>(ctx.module);
}

// =============================================================================
// Module-Level Emission
// =============================================================================

void lowerModuleDeclarations(ModuleAST* module, CodeGenContext& ctx) {
    if (!module) return;

    for (const DeclPtr decl : module->decls) {
        if (!decl) continue;

        // ─── Lower all declarations except function bodies ──────────────
        // For functions, this creates the prototype but not the body.
        switch (decl->kind) {
            case ASTKind::ImportDecl:
                // Imports are handled by the module resolver, not CodeGen.
                break;

            case ASTKind::FuncDecl:
                lowerFunctionDecl(decl->as<FuncDeclAST>(), ctx);
                break;

            case ASTKind::StructDecl:
                lowerStructDecl(decl->as<StructDeclAST>(), ctx);
                break;

            case ASTKind::EnumDecl:
                lowerEnumDecl(decl->as<EnumDeclAST>(), ctx);
                break;

            case ASTKind::VarDecl:
                lowerVarDecl(decl->as<VarDeclAST>(), ctx);
                break;

            default:
                // Other declaration kinds don't need special handling
                // in the declaration phase.
                break;
        }
    }
}

void lowerModuleBodies(ModuleAST* module, CodeGenContext& ctx) {
    if (!module) return;

    for (const DeclPtr decl : module->decls) {
        if (!decl) continue;

        // ─── Lower function bodies ───────────────────────────────────────
        if (decl->isa<FuncDeclAST>()) {
            lowerFunctionBody(decl->as<FuncDeclAST>(), ctx);
        }
        // Note: Struct and enum bodies are already handled in the
        // declaration phase. Variables with initializers are handled
        // in lowerVarDecl.
    }
}

// =============================================================================
// Declaration Lowering (Stubs - Implemented in CodeGenDecl.cpp)
// =============================================================================

void lowerDeclaration(DeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl:
            // Imports are handled by the module resolver.
            break;
        case ASTKind::FuncDecl:
            lowerFunctionDecl(decl->as<FuncDeclAST>(), ctx);
            break;
        case ASTKind::StructDecl:
            lowerStructDecl(decl->as<StructDeclAST>(), ctx);
            break;
        case ASTKind::EnumDecl:
            lowerEnumDecl(decl->as<EnumDeclAST>(), ctx);
            break;
        case ASTKind::VarDecl:
            lowerVarDecl(decl->as<VarDeclAST>(), ctx);
            break;
        default:
            break;
    }
}

void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    // Implemented in CodeGenDecl.cpp
    // This creates the llvm::Function prototype.
}

void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    // Implemented in CodeGenDecl.cpp
    // This generates IR for the function body.
}

void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx) {
    // Implemented in CodeGenDecl.cpp
    // This creates allocas for local variables and globals for module vars.
}

void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    // Implemented in CodeGenDecl.cpp
    // This creates the LLVM struct type.
}

void lowerEnumDecl(EnumDeclAST* decl, CodeGenContext& ctx) {
    // Implemented in CodeGenDecl.cpp
    // This creates integer constants for enum variants.
}

void lowerParam(ParamAST* param, CodeGenContext& ctx) {
    // Implemented in CodeGenDecl.cpp
    // This registers the parameter in the symbol table.
}

// =============================================================================
// Statement Lowering (Stubs - Implemented in CodeGenStmt.cpp)
// =============================================================================

void lowerStatement(StmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            lowerBlockStmt(stmt->as<BlockStmtAST>(), ctx);
            break;
        case ASTKind::IfStmt:
            lowerIfStmt(stmt->as<IfStmtAST>(), ctx);
            break;
        case ASTKind::SwitchStmt:
            lowerSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
            break;
        case ASTKind::ForStmt:
            lowerForStmt(stmt->as<ForStmtAST>(), ctx);
            break;
        case ASTKind::WhileStmt:
            lowerWhileStmt(stmt->as<WhileStmtAST>(), ctx);
            break;
        case ASTKind::DoWhileStmt:
            lowerDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
            break;
        case ASTKind::ReturnStmt:
            lowerReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
            break;
        case ASTKind::BreakStmt:
            lowerBreakStmt(stmt->as<BreakStmtAST>(), ctx);
            break;
        case ASTKind::ContinueStmt:
            lowerContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
            break;
        case ASTKind::ExprStmt:
            lowerExprStmt(stmt->as<ExprStmtAST>(), ctx);
            break;
        case ASTKind::DeclStmt:
            lowerDeclStmt(stmt->as<DeclStmtAST>(), ctx);
            break;
        case ASTKind::FuncRefStmt:
            lowerFuncRefStmt(stmt->as<FuncRefStmtAST>(), ctx);
            break;
        case ASTKind::AsyncStmt:
            lowerAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
            break;
        case ASTKind::AwaitStmt:
            lowerAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
            break;
        case ASTKind::SpawnStmt:
            lowerSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
            break;
        case ASTKind::JoinStmt:
            lowerJoinStmt(stmt->as<JoinStmtAST>(), ctx);
            break;
        default:
            break;
    }
}

void lowerBlockStmt(BlockStmtAST* block, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerIfStmt(IfStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerSwitchStmt(SwitchStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerForStmt(ForStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerWhileStmt(WhileStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerDoWhileStmt(DoWhileStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerReturnStmt(ReturnStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerBreakStmt(BreakStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerContinueStmt(ContinueStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerExprStmt(ExprStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerDeclStmt(DeclStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerFuncRefStmt(FuncRefStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerAsyncStmt(AsyncStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerAwaitStmt(AwaitStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerSpawnStmt(SpawnStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

void lowerJoinStmt(JoinStmtAST* stmt, CodeGenContext& ctx) {
    // Implemented in CodeGenStmt.cpp
}

// =============================================================================
// Expression Lowering (Stubs - Implemented in CodeGenExpr.cpp)
// =============================================================================

llvm::Value* lowerExpression(ExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return lowerLiteralExpr(expr->as<LiteralExprAST>(), ctx);
        case ASTKind::IdentifierExpr:
            return lowerIdentifierExpr(expr->as<IdentifierExprAST>(), ctx);
        case ASTKind::ArrayLiteralExpr:
            return lowerArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx);
        case ASTKind::StructLiteralExpr:
            return lowerStructLiteralExpr(expr->as<StructLiteralExprAST>(), ctx);
        case ASTKind::BinaryExpr:
            return lowerBinaryExpr(expr->as<BinaryExprAST>(), ctx);
        case ASTKind::UnaryExpr:
            return lowerUnaryExpr(expr->as<UnaryExprAST>(), ctx);
        case ASTKind::CallExpr:
            return lowerCallExpr(expr->as<CallExprAST>(), ctx);
        case ASTKind::IntrinsicCallExpr:
            return lowerIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), ctx);
        case ASTKind::IndexExpr:
            return lowerIndexExpr(expr->as<IndexExprAST>(), ctx);
        case ASTKind::SliceExpr:
            return lowerSliceExpr(expr->as<SliceExprAST>(), ctx);
        case ASTKind::FieldAccessExpr:
            return lowerFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx);
        case ASTKind::ModuleAccessExpr:
            return lowerModuleAccessExpr(expr->as<ModuleAccessExprAST>(), ctx);
        case ASTKind::NullCoalesceExpr:
            return lowerNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), ctx);
        case ASTKind::AssignExpr:
            return lowerAssignExpr(expr->as<AssignExprAST>(), ctx);
        case ASTKind::PipelineExpr:
            return lowerPipelineExpr(expr->as<PipelineExprAST>(), ctx);
        case ASTKind::ComposeExpr:
            return lowerComposeExpr(expr->as<ComposeExprAST>(), ctx);
        case ASTKind::AnonFuncExpr:
            return lowerAnonFuncExpr(expr->as<AnonFuncExprAST>(), ctx);
        case ASTKind::IfExpr:
            return lowerIfExpr(expr->as<IfExprAST>(), ctx);
        case ASTKind::RangeExpr:
            return lowerRangeExpr(expr->as<RangeExprAST>(), ctx);
        default:
            return nullptr;
    }
}

llvm::Value* lowerLiteralExpr(LiteralExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerIdentifierExpr(IdentifierExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerArrayLiteralExpr(ArrayLiteralExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerStructLiteralExpr(StructLiteralExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerBinaryExpr(BinaryExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerUnaryExpr(UnaryExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerCallExpr(CallExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerIntrinsicCallExpr(IntrinsicCallExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerIndexExpr(IndexExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerSliceExpr(SliceExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerFieldAccessExpr(FieldAccessExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerModuleAccessExpr(ModuleAccessExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerNullCoalesceExpr(NullCoalesceExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerAssignExpr(AssignExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerPipelineExpr(PipelineExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerPipelineStep(PipelineStepAST* step, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerComposeExpr(ComposeExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerComposeOperand(ComposeOperandAST* operand, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerAnonFuncExpr(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    // For closures, this calls lowerClosure.
    return nullptr;
}

llvm::Value* lowerIfExpr(IfExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

llvm::Value* lowerRangeExpr(RangeExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenExpr.cpp
    return nullptr;
}

// =============================================================================
// Closure Lowering (Stubs - Implemented in CodeGenClosure.cpp)
// =============================================================================

llvm::Value* lowerClosure(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenClosure.cpp
    return nullptr;
}

llvm::StructType* buildClosureEnvironment(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenClosure.cpp
    return nullptr;
}

llvm::Function* createClosureFunction(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    // Implemented in CodeGenClosure.cpp
    return nullptr;
}

llvm::Value* emitClosureCall(
    llvm::Value* funcPtr,
    llvm::Value* envPtr,
    llvm::ArrayRef<llvm::Value*> args,
    CodeGenContext& ctx
) {
    // Implemented in CodeGenClosure.cpp
    return nullptr;
}

// =============================================================================
// Helpers (Stubs - Implemented in CodeGenHelpers.cpp)
// =============================================================================

llvm::Value* loadIfNeeded(llvm::Value* value, bool isLValue, CodeGenContext& ctx) {
    // Implemented in CodeGenHelpers.cpp
    return value;
}

llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
) {
    // Implemented in CodeGenHelpers.cpp
    return nullptr;
}

llvm::BasicBlock* createBlock(const std::string& name, CodeGenContext& ctx) {
    // Implemented in CodeGenHelpers.cpp
    return nullptr;
}

void emitPanic(const std::string& message, CodeGenContext& ctx) {
    // Implemented in CodeGenHelpers.cpp
}

} // namespace codegen
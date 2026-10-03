/// @file compile/EmitDecl.cpp
/// @brief Lower a declaration's non-body parts: parameter slots, the
///        function's opening line entry.
///
/// ─── Scope of this file ───────────────────────────────────────────────────
/// "Declaration" here means a function declaration. Tables and top-level
/// bindings are handled by the driver's baking helpers (bakeTable,
/// bakeTopLevelBinding in Compiler.cpp), not by emitDecl — they produce
/// StaticData entries, not code. emitDecl only runs for a function whose
/// body will be emitted.
///
/// ─── What emitDecl does ───────────────────────────────────────────────────
/// The prologue: allocate a frame slot for every parameter, and record
/// the function's opening source location in the line table. Nothing
/// else. The body — the statements — is emitted by emitStmt, which
/// Compiler.cpp's pass B calls immediately after emitDecl.

#include "EmitDecl.hpp"
#include "CompilerContext.hpp"
#include "Compiler.hpp"
#include "TypeTranslation.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"

namespace lucid::bytecode::compile {

void emitDecl(DeclAST* decl, CompilerContext& ctx) {
    AST_ASSERT_MSG(decl != nullptr,
        "emitDecl: null declaration — the driver should not have "
        "dispatched a null decl");

    if (!decl->isa<FnDeclAST>()) {
        // The driver only calls emitDecl for a function. A table or
        // a top-level binding reaches the artifact through the baking
        // helpers, not through emitDecl.
        AST_ASSERT_MSG(false,
            "emitDecl: a non-function declaration reached the emitter — "
            "the driver dispatched the wrong kind");
        return;
    }

    auto* fn = decl->as<FnDeclAST>();

    // ─── Preconditions ─────────────────────────────────────────────────
    AST_ASSERT_MSG(!fn->isHostBound,
        "emitDecl: a host-bound function reached the emitter — "
        "a host-bound function has no body to emit");
    AST_ASSERT_MSG(fn->body != nullptr,
        "emitDecl: a Lucid-bodied function has no body block — "
        "the parser should have produced one");
    AST_ASSERT_MSG(fn->returnType != nullptr,
        "emitDecl: a function has no resolved return type — "
        "Sema should have resolved it");

    // ─── Function's opening line entry ─────────────────────────────────
    //
    // Record the function's source location at code offset 0. This is
    // what a stack trace names when a panic occurs before any user
    // statement has been reached (a missing-argument panic in the
    // prologue, say).
    ctx.noteLine(fn->loc, ctx.module()->filePath);

    // ─── Parameter slots ───────────────────────────────────────────────
    //
    // Allocate one slot per parameter, in declaration order. The
    // calling convention is: argument i arrives in slot i, for i in
    // [0, paramCount). The interpreter's frame setup matches this.
    //
    // The slot allocator refuses a duplicate allocation, so a
    // parameter name that was already allocated (which would happen
    // only if the parser produced the same ParamAST twice — a parser
    // bug) fires the allocator's assert.
    for (auto* param : fn->params) {
        AST_ASSERT_MSG(param != nullptr,
            "emitDecl: a function has a null parameter node — "
            "the parser should have produced a real ParamAST");
        ctx.slots().allocateParam(param->name);
    }

    // ─── Return-type setup ─────────────────────────────────────────────
    //
    // Nothing to emit. The return type is recorded on the FunctionProto
    // by finalizeProto; the body's return statements are what actually
    // produce the value. The prologue does not allocate a return slot —
    // the value stack carries the return value to the Return instruction.
    //
    // (A future calling convention that passes the return value through
    // a dedicated slot would add a slot allocation here. The current
    // convention uses the stack, so there is nothing to do.)

    // ─── Body ──────────────────────────────────────────────────────────
    //
    // emitDecl does NOT emit the body. Compiler.cpp's pass B calls
    // emitStmt(fn->body, ctx) after emitDecl returns, then emits an
    // implicit ReturnVoid, then calls ctx.finalizeProto(). The
    // separation exists so emitDecl can be reused for a future
    // declaration form that has a prologue but no body (a host-bound
    // function with a compile-time validation step, say).
}

} // namespace lucid::bytecode::compile
/// @file codegen/emit/Emitter.hpp
/// @brief The codegen emitter — one class, four entry points.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The emitter lowers Lucid AST nodes to LLVM IR. It has four public
/// methods:
///
///   - `Val emit(ExprAST*)`  — lower an expression, return its value and
///                             ownership tag.
///   - `void emit(StmtAST*)` — lower a statement.
///   - `void emit(DeclAST*)` — lower a declaration (function prototype,
///                             function body, variable binding, etc.).
///   - `void store(Place, Val)` — the single write path. Every write to
///                             a storage location goes through this.
///
/// Everything else is a private helper. The four public methods are the
/// only interface other codegen code sees.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the ownership engine. `Ownership` decides how values are
/// copied and released; the emitter calls into it.
///
/// It is NOT the type mapper. `Types` maps AST types to LLVM types; the
/// emitter reads from it.
///
/// It is NOT the ABI surface. `Abi` declares and calls runtime functions;
/// the emitter calls through it.
///
/// It is NOT a pass runner. `passes/` decides which declarations to emit
/// in which order. The emitter is told what to emit.
///
/// ─── Why One Class ────────────────────────────────────────────────────────
/// The old design had free functions scattered across four files, sharing
/// state through a global `CodeGenContext`. The new design makes the
/// emitter a class with its own state (the per-program `ProgramState` it
/// reads from, and the current `FunctionState` it reads through). The four
/// public methods are the only interface; internal helpers are private and
/// can be reorganized without touching call sites.
///
/// ─── The `store` Path ─────────────────────────────────────────────────────
/// `store(Place, Val)` is the single write path. Every assignment, every
/// initialization, every field write, every return-value store goes through
/// it. `store` is where `Ownership::intoOwned` runs on the incoming value
/// and where `Ownership::drop` runs on the outgoing one.
///
/// Before the redesign, each write site reimplemented those two calls
/// separately, and several of them were subtly wrong. `store` makes them
/// impossible to get wrong — there is one implementation.

#pragma once

#include "codegen/Types.hpp"
#include "codegen/Abi.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/ownership/Ownership.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace codegen {

class ProgramState;

// ─────────────────────────────────────────────────────────────────────────────
// Place — a storage location where a value of a given type can be written
// ─────────────────────────────────────────────────────────────────────────────
//
// A `Place` is the l-value form of an expression: a pointer to storage plus
// the AST type of what that storage holds. `Emitter::store` takes a `Place`
// and a `Val` and writes the value into the place, applying ownership rules.
//
// Places are built by the emitter's l-value paths:
//
//   - Identifier l-value    — the binding's alloca.
//   - Field access l-value  — a GEP into a struct.
//   - Index l-value         — a GEP into an array buffer.
//   - Deref l-value         — a load-then-store through a pointer (rare).
//
// Not every expression has a place. A literal doesn't. A binary expression
// doesn't. Only "assignable" expressions do, and only when the emitter is
// asked for the l-value form.

struct Place {
    llvm::Value* ptr = nullptr;  // pointer to the storage
    TypeAST* ty = nullptr;       // AST type of the stored value

    bool isValid() const { return ptr != nullptr && ty != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Emitter — the codegen lowering engine
// ─────────────────────────────────────────────────────────────────────────────

class Emitter {
public:
    Emitter(ProgramState& program);

    Emitter(const Emitter&) = delete;
    Emitter& operator=(const Emitter&) = delete;

    // ─── The Four Public Entry Points ─────────────────────────────────────

    /// @brief Lower an expression, return its value and ownership tag.
    ///
    /// The returned `Val` is `Owned` if the value carries a fresh claim the
    /// caller is responsible for, or `Borrowed` if it aliases a claim held
    /// elsewhere. `store` uses the tag to decide whether to acquire a new
    /// claim (via `intoOwned`) before writing.
    Val emit(ExprAST* expr);

    /// @brief Lower a statement.
    ///
    /// Statements don't produce values. Expression statements that produce
    /// an `Owned` value drop it (the value was produced for a side effect
    /// and no one consumes the claim).
    void emit(StmtAST* stmt);

    /// @brief Lower a declaration.
    ///
    /// For functions: emits the prototype (declare pass) or the body
    /// (define pass), depending on whether the function already has one.
    /// For variables: emits the alloca and initializer (define pass) or
    /// is a no-op at module level (handled by the module pass).
    /// For types: no-op (types are handled by `Types`).
    void emit(DeclAST* decl);

    /// @brief Write a value into a place, applying ownership rules.
    ///
    /// Steps:
    ///   1. `intoOwned(val)` — acquire a fresh claim for the incoming value.
    ///   2. If the place already holds a value and `decl` is alive, drop
    ///      the old value.
    ///   3. Store the new value.
    ///   4. If `decl` was not alive, mark it alive.
    ///
    /// `decl` may be null for anonymous places (temporary slots, struct
    /// field initializers, return-value slots). When non-null, it's the
    /// binding the store is for, and the tracker is updated.
    void store(Place place, Val val, ValueDeclAST* decl = nullptr);

    // ─── Current Function Access ──────────────────────────────────────────

    /// @brief The currently active `FunctionState`. Never null while a
    ///        function body is being emitted; asserting this invariant is
    ///        the emitter's first line of defense against ordering bugs.
    FunctionState& func();

    // ─── Program Access ───────────────────────────────────────────────────

    ProgramState& program;

private:
    // ─── Expression Emitters ──────────────────────────────────────────────
    //
    // One method per expression node kind. Dispatched from the public
    // `emit(ExprAST*)`.
    //
    // The methods take the specific AST node type, not the base, because
    // the dispatch in `emit` has already narrowed.

    Val emitLiteral(LiteralExprAST* expr);
    Val emitIdentifier(IdentifierExprAST* expr);
    Val emitArrayLiteral(ArrayLiteralExprAST* expr);
    Val emitStructLiteral(StructLiteralExprAST* expr);
    Val emitBinary(BinaryExprAST* expr);
    Val emitUnary(UnaryExprAST* expr);
    Val emitCall(CallExprAST* expr);
    Val emitIntrinsic(IntrinsicCallExprAST* expr);
    Val emitIndex(IndexExprAST* expr);
    Val emitSlice(SliceExprAST* expr);
    Val emitFieldAccess(FieldAccessExprAST* expr);
    Val emitModuleAccess(ModuleAccessExprAST* expr);
    Val emitArenaAccess(ArenaAccessExprAST* expr);
    Val emitNullCoalesce(NullCoalesceExprAST* expr);
    Val emitAssign(AssignExprAST* expr);
    Val emitPipeline(PipelineExprAST* expr);
    Val emitAnonFunc(AnonFuncExprAST* expr);
    Val emitIf(IfExprAST* expr);
    Val emitRange(RangeExprAST* expr);

    // ─── Statement Emitters ───────────────────────────────────────────────

    void emitBlock(BlockStmtAST* stmt);
    void emitIfStmt(IfStmtAST* stmt);
    void emitSwitchStmt(SwitchStmtAST* stmt);
    void emitForStmt(ForStmtAST* stmt);
    void emitWhileStmt(WhileStmtAST* stmt);
    void emitDoWhileStmt(DoWhileStmtAST* stmt);
    void emitReturnStmt(ReturnStmtAST* stmt);
    void emitBreakStmt(BreakStmtAST* stmt);
    void emitContinueStmt(ContinueStmtAST* stmt);
    void emitExprStmt(ExprStmtAST* stmt);
    void emitDeclStmt(DeclStmtAST* stmt);
    void emitAsyncStmt(AsyncStmtAST* stmt);
    void emitAwaitStmt(AwaitStmtAST* stmt);
    void emitSpawnStmt(SpawnStmtAST* stmt);
    void emitJoinStmt(JoinStmtAST* stmt);

    // ─── Declaration Emitters ─────────────────────────────────────────────

    void emitFuncDecl(FuncDeclAST* decl);
    void emitFuncBody(FuncDeclAST* decl);
    void emitVarDecl(VarDeclAST* decl);
    void emitStructDecl(StructDeclAST* decl);
    void emitEnumDecl(EnumDeclAST* decl);

    // ─── Place Construction (EmitPlace.cpp) ───────────────────────────────

    /// @brief Get the `Place` for an expression that names storage.
    ///
    /// Only "assignable" expressions have places: identifiers, field
    /// accesses on l-values, index expressions on l-value arrays, and
    /// dereferences. Anything else returns an invalid `Place`.
    Place emitPlace(ExprAST* expr);

    /// @brief Get the place for an identifier expression.
    Place emitIdentifierPlace(IdentifierExprAST* expr);

    /// @brief Get the place for a field access.
    Place emitFieldPlace(FieldAccessExprAST* expr);

    /// @brief Get the place for an index expression.
    Place emitIndexPlace(IndexExprAST* expr);

    // ─── Assignment Path ──────────────────────────────────────────────────
    //
    // `emitAssign` handles both plain and compound assignment. The core is:
    //   1. Get the l-value place.
    //   2. Load the old value (for compound ops, or for self-assign guard).
    //   3. Emit the RHS.
    //   4. `store(place, rhs, decl)` — this drops the old, retains the new.
    //
    // The self-assignment guard (comparing env pointers) is still needed
    // for `f = f` where `f` is a closure. It's emitted before `store`.

    // ─── Helper: Load a Value from a Place ────────────────────────────────

    /// Load the value currently in a place. Returns a `Borrowed` `Val`
    /// because the place still holds the claim (a load doesn't move).
    Val loadPlace(Place place, llvm::IRBuilder<>& builder);

    // ─── Helper: Get-or-Insert Function ───────────────────────────────────
    //
    // Function lookup goes through `ProgramState::lookupFunction`, which
    // was populated by the declare pass. A body emitter for a function
    // looks up its own prototype; a call emitter looks up the callee's.
    // If the lookup fails, it's a bug — the declare pass should have
    // populated the table.

    // ─── Helper: Value State (Null/Err) ───────────────────────────────────
    //
    // Emitters that produce tagged values (nullable, fallible) need to
    // know the tag. This is emitted inline as a load of field 0 of the
    // tagged slot. The helper centralizes the tag-load pattern.

    // ─── Helper: Truthiness ───────────────────────────────────────────────
    //
    // Condition evaluation needs to coerce any value to `i1`. The rules
    // are in `Truthiness.hpp`; the emitter calls into them.

    // ─── Helper: Tagged Slot Construction ─────────────────────────────────

    /// Construct a tagged slot `{ i8 tag, T value }` from a raw value
    /// and a tag. Used by nullable/fallible/combined emitters.
    llvm::Value* makeTaggedSlot(llvm::Value* raw, uint8_t tag, TypeAST* ty);

    // ─── Helper: Tagged Slot Unwrapping ───────────────────────────────────

    /// Extract the value from a tagged slot, asserting (via a runtime
    /// branch or a compile-time proof) that the tag indicates "present".
    /// Used when Sema has proven the slot is narrowed.
    llvm::Value* unwrapTagged(llvm::Value* slot, TypeAST* ty);
};

} // namespace codegen
/// @file codegen/emit/Emitter.hpp
/// @brief The codegen emitter — one class, four public entry points.
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
///
/// ─── How The `.cpp` Files Split ───────────────────────────────────────────
/// Every method is a member of one class; the definition files group the
/// members by subsystem. The file layout is the same as the directory
/// layout under `codegen/emit/`:
///
///   - `Emitter.cpp`
///       Constructor and `func()`. Nothing else.
///
///   - `EmitDecl.cpp`
///       `emit(DeclAST*)` and its per-kind dispatch:
///       `emitFuncDecl`, `emitFuncBody`, `emitForeignFuncDecl`,
///       `emitVarDecl`, `emitStructDecl`, `emitEnumDecl`.
///
///   - `EmitStmt.cpp`
///       `emit(StmtAST*)` and its per-kind dispatch, plus the scope
///       management primitives:
///       `createEntryAlloca`, `emitScopeFallthrough`, `emitUnwindTo`,
///       `dropScopeAlive`.
///
///   - `EmitPlace.cpp`
///       Place construction and the single write path:
///       `store`, `loadPlace`, `emitPlace`, `emitIdentifierPlace`,
///       `emitFieldPlace`, `emitIndexPlace`.
///
///   - `EmitClosure.cpp`
///       The closure subsystem:
///       `emitAnonFunc`, `emitClosureFuncDecl`,
///       `buildClosureEnvironment`, `createClosureFunction`,
///       `emitClosureBody`, `buildEnvDropFunction`,
///       `emitClosureCall`.
///
///   - `EmitConcurrency.cpp`
///       The concurrency subsystem:
///       `emitAsyncStmt`, `emitAwaitStmt`, `emitSpawnStmt`,
///       `emitJoinStmt`, `buildConcurrencyThunk`,
///       `buildConcurrencyPacket`.
///
///   - `expr/EmitExpr.cpp`
///       The expression subsystem's entry point:
///       `emit(ExprAST*)` and `emitFoldedConstant`. The file also
///       carries the ownership-tag table that every expression
///       emitter obeys.
///
///   - `expr/EmitScalar.cpp`
///       Scalar and control-flow-producing expressions:
///       `emitLiteral`, `emitIdentifier`, `emitBinary`, `emitUnary`,
///       `emitIf`, `emitRange`.
///
///   - `expr/EmitTruthiness.cpp`
///       The truthiness rules:
///       `emitTruthiness`.
///
///   - `expr/EmitAccess.cpp`
///       Reads from storage:
///       `emitIndex`, `emitSlice`, `emitFieldAccess`,
///       `emitModuleAccess`, `emitArenaAccess`.
///
///   - `expr/EmitAggregate.cpp`
///       Aggregate construction:
///       `emitStructLiteral`, `emitArrayLiteral`.
///
///   - `expr/EmitWrite.cpp`
///       Expressions that read-then-write or thread a value through
///       multiple evaluation points:
///       `emitAssign`, `emitNullCoalesce`, `emitPipeline`,
///       `applyCompoundOp`.
///
///   - `expr/EmitCall.cpp`
///       Calls and coercion:
///       `emitCall`, `emitIntrinsic`, `emitCallableCall`,
///       `coerceArgument`, `coerceTo`, `coerceValueToType`,
///       `materializeArgument`.
///
/// The `expr/` subdirectory groups the expression sub-emitters under the
/// single dispatcher in `EmitExpr.cpp`. Everything else is flat because
/// its role in the emitter is unique.
///
/// ─── The `emit` Dispatcher ────────────────────────────────────────────────
/// `emit(ExprAST*)` in `EmitExpr.cpp` is the only entry point into the
/// expression subsystem. Its switch statement names every expression
/// kind and routes it to the file that implements that kind. When you
/// want to know "where is `emitBinary`?", read the dispatcher's switch.

#pragma once

#include "codegen/Types.hpp"
#include "codegen/Abi.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/ownership/Ownership.hpp"
#include "codegen/FailureKind.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/ResourceKind.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include "runtime/RuntimeError.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace codegen {

class ProgramState;

// ─────────────────────────────────────────────────────────────────────────────
// Place — a storage location where a value of a given type can be written
// ─────────────────────────────────────────────────────────────────────────────
//
// A `Place` is the l-value form of an expression: a pointer to storage
// plus the AST type of what that storage holds. `Emitter::store` takes a
// `Place` and a `Val` and writes the value into the place, applying
// ownership rules.
//
// Places are built by the emitter's l-value paths:
//
//   - Identifier l-value    — the binding's storage.
//   - Field access l-value  — a GEP into a struct.
//   - Index l-value         — a GEP into an array buffer.
//   - Deref l-value         — a load-then-store through a pointer (rare).
//
// Not every expression has a place. A literal doesn't. A binary
// expression doesn't. Only "assignable" expressions do, and only when
// the emitter is asked for the l-value form.

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
    /// The returned `Val` is `Owned` if the value carries a fresh claim
    /// the caller is responsible for, or `Borrowed` if it aliases a claim
    /// held elsewhere. `store` uses the tag to decide whether to acquire
    /// a new claim (via `intoOwned`) before writing.
    Val emit(ExprAST* expr);

    /// @brief Lower a statement.
    ///
    /// Statements don't produce values. Expression statements that
    /// produce an `Owned` value drop it (the value was produced for a
    /// side effect and no one consumes the claim).
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
    ///   1. `intoOwned(val)` — acquire a fresh claim for the incoming
    ///      value.
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
    ///        function body is being emitted; asserting this invariant
    ///        is the emitter's first line of defense against ordering
    ///        bugs.
    FunctionState& func();

    // ─── Program Access ───────────────────────────────────────────────────

    ProgramState& program;

        // ─── Runtime Failure Emission ─────────────────────────────────────────
    //
    // Emit the failure path of a runtime check. Centralizes the
    // "branch to the `??` fallback if one is active, otherwise panic"
    // logic.
    //
    // Every runtime check the emitter inserts calls this. It's the
    // single place the coalesce tracker is consulted, and the single
    // place `emitPanic` is called from a runtime-check context.
    //
    // The caller is responsible for having set the insertion point to
    // the check's failure block. This function either branches out of
    // that block (fallback case) or terminates it (panic case); it
    // never returns with the block still unterminated.
    //
    // `kind` selects the diagnostic message and the interceptability.
    // See `FailureKind.hpp` for the metadata.
    void emitFailure(FailureKind kind, SourceLocation loc);

    // ─── Null-Coalesce Fallback Queries ───────────────────────────────────
    //
    // Public queries for the emitter's own use. `emitFailure` reads them
    // internally; they're exposed so the intrinsic emitters (which are
    // friends) can query the same state if they need to.

    /// True if a `??` fallback is currently in scope.
    bool insideNullCoalesce() const;

    /// The innermost `??` fallback block, or null if none.
    llvm::BasicBlock* nullCoalesceFallbackBlock() const;

    // ─── Calls and Coercion (expr/EmitCall.cpp) ───────────────────────────

    Val emitCall(CallExprAST* expr);
    Val emitIntrinsic(IntrinsicCallExprAST* expr);

    /// @brief Dispatch a call on the callee's `FuncShape`.
    ///
    /// `fn`-shaped callees are bare function pointers: cast and call.
    /// `cls`-shaped callees are fat pointers: extract `{fn, env}`,
    /// prepend `env` to the argument list, and call `fn` indirectly.
    llvm::Value* emitCallableCall(llvm::Value* callee,
                                  llvm::ArrayRef<llvm::Value*> args,
                                  llvm::FunctionType* fnType,
                                  FuncShape shape,
                                  const llvm::Twine& name);

    /// @brief Coerce an argument value to a parameter's declared type.
    ///
    /// Handles `fn → cls` widening, integer widening/narrowing, pointer
    /// casts, and aggregate-by-value conversions. Returns an invalid
    /// `Val` if the coercion is not supported (which is a Sema bug —
    /// Sema should have rejected the assignment).
    Val coerceArgument(Val arg, TypeAST* paramTy);

    /// @brief Coerce a value to a target AST type.
    ///
    /// Unlike `coerceArgument`, this handles return-value coercion, which
    /// has a slightly different surface (it may insert the `fn → cls`
    /// widening before the type-based coercions).
    Val coerceTo(Val val, TypeAST* targetTy);

    /// @brief Coerce an `llvm::Value*` to a target `llvm::Type*`.
    ///
    /// Low-level: integer widening/narrowing, pointer cast, aggregate
    /// bitcast. Used by the higher-level coercion helpers.
    llvm::Value* coerceValueToType(llvm::Value* val,
                                   llvm::Type* targetTy,
                                   llvm::IRBuilder<>& builder);

    /// @brief Spill an aggregate argument to a stack slot and return the
    ///        slot's pointer; pass scalars through unchanged.
    ///
    /// The runtime ABI passes aggregates by pointer, not by value. This
    /// helper implements the caller side of that convention.
    llvm::Value* materializeArgument(Val val);

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

    /// @brief Load the value currently in a place.
    ///
    /// Returns a `Borrowed` `Val`: the place still holds the claim, and
    /// the loaded value is an alias. Anyone who wants to store the loaded
    /// value must call `intoOwned` first (which `store` does).
    Val loadPlace(Place place, llvm::IRBuilder<>& builder);

    // ─── Scope Management (EmitStmt.cpp) ──────────────────────────────────

    /// @brief Allocate in the current function's entry block.
    ///
    /// Every emitter-side alloca goes in the entry block. Creating
    /// allocas in the current block would make a loop body allocate a
    /// new slot per iteration and accumulate them until the function
    /// returns. This helper enforces the discipline.
    ///
    /// Returns null if there's no current function to attach to.
    llvm::AllocaInst* createEntryAlloca(llvm::Type* ty,
                                        const llvm::Twine& name);

    /// @brief Emit drops for the current scope, then clear its alive set.
    ///
    /// Called at the natural end of a block (by `emitBlock`) and at the
    /// natural end of a function body (by `emitFuncBody` and
    /// `emitClosureBody`). Does not pop the scope — the caller does that.
    ///
    /// A no-op if the current insertion block is already terminated, or
    /// if the scope has no alive bindings.
    void emitScopeFallthrough();

    /// @brief Emit drops for every scope from the innermost down to
    ///        (but not including) `targetDepth`.
    ///
    /// Called by `return` (target 0), `break` and `continue` (target =
    /// the loop's entry scope depth). Does not pop the scopes — the
    /// structurally-paired `popScope` at each block's natural end pops
    /// them.
    void emitUnwindTo(size_t targetDepth);

    /// @brief Emit drops for every still-alive binding in one scope, in
    ///        reverse declaration order, and clear the scope's alive
    ///        set.
    ///
    /// The shared body of `emitScopeFallthrough` (called on the current
    /// scope) and `emitUnwindTo` (called on each scope in the unwind
    /// range). The caller is responsible for checking that the current
    /// insertion block isn't already terminated.
    void dropScopeAlive(Scope& scope);

    // ─── Runtime Diagnostics (EmitScalar.cpp) ─────────────────────────────

    /// @brief Emit a runtime panic with a formatted message.
    ///
    /// Format: `"file:line:col: description"`. Emits a call to
    /// `__lucid_panic` followed by `unreachable`. Used by every
    /// runtime-check emitter (division-by-zero, index bounds, arena
    /// capacity).
    void emitPanic(RuntimeErrorKind kind, SourceLocation loc);

    // ─── Bounds Checks (EmitAccess.cpp) ───────────────────────────────────

    /// @brief Bounds-check `0 <= index < size` on a fixed-size array.
    ///
    /// On failure, emits a panic and `unreachable`. On success, leaves
    /// the builder in the success block. Returns the `i1` in-bounds
    /// predicate.
    llvm::Value* emitFixedArrayBoundsCheck(llvm::Value* index,
                                            uint64_t size,
                                            SourceLocation loc);

    /// @brief Bounds-check `0 <= index < len` for a slice or dynamic
    ///        array.
    ///
    /// Same shape as `emitFixedArrayBoundsCheck`, but `len` is a runtime
    /// value.
    llvm::Value* emitSliceBoundsCheck(llvm::Value* index,
                                       llvm::Value* len,
                                       SourceLocation loc);

    // ─── Closure Lowering (EmitClosure.cpp) ───────────────────────────────

    /// @brief Build the environment struct type for a closure.
    llvm::StructType* buildClosureEnvironment(AnonFuncExprAST* expr);

    /// @brief Create the LLVM function that implements the closure body.
    llvm::Function* createClosureFunction(AnonFuncExprAST* expr);

    /// @brief Emit the body of a closure function.
    ///
    /// Uses a nested `FunctionState` for the closure's scope, and binds
    /// each captured declaration to its environment-loaded value (or its
    /// spilled alloca for by-value captures).
    void emitClosureBody(AnonFuncExprAST* expr,
                         llvm::Function* closureFn,
                         llvm::Value* envPtr);

    /// @brief Generate the environment-drop function for a closure.
    ///
    /// Returns null if the closure's environment owns nothing that needs
    /// releasing (e.g. all captures are `fn`-shaped or non-resources).
    llvm::Function* buildEnvDropFunction(AnonFuncExprAST* expr,
                                         llvm::StructType* envType);

    /// @brief Emit the fat-pointer construction for a `cls`-shaped
    ///        named function declaration.
    void emitClosureFuncDecl(FuncDeclAST* decl);

    /// @brief The public closure-literal entry point.
    ///
    /// Produces a `{ ptr fn, ptr env }` fat pointer. Allocates the
    /// environment, stores the captures, retains captured `cls` envs,
    /// and constructs the fat pointer.
    Val emitAnonFunc(AnonFuncExprAST* expr);

    /// @brief The low-level call through a closure fat pointer.
    ///
    /// Extracts `{func, env}`, prepends `env` to the argument list, and
    /// calls `func` indirectly.
    llvm::Value* emitClosureCall(llvm::Value* funcPtr,
                                 llvm::Value* envPtr,
                                 llvm::ArrayRef<llvm::Value*> args,
                                 llvm::Type* returnType);

private:
    // ─── Expression Emitters (expr/*.cpp) ─────────────────────────────────
    //
    // One method per expression node kind. Dispatched from the public
    // `emit(ExprAST*)` in `expr/EmitExpr.cpp`.
    //
    // The methods take the specific AST node type, not the base, because
    // the dispatch has already narrowed.


    // ─── Folded constant ─────────────────────────────────────────────────
    // Sema folds some expressions to `ConstantValue`s. If this one was
    // folded, emit the constant directly. `emitFoldedConstant` returns an
    // invalid `Val` for constants it can't lower (structs, arrays,
    // function pointers), which fall through to the per-kind emitter.
    Val emitFoldedConstant(ExprAST* expr);

    // ─── Scalar (expr/EmitScalar.cpp) ─────────────────────────────────────

    Val emitLiteral(LiteralExprAST* expr);
    Val emitIdentifier(IdentifierExprAST* expr);
    Val emitBinary(BinaryExprAST* expr);
    Val emitUnary(UnaryExprAST* expr);
    Val emitIf(IfExprAST* expr);
    Val emitRange(RangeExprAST* expr);

    // ─── Truthiness (expr/EmitTruthiness.cpp) ─────────────────────────────

    /// @brief Coerce a Lucid value to an LLVM `i1`.
    ///
    /// Lucid's condition positions accept any type. The truthiness rules
    /// (nonzero for numbers, non-empty for strings, present for tagged
    /// types, always-true for structs and functions) are in the
    /// implementation file. Returns null if the input is invalid.
    llvm::Value* emitTruthiness(Val val);

    // ─── Storage Access (expr/EmitAccess.cpp) ─────────────────────────────

    Val emitIndex(IndexExprAST* expr);
    Val emitSlice(SliceExprAST* expr);
    Val emitFieldAccess(FieldAccessExprAST* expr);
    Val emitModuleAccess(ModuleAccessExprAST* expr);
    Val emitArenaAccess(ArenaAccessExprAST* expr);

    // ─── Aggregates (expr/EmitAggregate.cpp) ──────────────────────────────

    Val emitArrayLiteral(ArrayLiteralExprAST* expr);
    Val emitStructLiteral(StructLiteralExprAST* expr);

    // ─── Read-then-Write (expr/EmitWrite.cpp) ─────────────────────────────

    Val emitAssign(AssignExprAST* expr);
    Val emitNullCoalesce(NullCoalesceExprAST* expr);
    Val emitPipeline(PipelineExprAST* expr);

    /// @brief Apply a compound-assignment operator to two values.
    ///
    /// Used by `emitAssign` for `x += y`, `x -= y`, etc. The operator
    /// dispatch mirrors `emitBinary`'s but operates on already-emitted
    /// values rather than AST expressions.
    Val applyCompoundOp(AssignOp op, Val oldValue, Val rhs, SourceLocation loc);

    // ─── Null-Coalesce Lowering ───────────────────────────────────────────

    /// `x ?? fallback` where `x`'s type is `T?`, `T!`, or `T?!`.
    /// Extracts the tag, branches, and PHIs the narrowed inner value
    /// with the fallback.
    Val emitNullCoalesceTagged(NullCoalesceExprAST* expr, TypeAST* lhsTy);

    /// `x ?? fallback` where `x` is a risky operation (division, index,
    /// slice, or one of the risky intrinsics). Pushes a fallback onto
    /// the coalesce tracker, emits the LHS (whose runtime check will
    /// branch to the fallback on failure), pops the tracker, and PHIs
    /// the success value with the fallback.
    Val emitNullCoalesceRisky(NullCoalesceExprAST* expr);

    // ─── Statement Emitters (EmitStmt.cpp) ────────────────────────────────

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

    // ─── Declaration Emitters (EmitDecl.cpp) ──────────────────────────────

    void emitFuncDecl(FuncDeclAST* decl);
    void emitFuncBody(FuncDeclAST* decl);
    void emitForeignFuncDecl(FuncDeclAST* decl);
    void emitVarDecl(VarDeclAST* decl);
    void emitStructDecl(StructDeclAST* decl);
    void emitEnumDecl(EnumDeclAST* decl);

    // ─── Concurrency Lowering (EmitConcurrency.cpp) ───────────────────────

    void emitAsyncStmt(AsyncStmtAST* stmt);
    void emitAwaitStmt(AwaitStmtAST* stmt);
    void emitSpawnStmt(SpawnStmtAST* stmt);
    void emitJoinStmt(JoinStmtAST* stmt);

    /// @brief Build a thunk function for an async/spawn call.
    ///
    /// The thunk has signature `void* (void* packet)`. It unpacks the
    /// packet (loads each argument from its field), frees the packet,
    /// calls the real function, boxes the result, and returns the box
    /// pointer.
    llvm::Function* buildConcurrencyThunk(CallExprAST* call,
                                          TypeAST* returnType);

    /// @brief Build a heap packet holding the arguments for an
    ///        async/spawn call.
    ///
    /// The packet is an LLVM struct with one field per argument. The
    /// emitter allocates it via `__lucid_alloc`, stores each argument
    /// into its field, and returns a pointer to the packet.
    llvm::Value* buildConcurrencyPacket(CallExprAST* call);

    // ─── Resource Classification ──────────────────────────────────────────

    /// @brief The declaration's cached resource kind.
    ///
    /// Reads `decl->resourceKind`, populated by Sema. Null-safe.
    ResourceKind classifyResource(ValueDeclAST* decl) const {
        return decl ? decl->resourceKind : ResourceKind::None;
    }

    /// @brief True if the declaration owns a heap resource.
    bool ownsResource(ValueDeclAST* decl) const {
        return classifyResource(decl) != ResourceKind::None;
    }

        // ─── Intrinsic Subsystem (codegen/intrinsic/*) ────────────────────────
    //
    // The intrinsic emitters are peers of `Emitter`, not members: they
    // live in their own translation units and their own headers, and they
    // are dispatched from `emit(ExprAST*)` via `emitIntrinsicFromAST`.
    // They need access to a handful of `Emitter`'s private helpers —
    // `emitPlace` (for `#addrof`, `#ptrstr`, `#toRef`), `createEntryAlloca`
    // (for out-pointer runtime calls), `emitPanic` (for the null check in
    // `#toRef` and the bounds check in `#simd_extract`/`#simd_insert`),
    // and `emitClosureCall` (for the `str`-override path in `#tostr`) —
    // but promoting those to public would widen `Emitter`'s public surface
    // far beyond the four entry points the header's design commentary
    // commits to.
    //
    // Friendship is the right tool: it grants exactly the two dispatcher
    // entry points access to exactly the helpers they need, without
    // exposing anything to the rest of the codebase.
    //
    // The two functions are declared in
    // `codegen/intrinsic/LLVMIntrinsicEmitter.hpp` and
    // `codegen/intrinsic/LucidIntrinsicEmitter.hpp`. They are named here
    // as friends; no declaration is needed in this header.
    friend Val emitLLVMIntrinsic(IntrinsicCallExprAST*, const IntrinsicInfo&, Emitter&);
    friend Val emitLucidIntrinsic(IntrinsicCallExprAST*, const IntrinsicInfo&, Emitter&);

    // ─── Scope-Exit Callback Emission ─────────────────────────────────────
    //
    // `#scope_exit(f, args)` registers `f` with the enclosing block during
    // Sema (`BlockStmtAST::scopeExits`). At block exit — both the natural
    // fall-through and every unwind path (`return`, `break`, `continue`) —
    // the emitter walks the block's registrations in reverse (LIFO) and
    // emits a call to each.
    //
    // Declared here because the walk happens in `dropScopeAlive`
    // (EmitStmt.cpp), and the intrinsic subsystem owns the calling
    // convention (plain function reference vs. closure fat pointer).
    // `#scope_exit`'s own `emitLucidIntrinsic` case is a no-op — the call
    // site emits nothing; the callback is emitted here.
    //
    // `reg` must be non-null. Sema guarantees every entry in
    // `scopeExits` is a valid registration.
    //
    // The body is a straight port of the old
    // `emitScopeExitCallback(reg, ctx)` free function: it reads
    // `reg->callback` (plain function reference, resolved to an
    // `llvm::Function*` via `program.lookupFunction`), or, if that is
    // null, `reg->callExpr->args[0]` (a closure expression, lowered to a
    // fat pointer and called through `emitClosureCall`). All access to
    // `ctx` becomes access to `*this` / `program`.
    void emitScopeExitCallback(const ScopeExitRegistration* reg);
};

} // namespace codegen
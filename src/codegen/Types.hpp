/// @file codegen/Types.hpp
/// @brief Lucid AST types → LLVM types, plus per-program type caching.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The type mapping layer. Given a `TypeAST*` produced by Sema, produce the
/// corresponding `llvm::Type*`. Given a `StructDeclAST*` or `EnumDeclAST*`,
/// produce the corresponding named LLVM type. Cache every result so that
/// repeated calls with the same AST node return the same LLVM object.
///
/// This is a class rather than a set of free functions because the cache is
/// state, and state belongs to an object with a lifetime. In the new design
/// that lifetime is the `ProgramState`: one `Types` per program, constructed
/// once at the top of `generate()` and destroyed when the program's
/// `CodegenResult` is destroyed. This is the same shape as `Abi` and
/// `Ownership` and is what lets multiple `ProgramState`s coexist — the
/// interpreter will eventually need one per loaded program.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT a translation of Lucid semantics into IR. It only translates
/// *types*. It does not decide how a value of a given type is stored,
/// copied, or freed — that's `Ownership`. It does not emit any instructions.
/// It does not know about any AST node except the type nodes and the
/// declaration nodes that define user types.
///
/// It is NOT the resource classifier. `classifyResourceKind(TypeAST*)` lives
/// in `core/ast/ResourceKind.hpp`, shared by Sema and codegen. `Types.hpp`
/// includes that header so anything that needs the classifier and already
/// has `Types.hpp` in scope finds it transitively.
///
/// ─── Dependencies ─────────────────────────────────────────────────────────
/// `Types` depends on:
///   - `llvm::LLVMContext&` — for constructing LLVM types
///   - `llvm::Module&` — for `DataLayout`
///   - `StringPool&` — for naming LLVM struct types
///
/// It does NOT depend on `CodeGenContext`, `ProgramState`, `Emitter`,
/// `Ownership`, `Abi`, or any other codegen component. Its inputs are LLVM
/// types, a string pool, and the AST. This is what makes it a leaf: nothing
/// below it, and it can be unit-tested with a bare `LLVMContext` and a
/// `StringPool`.

#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ResourceKind.hpp"
#include "core/memory/StringPool.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// ModuleInstanceLayout — a module's state struct, and its field order
// ─────────────────────────────────────────────────────────────────────────────
//
// Under the module-as-namespace model, a module's top-level state lives in
// one LLVM struct per module, stored as a global. This describes that
// struct: its LLVM type, the ordered list of declarations whose storage
// occupies it (one field per binding, in declaration order), and a reverse
// map from declaration to field index.
//
// It lives here rather than in `ProgramState` because both `Types` and
// `ProgramState` need it: `Types` produces it (by walking the module's
// declarations and calling `Types::get` on each binding's type), and
// `ProgramState` consumes it (to emit the global and to GEP into it).
// Putting the struct in `Types.hpp` and the collection-of-layouts map in
// `ProgramState` keeps each on the side that uses it.

struct ModuleInstanceLayout {
    llvm::StructType* type = nullptr;
    std::vector<ValueDeclAST*> fields;
    std::unordered_map<ValueDeclAST*, size_t> fieldOf;
};

// ─────────────────────────────────────────────────────────────────────────────
// Types — the type mapping and its cache
// ─────────────────────────────────────────────────────────────────────────────

class Types {
public:
    /// @brief Construct a type mapper for one program.
    ///
    /// The three references must outlive the `Types` object. In the target
    /// design they come from `ProgramState`, which owns the LLVM context and
    /// module and holds a reference to the caller-supplied `StringPool`.
    Types(llvm::LLVMContext& llvmCtx, llvm::Module& module, StringPool& pool);

    /// The `Types` object is not copyable or movable — its caches are keyed
    /// on AST node pointers, and moving it would move the caches, which no
    /// caller wants. It lives inside `ProgramState` by value and is reached
    /// through a member accessor.
    Types(const Types&) = delete;
    Types& operator=(const Types&) = delete;
    Types(Types&&) = delete;
    Types& operator=(Types&&) = delete;

    // ─── Main Entry Point ─────────────────────────────────────────────────

    /// @brief Get the LLVM type for a Lucid type annotation.
    ///
    /// Idempotent. The first call for a given `TypeAST*` constructs the LLVM
    /// type and caches it; subsequent calls return the cached value. The
    /// returned pointer is stable for the lifetime of the `Types` object.
    ///
    /// Returns null on error (diagnostic emitted by the caller).
    llvm::Type* get(TypeAST* type);

    // ─── Built-in Type Accessors ──────────────────────────────────────────

    /// @brief LLVM vector type for `Simd<T, N>`.
    llvm::VectorType* simdType(SimdTypeAST* simd);

    /// @brief `lucid.Arena`: { ptr base, i64 size, i64 cursor }.
    llvm::StructType* arenaType();

    /// @brief `lucid.ArenaDescriptor`: { ptr base, i64 size }.
    llvm::StructType* arenaDescriptorType();

    /// @brief `lucid.String`: { ptr data, i64 len, i64 cap }.
    ///
    /// Provided as a method rather than reached through a `TypeAST*` because
    /// runtime-ABI code needs to name this type directly without having a
    /// `PrimitiveTypeAST(String)` on hand. It is the same LLVM struct type
    /// that `get` returns for a `string` type.
    llvm::StructType* stringType();

    /// @brief `lucid.Slice`: { ptr data, i64 len, i64 cap }.
    ///
    /// The generic slice shape, used where a slice's element type is not
    /// statically known (e.g. the variadic-argument slot in a call to a
    /// variadic function). The typed slice for a specific element type is
    /// produced by `arrayType` with `ArrayKind::Slice`.
    llvm::StructType* sliceType();

    /// @brief `lucid.Closure`: { ptr fn, ptr env }.
    ///
    /// The runtime shape of every `cls`-shaped function value. A `fn`-shaped
    /// function value is a bare `ptr` and does not use this type.
    llvm::StructType* closureType();

    // ─── Declaration-Based Type Accessors ─────────────────────────────────

    /// @brief LLVM struct type for a struct declaration.
    ///
    /// Idempotent, cache-hit on the second call. Handles self-reference by
    /// creating an opaque type first, caching it, then setting its body.
    llvm::StructType* structType(StructDeclAST* decl);

    /// @brief LLVM integer type for an enum declaration.
    ///
    /// Enums lower to a bare integer of the backing type's width. Two enums
    /// with the same backing type share the same `llvm::IntegerType*`, because
    /// LLVM interns integer types by bit width per context.
    llvm::IntegerType* enumType(const EnumDeclAST* decl);

    // ─── Function Type Accessors ──────────────────────────────────────────

    /// @brief LLVM function type for a Lucid function type.
    ///
    /// `isClosure` prepends a leading `ptr env` parameter. This is used for
    /// the runtime signature of a `cls`-shaped function's underlying
    /// implementation: the caller passes the environment pointer, then the
    /// declared parameters.
    llvm::FunctionType* functionType(FuncTypeAST* funcType,
                                     bool isClosure = false);

    /// @brief LLVM type for a function *value* at runtime.
    ///
    /// `fn` shapes lower to `ptr`; `cls` shapes lower to `closureType()`.
    /// This is the type of a binding that holds a function value, not the
    /// type of the function itself.
    llvm::Type* functionRuntimeType(FuncTypeAST* funcType, bool isClosure);

    // ─── Specific Type Mappers ────────────────────────────────────────────
    //
    // These are public because they are called recursively: `get` dispatches
    // to one of them, and each of them may dispatch back into `get` for its
    // inner types. Making them private would mean `get` and the specific
    // mappers can't see each other without friend declarations.

    llvm::Type* primitiveType(PrimitiveTypeAST* type);
    llvm::Type* ptrType(PtrTypeAST* type);
    llvm::Type* refType(RefTypeAST* type);
    llvm::Type* arrayType(ArrayTypeAST* type);
    llvm::StructType* nullableType(NullableTypeAST* type);
    llvm::StructType* fallibleType(FallibleTypeAST* type);
    llvm::StructType* combinedType(CombinedTypeAST* type);
    llvm::StructType* futureType(FutureTypeAST* type);
    llvm::StructType* threadType(const ThreadTypeAST* type);
    llvm::Type* namedType(NamedTypeAST* named);
    llvm::Type* moduleTypeAccess(ModuleTypeAccessAST* type);

    // ─── Module Instance Layout ───────────────────────────────────────────

    /// @brief Get (or create) the LLVM struct type for a module's state.
    ///
    /// Reads `ValueDeclAST::moduleFieldIndex`, which Sema set in Phase 1.
    /// This function does NOT assign indices — that is Sema's job, so the
    /// layout is a fact independent of codegen's type-resolution order.
    ///
    /// The `layouts` argument is the caller's map. It is not a member of
    /// `Types` because it belongs to the program state, not the type layer.
    /// Callers pass `program.moduleLayouts()`.
    llvm::StructType* moduleInstanceType(
        ModuleAST* module,
        std::unordered_map<ModuleAST*, ModuleInstanceLayout>& layouts);

    // ─── Helpers ──────────────────────────────────────────────────────────

    /// @brief Integer LLVM type for a primitive kind.
    llvm::IntegerType* integerType(PrimitiveKind kind);

    /// @brief Floating-point LLVM type for a primitive kind.
    llvm::Type* floatType(PrimitiveKind kind);

    /// @brief Human-readable name for a Lucid type.
    ///
    /// Used to name LLVM struct types (`nullable_int`, `slice_Point`, ...)
    /// so generated IR is readable. It is NOT the mangled name — that comes
    /// from Sema and is for linker symbols.
    std::string typeName(TypeAST* type);

    /// @brief Size of a Lucid type in bytes, per the module's DataLayout.
    uint64_t sizeOf(TypeAST* type);

    /// @brief ABI alignment of a Lucid type in bytes, per the module's DataLayout.
    uint64_t alignOf(TypeAST* type);

    // ─── Static Predicates ────────────────────────────────────────────────

    /// @brief True if this is an owned buffer — a string or a dynamic array.
    ///
    /// Both lower to the same LLVM shape (`{ ptr, i64, i64 }`) and share the
    /// same ownership semantics (deep-copy on copy, free on drop). Used by
    /// the ownership layer to classify a value, and by the closure-capture
    /// path to reject by-value captures that would require a deep copy the
    /// capture path doesn't currently implement.
    ///
    /// This is a narrower predicate than `classifyResourceKind`. Use it only
    /// where "is this an owned buffer?" is specifically the question. For
    /// "does this type own a resource?" use
    /// `classifyResourceKind(type) != ResourceKind::None`.
    static bool isOwnedBuffer(TypeAST* type);

private:
    // ─── State ────────────────────────────────────────────────────────────

    llvm::LLVMContext& llvmCtx;
    llvm::Module& module;
    StringPool& pool;

    /// Cache from AST type node to LLVM type. Keyed on the canonical node
    /// from Sema's type cache, so identical types share an entry.
    std::unordered_map<TypeAST*, llvm::Type*> typeCache;

    /// Cache from struct declaration to LLVM struct type. Separate from
    /// `typeCache` because the same `StructDeclAST*` can be reached through
    /// many different `NamedTypeAST*` nodes (one per source-level mention),
    /// and all of them must produce the same LLVM struct.
    std::unordered_map<StructDeclAST*, llvm::StructType*> structCache;
};

} // namespace codegen
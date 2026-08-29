/// @file support/RuntimeError.hpp
/// @brief Central registry of all runtime errors that can occur.
///
/// This file defines every runtime error that can be emitted by the compiler.
/// All error messages are centralized here for consistency and easy auditing.

#pragma once

#include "core/diagnostics/DiagCode.hpp"

#include <string>

namespace codegen {

/// @brief Enumeration of all possible runtime errors.
///
/// These are errors that can occur at runtime and will trigger a panic.
/// Each error has a unique name and a corresponding message.
enum class RuntimeErrorKind {
    // ─── Arithmetic Errors ────────────────────────────────────────────────
    DivisionByZero,          ///< Integer division by zero
    ModuloByZero,            ///< Integer modulo by zero
    IntegerOverflow,         ///< Integer overflow (if checked)
    NegationOverflow,        ///< Negation overflow (e.g., INT_MIN)
    
    // ─── Array/Slice Errors ────────────────────────────────────────────────
    ArrayIndexOutOfBounds,   ///< Index out of bounds for array access
    SliceBoundsOutOfRange,   ///< Slice start/end out of range
    NegativeArraySize,       ///< Negative size in fixed array literal
    
    // ─── Pointer Errors ────────────────────────────────────────────────────
    NullPointerDereference,  ///< Dereferencing a null pointer (#toRef)
    DanglingPointer,         ///< Dereferencing a dangling pointer
    
    // ─── Memory Errors ────────────────────────────────────────────────────
    DoubleFree,              ///< Freeing already freed memory
    FreeNullPointer,         ///< Calling #free on null pointer
    AllocationFailed,        ///< Memory allocation failure
    ArenaAllocationFailed,   ///< Arena allocation failure
    ArenaInvalidDescriptor,  ///< Invalid arena descriptor
    
    // ─── Type System Errors ──────────────────────────────────────────────
    UnwrappedNil,            ///< Using nil value without narrowing
    UnwrappedErr,            ///< Using err value without narrowing
    TagMismatch,             ///< Tagged slot tag mismatch
    
    // ─── Foreign Function Errors ──────────────────────────────────────────
    ForeignCallFailed,       ///< Foreign function call failed
    ForeignSymbolNotFound,   ///< Foreign symbol not found
    
    // ─── Concurrency Errors ──────────────────────────────────────────────
    AwaitOnNonFuture,        ///< Awaiting something that isn't a Future
    JoinOnNonThread,         ///< Joining something that isn't a Thread
    FutureAlreadyConsumed,   ///< Awaiting a future that was already consumed
    ThreadAlreadyJoined,     ///< Joining a thread that was already joined
    
    // ─── Runtime Library Errors ──────────────────────────────────────────
    RuntimePanic,            ///< Generic runtime panic
    AssertionFailed,         ///< Assertion failed
};

/// @brief Get the error message for a runtime error kind.
inline const char* getRuntimeErrorMessage(RuntimeErrorKind kind) {
    switch (kind) {
        case RuntimeErrorKind::DivisionByZero:          return "division by zero";
        case RuntimeErrorKind::ModuloByZero:            return "modulo by zero";
        case RuntimeErrorKind::IntegerOverflow:         return "integer overflow";
        case RuntimeErrorKind::NegationOverflow:        return "negation overflow";
        case RuntimeErrorKind::ArrayIndexOutOfBounds:   return "array index out of bounds";
        case RuntimeErrorKind::SliceBoundsOutOfRange:   return "slice bounds out of range";
        case RuntimeErrorKind::NegativeArraySize:       return "negative array size";
        case RuntimeErrorKind::NullPointerDereference:  return "null pointer dereference";
        case RuntimeErrorKind::DanglingPointer:         return "dangling pointer dereference";
        case RuntimeErrorKind::DoubleFree:              return "double free detected";
        case RuntimeErrorKind::FreeNullPointer:         return "free called on null pointer";
        case RuntimeErrorKind::AllocationFailed:        return "memory allocation failed";
        case RuntimeErrorKind::ArenaAllocationFailed:   return "arena allocation failed";
        case RuntimeErrorKind::ArenaInvalidDescriptor:  return "invalid arena descriptor";
        case RuntimeErrorKind::UnwrappedNil:            return "unwrapped nil value";
        case RuntimeErrorKind::UnwrappedErr:            return "unwrapped err value";
        case RuntimeErrorKind::TagMismatch:             return "tagged slot tag mismatch";
        case RuntimeErrorKind::ForeignCallFailed:       return "foreign function call failed";
        case RuntimeErrorKind::ForeignSymbolNotFound:   return "foreign symbol not found";
        case RuntimeErrorKind::AwaitOnNonFuture:        return "await called on non-future value";
        case RuntimeErrorKind::JoinOnNonThread:         return "join called on non-thread value";
        case RuntimeErrorKind::FutureAlreadyConsumed:   return "future already consumed";
        case RuntimeErrorKind::ThreadAlreadyJoined:     return "thread already joined";
        case RuntimeErrorKind::RuntimePanic:            return "runtime panic";
        case RuntimeErrorKind::AssertionFailed:         return "assertion failed";
        default:                                        return "unknown runtime error";
    }
}

} // namespace codegen

// ─── DiagCode Mapping ──────────────────────────────────────────────────────

namespace codegen {

/// @brief Map a runtime error kind to its compile-time-diagnostic DiagCode.
///
/// This is what lets a *runtime* panic (emitted by CodeGenPanic.cpp into the
/// compiled program) carry the same code a *compile-time* diagnostic for the
/// identical error would use - e.g. a literal `x / 0` caught during const
/// evaluation reports `DiagCode::Sem_DivisionByZero`, and a runtime `x / y`
/// check that fires at runtime embeds that exact same code into the panic
/// message via this mapping. Several codes below (see DiagCode.hpp's
/// "Added for RuntimeErrorKind" comments) exist purely to give a runtime-only
/// error - one with no compile-time equivalent, like a failed heap
/// allocation - a stable code of its own, per the "represents WHAT the error
/// is, not WHEN it occurs" design decision in DiagCode.hpp.
///
/// Several kinds intentionally share one DiagCode (e.g. DivisionByZero and
/// ModuloByZero both map to Sem_DivisionByZero) where the existing
/// compile-time code was already documented as covering both.
inline DiagCode toDiagCode(RuntimeErrorKind kind) {
    switch (kind) {
        // ─── Arithmetic Errors ────────────────────────────────────────────
        case RuntimeErrorKind::DivisionByZero:          return DiagCode::Sem_DivisionByZero;
        case RuntimeErrorKind::ModuloByZero:            return DiagCode::Sem_DivisionByZero;
        case RuntimeErrorKind::IntegerOverflow:         return DiagCode::Sem_IntegerOverflow;
        case RuntimeErrorKind::NegationOverflow:        return DiagCode::Sem_IntegerOverflow;

        // ─── Array/Slice Errors ───────────────────────────────────────────
        case RuntimeErrorKind::ArrayIndexOutOfBounds:   return DiagCode::Sem_ArrayIndexOutOfBounds;
        case RuntimeErrorKind::SliceBoundsOutOfRange:   return DiagCode::Sem_SliceBoundsOutOfRange;
        case RuntimeErrorKind::NegativeArraySize:       return DiagCode::Sem_NegativeArraySize;

        // ─── Pointer Errors ───────────────────────────────────────────────
        case RuntimeErrorKind::NullPointerDereference:  return DiagCode::Sem_PtrDeref;
        case RuntimeErrorKind::DanglingPointer:         return DiagCode::Sem_DanglingPointer;

        // ─── Memory Errors ────────────────────────────────────────────────
        case RuntimeErrorKind::DoubleFree:              return DiagCode::Sem_DoubleFree;
        case RuntimeErrorKind::FreeNullPointer:         return DiagCode::Sem_FreeNullPointer;
        case RuntimeErrorKind::AllocationFailed:        return DiagCode::Sem_AllocationFailed;
        case RuntimeErrorKind::ArenaAllocationFailed:   return DiagCode::Sem_ArenaAllocationFailed;
        case RuntimeErrorKind::ArenaInvalidDescriptor:  return DiagCode::Sem_ArenaInvalidDescriptor;

        // ─── Type System Errors ───────────────────────────────────────────
        case RuntimeErrorKind::UnwrappedNil:            return DiagCode::Sem_UnhandledNil;
        case RuntimeErrorKind::UnwrappedErr:             return DiagCode::Sem_UnhandledErr;
        case RuntimeErrorKind::TagMismatch:             return DiagCode::Sem_TagMismatch;

        // ─── Foreign Function Errors ──────────────────────────────────────
        case RuntimeErrorKind::ForeignCallFailed:       return DiagCode::Ffi_CallFailed;
        case RuntimeErrorKind::ForeignSymbolNotFound:   return DiagCode::Ffi_SymbolNotFound;

        // ─── Concurrency Errors ───────────────────────────────────────────
        case RuntimeErrorKind::AwaitOnNonFuture:        return DiagCode::Sem_AwaitNonAsync;
        case RuntimeErrorKind::JoinOnNonThread:         return DiagCode::Sem_JoinNonSpawn;
        case RuntimeErrorKind::FutureAlreadyConsumed:   return DiagCode::Sem_DoubleAwait;
        case RuntimeErrorKind::ThreadAlreadyJoined:     return DiagCode::Sem_DoubleJoin;

        // ─── Runtime Library Errors ───────────────────────────────────────
        case RuntimeErrorKind::RuntimePanic:            return DiagCode::Sem_RuntimePanic;
        case RuntimeErrorKind::AssertionFailed:         return DiagCode::Sem_AssertionFailed;

        default:                                        return DiagCode::Sem_RuntimePanic;
    }
}

} // namespace codegen
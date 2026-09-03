/**
 * @file DiagCode.hpp
 * @brief The diagnostic code space - severity levels and error/warning codes.
 *
 * Split out of Diagnostic.hpp so this data can be depended on without pulling
 * in DiagnosticEngine (which needs BaseAST, StringPool, and <ostream>).
 * DiagCode is plain, state-free data - a `RuntimeErrorKind` (see
 * codegen/support/RuntimeError.hpp) can map to a DiagCode and embed its
 * numeric value into a compiled program's panic message without needing
 * DiagnosticEngine, StringPool, or any other part of the compiler's own
 * process to be alive - which matters for AOT-compiled binaries that run
 * standalone, long after the compiler that produced them has exited.
 *
 * @design_decision Code ranges
 *   1000-1999: Lexical
 *   2000-2999: Syntax
 *   3000-3999: Semantic - Name Resolution
 *   4000-4999: Semantic - Type Checking
 *   5000-5999: Semantic - Generics/Traits/FFI
 *   6000-6999: Semantic - Other
 *   7000-7999: Backend
 *   8000-8999: Warnings (cross-cutting)
 */

#pragma once

#include <cstdint>

// ─── Severity ──────────────────────────────────────────────────────────────

enum class Severity : uint8_t {
    Hint    = 0,
    Note    = 1,
    Warning = 2,
    Error   = 3,
    Fatal   = 4,
};

inline const char* severityName(Severity s) {
    switch (s) {
        case Severity::Hint:    return "HINT";
        case Severity::Note:    return "NOTE";
        case Severity::Warning: return "WARNING";
        case Severity::Error:   return "ERROR";
        case Severity::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

// ─── Diagnostic Codes ──────────────────────────────────────────────────────

/// @brief Unique diagnostic codes with clear category prefixes.
///
/// @design_decision Error codes represent WHAT the error is, not WHEN it occurs.
///   - A division by zero is a division by zero whether at compile-time or runtime
///   - A type mismatch is a type mismatch whether in const eval or normal code
///   - This eliminates duplicate codes and enables unified error handling
///
///   This applies across the compile-time/runtime boundary too: a handful of
///   codes below (see the "Added for RuntimeErrorKind" comments) exist purely
///   so a *runtime* panic (RuntimeErrorKind, emitted by CodeGenPanic.cpp) can
///   carry the same code space as a compile-time diagnostic, even when no
///   compile-time diagnostic naturally produces that code today.
///
/// @design_decision Code Ranges by Semantic Category
///   1000-1999: Lexical Errors
///   2000-2999: Syntax Errors  
///   3000-3999: Name Resolution
///   4000-4999: Type & Value Errors (compile-time + runtime)
///   5000-5499: FFI/Foreign Errors (compile-time + runtime)
///   5500-5999: Control Flow & Concurrency
///   6000-6499: Generics & Traits
///   6500-6999: Memory & Ownership
///   7000-7999: Backend & Linking (truly phase-specific)
///   8000-8999: Warnings (cross-cutting)
enum class DiagCode : uint32_t {
    // ──────────────────────────────────────────────────────────────────────────
    // LEXICAL ERRORS (1000-1999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Lex_InvalidCharacter         = 1001,
    Lex_UnterminatedString       = 1002,
    Lex_UnterminatedRawString    = 1003,
    Lex_UnterminatedBlockComment = 1004,
    Lex_UnknownCharacter         = 1005,
    Lex_InvalidEscapeSequence    = 1006,
    Lex_InvalidNumberLiteral     = 1007,
    Lex_UnterminatedCharLiteral  = 1008,

    // ──────────────────────────────────────────────────────────────────────────
    // SYNTAX ERRORS (2000-2999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Syntax_ExpectedIdentifier               = 2001,
    Syntax_ExpectedType                     = 2002,
    Syntax_ExpectedToken                    = 2003,
    Syntax_UnexpectedToken                  = 2004,
    Syntax_ExpectedExpression               = 2005,
    Syntax_ExpectedBlock                    = 2006,
    Syntax_MultipleDefaults                 = 2007,
    Syntax_EmptyGroup                       = 2008,
    Syntax_ExpectedPipelineSeed             = 2009,
    Syntax_ExpectedModulePath               = 2010,
    Syntax_ExpectedAttributeLiteral         = 2011,
    Syntax_ExpectedSwitchSubject            = 2012,
    Syntax_ExpectedCaseValue                = 2013,
    Syntax_ExpectedForBinding               = 2014,
    Syntax_ExpectedRangeBound               = 2015,
    Syntax_InvalidAttributeTarget           = 2016,
    Syntax_MissingAttributeArgs             = 2017,
    Syntax_TrailingComma                    = 2018,
    Syntax_ExpectedLiteral                  = 2019,
    Syntax_UnexpectedColonAfterField        = 2020,
    Syntax_AnonymousFunctionAtDeclaration   = 2021,
    Syntax_IncompleteDeclaration            = 2022,

    // ──────────────────────────────────────────────────────────────────────────
    // NAME RESOLUTION ERRORS (3000-3999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Sem_UndefinedValue            = 3001,
    Sem_UndefinedType             = 3002,
    Sem_NotCallable               = 3003,
    Sem_Redeclaration             = 3004,
    Sem_UndefinedModule           = 3005,
    Sem_UndefinedMember           = 3006,
    Sem_GenericParamUnused        = 3007,
    Sem_TraitNotFound             = 3008,
    Sem_NotATrait                 = 3009,
    Sem_FieldNotFound             = 3010,
    Sem_GenericParamRedeclaration = 3011,
    Sem_ImportAliasRedeclaration  = 3012,
    Sem_ModuleNotAnalyzed         = 3013,
    Sem_AmbiguousName             = 3014,
    Sem_PrivateMember             = 3015,
    Sem_ModuleCycle               = 3016,

    // ──────────────────────────────────────────────────────────────────────────
    // TYPE & VALUE ERRORS (4000-4999)
    // These apply to BOTH compile-time and runtime evaluation.
    // ──────────────────────────────────────────────────────────────────────────
    
    // Type System (4000-4099)
    Sem_TypeMismatch             = 4001,
    Sem_ArgCountMismatch         = 4002,
    Sem_MissingInitializer       = 4003,
    Sem_ConstNullable            = 4004,
    Sem_MissingReturn            = 4005,
    Sem_DuplicateValue           = 4006,
    Sem_UnknownIntrinsic         = 4007,
    Sem_InvalidSwitchType        = 4009,
    Sem_MissingCase              = 4010,
    Sem_PipelineMismatch         = 4011,
    Sem_CompositionMismatch      = 4012,
    Sem_RefInStruct              = 4013,
    Sem_IllegalNilErr            = 4014,
    Sem_InvalidGenericArg        = 4015,
    Sem_UnknownType              = 4016,
    Sem_InvalidArrayElement      = 4017,
    Sem_RefInArray               = 4018,
    Sem_FunctionNullable         = 4019,
    Sem_ArrayNullable            = 4020,
    Sem_RefToTrait               = 4021,
    Sem_InvalidPointerTarget     = 4022,
    Sem_InvalidParamType         = 4023,
    Sem_InvalidReturnType        = 4024,
    Sem_ReturnRef                = 4025,
    Sem_TraitInvalidContext      = 4026,
    Sem_GenericParamNotCallable  = 4027,
    Sem_SelfReferentialInit      = 4028,
    Sem_InvalidAssignment        = 4029,
    Sem_InvalidUnary             = 4030,
    Sem_InvalidBinary            = 4031,
    Sem_InvalidRange             = 4032,
    // Added for RuntimeErrorKind - runtime array/slice checks with no prior
    // compile-time code (a literal-index compile-time OOB check would reuse
    // one of these too, since "index out of bounds" is the same error at
    // either time - see the design decision above).
    Sem_ArrayIndexOutOfBounds    = 4033,
    Sem_SliceBoundsOutOfRange    = 4034,
    Sem_NegativeArraySize        = 4035,
    
    // Arithmetic & Numeric Errors (shared with runtime)
    Sem_DivisionByZero           = 4101,  // Division or modulo by zero
    Sem_IntegerOverflow          = 4102,  // Integer overflow
    Sem_InvalidShift             = 4103,  // Invalid shift operation
    Sem_NegativeShift            = 4104,  // Negative shift amount
    Sem_InvalidCast              = 4105,  // Invalid type cast/conversion
    Sem_CircularDependency       = 4106,  // Circular dependency (const or otherwise)
    Sem_InvalidBitwiseOp         = 4107,  // Bitwise op on non-integer
    Sem_InvalidLogicalOp         = 4108,  // Logical op on non-bool
    Sem_NumericOverflow          = 4109,  // General numeric overflow
    Sem_InvalidIterator          = 4110,  // Invalid for-loop iterator
    
    // Assignment & Mutability (4200-4299)
    Sem_ConstAssignment          = 4200,  // Assigning to const variable
    Sem_ReadOnlyField            = 4201,  // Assigning to const struct field
    Sem_ModuleReadOnly           = 4202,  // Assigning to module member
    Sem_NonLValueAssignment      = 4203,  // Assignment to non-lvalue
    Sem_ConstParamAssignment     = 4204,  // Assigning to const parameter
    Sem_MissingFuncBody          = 4205,  // The function was initialized with missing body
    
    // Fallible/Nullable Errors (4300-4399)
    Sem_UnhandledNil             = 4300,  // Nil not handled
    Sem_UnhandledErr             = 4301,  // Err not handled
    Sem_InvalidNilCheck          = 4302,  // Nil check on non-nullable
    Sem_InvalidErrCheck          = 4303,  // Err check on non-fallible
    Sem_NilInConst               = 4304,  // Nil not allowed in const context
    Sem_ErrInConst               = 4305,  // Err not allowed in const context


    // Arena & Built-in Types (4400-4499)
    Sem_ConstRequired             = 4400,  // const required for Arena binding
    Sem_InvalidArenaInit          = 4401,  // Invalid Arena initializer
    Sem_InvalidArenaAccess        = 4402,  // Invalid Arena access (wrong form)
    Sem_UnknownMethod             = 4403,  // Unknown method on builtin type
    Sem_ArenaMethodArgCount       = 4404,  // Wrong argument count for Arena method
    Sem_ArenaMethodGenericArg     = 4405,  // Missing or extra generic argument for Arena method
    Sem_ArenaMethodStatic         = 4406,  // Static/instance method mismatch
    Sem_ArenaMethodNotFound       = 4407,  // Arena method not found
    Sem_ArenaInvalidLHS           = 4408,  // Invalid LHS for Arena access
    Sem_ArenaNotConst             = 4409,  // Arena binding not const
    Sem_ArenaDescriptorLiteral    = 4410,  // ArenaDescriptor cannot be constructed via literal
    Sem_ArenaDescriptorNotFound   = 4411,  // ArenaDescriptor type not found
    Sem_ArenaAllocNoGenericArg    = 4412,  // arena::alloc<T> missing type argument
    Sem_ArenaSpaceNoGenericArg    = 4413,  // arena::space<T> missing type argument
    Sem_ArenaCanFitNoGenericArg   = 4414,  // arena::canFit<T> missing type argument
    Sem_ArenaCannotFit            = 4415,  // Not enough capacity in arena
    Sem_ArenaEmptyCapacity        = 4416,  // Arena::create(0) is invalid
    Sem_ArenaCapacityOverflow     = 4417,  // Requested allocation exceeds arena capacity
    Sem_ArenaInvalidInit          = 4418,  // Invalid initial value for the arena

    // Built-in Types (4450-4499)
    Sem_BuiltinTypeMisuse         = 4450,  // Built-in type used incorrectly
    Sem_BuiltinTypeNotConstructible = 4451,  // Built-in type cannot be constructed
    Sem_BuiltinTypeNoUserDef      = 4452,  // Built-in type cannot be redefined by user
    Sem_BuiltinFieldNotFound      = 4453,  // Field not found on built-in type
    Sem_BuiltinMethodNotFound     = 4454,  // Method not found on built-in type
    Sem_BuiltinTypeMismatch       = 4455,  // Expected built-in type, got something else

    // ──────────────────────────────────────────────────────────────────────────
    // FFI/FOREIGN ERRORS (5000-5499)
    // These can happen at compile-time OR runtime.
    // ──────────────────────────────────────────────────────────────────────────
    
    Ffi_UnknownSymbol        = 5001,  // Symbol not found (link-time error)
    Ffi_SymbolMismatch       = 5002,  // Symbol type/signature mismatch
    Ffi_ABIIncompatible      = 5003,  // ABI/calling convention mismatch
    Ffi_InvalidPointer       = 5004,  // Invalid pointer usage (e.g., &T vs *T)
    Ffi_InvalidForeign       = 5005,  // Invalid @[foreign] declaration
    Ffi_ConstContext         = 5006,  // Foreign call in const context (can't eval)
    Ffi_TypeNotFFI           = 5007,  // Type can't be passed to FFI
    Ffi_UnsafeReturn         = 5008,  // Returning pointer to stack memory
    Ffi_LibraryNotFound      = 5009,  // Library not found at link time
    Ffi_SymbolNotFound       = 5010,  // Symbol not found in library
    Ffi_InvalidABIAttribute  = 5011,  // Invalid ABI attribute value
    Ffi_MissingLibrary       = 5012,  // Missing library specification
    Ffi_UnsupportedType      = 5013,  // Type not supported by FFI
    Ffi_InvalidParamPassing  = 5014,  // Invalid parameter passing mode
    Ffi_VariadicMismatch     = 5015,  // Variadic argument mismatch
    Ffi_ReturnMismatch       = 5016,  // Return type mismatch with C
    Ffi_SizeMismatch         = 5017,  // Size mismatch for struct/union
    // Added for RuntimeErrorKind::ForeignCallFailed - the call itself
    // failing at runtime, distinct from Ffi_SymbolNotFound (couldn't even
    // find the symbol) and the compile-time signature/ABI checks above.
    Ffi_CallFailed           = 5018,

    // ──────────────────────────────────────────────────────────────────────────
    // CONTROL FLOW & CONCURRENCY ERRORS (5500-5999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Sem_InvalidBreak          = 5501,  // Break outside loop
    Sem_InvalidContinue       = 5502,  // Continue outside loop
    Sem_UnawaitedAsync        = 5503,  // Async never awaited (warning)
    Sem_UnjoinedSpawn         = 5504,  // Spawn never joined (warning)
    Sem_AsyncOutsideFunction  = 5505,  // Async outside function body
    Sem_SpawnOutsideFunction  = 5506,  // Spawn outside function body
    Sem_AwaitOutsideFunction  = 5507,  // Await outside function body
    Sem_JoinOutsideFunction   = 5508,  // Join outside function body
    Sem_AwaitNonAsync         = 5509,  // Await on non-async value
    Sem_JoinNonSpawn          = 5510,  // Join on non-spawn value
    Sem_DoubleAwait           = 5511,  // Awaiting already awaited value
    Sem_DoubleJoin            = 5512,  // Joining already joined value
    Sem_ReturnInAsync         = 5513,  // Return in async context
    Sem_ReturnInSpawn         = 5514,  // Return in spawn context
    Sem_SwitchExhaustive      = 5515,  // Switch not exhaustive
    Sem_DefaultNotLast        = 5516,  // Default clause not last
    Sem_DuplicateCase         = 5517,  // Duplicate case value

    // ──────────────────────────────────────────────────────────────────────────
    // GENERICS & TRAITS ERRORS (6000-6499)
    // ──────────────────────────────────────────────────────────────────────────

    Sem_GenericArityMismatch   = 6001,
    Sem_GenericConstraint      = 6002,
    Sem_TraitImplementation    = 6003,
    Sem_TraitConflict          = 6004,
    Sem_ForeignInvalid         = 6005,
    Sem_ForeignABI             = 6006,
    Sem_AttributeInvalid       = 6007,
    Sem_AttributeArgCount      = 6008,
    Sem_UnknownAttribute       = 6009,
    Sem_GenericParamRequired   = 6010,
    Sem_GenericParamInference  = 6011,
    Sem_GenericInstantiate     = 6012,
    Sem_TraitFieldMissing      = 6013,
    Sem_TraitFieldTypeMismatch = 6014,
    Sem_TraitConstMismatch     = 6015,
    Sem_TraitDuplicate         = 6016,
    Sem_GenericCycle           = 6017,
    Sem_AttributeArgValue      = 6018,
    Sem_AttributeDuplicate     = 6019, // Duplicate attribute on same declaration
    Sem_AttributeNotApplicable = 6020, // attribute doesn't apply to this declaration

    // ──────────────────────────────────────────────────────────────────────────
    // MEMORY & OWNERSHIP ERRORS (6500-6999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Sem_InvalidRef            = 6501,  // Invalid reference
    Sem_RefEscape             = 6502,  // Reference escapes scope
    Sem_UseAfterFree          = 6503,  // Use after free
    Sem_DoubleFree            = 6504,  // Double free
    Sem_InvalidPtr            = 6505,  // Invalid pointer operation
    Sem_PtrArithmetic         = 6506,  // Invalid pointer arithmetic
    Sem_PtrDeref              = 6507,  // Invalid pointer dereference
    Sem_RefToStack            = 6508,  // Reference to stack-allocated data
    Sem_MoveAfterUse          = 6509,  // Move after use
    Sem_UninitVariable        = 6510,  // Uninitialized variable
    Sem_InvalidCapture        = 6511,  // Can't capture borrowed type in closure
    // Added for RuntimeErrorKind values with no prior compile-time code.
    Sem_DanglingPointer        = 6512,  // Dereferencing a dangling pointer
    Sem_FreeNullPointer        = 6513,  // #free called on a null pointer
    Sem_AllocationFailed       = 6514,  // Runtime memory allocation failed
    Sem_ArenaAllocationFailed  = 6515,  // Runtime arena allocation failed
    Sem_ArenaInvalidDescriptor = 6516,  // Invalid arena descriptor used at runtime
    Sem_ArenaOutOfCapacity     = 6517,  // Arena out of remaining capacity
    Sem_TagMismatch            = 6518,  // Tagged slot tag mismatch
    Sem_RuntimePanic           = 6519,  // Generic/uncategorized runtime panic
    Sem_AssertionFailed        = 6520,  // #assert failed at runtime
    Sem_UnsupportedOperation   = 6521,  // Unsupported operation at runtime

    // ──────────────────────────────────────────────────────────────────────────
    // BACKEND & LINKING ERRORS (7000-7999)
    // These are truly phase-specific - only happen during codegen/linking.
    // ──────────────────────────────────────────────────────────────────────────
    
    Backend_LinkerError       = 7001,
    Backend_CodegenError      = 7002,
    Backend_TargetUnsupported = 7003,
    Backend_OutOfMemory       = 7004,
    Backend_InvalidIR         = 7005,
    Backend_OptimizerError    = 7006,
    Backend_ObjectWriteError  = 7007,
    Backend_AsmError          = 7008,
    Backend_RelocationError   = 7009,

    // ──────────────────────────────────────────────────────────────────────────
    // WARNINGS (8000-8999)
    // Cross-cutting - all phases can emit these.
    // ──────────────────────────────────────────────────────────────────────────
    
    Warn_UnreachableCode   = 8001,
    Warn_UnusedVariable    = 8002,
    Warn_UnusedParameter   = 8003,
    Warn_UnusedFunction    = 8004,
    Warn_Deprecated        = 8005,
    Warn_UnawaitedAsync    = 8006,
    Warn_UnjoinedSpawn     = 8007,
    Warn_UnreachableCase   = 8008,
    Warn_DiscardedResult   = 8009,
    Warn_RedundantNilCheck = 8010,
    Warn_ShadowedName      = 8011,
    Warn_UnusedImport      = 8012,
    Warn_UnusedType        = 8013,
    Warn_UnusedField       = 8014,
    Warn_IneffectiveConst  = 8015,  // Const doesn't help optimization
    Warn_PotentialOverflow = 8016,  // Potential integer overflow
    Warn_Fallthrough       = 8017,  // Missing case in switch
    Warn_UnsafeFFI         = 8018,  // Unsafe FFI usage
    Warn_ForeignBody       = 8019,  // Foreign function has a body
    Warn_ForeignInline     = 8020,  // Cannot inline foreign function
    Warn_ArenaSmallCapacity = 8021,
};


// ─── Helpers ─────────────────────────────────────────────────────────────

/// Get the category name from an error code.
inline const char* categoryName(DiagCode code) {
    uint32_t raw = static_cast<uint32_t>(code);
    if (raw >= 1000 && raw < 2000) return "Lexical";
    if (raw >= 2000 && raw < 3000) return "Syntax";
    if (raw >= 3000 && raw < 7000) return "Semantic";
    if (raw >= 7000 && raw < 8000) return "Backend";
    if (raw >= 8000 && raw < 9000) return "Warning";
    return "Unknown";
}

/// Get the severity from an error code.
inline Severity severityFromCode(DiagCode code) {
    uint32_t raw = static_cast<uint32_t>(code);
    if (raw >= 8000) return Severity::Warning;
    return Severity::Error;
}

/// Check if a code is a warning.
inline bool isWarningCode(DiagCode code) {
    return static_cast<uint32_t>(code) >= 8000;
}

/// Check if a code is an error.
inline bool isErrorCode(DiagCode code) {
    return !isWarningCode(code);
}
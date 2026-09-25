/**
 * @file DiagCode.hpp
 * @brief The diagnostic code space - severity levels, categories, and codes.
 *
 * Split out of Diagnostic.hpp so this data can be depended on without pulling
 * in DiagnosticEngine (which needs BaseAST, StringPool, and <ostream>).
 * DiagCode is plain, state-free data: a runtime panic (a value the interpreter
 * carries in its diagnostic slot) can carry a DiagCode and embed its numeric
 * value without needing the compiler's DiagnosticEngine, StringPool, or any
 * other part of the compiler's own process to be alive.
 *
 * ─── Design: codes describe WHAT, not WHEN ────────────────────────────────
 * A code names a *concept*, not a pipeline stage. "Division by zero" is the
 * same code whether it is caught at compile time by constant folding, or at
 * runtime by the interpreter. "Unconsumed Deferred<T>" is the same code
 * whether it is reported by Sema on a source file or by an LSP diagnostic
 * on a buffer. The band a code lives in groups concepts that belong
 * together, and the bands are ordered so a reader can infer the phase a
 * code is most likely emitted from, but no code is *defined* by its phase.
 *
 * ─── Design: the code space reflects the base product ─────────────────────
 * Lucid is a bytecode-interpreted embedded scripting language. There is no
 * LLVM, no linker, no object files, no AOT compiler in the base product.
 * The code space therefore has no "linker" band and no "IR" band. What a
 * systems-language diagnostic space would spend on ABI mismatch and library
 * resolution, Lucid spends on host-registry resolution and bytecode-format
 * validation.
 *
 * ─── Categories and severity ──────────────────────────────────────────────
 * The numeric ranges below are the contract. Severity is a pure function of
 * the code's range (see severityFromCode): 8000+ is a warning, everything
 * else is an error. Category is a pure function of the code's range (see
 * categoryFromCode).
 *
 *   1000-1999  Lexical        - lexer-level errors
 *   2000-2999  Syntax         - parser-level errors
 *   3000-3999  Name           - name resolution: undefined, redeclared, etc.
 *   4000-4499  Type           - type system: mismatch, arity, nullability
 *   4500-4699  Value          - values and numerics: div-by-zero, overflow
 *   4700-4799  Mutability     - const/let violations, non-lvalue assignment
 *   4800-4899  Sentinel       - nil/err/narrowing: unconsumed, invalid check
 *   5000-5199  Host           - #host / #native / #builtin registration
 *   5200-5299  Concurrency    - async/spawn/start/await/cancel, Deferred<T>
 *   5300-5399  Generics       - generic arity, constraints, instantiation
 *   5400-5499  Traits         - trait conformance, missing/extra members
 *   5500-5599  Attributes     - attribute placement, args, unknown attribute
 *   5600-5799  Memory         - linear values, allocation, runtime panics
 *   6000-6999  Bytecode       - bytecode format, module loading, serialization
 *   8000-8999  Warnings       - cross-cutting warnings (see sub-bands below)
 *
 * Warnings are further subdivided so "which phase emits this" is visible
 * from the number:
 *   8000-8099  Warnings - general (unreachable, unused, shadowed)
 *   8100-8199  Warnings - types (redundant checks, ineffective const)
 *   8200-8299  Warnings - concurrency (unawaited handle, spawn discard)
 *   8300-8399  Warnings - host (foreign-body, inline-foreign)
 *   8400-8499  Warnings - attributes (deprecated, unknown placement)
 */

#pragma once

#include <cstdint>

namespace lucid::diag {

// ─────────────────────────────────────────────────────────────────────────────
// Severity
// ─────────────────────────────────────────────────────────────────────────────

enum class Severity : uint8_t {
    Hint    = 0,
    Note    = 1,
    Warning = 2,
    Error   = 3,
    Fatal   = 4,
};

inline const char* severityName(Severity s) noexcept {
    switch (s) {
        case Severity::Hint:    return "HINT";
        case Severity::Note:    return "NOTE";
        case Severity::Warning: return "WARNING";
        case Severity::Error:   return "ERROR";
        case Severity::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────────────────────
// Category
// ─────────────────────────────────────────────────────────────────────────────

enum class DiagCategory : uint8_t {
    Lexical,
    Syntax,
    Name,
    Type,
    Value,
    Mutability,
    Sentinel,
    Host,
    Concurrency,
    Generics,
    Traits,
    Attributes,
    Memory,
    Bytecode,
    Warning,
    Internal,   // reserved for codes outside the user-facing space (code 0)
    Unknown,
};

inline const char* categoryName(DiagCategory c) noexcept {
    switch (c) {
        case DiagCategory::Lexical:     return "Lexical";
        case DiagCategory::Syntax:      return "Syntax";
        case DiagCategory::Name:        return "Name";
        case DiagCategory::Type:        return "Type";
        case DiagCategory::Value:       return "Value";
        case DiagCategory::Mutability:  return "Mutability";
        case DiagCategory::Sentinel:    return "Sentinel";
        case DiagCategory::Host:        return "Host";
        case DiagCategory::Concurrency: return "Concurrency";
        case DiagCategory::Generics:    return "Generics";
        case DiagCategory::Traits:      return "Traits";
        case DiagCategory::Attributes:  return "Attributes";
        case DiagCategory::Memory:      return "Memory";
        case DiagCategory::Bytecode:    return "Bytecode";
        case DiagCategory::Warning:     return "Warning";
        case DiagCategory::Internal:    return "Internal";
        case DiagCategory::Unknown:     return "Unknown";
    }
    return "Unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// DiagCode
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Unique diagnostic codes.
///
/// @note Code 0 is reserved for free-text notes and hints that have no
///       diagnostic identity of their own. It is not a member of the enum.
enum class DiagCode : uint32_t {

    // ─────────────────────────────────────────────────────────────────────────
    // LEXICAL (1000-1999)
    // ─────────────────────────────────────────────────────────────────────────

    Lex_InvalidCharacter         = 1001,
    Lex_UnknownCharacter         = 1002,
    Lex_UnterminatedString       = 1003,
    Lex_UnterminatedRawString    = 1004,
    Lex_UnterminatedCharLiteral  = 1005,
    Lex_UnterminatedBlockComment = 1006,
    Lex_InvalidEscapeSequence    = 1007,
    Lex_InvalidNumberLiteral     = 1008,
    Lex_InvalidRadixLiteral      = 1009,  // 0x/0b/0o with no digits, bad digit
    Lex_InterpolationInRawString = 1010,  // \(...) in a """...""" literal
    Lex_NewlineInString          = 1011,  // literal newline in a "..." literal

    // ─────────────────────────────────────────────────────────────────────────
    // SYNTAX (2000-2999)
    // ─────────────────────────────────────────────────────────────────────────

    // General
    Syntax_ExpectedIdentifier   = 2001,
    Syntax_ExpectedType         = 2002,
    Syntax_ExpectedExpression   = 2003,
    Syntax_ExpectedToken        = 2004,
    Syntax_UnexpectedToken      = 2005,
    Syntax_ExpectedLiteral      = 2006,
    Syntax_ExpectedBlock        = 2007,
    Syntax_ExpectedModulePath   = 2008,
    Syntax_IncompleteDeclaration = 2009,

    // Declarations and targets
    Syntax_ExpectedDeclTarget       = 2010,  // '=' with no target
    Syntax_InvalidTargetShape       = 2011,  // target is not #host/#native/#builtin/ident/expr/block
    Syntax_AnonymousFunctionInDecl  = 2012,  // bare `(x int) -> ...` where a declaration is expected
    Syntax_MissingBody              = 2013,  // FN/const with no '=' target

    // Function types
    Syntax_MissingFuncShapeMarker   = 2014,  // group with no fn/cls marker
    Syntax_ExpectedFuncGroup        = 2015,  // after fn/cls, no '('

    // Type syntax
    Syntax_InvalidNullableOrder     = 2016,  // `!?` instead of `?!`
    Syntax_ExpectedTypeSuffix       = 2017,  // trailing `?`/`!` with no type
    Syntax_InvalidArraySize         = 2018,  // `[x]` where x is not `*`, `_`, or int

    // Statements and control flow
    Syntax_ExpectedSwitchSubject    = 2019,
    Syntax_ExpectedCaseValue        = 2020,
    Syntax_MultipleDefaults         = 2021,
    Syntax_ExpectedForBinding       = 2022,  // collection for-loop with one binding
    Syntax_ExpectedRangeBound       = 2023,
    Syntax_TrailingComma            = 2024,
    Syntax_UnexpectedColon          = 2025,  // ':' in a place that does not take one

    // Attributes
    Syntax_ExpectedAttributeLiteral = 2026,
    Syntax_MissingAttributeArgs     = 2027,
    Syntax_InvalidAttributeTarget   = 2028,

    // Expressions
    Syntax_ExpectedPipelineSeed     = 2029,
    Syntax_ExpectedPipelineStep     = 2030,
    Syntax_EmptyGroup               = 2031,  // `()` used as a value with no context

    // ─────────────────────────────────────────────────────────────────────────
    // NAME RESOLUTION (3000-3999)
    // ─────────────────────────────────────────────────────────────────────────

    // Values and types
    Name_UndefinedValue             = 3001,
    Name_UndefinedType              = 3002,
    Name_UndefinedModule            = 3003,
    Name_UndefinedMember            = 3004,
    Name_NotCallable                = 3005,
    Name_NotAType                   = 3006,
    Name_NotATrait                  = 3007,
    Name_FieldNotFound              = 3008,
    Name_MethodNotFound             = 3009,
    Name_PrivateMember              = 3010,

    // Declaration collisions
    Name_Redeclaration              = 3011,
    Name_GenericParamRedeclaration  = 3012,
    Name_ImportAliasRedeclaration   = 3013,
    Name_DuplicateValue             = 3014,  // two enum variants, etc.
    Name_AmbiguousName              = 3015,  // overload-resolution ambiguity
    Name_GenericParamUnused         = 3016,

    // Modules
    Name_ModuleCycle                = 3017,
    Name_ModuleNotAnalyzed          = 3018,

    // ─────────────────────────────────────────────────────────────────────────
    // TYPES (4000-4499)
    // ─────────────────────────────────────────────────────────────────────────

    Type_Mismatch                   = 4001,
    Type_ArgCountMismatch           = 4002,  // call-site arity
    Type_MissingInitializer         = 4003,
    Type_MissingReturn              = 4004,  // not all paths return
    Type_ReturnMismatch             = 4005,
    Type_InvalidAssignment          = 4006,
    Type_InvalidUnary               = 4007,
    Type_InvalidBinary              = 4008,
    Type_InvalidRange               = 4009,
    Type_InvalidArrayElement        = 4010,
    Type_InvalidParamType           = 4011,
    Type_InvalidReturnType          = 4012,
    Type_InvalidPointerTarget       = 4013,
    Type_UnknownType                = 4014,
    Type_InvalidGenericArg          = 4015,
    Type_UnknownIntrinsic           = 4016,  // #builtin name not in the registry

    // Function types
    Type_FunctionShapeMismatch      = 4020,  // body captures, declared 'fn'
    Type_FunctionNullable           = 4021,  // `?`/`!` on a function type
    Type_FunctionCurryMismatch      = 4022,  // wrong number of curry stages

    // Sentinels on types
    Type_ArrayNullable              = 4030,  // `?` on an array type, not element
    Type_ConstNullable              = 4031,  // `const` field of nullable type
    Type_SelfReferentialInit        = 4032,  // non-nullable recursive field

    // Switch/match
    Type_InvalidSwitchType          = 4040,  // float/struct/array subject
    Type_MissingCase                = 4041,  // non-exhaustive enum switch
    Type_DuplicateCase              = 4042,
    Type_DefaultNotLast             = 4043,

    // Pipeline
    Type_PipelineMismatch           = 4050,
    Type_CompositionMismatch        = 4051,

    // ─────────────────────────────────────────────────────────────────────────
    // VALUES AND NUMERICS (4500-4699)
    // These apply at both compile time (const-eval) and runtime.
    // ─────────────────────────────────────────────────────────────────────────

    Value_DivisionByZero            = 4501,
    Value_ModuloByZero              = 4502,
    Value_IntegerOverflow           = 4503,
    Value_NumericOverflow           = 4504,  // floats, general
    Value_InvalidCast               = 4505,
    Value_InvalidShift              = 4506,  // shift by >= bit width, negative
    Value_InvalidBitwiseOp          = 4507,  // bitwise on non-integer
    Value_InvalidLogicalOp          = 4508,  // `and`/`or` on non-truthy-able
    Value_InvalidIterator           = 4509,  // for-loop over non-iterable
    Value_CircularDependency        = 4510,  // const-eval cycle
    Value_NilInConst                = 4511,  // nil where const expected
    Value_ErrInConst                = 4512,  // err where const expected
    Value_ArrayIndexOutOfBounds     = 4513,
    Value_SliceBoundsOutOfRange     = 4514,
    Value_NegativeArraySize         = 4515,
    Value_StringIndexOutOfBounds    = 4516,

    // ─────────────────────────────────────────────────────────────────────────
    // MUTABILITY (4700-4799)
    // ─────────────────────────────────────────────────────────────────────────

    Mut_ConstAssignment             = 4701,  // assign to `const` binding
    Mut_ReadOnlyField               = 4702,  // assign to `const` field
    Mut_ConstParamAssignment        = 4703,  // assign to `const` param
    Mut_ModuleReadOnly              = 4704,  // assign to module-level `const`
    Mut_NonLValueAssignment         = 4705,  // assign to a non-lvalue
    Mut_LoopBindingAssignment       = 4706,  // assign to a for-loop binding
    Mut_AssignToVariant             = 4707,  // `Direction.North = ...`

    // ─────────────────────────────────────────────────────────────────────────
    // SENTINELS AND NARROWING (4800-4899)
    // ─────────────────────────────────────────────────────────────────────────

    Sent_UnhandledNil               = 4801,  // `T?` used as `T`
    Sent_UnhandledErr               = 4802,  // `T!` used as `T`
    Sent_UnhandledBoth              = 4803,  // `T?!` used as `T`
    Sent_InvalidNilCheck            = 4804,  // `== nil` on non-nullable
    Sent_InvalidErrCheck            = 4805,  // `== err` on non-fallible
    Sent_IllegalNilErr              = 4806,  // `nil`/`err` where not allowed

    // ─────────────────────────────────────────────────────────────────────────
    // HOST REGISTRY (5000-5199)
    // ─────────────────────────────────────────────────────────────────────────

    Host_SymbolNotRegistered        = 5001,  // #host(name): not in registry
    Host_SymbolSignatureMismatch    = 5002,  // declared vs registered
    Host_KindMismatch               = 5003,  // #host used for a #native name
    Host_NativeInUserScript         = 5004,  // #native only in core scripts
    Host_BuiltinInUserScript        = 5005,  // #builtin only in core scripts
    Host_TypeNotRegistered          = 5006,  // TYPE X = #host(T): not registered
    Host_TypeSignatureMismatch      = 5007,  // TYPE X = #host(T): layout mismatch
    Host_HostOnlyCalledFromLucid    = 5008,  // @[host_only] function called
    Host_MissingRegistration        = 5009,  // registry entry absent at load time

    // ─────────────────────────────────────────────────────────────────────────
    // CONCURRENCY (5200-5299)
    // The Deferred<T> linear-value rules and the async call forms.
    // ─────────────────────────────────────────────────────────────────────────

    Conc_AsyncBareCall              = 5201,  // async f() called without spawn/start/await
    Conc_SpawnNonAsync              = 5202,
    Conc_StartNonAsync              = 5203,
    Conc_AwaitNonDeferred           = 5204,
    Conc_CancelNonDeferred          = 5205,
    Conc_UnconsumedDeferred         = 5206,  // live Deferred<T> at scope exit
    Conc_DoubleConsume              = 5207,  // second await, or await after cancel
    Conc_DeferredInField            = 5208,  // Deferred<T> stored in a struct/array
    Conc_DeferredCaptured           = 5209,  // Deferred<T> captured by a closure
    Conc_DeferredEscapes             = 5210,  // returned from its scope
    Conc_AwaitOutsideAsync          = 5211,  // await at top level or in non-async fn
    Conc_SpawnOutsideFunction       = 5212,
    Conc_StartOutsideFunction       = 5213,
    Conc_AwaitAllEmpty              = 5214,  // `await all()` with no targets
    Conc_AwaitAnyEmpty              = 5215,  // `await any()` with no targets
    Conc_CancelOfResolved           = 5216,  // cancel after the fiber has completed (warning-as-error?)

    // ─────────────────────────────────────────────────────────────────────────
    // GENERICS (5300-5399)
    // ─────────────────────────────────────────────────────────────────────────

    Gen_ArityMismatch               = 5301,  // wrong number of type args
    Gen_ConstraintUnsatisfied       = 5302,  // T does not satisfy C
    Gen_ParamRequired               = 5303,  // bare generic name where a value is needed
    Gen_CannotInfer                 = 5304,  // (reserved: no inference in the language)
    Gen_InstantiationFailed         = 5305,
    Gen_Cycle                       = 5306,
    Gen_RequiresConst               = 5307,  // `let`-bound generic function

    // ─────────────────────────────────────────────────────────────────────────
    // TRAITS (5400-5499)
    // ─────────────────────────────────────────────────────────────────────────

    Trait_NotSatisfied              = 5401,  // constraint site, no satisfy block
    Trait_MissingMember             = 5402,  // REQUIRE clause with no DEF
    Trait_FieldMissing              = 5403,  // FIELD clause with no matching field
    Trait_FieldTypeMismatch         = 5404,
    Trait_ConstMismatch             = 5405,  // trait requires const, field is let
    Trait_Duplicate                 = 5406,  // two satisfy blocks for the same pair
    Trait_ParentUnsatisfied         = 5407,  // `trait X : A` where type fails A
    Trait_SelfReferenceNonNullable  = 5408,  // `FIELD next Self;` (no `?`)
    Trait_NotATrait                 = 5409,  // name does not resolve to a trait
    Trait_ConstraintNotATrait       = 5410,  // `<T : NotATrait>`

    // ─────────────────────────────────────────────────────────────────────────
    // ATTRIBUTES (5500-5599)
    // ─────────────────────────────────────────────────────────────────────────

    Attr_Unknown                    = 5501,
    Attr_InvalidArgCount            = 5502,
    Attr_InvalidArgValue            = 5503,
    Attr_Duplicate                  = 5504,  // same attribute twice
    Attr_NotApplicable              = 5505,  // @[inline] on a struct
    Attr_ExportInLocalScope         = 5506,  // @[export] inside a block
    Attr_ExportOnNonTopLevel        = 5507,  // @[export] on a non-declaration

    // ─────────────────────────────────────────────────────────────────────────
    // MEMORY AND RUNTIME PANICS (5600-5799)
    // Linear values, allocation, and interpreter-level failures.
    // ─────────────────────────────────────────────────────────────────────────

    Mem_InvalidRef                  = 5601,  // malformed reference at runtime
    Mem_DanglingPointer             = 5602,  // dereference of a dangling pointer
    Mem_UseAfterFree                = 5603,
    Mem_DoubleFree                  = 5604,
    Mem_FreeNullPointer             = 5605,  // #free(nil)
    Mem_AllocationFailed            = 5606,
    Mem_UninitVariable              = 5607,
    Mem_TagMismatch                 = 5608,  // payload-enum tag mismatch
    Mem_InvalidPtr                  = 5609,
    Mem_PtrArithmetic               = 5610,
    Mem_PtrDeref                    = 5611,
    Mem_InvalidCapture              = 5612,  // cannot capture borrowed value

    // Generic interpreter panics
    Panic_Generic                   = 5701,  // #builtin(panic_str) / error(msg)
    Panic_AssertionFailed           = 5702,  // #assert at runtime
    Panic_Unreachable               = 5703,  // #builtin(unreachable)
    Panic_UnsupportedOperation      = 5704,  // opcode not implemented for type
    Panic_StackOverflow             = 5705,  // interpreter recursion limit
    Panic_HostCallFailed            = 5706,  // a #host function returned failure

    // ─────────────────────────────────────────────────────────────────────────
    // BYTECODE AND MODULE LOADING (6000-6999)
    // Bytecode-format validation, .lucb loading, serialization.
    // ─────────────────────────────────────────────────────────────────────────

    Bc_FormatVersionMismatch        = 6001,  // .lucb produced by an incompatible version
    Bc_BadMagic                     = 6002,  // not a .lucb file
    Bc_Truncated                    = 6003,  // file ends mid-structure
    Bc_UnknownOpcode                = 6004,  // opcode not in this interpreter's set
    Bc_InvalidConstantRef           = 6005,  // instruction references a missing constant
    Bc_InvalidSlotRef               = 6006,  // instruction references an out-of-frame slot
    Bc_InvalidHostSymbolRef         = 6007,  // instruction references an unregistered symbol
    Bc_InvalidModuleIndex           = 6008,
    Bc_InvalidFunctionIndex         = 6009,
    Bc_HostSymbolUnresolved         = 6010,  // host symbol in the module not in the registry
    Bc_SerializationFailed          = 6011,  // writer error
    Bc_DeserializationFailed        = 6012,  // reader error (not covered above)

    // ─────────────────────────────────────────────────────────────────────────
    // WARNINGS (8000-8999)
    // ─────────────────────────────────────────────────────────────────────────

    // General (8000-8099)
    Warn_UnreachableCode            = 8001,
    Warn_UnusedVariable             = 8002,
    Warn_UnusedParameter            = 8003,
    Warn_UnusedFunction             = 8004,
    Warn_UnusedType                 = 8005,
    Warn_UnusedField                = 8006,
    Warn_UnusedImport               = 8007,
    Warn_ShadowedName               = 8008,
    Warn_DiscardedResult            = 8009,
    Warn_TrivialCondition           = 8010,  // `if x` where x is always true
    Warn_RedundantNilCheck          = 8011,
    Warn_PotentialOverflow          = 8012,
    Warn_IneffectiveConst           = 8013,  // const does not help optimization

    // Types (8100-8199)
    Warn_RedundantCast              = 8101,
    Warn_ImplicitFnToCls            = 8102,  // fn coerced to cls at a call site
    Warn_UnnecessaryCls             = 8103,  // cls declared, body never captures

    // Concurrency (8200-8299)
    Warn_UnawaitedHandle            = 8201,  // Deferred<T> declared, later awaited
    Warn_UnnecessaryAsync           = 8202,  // async function never awaits
    Warn_SpawnDiscard               = 8203,  // spawn's result silently dropped
    Warn_CancelOfCompleted          = 8204,  // cancel on an already-completed fiber

    // Host (8300-8399)
    Warn_ForeignBody                = 8301,  // #host declaration with a Lucid body
    Warn_InlineForeign              = 8302,  // @[inline] on a #host-bound function
    Warn_HostTypeOpaqueField        = 8303,  // non-opaque field on a #host-backed type

    // Attributes (8400-8499)
    Warn_Deprecated                 = 8401,  // @[deprecated] use site
    Warn_DuplicateAttribute         = 8402,  // attribute applied twice, second ignored
    Warn_UnknownAttributeArg        = 8403,  // attribute with unexpected arg
    Warn_CommentStyle               = 8404,  // doc-comment form mismatch
};

// ─────────────────────────────────────────────────────────────────────────────
// Range checks and derived properties
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The numeric value of a code.
inline constexpr uint32_t raw(DiagCode c) noexcept {
    return static_cast<uint32_t>(c);
}

/// @brief The category a code belongs to.
///
/// Pure function of the code's range. Code 0 is Internal (free-text note).
inline constexpr DiagCategory categoryFromCode(DiagCode c) noexcept {
    const uint32_t v = raw(c);
    if (v == 0)          return DiagCategory::Internal;
    if (v < 2000)        return DiagCategory::Lexical;
    if (v < 3000)        return DiagCategory::Syntax;
    if (v < 4000)        return DiagCategory::Name;
    if (v < 4500)        return DiagCategory::Type;
    if (v < 4700)        return DiagCategory::Value;
    if (v < 4800)        return DiagCategory::Mutability;
    if (v < 5000)        return DiagCategory::Sentinel;
    if (v < 5200)        return DiagCategory::Host;
    if (v < 5300)        return DiagCategory::Concurrency;
    if (v < 5400)        return DiagCategory::Generics;
    if (v < 5500)        return DiagCategory::Traits;
    if (v < 5600)        return DiagCategory::Attributes;
    if (v < 6000)        return DiagCategory::Memory;
    if (v < 7000)        return DiagCategory::Bytecode;
    if (v < 9000)        return DiagCategory::Warning;
    return DiagCategory::Unknown;
}

/// @brief The severity a code implies.
///
/// The warning band is the only band that is not an error. `Fatal` is a
/// property a caller can promote an error to at the point of reporting; it
/// is not encoded in the code itself.
inline constexpr Severity severityFromCode(DiagCode c) noexcept {
    return raw(c) >= 8000 ? Severity::Warning : Severity::Error;
}

inline constexpr bool isWarningCode(DiagCode c) noexcept {
    return raw(c) >= 8000 && raw(c) < 9000;
}

inline constexpr bool isErrorCode(DiagCode c) noexcept {
    return !isWarningCode(c) && raw(c) != 0;
}

} // namespace lucid::diag
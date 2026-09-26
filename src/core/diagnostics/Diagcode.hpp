/**
 * @file DiagCode.hpp
 * @brief The diagnostic code space: severity, category, and codes.
 *
 * ─── Design: codes describe what, not when ────────────────────────────────
 * A code names a *concept*, not a pipeline stage. "Division by zero" is
 * the same code whether the constant evaluator catches it at compile
 * time or the interpreter at runtime. "Column name duplicate" is the
 * same code whether Sema reports it on a source file or an LSP reports
 * it on a buffer.
 *
 * ─── Design: the code space reflects the language's concepts ──────────────
 * The bands follow the language's structure: the lexer, the parser, name
 * resolution, the type system, mutability, the host registry, tables,
 * sequences, attributes, bytecode, runtime memory, warnings. Each band
 * is 100 codes; none will be filled.
 *
 * ─── Categories and severity ──────────────────────────────────────────────
 * Severity is a pure function of the code's range: 8000+ is a warning;
 * everything else is an error.
 *
 *   1000-1099  Lexical
 *   2000-2299  Syntax
 *   3000-3199  Name resolution
 *   4000-4299  Type and value
 *   5000-5399  Host, tables, sequences, attributes
 *   6000-6099  Bytecode
 *   7000-7099  Memory and runtime panics
 *   8000-8299  Warnings
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
    Host,
    Table,
    Sequence,
    Attribute,
    Bytecode,
    Memory,
    Warning,
    Internal,   // reserved for free-text notes and hints (code 0)
    Unknown,
};

inline const char* categoryName(DiagCategory c) noexcept {
    switch (c) {
        case DiagCategory::Lexical:   return "Lexical";
        case DiagCategory::Syntax:    return "Syntax";
        case DiagCategory::Name:      return "Name";
        case DiagCategory::Type:      return "Type";
        case DiagCategory::Value:     return "Value";
        case DiagCategory::Mutability: return "Mutability";
        case DiagCategory::Host:      return "Host";
        case DiagCategory::Table:     return "Table";
        case DiagCategory::Sequence:  return "Sequence";
        case DiagCategory::Attribute: return "Attribute";
        case DiagCategory::Bytecode:  return "Bytecode";
        case DiagCategory::Memory:    return "Memory";
        case DiagCategory::Warning:   return "Warning";
        case DiagCategory::Internal:  return "Internal";
        case DiagCategory::Unknown:   return "Unknown";
    }
    return "Unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// DiagCode
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Unique diagnostic codes.
///
/// Code 0 is reserved for free-text notes and hints.
enum class DiagCode : uint32_t {

    // ═════════════════════════════════════════════════════════════════════════
    // LEXICAL (1000-1099)
    // ═════════════════════════════════════════════════════════════════════════

    Lex_InvalidCharacter         = 1001,
    Lex_UnknownCharacter         = 1002,
    Lex_UnterminatedString       = 1003,
    Lex_UnterminatedRawString    = 1004,
    Lex_UnterminatedCharLiteral  = 1005,
    Lex_UnterminatedBlockComment = 1006,
    Lex_InvalidEscapeSequence    = 1007,
    Lex_InvalidNumberLiteral     = 1008,
    Lex_InvalidRadixLiteral      = 1009,
    Lex_NewlineInString          = 1010,

    // ═════════════════════════════════════════════════════════════════════════
    // SYNTAX (2000-2299)
    // ═════════════════════════════════════════════════════════════════════════

    // General
    Syntax_ExpectedIdentifier      = 2001,
    Syntax_ExpectedType            = 2002,
    Syntax_ExpectedExpression      = 2003,
    Syntax_ExpectedToken           = 2004,
    Syntax_UnexpectedToken         = 2005,
    Syntax_ExpectedLiteral         = 2006,
    Syntax_ExpectedBlock           = 2007,
    Syntax_IncompleteDeclaration   = 2008,
    Syntax_TrailingComma           = 2009,
    Syntax_ExpectedModulePath      = 2010,

    // Declarations
    Syntax_ExpectedDeclTarget      = 2101,
    Syntax_ExpectedHostTarget      = 2102,
    Syntax_InvalidTargetShape      = 2103,

    // Tables
    Syntax_ExpectedColumn          = 2201,
    Syntax_ExpectedTableBody       = 2202,
    Syntax_ExpectedRow             = 2203,

    // Statements
    Syntax_ExpectedSwitchSubject   = 2301,
    Syntax_ExpectedCaseValue       = 2302,
    Syntax_ExpectedForBinding      = 2303,
    Syntax_ExpectedRangeBound      = 2304,
    Syntax_MultipleDefaults        = 2305,
    Syntax_ExpectedAttribute       = 2306,

    // ═════════════════════════════════════════════════════════════════════════
    // NAME RESOLUTION (3000-3199)
    // ═════════════════════════════════════════════════════════════════════════

    // Values and types
    Name_UndefinedValue           = 3001,
    Name_UndefinedType            = 3002,
    Name_UndefinedModule          = 3003,
    Name_UndefinedMember          = 3004,
    Name_NotCallable              = 3005,
    Name_NotAType                 = 3006,
    Name_FieldNotFound            = 3007,
    Name_MethodNotFound           = 3008,
    Name_Redeclaration            = 3009,
    Name_PrivateMember            = 3010,

    // Tables
    Name_ColumnNotFound           = 3101,
    Name_ColumnDuplicate          = 3102,

    // ═════════════════════════════════════════════════════════════════════════
    // TYPE AND VALUE (4000-4299)
    // ═════════════════════════════════════════════════════════════════════════

    // Types
    Type_Mismatch                 = 4001,
    Type_ArgCountMismatch         = 4002,
    Type_MissingInitializer       = 4003,
    Type_MissingReturn            = 4004,
    Type_ReturnMismatch           = 4005,
    Type_InvalidAssignment        = 4006,
    Type_InvalidUnary             = 4007,
    Type_InvalidBinary            = 4008,
    Type_InvalidArrayElement      = 4009,
    Type_InvalidParamType         = 4010,
    Type_InvalidReturnType        = 4011,
    Type_UnknownType              = 4012,
    Type_InvalidSwitchType        = 4013,
    Type_DuplicateCase            = 4014,
    Type_RangeBoundTypeMismatch   = 4015,
    Type_RangeStepZero            = 4016,
    Type_InvalidArraySize         = 4017,
    Type_MissingCase              = 4018,   // fixed-table switch warning (see §12.2)

    // Values and numerics
    Value_DivisionByZero          = 4101,
    Value_ModuloByZero            = 4102,
    Value_IntegerOverflow         = 4103,
    Value_NumericOverflow         = 4104,
    Value_InvalidShift            = 4105,
    Value_InvalidBitwiseOp        = 4106,
    Value_ArrayIndexOutOfBounds   = 4107,
    Value_StringIndexOutOfBounds  = 4108,
    Value_NegativeArraySize       = 4109,
    Value_InvalidCast             = 4110,
    Value_CircularDependency      = 4111,
    Value_InvalidIterator         = 4112,

    // Mutability
    Mut_ConstAssignment           = 4201,
    Mut_ConstParamAssignment      = 4202,
    Mut_ReadOnlyField             = 4203,
    Mut_ModuleReadOnly            = 4204,
    Mut_NonLValueAssignment       = 4205,
    Mut_LoopBindingAssignment     = 4206,

    // ═════════════════════════════════════════════════════════════════════════
    // HOST REGISTRY (5000-5099)
    // ═════════════════════════════════════════════════════════════════════════

    Host_SymbolNotRegistered      = 5001,
    Host_SymbolSignatureMismatch  = 5002,
    Host_TypeNotRegistered        = 5003,
    Host_TypeKindMismatch         = 5004,
    Host_HostOnlyCalledFromLucid  = 5005,

    // ═════════════════════════════════════════════════════════════════════════
    // TABLES (5100-5199)
    // ═════════════════════════════════════════════════════════════════════════

    Table_AddArgCountMismatch     = 5101,
    Table_AddArgTypeMismatch      = 5102,
    Table_AddOnFixed              = 5103,
    Table_AddOnReadonly           = 5104,
    Table_AddOnCapped             = 5105,
    Table_RemoveOnFixed           = 5106,
    Table_RemoveOnReadonly        = 5107,
    Table_DuplicateUniqueValue    = 5108,
    Table_IndexOutOfBounds        = 5109,
    Table_MultiplePrimary         = 5110,
    Table_SortedColumnNotFound    = 5111,
    Table_CappedExclusiveWithRows = 5112,
    Table_PackedNonPrimitive      = 5113,
    Table_RequestOnNonHost        = 5114,
    Table_FixedCellNotConstant    = 5115,
    Table_FixedCycle              = 5116,

    // ═════════════════════════════════════════════════════════════════════════
    // SEQUENCES (5200-5299)
    // ═════════════════════════════════════════════════════════════════════════

    Seq_SuspendOutsideSequence    = 5201,
    Seq_SequenceCalledDirectly    = 5202,
    Seq_NonSequenceStarted        = 5203,
    Seq_SequenceReturnsValue      = 5204,
    Seq_SequenceHasHostBody       = 5205,
    Seq_SequenceCallingSequence   = 5206,
    Seq_SequenceAsFunctionValue   = 5207,
    Seq_WaitUntilArgTypeMismatch  = 5208,
    Seq_WaitForEventNotAFixedTable = 5209,
    Seq_WaitForRequestNotARequest = 5210,

    // ═════════════════════════════════════════════════════════════════════════
    // ATTRIBUTES (5300-5399)
    // ═════════════════════════════════════════════════════════════════════════

    Attr_Unknown                  = 5301,
    Attr_InvalidArgCount          = 5302,
    Attr_InvalidArgValue          = 5303,
    Attr_Duplicate                = 5304,
    Attr_NotApplicable            = 5305,
    Attr_ExportInLocalScope       = 5306,
    Attr_OnRequiresExport         = 5307,

    // ═════════════════════════════════════════════════════════════════════════
    // BYTECODE (6000-6099)
    // ═════════════════════════════════════════════════════════════════════════

    Bc_FormatVersionMismatch      = 6001,
    Bc_BadMagic                   = 6002,
    Bc_Truncated                  = 6003,
    Bc_UnknownOpcode              = 6004,
    Bc_InvalidConstantRef         = 6005,
    Bc_InvalidSlotRef             = 6006,
    Bc_InvalidHostSymbolRef       = 6007,
    Bc_InvalidModuleIndex         = 6008,
    Bc_InvalidFunctionIndex       = 6009,
    Bc_HostSymbolUnresolved       = 6010,
    Bc_SerializationFailed        = 6011,
    Bc_DeserializationFailed      = 6012,

    // ═════════════════════════════════════════════════════════════════════════
    // MEMORY AND RUNTIME PANICS (7000-7099)
    // ═════════════════════════════════════════════════════════════════════════

    Mem_AllocationFailed          = 7001,
    Mem_DanglingPointer           = 7002,
    Mem_FreeNullPointer           = 7003,
    Mem_InvalidRef                = 7004,
    Mem_TagMismatch               = 7005,

    Panic_Generic                 = 7101,
    Panic_StackOverflow           = 7102,
    Panic_HostCallFailed          = 7103,
    Panic_UnsupportedOperation    = 7104,

    // ═════════════════════════════════════════════════════════════════════════
    // WARNINGS (8000-8299)
    // ═════════════════════════════════════════════════════════════════════════

    // General (8000-8099)
    Warn_UnreachableCode          = 8001,
    Warn_UnusedVariable           = 8002,
    Warn_UnusedParameter          = 8003,
    Warn_UnusedFunction           = 8004,
    Warn_UnusedType               = 8005,
    Warn_UnusedImport             = 8006,
    Warn_ShadowedName             = 8007,
    Warn_DiscardedResult          = 8008,
    Warn_TrivialCondition         = 8009,
    Warn_RedundantNilCheck        = 8010,
    Warn_PotentialOverflow        = 8011,
    Warn_Deprecated               = 8012,

    // Tables (8100-8199)
    Warn_SwitchMissingMember      = 8101,
    Warn_TableNeverPopulated      = 8102,

    // Sequences (8200-8299)
    Warn_SequenceNeverSuspends    = 8201,
    Warn_SequenceOnlySuspends     = 8202,
};

// ─────────────────────────────────────────────────────────────────────────────
// Derived properties
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr uint32_t raw(DiagCode c) noexcept {
    return static_cast<uint32_t>(c);
}

inline constexpr DiagCategory categoryFromCode(DiagCode c) noexcept {
    const uint32_t v = raw(c);
    if (v == 0)          return DiagCategory::Internal;
    if (v < 2000)        return DiagCategory::Lexical;
    if (v < 3000)        return DiagCategory::Syntax;
    if (v < 4000)        return DiagCategory::Name;
    if (v < 4100)        return DiagCategory::Type;
    if (v < 4200)        return DiagCategory::Value;
    if (v < 5000)        return DiagCategory::Mutability;
    if (v < 5100)        return DiagCategory::Host;
    if (v < 5200)        return DiagCategory::Table;
    if (v < 5300)        return DiagCategory::Sequence;
    if (v < 6000)        return DiagCategory::Attribute;
    if (v < 7000)        return DiagCategory::Bytecode;
    if (v < 8000)        return DiagCategory::Memory;
    if (v < 9000)        return DiagCategory::Warning;
    return DiagCategory::Unknown;
}

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
#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Primitive Type
// ============================================================================
// 
// parsePrimitiveType() parses a primitive type keyword.
// 
// Grammar: `bool` | `int` | `float` | `string` | `any` | ...
// 
// The complete list includes:
//   - Boolean:   bool
//   - Signed:    byte, short, int, long, int8, int16, int32, int64
//   - Unsigned:  ubyte, ushort, uint, ulong, uint8, uint16, uint32, uint64
//   - Floating:  float, double, decimal
//   - Text:      string, char
//   - Dynamic:   any
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at a primitive keyword
// On exit:  positioned after the keyword
// 
// ─── Kind Mapping ─────────────────────────────────────────────────────────
// Maps each TokenType to the corresponding PrimitiveKind enum value.
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Called only on valid primitive tokens (caller guarantees)
// - Internal error: returns UnknownTypeAST if called on non-primitive
// ============================================================================

TypePtr Parser::parsePrimitiveType() {
    SourceLocation loc = ts_.currentLoc();
    Token tok = ts_.advance();

    PrimitiveKind kind;
    switch (tok.type) {
        case TokenType::TYPE_BOOL:   kind = PrimitiveKind::Bool; break;
        case TokenType::TYPE_BYTE:   kind = PrimitiveKind::Byte; break;
        case TokenType::TYPE_SHORT:  kind = PrimitiveKind::Short; break;
        case TokenType::TYPE_INT:    kind = PrimitiveKind::Int; break;
        case TokenType::TYPE_LONG:   kind = PrimitiveKind::Long; break;
        case TokenType::TYPE_UBYTE:  kind = PrimitiveKind::Ubyte; break;
        case TokenType::TYPE_USHORT: kind = PrimitiveKind::Ushort; break;
        case TokenType::TYPE_UINT:   kind = PrimitiveKind::Uint; break;
        case TokenType::TYPE_ULONG:  kind = PrimitiveKind::Ulong; break;
        case TokenType::TYPE_INT8:   kind = PrimitiveKind::Int8; break;
        case TokenType::TYPE_INT16:  kind = PrimitiveKind::Int16; break;
        case TokenType::TYPE_INT32:  kind = PrimitiveKind::Int32; break;
        case TokenType::TYPE_INT64:  kind = PrimitiveKind::Int64; break;
        case TokenType::TYPE_UINT8:  kind = PrimitiveKind::Uint8; break;
        case TokenType::TYPE_UINT16: kind = PrimitiveKind::Uint16; break;
        case TokenType::TYPE_UINT32: kind = PrimitiveKind::Uint32; break;
        case TokenType::TYPE_UINT64: kind = PrimitiveKind::Uint64; break;
        case TokenType::TYPE_FLOAT:  kind = PrimitiveKind::Float; break;
        case TokenType::TYPE_DOUBLE: kind = PrimitiveKind::Double; break;
        case TokenType::TYPE_DECIMAL: kind = PrimitiveKind::Decimal; break;
        case TokenType::TYPE_STRING: kind = PrimitiveKind::String; break;
        case TokenType::TYPE_CHAR:   kind = PrimitiveKind::Char; break;
        case TokenType::TYPE_ANY:    kind = PrimitiveKind::Any; break;
        default:
            errorAt(DiagCode::E2002, "internal error: expected primitive type");
            return arena_.make<UnknownTypeAST>();
    }

    auto node = arena_.make<PrimitiveTypeAST>(kind);
    node->loc = loc;
    return node;
}
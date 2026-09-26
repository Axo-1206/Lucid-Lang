/**
 * @file Tokens.cpp
 * @brief Definitions of the token-name lookup tables.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - tokenTypeName        the canonical spelling of a token type
 *   - tokenTypeDescription the human-readable name for diagnostics
 *
 * Both functions are `switch`es over the `TokenType` enum. They are here
 * rather than in the header because their tables are long, and putting
 * them in the header would pull the whole string table into every
 * translation unit that includes Tokens.hpp.
 *
 * ─── Design: the two tables have different audiences ─────────────────────
 * `tokenTypeName` is for machine-readable output: JSON dumps, trace
 * logs, test assertions. It returns the enum's own name as a string —
 * `"PLUS"`, `"KW_IF"`, `"STRING_LITERAL"`.
 *
 * `tokenTypeDescription` is for human-readable output: the parser's
 * "expected X, found Y" diagnostics. It returns the source spelling for
 * a keyword (`"'if'"`), the operator's own symbol for punctuation
 * (`"'+'"`), and a short English description for the abstract cases
 * (`"identifier"`, `"end of input"`).
 *
 * ─── Design: no default case ──────────────────────────────────────────────
 * Neither switch has a `default:` clause. Both return `<unknown-token>`
 * after the switch, which is unreachable for a well-formed `TokenType`.
 * The absence of a `default:` is deliberate: it lets the compiler warn
 * if a new `TokenType` value is added without a corresponding entry
 * here, which is a bug in the token set, not a runtime condition to
 * handle.
 */

#include "core/Tokens.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// tokenTypeName
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The canonical spelling of a token type.
///
/// Returns the enum's own name as a string. Used by machine-readable
/// output — JSON dumps, trace logs, test assertions — where the exact
/// spelling matters and a human is not the reader.
///
/// Returns a string literal; no allocation.
const char* tokenTypeName(TokenType t) noexcept {
    switch (t) {
        // ─── End of input and error recovery ────────────────────────────
        case TokenType::EOF_TOKEN:              return "EOF_TOKEN";
        case TokenType::UNKNOWN:                return "UNKNOWN";
        case TokenType::IDENTIFIER:             return "IDENTIFIER";

        // ─── Declaration keywords ───────────────────────────────────────
        case TokenType::KW_TABLE:               return "KW_TABLE";
        case TokenType::KW_FN:                  return "KW_FN";
        case TokenType::KW_LET:                 return "KW_LET";
        case TokenType::KW_CONST:               return "KW_CONST";
        case TokenType::KW_IMPORT:              return "KW_IMPORT";
        case TokenType::KW_AS:                  return "KW_AS";
        case TokenType::KW_HOST:                return "KW_HOST";

        // ─── Primitive type keywords ────────────────────────────────────
        case TokenType::KW_BOOL:                return "KW_BOOL";
        case TokenType::KW_CHAR:                return "KW_CHAR";
        case TokenType::KW_STRING:              return "KW_STRING";
        case TokenType::KW_UNIT:                return "KW_UNIT";

        case TokenType::KW_INT8:                return "KW_INT8";
        case TokenType::KW_INT16:               return "KW_INT16";
        case TokenType::KW_INT32:               return "KW_INT32";
        case TokenType::KW_INT64:               return "KW_INT64";
        case TokenType::KW_UINT8:               return "KW_UINT8";
        case TokenType::KW_UINT16:              return "KW_UINT16";
        case TokenType::KW_UINT32:              return "KW_UINT32";
        case TokenType::KW_UINT64:              return "KW_UINT64";
        case TokenType::KW_FLOAT32:             return "KW_FLOAT32";
        case TokenType::KW_FLOAT64:             return "KW_FLOAT64";

        // Sized aliases
        case TokenType::KW_INT:                 return "KW_INT";
        case TokenType::KW_LONG:                return "KW_LONG";
        case TokenType::KW_UINT:                return "KW_UINT";
        case TokenType::KW_ULONG:               return "KW_ULONG";
        case TokenType::KW_FLOAT:               return "KW_FLOAT";
        case TokenType::KW_DOUBLE:              return "KW_DOUBLE";

        // ─── Statement keywords ─────────────────────────────────────────
        case TokenType::KW_IF:                  return "KW_IF";
        case TokenType::KW_ELSE:                return "KW_ELSE";
        case TokenType::KW_SWITCH:              return "KW_SWITCH";
        case TokenType::KW_CASE:                return "KW_CASE";
        case TokenType::KW_DEFAULT:             return "KW_DEFAULT";
        case TokenType::KW_FOR:                 return "KW_FOR";
        case TokenType::KW_IN:                  return "KW_IN";
        case TokenType::KW_WHILE:               return "KW_WHILE";
        case TokenType::KW_RETURN:              return "KW_RETURN";
        case TokenType::KW_BREAK:               return "KW_BREAK";
        case TokenType::KW_CONTINUE:            return "KW_CONTINUE";

        // ─── Sequence keywords ──────────────────────────────────────────
        case TokenType::KW_WAIT:                return "KW_WAIT";
        case TokenType::KW_WAIT_FRAMES:         return "KW_WAIT_FRAMES";
        case TokenType::KW_WAIT_UNTIL:          return "KW_WAIT_UNTIL";
        case TokenType::KW_WAIT_FOR_EVENT:      return "KW_WAIT_FOR_EVENT";
        case TokenType::KW_WAIT_FOR_REQUEST:    return "KW_WAIT_FOR_REQUEST";
        case TokenType::KW_START:               return "KW_START";

        // ─── Operator keywords ──────────────────────────────────────────
        case TokenType::KW_AND:                 return "KW_AND";
        case TokenType::KW_OR:                  return "KW_OR";
        case TokenType::KW_NOT:                 return "KW_NOT";

        // ─── Literal keywords ───────────────────────────────────────────
        case TokenType::KW_TRUE:                return "KW_TRUE";
        case TokenType::KW_FALSE:               return "KW_FALSE";
        case TokenType::KW_NIL:                 return "KW_NIL";

        // ─── Literals with a payload ────────────────────────────────────
        case TokenType::INT_LITERAL:            return "INT_LITERAL";
        case TokenType::FLOAT_LITERAL:          return "FLOAT_LITERAL";
        case TokenType::HEX_LITERAL:            return "HEX_LITERAL";
        case TokenType::BINARY_LITERAL:         return "BINARY_LITERAL";
        case TokenType::OCTAL_LITERAL:          return "OCTAL_LITERAL";
        case TokenType::CHAR_LITERAL:           return "CHAR_LITERAL";
        case TokenType::STRING_LITERAL:         return "STRING_LITERAL";
        case TokenType::RAW_STRING_LITERAL:     return "RAW_STRING_LITERAL";

        // ─── Comments ───────────────────────────────────────────────────
        case TokenType::DOC_COMMENT:            return "DOC_COMMENT";

        // ─── Delimiters ─────────────────────────────────────────────────
        case TokenType::LPAREN:                 return "LPAREN";
        case TokenType::RPAREN:                 return "RPAREN";
        case TokenType::LBRACE:                 return "LBRACE";
        case TokenType::RBRACE:                 return "RBRACE";
        case TokenType::LBRACKET:               return "LBRACKET";
        case TokenType::RBRACKET:               return "RBRACKET";
        case TokenType::COMMA:                  return "COMMA";
        case TokenType::SEMICOLON:              return "SEMICOLON";
        case TokenType::DOT:                    return "DOT";
        case TokenType::COLON:                  return "COLON";
        case TokenType::ARROW:                  return "ARROW";
        case TokenType::VARIADIC:               return "VARIADIC";
        case TokenType::AT_SIGN:                return "AT_SIGN";

        // ─── Operators ──────────────────────────────────────────────────

        // Assignment
        case TokenType::ASSIGN:                 return "ASSIGN";
        case TokenType::PLUS_ASSIGN:            return "PLUS_ASSIGN";
        case TokenType::MINUS_ASSIGN:           return "MINUS_ASSIGN";
        case TokenType::MUL_ASSIGN:             return "MUL_ASSIGN";
        case TokenType::DIV_ASSIGN:             return "DIV_ASSIGN";
        case TokenType::MOD_ASSIGN:             return "MOD_ASSIGN";
        case TokenType::BIT_AND_ASSIGN:         return "BIT_AND_ASSIGN";
        case TokenType::BIT_OR_ASSIGN:          return "BIT_OR_ASSIGN";
        case TokenType::BIT_XOR_ASSIGN:         return "BIT_XOR_ASSIGN";
        case TokenType::SHL_ASSIGN:             return "SHL_ASSIGN";
        case TokenType::SHR_ASSIGN:             return "SHR_ASSIGN";

        // Arithmetic
        case TokenType::PLUS:                   return "PLUS";
        case TokenType::MINUS:                  return "MINUS";
        case TokenType::MUL:                    return "MUL";
        case TokenType::DIV:                    return "DIV";
        case TokenType::MOD:                    return "MOD";
        case TokenType::POW:                    return "POW";

        // Comparison
        case TokenType::EQUAL_EQUAL:            return "EQUAL_EQUAL";
        case TokenType::NOT_EQUAL:              return "NOT_EQUAL";
        case TokenType::LESS:                   return "LESS";
        case TokenType::LESS_EQUAL:             return "LESS_EQUAL";
        case TokenType::GREATER:                return "GREATER";
        case TokenType::GREATER_EQUAL:          return "GREATER_EQUAL";

        // Bitwise
        case TokenType::BIT_AND:                return "BIT_AND";
        case TokenType::BIT_OR:                 return "BIT_OR";
        case TokenType::BIT_XOR:                return "BIT_XOR";
        case TokenType::BIT_NOT:                return "BIT_NOT";
        case TokenType::SHL:                    return "SHL";
        case TokenType::SHR:                    return "SHR";

        // Null coalescing
        case TokenType::QUESTION_QUESTION:      return "QUESTION_QUESTION";
    }
    return "<unknown-token>";
}

// ─────────────────────────────────────────────────────────────────────────────
// tokenTypeDescription
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The human-readable name of a token type, for diagnostics.
///
/// Used by the parser's "expected X, found Y" messages. Returns the
/// source spelling for keywords and punctuation, and a short English
/// description for the abstract cases.
///
/// The spelling convention for keywords and operators is: the source
/// form wrapped in single quotes. That matches the way diagnostics are
/// conventionally written — `expected 'if', got 'else'` — and reads
/// well in a message built by concatenation.
///
/// Returns a string literal; no allocation.
const char* tokenTypeDescription(TokenType t) noexcept {
    switch (t) {
        // ─── End of input and error recovery ────────────────────────────
        case TokenType::EOF_TOKEN:              return "end of input";
        case TokenType::UNKNOWN:                return "unknown token";
        case TokenType::IDENTIFIER:             return "identifier";

        // ─── Declaration keywords ───────────────────────────────────────
        case TokenType::KW_TABLE:               return "'TABLE'";
        case TokenType::KW_FN:                  return "'FN'";
        case TokenType::KW_LET:                 return "'let'";
        case TokenType::KW_CONST:               return "'const'";
        case TokenType::KW_IMPORT:              return "'import'";
        case TokenType::KW_AS:                  return "'as'";
        case TokenType::KW_HOST:                return "'host'";

        // ─── Primitive type keywords ────────────────────────────────────
        case TokenType::KW_BOOL:                return "'bool'";
        case TokenType::KW_CHAR:                return "'char'";
        case TokenType::KW_STRING:              return "'string'";
        case TokenType::KW_UNIT:                return "'unit'";

        case TokenType::KW_INT8:                return "'int8'";
        case TokenType::KW_INT16:               return "'int16'";
        case TokenType::KW_INT32:               return "'int32'";
        case TokenType::KW_INT64:               return "'int64'";
        case TokenType::KW_UINT8:               return "'uint8'";
        case TokenType::KW_UINT16:              return "'uint16'";
        case TokenType::KW_UINT32:              return "'uint32'";
        case TokenType::KW_UINT64:              return "'uint64'";
        case TokenType::KW_FLOAT32:             return "'float32'";
        case TokenType::KW_FLOAT64:             return "'float64'";

        // Sized aliases
        case TokenType::KW_INT:                 return "'int'";
        case TokenType::KW_LONG:                return "'long'";
        case TokenType::KW_UINT:                return "'uint'";
        case TokenType::KW_ULONG:               return "'ulong'";
        case TokenType::KW_FLOAT:               return "'float'";
        case TokenType::KW_DOUBLE:              return "'double'";

        // ─── Statement keywords ─────────────────────────────────────────
        case TokenType::KW_IF:                  return "'if'";
        case TokenType::KW_ELSE:                return "'else'";
        case TokenType::KW_SWITCH:              return "'switch'";
        case TokenType::KW_CASE:                return "'case'";
        case TokenType::KW_DEFAULT:             return "'default'";
        case TokenType::KW_FOR:                 return "'for'";
        case TokenType::KW_IN:                  return "'in'";
        case TokenType::KW_WHILE:               return "'while'";
        case TokenType::KW_RETURN:              return "'return'";
        case TokenType::KW_BREAK:               return "'break'";
        case TokenType::KW_CONTINUE:            return "'continue'";

        // ─── Sequence keywords ──────────────────────────────────────────
        case TokenType::KW_WAIT:                return "'wait'";
        case TokenType::KW_WAIT_FRAMES:         return "'waitFrames'";
        case TokenType::KW_WAIT_UNTIL:          return "'waitUntil'";
        case TokenType::KW_WAIT_FOR_EVENT:      return "'waitForEvent'";
        case TokenType::KW_WAIT_FOR_REQUEST:    return "'waitForRequest'";
        case TokenType::KW_START:               return "'start'";

        // ─── Operator keywords ──────────────────────────────────────────
        case TokenType::KW_AND:                 return "'and'";
        case TokenType::KW_OR:                  return "'or'";
        case TokenType::KW_NOT:                 return "'not'";

        // ─── Literal keywords ───────────────────────────────────────────
        case TokenType::KW_TRUE:                return "'true'";
        case TokenType::KW_FALSE:               return "'false'";
        case TokenType::KW_NIL:                 return "'nil'";

        // ─── Literals with a payload ────────────────────────────────────
        case TokenType::INT_LITERAL:            return "integer literal";
        case TokenType::FLOAT_LITERAL:          return "float literal";
        case TokenType::HEX_LITERAL:            return "hex literal";
        case TokenType::BINARY_LITERAL:         return "binary literal";
        case TokenType::OCTAL_LITERAL:          return "octal literal";
        case TokenType::CHAR_LITERAL:           return "character literal";
        case TokenType::STRING_LITERAL:         return "string literal";
        case TokenType::RAW_STRING_LITERAL:     return "raw string literal";

        // ─── Comments ───────────────────────────────────────────────────
        case TokenType::DOC_COMMENT:            return "doc comment";

        // ─── Delimiters ─────────────────────────────────────────────────
        case TokenType::LPAREN:                 return "'('";
        case TokenType::RPAREN:                 return "')'";
        case TokenType::LBRACE:                 return "'{'";
        case TokenType::RBRACE:                 return "'}'";
        case TokenType::LBRACKET:               return "'['";
        case TokenType::RBRACKET:               return "']'";
        case TokenType::COMMA:                  return "','";
        case TokenType::SEMICOLON:              return "';'";
        case TokenType::DOT:                    return "'.'";
        case TokenType::COLON:                  return "':'";
        case TokenType::ARROW:                  return "'->'";
        case TokenType::VARIADIC:               return "'...'";
        case TokenType::AT_SIGN:                return "'@'";

        // ─── Operators ──────────────────────────────────────────────────

        // Assignment
        case TokenType::ASSIGN:                 return "'='";
        case TokenType::PLUS_ASSIGN:            return "'+='";
        case TokenType::MINUS_ASSIGN:           return "'-='";
        case TokenType::MUL_ASSIGN:             return "'*='";
        case TokenType::DIV_ASSIGN:             return "'/='";
        case TokenType::MOD_ASSIGN:             return "'%='";
        case TokenType::BIT_AND_ASSIGN:         return "'&='";
        case TokenType::BIT_OR_ASSIGN:          return "'|='";
        case TokenType::BIT_XOR_ASSIGN:         return "'^='";
        case TokenType::SHL_ASSIGN:             return "'<<='";
        case TokenType::SHR_ASSIGN:             return "'>>='";

        // Arithmetic
        case TokenType::PLUS:                   return "'+'";
        case TokenType::MINUS:                  return "'-'";
        case TokenType::MUL:                    return "'*'";
        case TokenType::DIV:                    return "'/'";
        case TokenType::MOD:                    return "'%'";
        case TokenType::POW:                    return "'**'";

        // Comparison
        case TokenType::EQUAL_EQUAL:            return "'=='";
        case TokenType::NOT_EQUAL:              return "'!='";
        case TokenType::LESS:                   return "'<'";
        case TokenType::LESS_EQUAL:             return "'<='";
        case TokenType::GREATER:                return "'>'";
        case TokenType::GREATER_EQUAL:          return "'>='";

        // Bitwise
        case TokenType::BIT_AND:                return "'&'";
        case TokenType::BIT_OR:                 return "'|'";
        case TokenType::BIT_XOR:                return "'^'";
        case TokenType::BIT_NOT:                return "'~'";
        case TokenType::SHL:                    return "'<<'";
        case TokenType::SHR:                    return "'>>'";

        // Null coalescing
        case TokenType::QUESTION_QUESTION:      return "'?\?'";
    }
    return "<unknown-token>";
}
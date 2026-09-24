/**
 * @file Tokens.hpp
 *
 * @responsibility Data structures for the Lexer's output and the Parser's input.
 *
 * @fundamental This file is used by every stage of the compiler/interpreter.
 * Changes here usually require updates to the Lexer AND the Parser.
 *
 * ─── Design ─────────────────────────────────────────────────────────────
 * This file defines the fixed vocabulary the lexer recognizes: keywords,
 * content markers, operators, delimiters, and literals. It also provides
 * a small set of classification helpers used by the parser to dispatch
 * on token categories.
 *
 * The categories here are limited to the ones the *parser* needs. Sema-side
 * categories (is this type an integer? is this a numeric type?) live in
 * TypeAST.hpp, where the `PrimitiveKind` predicates are defined.
 */

#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

/**
 * @brief All possible token types in the Lucid language.
 *
 * This is a plain enum (not enum class) for convenience:
 * - Values can be used directly in switch statements
 * - Bitwise operations work naturally
 * - No need for static_cast when comparing
 */
enum TokenType {
    // ─── End of File ────────────────────────────────────────────────────
    EOF_TOKEN,

    // ─── Frame Keywords ─────────────────────────────────────────────────
    // Value-frame keywords (lowercase)
    IMPORT,        // import std.io
    AS,            // as math
    LET,           // let x int = 42
    CONST,         // const pi float = 3.14
    TRAIT,         // trait Vector2 { ... }
    SATISFY,       // satisfy Numeric for int { ... }

    // Host- and behavior-frame keywords (uppercase)
    TYPE_KW,       // TYPE X = ...
    FN_KW,         // FN f(...) = #host(...)
    DEF_KW,        // DEF BINARY_OP '+' (...) = ...
    REQUIRE_KW,    // REQUIRE BINARY_OP '+' (...) -> ...;   (inside trait)
    FIELD_KW,      // FIELD name type;                       (inside trait)

    // ─── Type Content Markers ───────────────────────────────────────────
    STRUCT,        // struct Vec2 { ... }   (sugar for TYPE X = struct { ... })
    ENUM,          // enum Direction { ... } (sugar for TYPE X = enum { ... })

    // ─── Control Flow Keywords ──────────────────────────────────────────
    IF,            // if condition { ... }
    ELSE,          // else { ... }
    SWITCH,        // switch value { case 1: ... }
    CASE,          // case 1, 2, 3: ...
    DEFAULT,       // default: ...
    WHILE,         // while condition { ... }
    FOR,           // for i int in 0..10 { ... }
    IN,            // for item T in collection { ... }
    DO,            // do { ... } while condition
    RETURN,        // return value
    BREAK,         // break
    CONTINUE,      // continue

    // ─── Concurrency Keywords ───────────────────────────────────────────
    ASYNC,         // async f(...) -> T      (declaration marker)
    SPAWN,         // spawn f(args);          (fire-and-forget)
    START,         // start d T = f(args);    (produces Deferred<T>)
    AWAIT,         // await d;                (consume deferred)
    ALL,           // await all(a, b, c);
    ANY,           // await any(a, b, c);

    // ─── Logical Keywords ───────────────────────────────────────────────
    AND,           // and
    OR,            // or
    NOT,           // not

    // ─── Literal Keywords ───────────────────────────────────────────────
    TRUE,          // true
    FALSE,         // false
    NIL,           // nil
    ERR,           // err

    // ─── Type Markers ───────────────────────────────────────────────────
    TYPE_FN,       // fn   (function-type stage, content marker)
    TYPE_CLS,      // cls  (function-type stage, content marker)

    // ─── Primitive Type Names ───────────────────────────────────────────
    TYPE_BOOL,
    TYPE_INT8,  TYPE_INT16,  TYPE_INT32,  TYPE_INT64,
    TYPE_UINT8, TYPE_UINT16, TYPE_UINT32, TYPE_UINT64,
    TYPE_BYTE,  TYPE_SHORT,  TYPE_INT,    TYPE_LONG,
    TYPE_UBYTE, TYPE_USHORT, TYPE_UINT,   TYPE_ULONG,
    TYPE_FLOAT, TYPE_DOUBLE, TYPE_DECIMAL,
    TYPE_STRING, TYPE_CHAR,

    // ─── Array Size Qualifiers ──────────────────────────────────────────
    ARRAY_STAR,    // [*]T   (dynamic array)
    ARRAY_UNDER,   // [_]T   (slice)
    // INT_LITERAL is used for fixed-size arrays: [N]T

    // ─── Attribute and Target Sigils ────────────────────────────────────
    AT_SIGN,       // @      (attribute prefix: @[export], @[inline])
    HASH,          // #      (target prefix: #host, #native, #builtin)

    // ─── Assignment Operators ───────────────────────────────────────────
    ASSIGN,         // =
    PLUS_ASSIGN,    // +=
    MINUS_ASSIGN,   // -=
    MUL_ASSIGN,     // *=
    DIV_ASSIGN,     // /=
    MOD_ASSIGN,     // %=
    POW_ASSIGN,     // **=
    BIT_AND_ASSIGN, // &=
    BIT_OR_ASSIGN,  // |=
    BIT_XOR_ASSIGN, // ^=
    SHL_ASSIGN,     // <<=
    SHR_ASSIGN,     // >>=

    // ─── Arithmetic Operators ───────────────────────────────────────────
    PLUS,    // +
    MINUS,   // -
    MUL,     // *
    DIV,     // /
    MOD,     // %
    POW,     // **

    // ─── Bitwise Operators ──────────────────────────────────────────────
    BIT_AND,    // &       (also reference type marker in type position)
    BIT_OR,     // |
    BIT_XOR,    // ^
    BIT_NOT,    // ~
    SHL,        // <<
    SHR,        // >>

    // ─── Comparison Operators ───────────────────────────────────────────
    EQUAL_EQUAL,    // ==
    NOT_EQUAL,      // !=
    LESS,           // <     (also used in generics: <T>)
    LESS_EQUAL,     // <=
    GREATER,        // >     (also used in generics: <T>)
    GREATER_EQUAL,  // >=

    // ─── Special Operators ──────────────────────────────────────────────
    ARROW,              // ->   (function return type)
    PIPELINE,           // |>   (pipeline operator)
    RANGE,              // ..   (inclusive range)
    RANGE_EXCLUSIVE,    // ..<  (exclusive range)
    VARIADIC,           // ...  (variadic parameters)
    BANG,               // !    (fallible suffix / pipeline arg pack)
    QUESTION,           // ?    (nullable suffix)
    QUESTION_QUESTION,  // ??   (nil/err fallback)

    // ─── Access ─────────────────────────────────────────────────────────
    DOT,          // .    (field access, enum variant access)
    COLON,        // :    (trait constraint only: <T : Trait>)
    COLON_COLON,  // ::   (module access, static struct member access)

    // ─── Delimiters ─────────────────────────────────────────────────────
    COMMA,      // ,
    SEMICOLON,  // ;
    LPAREN,     // (
    RPAREN,     // )
    LBRACE,     // {
    RBRACE,     // }
    LBRACKET,   // [
    RBRACKET,   // ]

    // ─── Special Symbols ────────────────────────────────────────────────
    UNDERSCORE, // _   (discard pattern)

    // ─── Literals ───────────────────────────────────────────────────────
    IDENTIFIER,
    INT_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,
    RAW_STRING_LITERAL,
    CHAR_LITERAL,
    HEX_LITERAL,
    BINARY_LITERAL,

    // ─── Comments ───────────────────────────────────────────────────────
    DOC_COMMENT,     // /-- ... --/
    BLOCK_COMMENT,   // /- ... -/
    LINE_COMMENT,    // -- text

    // ─── Error ──────────────────────────────────────────────────────────
    UNKNOWN
};

/**
 * @brief A single token produced by the lexer.
 *
 * Tokens carry:
 * - The token type (IDENTIFIER, INT_LITERAL, etc.)
 * - The raw lexeme (the actual text from source)
 * - Source location (line, column)
 */
struct Token {
    TokenType type;
    std::string value;     // raw lexeme
    unsigned int line;
    unsigned short column;

    // ─── Classification Helpers ─────────────────────────────────────────
    bool is_operator() const;
    bool is_assignment_op() const;
    bool is_binary_op() const;
    bool is_literal() const;
    bool is_keyword() const;
    bool is_function_type_keyword() const;
    bool is_primitive_type() const;

    std::string to_string() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Parser-Relevant Classification Helpers
//
// Only the categories the parser actually dispatches on are listed here.
// Semantic categories (is this an integer type? is this numeric?) live in
// TypeAST.hpp with the PrimitiveKind predicates.
// ─────────────────────────────────────────────────────────────────────────────

inline bool is_primitive_type(TokenType type) {
    switch (type) {
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_INT8:   case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:  case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:  case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32: case TokenType::TYPE_UINT64:
        case TokenType::TYPE_BYTE:   case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:    case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:  case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:   case TokenType::TYPE_ULONG:
        case TokenType::TYPE_FLOAT:  case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING: case TokenType::TYPE_CHAR:
            return true;
        default:
            return false;
    }
}

inline bool is_function_type_keyword(TokenType type) {
    return type == TokenType::TYPE_FN || type == TokenType::TYPE_CLS;
}

inline bool is_operator(TokenType type) {
    switch (type) {
        // Arithmetic
        case TokenType::PLUS:   case TokenType::MINUS:
        case TokenType::MUL:    case TokenType::DIV:
        case TokenType::MOD:    case TokenType::POW:
        // Assignment
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:   case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:    case TokenType::DIV_ASSIGN:
        case TokenType::MOD_ASSIGN:    case TokenType::POW_ASSIGN:
        case TokenType::BIT_AND_ASSIGN: case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN: case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
        // Bitwise
        case TokenType::BIT_AND: case TokenType::BIT_OR:
        case TokenType::BIT_XOR: case TokenType::BIT_NOT:
        case TokenType::SHL:     case TokenType::SHR:
        // Comparison
        case TokenType::EQUAL_EQUAL: case TokenType::NOT_EQUAL:
        case TokenType::LESS:        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:     case TokenType::GREATER_EQUAL:
        // Logical
        case TokenType::AND: case TokenType::OR: case TokenType::NOT:
        // Special
        case TokenType::PIPELINE:
        case TokenType::RANGE: case TokenType::RANGE_EXCLUSIVE:
        case TokenType::BANG:  case TokenType::QUESTION:
        case TokenType::QUESTION_QUESTION:
        case TokenType::DOT:   case TokenType::COLON:
        case TokenType::COLON_COLON:
            return true;
        default:
            return false;
    }
}

inline bool is_assignment_op(TokenType type) {
    switch (type) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:   case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:    case TokenType::DIV_ASSIGN:
        case TokenType::MOD_ASSIGN:    case TokenType::POW_ASSIGN:
        case TokenType::BIT_AND_ASSIGN: case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN: case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            return true;
        default:
            return false;
    }
}

inline bool is_binary_op(TokenType type) {
    switch (type) {
        // Arithmetic
        case TokenType::PLUS:   case TokenType::MINUS:
        case TokenType::MUL:    case TokenType::DIV:
        case TokenType::MOD:    case TokenType::POW:
        // Comparison
        case TokenType::EQUAL_EQUAL: case TokenType::NOT_EQUAL:
        case TokenType::LESS:        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:     case TokenType::GREATER_EQUAL:
        // Logical
        case TokenType::AND: case TokenType::OR:
        // Bitwise
        case TokenType::BIT_AND: case TokenType::BIT_OR:
        case TokenType::BIT_XOR: case TokenType::SHL:
        case TokenType::SHR:
        // Range
        case TokenType::RANGE: case TokenType::RANGE_EXCLUSIVE:
        // Pipeline
        case TokenType::PIPELINE:
        // Fallback
        case TokenType::QUESTION_QUESTION:
            return true;
        default:
            return false;
    }
}

inline bool is_literal(TokenType type) {
    switch (type) {
        case TokenType::INT_LITERAL:
        case TokenType::FLOAT_LITERAL:
        case TokenType::STRING_LITERAL:
        case TokenType::RAW_STRING_LITERAL:
        case TokenType::CHAR_LITERAL:
        case TokenType::HEX_LITERAL:
        case TokenType::BINARY_LITERAL:
        case TokenType::TRUE:
        case TokenType::FALSE:
        case TokenType::NIL:
        case TokenType::ERR:
            return true;
        default:
            return false;
    }
}

inline bool is_keyword(TokenType type) {
    switch (type) {
        // Frame keywords
        case TokenType::IMPORT:   case TokenType::AS:
        case TokenType::LET:      case TokenType::CONST:
        case TokenType::TRAIT:    case TokenType::SATISFY:
        case TokenType::TYPE_KW:  case TokenType::FN_KW:
        case TokenType::DEF_KW:   case TokenType::REQUIRE_KW:
        case TokenType::FIELD_KW:
        // Content markers
        case TokenType::STRUCT:   case TokenType::ENUM:
        // Control flow
        case TokenType::IF:       case TokenType::ELSE:
        case TokenType::SWITCH:   case TokenType::CASE:
        case TokenType::DEFAULT:  case TokenType::WHILE:
        case TokenType::FOR:      case TokenType::IN:
        case TokenType::DO:       case TokenType::RETURN:
        case TokenType::BREAK:    case TokenType::CONTINUE:
        // Concurrency
        case TokenType::ASYNC:    case TokenType::SPAWN:
        case TokenType::START:    case TokenType::AWAIT:
        case TokenType::ALL:      case TokenType::ANY:
        // Logical
        case TokenType::AND:      case TokenType::OR:
        case TokenType::NOT:
        // Literal keywords
        case TokenType::TRUE:     case TokenType::FALSE:
        case TokenType::NIL:      case TokenType::ERR:
        // Type markers
        case TokenType::TYPE_FN:  case TokenType::TYPE_CLS:
        // Primitive type names
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_INT8:   case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:  case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:  case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32: case TokenType::TYPE_UINT64:
        case TokenType::TYPE_BYTE:   case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:    case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:  case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:   case TokenType::TYPE_ULONG:
        case TokenType::TYPE_FLOAT:  case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING: case TokenType::TYPE_CHAR:
            return true;
        default:
            return false;
    }
}

// ─── Token Method Implementations ─────────────────────────────────────────

inline bool Token::is_operator() const { return ::is_operator(type); }
inline bool Token::is_assignment_op() const { return ::is_assignment_op(type); }
inline bool Token::is_binary_op() const { return ::is_binary_op(type); }
inline bool Token::is_literal() const { return ::is_literal(type); }
inline bool Token::is_keyword() const { return ::is_keyword(type); }
inline bool Token::is_function_type_keyword() const { return ::is_function_type_keyword(type); }
inline bool Token::is_primitive_type() const { return ::is_primitive_type(type); }

// ─────────────────────────────────────────────────────────────────────────────
// Token Type Name Mapping
// ─────────────────────────────────────────────────────────────────────────────

inline std::string token_type_name(TokenType type) {
    static const std::unordered_map<TokenType, std::string> names = {
        {TokenType::EOF_TOKEN, "EOF"},
        {TokenType::IMPORT, "import"},
        {TokenType::AS, "as"},
        {TokenType::LET, "let"},
        {TokenType::CONST, "const"},
        {TokenType::TRAIT, "trait"},
        {TokenType::SATISFY, "satisfy"},
        {TokenType::TYPE_KW, "TYPE"},
        {TokenType::FN_KW, "FN"},
        {TokenType::DEF_KW, "DEF"},
        {TokenType::REQUIRE_KW, "REQUIRE"},
        {TokenType::FIELD_KW, "FIELD"},
        {TokenType::STRUCT, "struct"},
        {TokenType::ENUM, "enum"},
        {TokenType::IF, "if"},
        {TokenType::ELSE, "else"},
        {TokenType::SWITCH, "switch"},
        {TokenType::CASE, "case"},
        {TokenType::DEFAULT, "default"},
        {TokenType::WHILE, "while"},
        {TokenType::FOR, "for"},
        {TokenType::IN, "in"},
        {TokenType::DO, "do"},
        {TokenType::RETURN, "return"},
        {TokenType::BREAK, "break"},
        {TokenType::CONTINUE, "continue"},
        {TokenType::ASYNC, "async"},
        {TokenType::SPAWN, "spawn"},
        {TokenType::START, "start"},
        {TokenType::AWAIT, "await"},
        {TokenType::ALL, "all"},
        {TokenType::ANY, "any"},
        {TokenType::AND, "and"},
        {TokenType::OR, "or"},
        {TokenType::NOT, "not"},
        {TokenType::TRUE, "true"},
        {TokenType::FALSE, "false"},
        {TokenType::NIL, "nil"},
        {TokenType::ERR, "err"},
        {TokenType::TYPE_FN, "fn"},
        {TokenType::TYPE_CLS, "cls"},
        {TokenType::TYPE_BOOL, "bool"},
        {TokenType::TYPE_INT8, "int8"},
        {TokenType::TYPE_INT16, "int16"},
        {TokenType::TYPE_INT32, "int32"},
        {TokenType::TYPE_INT64, "int64"},
        {TokenType::TYPE_UINT8, "uint8"},
        {TokenType::TYPE_UINT16, "uint16"},
        {TokenType::TYPE_UINT32, "uint32"},
        {TokenType::TYPE_UINT64, "uint64"},
        {TokenType::TYPE_BYTE, "byte"},
        {TokenType::TYPE_SHORT, "short"},
        {TokenType::TYPE_INT, "int"},
        {TokenType::TYPE_LONG, "long"},
        {TokenType::TYPE_UBYTE, "ubyte"},
        {TokenType::TYPE_USHORT, "ushort"},
        {TokenType::TYPE_UINT, "uint"},
        {TokenType::TYPE_ULONG, "ulong"},
        {TokenType::TYPE_FLOAT, "float"},
        {TokenType::TYPE_DOUBLE, "double"},
        {TokenType::TYPE_DECIMAL, "decimal"},
        {TokenType::TYPE_STRING, "string"},
        {TokenType::TYPE_CHAR, "char"},
        {TokenType::ARRAY_STAR, "[*]"},
        {TokenType::ARRAY_UNDER, "[_]"},
        {TokenType::AT_SIGN, "@"},
        {TokenType::HASH, "#"},
        {TokenType::ASSIGN, "="},
        {TokenType::PLUS_ASSIGN, "+="},
        {TokenType::MINUS_ASSIGN, "-="},
        {TokenType::MUL_ASSIGN, "*="},
        {TokenType::DIV_ASSIGN, "/="},
        {TokenType::MOD_ASSIGN, "%="},
        {TokenType::POW_ASSIGN, "**="},
        {TokenType::BIT_AND_ASSIGN, "&="},
        {TokenType::BIT_OR_ASSIGN, "|="},
        {TokenType::BIT_XOR_ASSIGN, "^="},
        {TokenType::SHL_ASSIGN, "<<="},
        {TokenType::SHR_ASSIGN, ">>="},
        {TokenType::PLUS, "+"},
        {TokenType::MINUS, "-"},
        {TokenType::MUL, "*"},
        {TokenType::DIV, "/"},
        {TokenType::MOD, "%"},
        {TokenType::POW, "**"},
        {TokenType::BIT_AND, "&"},
        {TokenType::BIT_OR, "|"},
        {TokenType::BIT_XOR, "^"},
        {TokenType::BIT_NOT, "~"},
        {TokenType::SHL, "<<"},
        {TokenType::SHR, ">>"},
        {TokenType::EQUAL_EQUAL, "=="},
        {TokenType::NOT_EQUAL, "!="},
        {TokenType::LESS, "<"},
        {TokenType::LESS_EQUAL, "<="},
        {TokenType::GREATER, ">"},
        {TokenType::GREATER_EQUAL, ">="},
        {TokenType::ARROW, "->"},
        {TokenType::PIPELINE, "|>"},
        {TokenType::RANGE, ".."},
        {TokenType::RANGE_EXCLUSIVE, "..<"},
        {TokenType::VARIADIC, "..."},
        {TokenType::BANG, "!"},
        {TokenType::QUESTION, "?"},
        {TokenType::QUESTION_QUESTION, "??"},
        {TokenType::DOT, "."},
        {TokenType::COLON, ":"},
        {TokenType::COLON_COLON, "::"},
        {TokenType::COMMA, ","},
        {TokenType::SEMICOLON, ";"},
        {TokenType::LPAREN, "("},
        {TokenType::RPAREN, ")"},
        {TokenType::LBRACE, "{"},
        {TokenType::RBRACE, "}"},
        {TokenType::LBRACKET, "["},
        {TokenType::RBRACKET, "]"},
        {TokenType::UNDERSCORE, "_"},
        {TokenType::IDENTIFIER, "IDENTIFIER"},
        {TokenType::INT_LITERAL, "INT_LITERAL"},
        {TokenType::FLOAT_LITERAL, "FLOAT_LITERAL"},
        {TokenType::STRING_LITERAL, "STRING_LITERAL"},
        {TokenType::RAW_STRING_LITERAL, "RAW_STRING_LITERAL"},
        {TokenType::CHAR_LITERAL, "CHAR_LITERAL"},
        {TokenType::HEX_LITERAL, "HEX_LITERAL"},
        {TokenType::BINARY_LITERAL, "BINARY_LITERAL"},
        {TokenType::DOC_COMMENT, "DOC_COMMENT"},
        {TokenType::LINE_COMMENT, "LINE_COMMENT"},
        {TokenType::BLOCK_COMMENT, "BLOCK_COMMENT"},
        {TokenType::UNKNOWN, "UNKNOWN"}
    };
    auto it = names.find(type);
    return it != names.end() ? it->second : "UNKNOWN_TOKEN";
}

/// @brief Convert a TokenType to a human-readable string.
inline std::string tokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::EOF_TOKEN:              return "EOF";
        case TokenType::IDENTIFIER:             return "IDENTIFIER";
        case TokenType::INT_LITERAL:            return "INT_LITERAL";
        case TokenType::FLOAT_LITERAL:          return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL:         return "STRING_LITERAL";
        case TokenType::RAW_STRING_LITERAL:     return "RAW_STRING_LITERAL";
        case TokenType::CHAR_LITERAL:           return "CHAR_LITERAL";
        case TokenType::HEX_LITERAL:            return "HEX_LITERAL";
        case TokenType::BINARY_LITERAL:         return "BINARY_LITERAL";
        case TokenType::TRUE:                   return "TRUE";
        case TokenType::FALSE:                  return "FALSE";
        case TokenType::NIL:                    return "NIL";
        case TokenType::ERR:                    return "ERR";
        case TokenType::UNDERSCORE:             return "_";

        // Frame keywords
        case TokenType::IMPORT:                 return "import";
        case TokenType::AS:                     return "as";
        case TokenType::LET:                    return "let";
        case TokenType::CONST:                  return "const";
        case TokenType::TRAIT:                  return "trait";
        case TokenType::SATISFY:                return "satisfy";
        case TokenType::TYPE_KW:                return "TYPE";
        case TokenType::FN_KW:                  return "FN";
        case TokenType::DEF_KW:                 return "DEF";
        case TokenType::REQUIRE_KW:             return "REQUIRE";
        case TokenType::FIELD_KW:               return "FIELD";
        case TokenType::STRUCT:                 return "struct";
        case TokenType::ENUM:                   return "enum";

        // Control flow
        case TokenType::IF:                     return "if";
        case TokenType::ELSE:                   return "else";
        case TokenType::SWITCH:                 return "switch";
        case TokenType::CASE:                   return "case";
        case TokenType::DEFAULT:                return "default";
        case TokenType::WHILE:                  return "while";
        case TokenType::FOR:                    return "for";
        case TokenType::IN:                     return "in";
        case TokenType::DO:                     return "do";
        case TokenType::RETURN:                 return "return";
        case TokenType::BREAK:                  return "break";
        case TokenType::CONTINUE:               return "continue";

        // Concurrency
        case TokenType::ASYNC:                  return "async";
        case TokenType::SPAWN:                  return "spawn";
        case TokenType::START:                  return "start";
        case TokenType::AWAIT:                  return "await";
        case TokenType::ALL:                    return "all";
        case TokenType::ANY:                    return "any";

        // Logical
        case TokenType::AND:                    return "and";
        case TokenType::OR:                     return "or";
        case TokenType::NOT:                    return "not";

        // Type markers
        case TokenType::TYPE_FN:                return "fn";
        case TokenType::TYPE_CLS:               return "cls";

        // Primitive type names
        case TokenType::TYPE_BOOL:              return "bool";
        case TokenType::TYPE_INT8:              return "int8";
        case TokenType::TYPE_INT16:             return "int16";
        case TokenType::TYPE_INT32:             return "int32";
        case TokenType::TYPE_INT64:             return "int64";
        case TokenType::TYPE_UINT8:             return "uint8";
        case TokenType::TYPE_UINT16:            return "uint16";
        case TokenType::TYPE_UINT32:            return "uint32";
        case TokenType::TYPE_UINT64:            return "uint64";
        case TokenType::TYPE_BYTE:              return "byte";
        case TokenType::TYPE_SHORT:             return "short";
        case TokenType::TYPE_INT:               return "int";
        case TokenType::TYPE_LONG:              return "long";
        case TokenType::TYPE_UBYTE:             return "ubyte";
        case TokenType::TYPE_USHORT:            return "ushort";
        case TokenType::TYPE_UINT:              return "uint";
        case TokenType::TYPE_ULONG:             return "ulong";
        case TokenType::TYPE_FLOAT:             return "float";
        case TokenType::TYPE_DOUBLE:            return "double";
        case TokenType::TYPE_DECIMAL:           return "decimal";
        case TokenType::TYPE_STRING:            return "string";
        case TokenType::TYPE_CHAR:              return "char";

        // Array size qualifiers
        case TokenType::ARRAY_STAR:             return "[*]";
        case TokenType::ARRAY_UNDER:            return "[_]";

        // Sigils
        case TokenType::AT_SIGN:                return "@";
        case TokenType::HASH:                   return "#";

        // Operators
        case TokenType::PLUS:                   return "+";
        case TokenType::MINUS:                  return "-";
        case TokenType::MUL:                    return "*";
        case TokenType::DIV:                    return "/";
        case TokenType::MOD:                    return "%";
        case TokenType::POW:                    return "**";
        case TokenType::BIT_AND:                return "&";
        case TokenType::BIT_OR:                 return "|";
        case TokenType::BIT_XOR:                return "^";
        case TokenType::BIT_NOT:                return "~";
        case TokenType::SHL:                    return "<<";
        case TokenType::SHR:                    return ">>";
        case TokenType::EQUAL_EQUAL:            return "==";
        case TokenType::NOT_EQUAL:              return "!=";
        case TokenType::LESS:                   return "<";
        case TokenType::LESS_EQUAL:             return "<=";
        case TokenType::GREATER:                return ">";
        case TokenType::GREATER_EQUAL:          return ">=";
        case TokenType::ASSIGN:                 return "=";
        case TokenType::PLUS_ASSIGN:            return "+=";
        case TokenType::MINUS_ASSIGN:           return "-=";
        case TokenType::MUL_ASSIGN:             return "*=";
        case TokenType::DIV_ASSIGN:             return "/=";
        case TokenType::MOD_ASSIGN:             return "%=";
        case TokenType::POW_ASSIGN:             return "**=";
        case TokenType::BIT_AND_ASSIGN:         return "&=";
        case TokenType::BIT_OR_ASSIGN:          return "|=";
        case TokenType::BIT_XOR_ASSIGN:         return "^=";
        case TokenType::SHL_ASSIGN:             return "<<=";
        case TokenType::SHR_ASSIGN:             return ">>=";
        case TokenType::ARROW:                  return "->";
        case TokenType::PIPELINE:               return "|>";
        case TokenType::RANGE:                  return "..";
        case TokenType::RANGE_EXCLUSIVE:        return "..<";
        case TokenType::BANG:                   return "!";
        case TokenType::QUESTION:               return "?";
        case TokenType::QUESTION_QUESTION:      return "??";
        case TokenType::VARIADIC:               return "...";
        case TokenType::DOT:                    return ".";
        case TokenType::COLON:                  return ":";
        case TokenType::COLON_COLON:            return "::";
        case TokenType::COMMA:                  return ",";
        case TokenType::SEMICOLON:              return ";";
        case TokenType::LPAREN:                 return "(";
        case TokenType::RPAREN:                 return ")";
        case TokenType::LBRACE:                 return "{";
        case TokenType::RBRACE:                 return "}";
        case TokenType::LBRACKET:               return "[";
        case TokenType::RBRACKET:               return "]";

        // Comments
        case TokenType::DOC_COMMENT:            return "/-- ... --/";
        case TokenType::LINE_COMMENT:           return "-- ...";
        case TokenType::BLOCK_COMMENT:          return "/- ... -/";

        case TokenType::UNKNOWN:                return "UNKNOWN";
        default: return "Token(" + std::to_string(static_cast<int>(type)) + ")";
    }
}

/// @brief Convert a Token to a human-readable string.
inline std::string tokenToString(const Token& token) {
    std::string result = tokenTypeToString(token.type);
    if (!token.value.empty()) {
        result += "('" + token.value + "')";
    }
    return result;
}

inline std::string Token::to_string() const {
    std::string result = "Token(";
    result += token_type_name(type);
    result += ", '" + value + "', ";
    result += std::to_string(line) + ":" + std::to_string(column);
    result += ")";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyword String Helpers (used by the lexer)
// ─────────────────────────────────────────────────────────────────────────────

inline bool is_keyword(const std::string& str) {
    static const std::unordered_set<std::string> keywords = {
        // Frame keywords
        "import", "as", "let", "const", "trait", "satisfy",
        "TYPE", "FN", "DEF", "REQUIRE", "FIELD",
        // Content markers
        "struct", "enum",
        // Control flow
        "if", "else", "switch", "case", "default",
        "while", "for", "in", "do",
        "return", "break", "continue",
        // Concurrency
        "async", "spawn", "start", "await", "all", "any",
        // Logical
        "and", "or", "not",
        // Literal keywords
        "true", "false", "nil", "err",
        // Type markers
        "fn", "cls",
        // Primitive type names
        "bool",
        "int8", "int16", "int32", "int64",
        "uint8", "uint16", "uint32", "uint64",
        "byte", "short", "int", "long",
        "ubyte", "ushort", "uint", "ulong",
        "float", "double", "decimal",
        "string", "char"
    };
    return keywords.find(str) != keywords.end();
}

inline TokenType keyword_to_type(const std::string& str) {
    static const std::unordered_map<std::string, TokenType> keyword_map = {
        // Frame keywords
        {"import", TokenType::IMPORT},
        {"as", TokenType::AS},
        {"let", TokenType::LET},
        {"const", TokenType::CONST},
        {"trait", TokenType::TRAIT},
        {"satisfy", TokenType::SATISFY},
        {"TYPE", TokenType::TYPE_KW},
        {"FN", TokenType::FN_KW},
        {"DEF", TokenType::DEF_KW},
        {"REQUIRE", TokenType::REQUIRE_KW},
        {"FIELD", TokenType::FIELD_KW},
        // Content markers
        {"struct", TokenType::STRUCT},
        {"enum", TokenType::ENUM},
        // Control flow
        {"if", TokenType::IF},
        {"else", TokenType::ELSE},
        {"switch", TokenType::SWITCH},
        {"case", TokenType::CASE},
        {"default", TokenType::DEFAULT},
        {"while", TokenType::WHILE},
        {"for", TokenType::FOR},
        {"in", TokenType::IN},
        {"do", TokenType::DO},
        {"return", TokenType::RETURN},
        {"break", TokenType::BREAK},
        {"continue", TokenType::CONTINUE},
        // Concurrency
        {"async", TokenType::ASYNC},
        {"spawn", TokenType::SPAWN},
        {"start", TokenType::START},
        {"await", TokenType::AWAIT},
        {"all", TokenType::ALL},
        {"any", TokenType::ANY},
        // Logical
        {"and", TokenType::AND},
        {"or", TokenType::OR},
        {"not", TokenType::NOT},
        // Literal keywords
        {"true", TokenType::TRUE},
        {"false", TokenType::FALSE},
        {"nil", TokenType::NIL},
        {"err", TokenType::ERR},
        // Type markers
        {"fn", TokenType::TYPE_FN},
        {"cls", TokenType::TYPE_CLS},
        // Primitive type names
        {"bool", TokenType::TYPE_BOOL},
        {"int8", TokenType::TYPE_INT8},
        {"int16", TokenType::TYPE_INT16},
        {"int32", TokenType::TYPE_INT32},
        {"int64", TokenType::TYPE_INT64},
        {"uint8", TokenType::TYPE_UINT8},
        {"uint16", TokenType::TYPE_UINT16},
        {"uint32", TokenType::TYPE_UINT32},
        {"uint64", TokenType::TYPE_UINT64},
        {"byte", TokenType::TYPE_BYTE},
        {"short", TokenType::TYPE_SHORT},
        {"int", TokenType::TYPE_INT},
        {"long", TokenType::TYPE_LONG},
        {"ubyte", TokenType::TYPE_UBYTE},
        {"ushort", TokenType::TYPE_USHORT},
        {"uint", TokenType::TYPE_UINT},
        {"ulong", TokenType::TYPE_ULONG},
        {"float", TokenType::TYPE_FLOAT},
        {"double", TokenType::TYPE_DOUBLE},
        {"decimal", TokenType::TYPE_DECIMAL},
        {"string", TokenType::TYPE_STRING},
        {"char", TokenType::TYPE_CHAR}
    };
    auto it = keyword_map.find(str);
    return it != keyword_map.end() ? it->second : TokenType::IDENTIFIER;
}

// ─────────────────────────────────────────────────────────────────────────────
// EOF Token Sentinel
// ─────────────────────────────────────────────────────────────────────────────

inline const Token EOF_TOKEN_SENTINEL = {TokenType::EOF_TOKEN, "EOF", 0, 0};
/**
 * @file Tokens.hpp
 * 
 * @responsibility Data structures for the Lexer's output and the Parser's input.
 *
 * @fundamental This file is used by every stage of the compiler/interpreter.
 * Changes here usually require updates to the Lexer AND the Parser.
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
 * 
 * @note The prefix `TOKEN_` is not used because the enum name itself
 *       provides context: `TokenType::IDENTIFIER`.
 */
enum TokenType {
    // ─── End of File ────────────────────────────────────────────────────
    EOF_TOKEN,

    // ─── Keywords: Top Level ───────────────────────────────────────────
    USE,      // use std.io
    AS,       // as math
    STRUCT,   // struct Vec2 { x float, y float }
    ENUM,     // enum Direction { North = 0, South = 1 }
    TRAIT,    // trait Vector2 { x float, y float }

    // ─── Keywords: Declarations ────────────────────────────────────────
    LET,      // let x int = 42          (mutable)
    CONST,    // const pi float = 3.14   (immutable)

    // ─── Keywords: Control Flow ────────────────────────────────────────
    IF,       // if condition { ... }
    ELSE,     // else { ... }
    SWITCH,   // switch value { case 1: ... }
    CASE,     // case 1, 2, 3: ...
    DEFAULT,  // default: ...
    WHILE,    // while condition { ... }
    FOR,      // for i int in 0..10 { ... }
    IN,       // for item T in collection { ... }
    DO,       // do { ... } while condition
    RETURN,   // return value
    BREAK,    // break
    CONTINUE, // continue

    // ─── Keywords: Concurrency ──────────────────────────────────────────
    SPAWN,    // spawn result = function()   (parallelism with optional join)
    JOIN,     // join result                 (wait for spawn)
    ASYNC,    // async result = function()   (concurrency)
    AWAIT,    // await result                (wait for async)

    // ─── Keywords: Logical ─────────────────────────────────────────────
    AND,      // and      (logical AND, short-circuits)
    OR,       // or       (logical OR, short-circuits)
    NOT,      // not      (logical NOT)

    // ─── Keywords: Literals ────────────────────────────────────────────
    TRUE,     // true
    FALSE,    // false
    NIL,      // nil      (null value)
    ERR,      // err      (error sentinel)

    // ─── Types: Primitives ─────────────────────────────────────────────
    // Boolean
    TYPE_BOOL,   // bool

    // Signed integers (fixed-width)
    TYPE_INT8,   // int8
    TYPE_INT16,  // int16
    TYPE_INT32,  // int32
    TYPE_INT64,  // int64

    // Unsigned integers (fixed-width)
    TYPE_UINT8,  // uint8
    TYPE_UINT16, // uint16
    TYPE_UINT32, // uint32
    TYPE_UINT64, // uint64

    // Platform-dependent integer aliases
    TYPE_BYTE,   // byte      (int8)
    TYPE_SHORT,  // short     (int16)
    TYPE_INT,    // int       (int32)
    TYPE_LONG,   // long      (int64)
    TYPE_UBYTE,  // ubyte     (uint8)
    TYPE_USHORT, // ushort    (uint16)
    TYPE_UINT,   // uint      (uint32)
    TYPE_ULONG,  // ulong     (uint64)

    // Floating point
    TYPE_FLOAT,   // float     (32-bit)
    TYPE_DOUBLE,  // double    (64-bit)
    TYPE_DECIMAL, // decimal   (128-bit, high precision)

    // Text
    TYPE_STRING, // string
    TYPE_CHAR,   // char

    // ─── Types: Special ─────────────────────────────────────────────────
    TYPE_FUTURE, // Future<T> (internal, not written by user)

    // ─── Types: Array Size Qualifiers ──────────────────────────────────
    ARRAY_STAR,  // [*]T      (owned heap array)
    ARRAY_UNDER, // [_]T      (slice, borrowed view)
    // INT_LITERAL is used for fixed-size arrays: [N]T

    // ─── Attributes ─────────────────────────────────────────────────────
    AT_SIGN,    // @         (attribute prefix: @[export], @[inline])

    // ─── Intrinsics ────────────────────────────────────────────────────
    HASH,       // #         (intrinsic prefix: #sizeof, #sqrt)

    // ─── Operators: Assignment ─────────────────────────────────────────
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

    // ─── Operators: Arithmetic ──────────────────────────────────────────
    PLUS,    // +
    MINUS,   // -
    MUL,     // *
    DIV,     // /
    MOD,     // %
    POW,     // **

    // ─── Operators: Bitwise ─────────────────────────────────────────────
    BIT_AND,    // &       (bitwise AND)
    BIT_OR,     // |       (bitwise OR)
    BIT_XOR,    // ^       (bitwise XOR)
    BIT_NOT,    // ~       (bitwise NOT, unary)
    SHL,        // <<      (shift left)
    SHR,        // >>      (shift right)

    // ─── Operators: Comparison ──────────────────────────────────────────
    EQUAL_EQUAL,    // ==    (value equality)
    NOT_EQUAL,      // !=
    LESS,           // <     (also used for generics: <T>)
    LESS_EQUAL,     // <=
    GREATER,        // >     (also used for generics: <T>)
    GREATER_EQUAL,  // >=

    // ─── Operators: Special ─────────────────────────────────────────────
    ARROW,          // ->      (function return type)
    COMPOSE,        // +>      (function composition)
    PIPELINE,       // |>      (pipeline operator)
    RANGE,          // ..      (inclusive range: 0..10)
    RANGE_EXCLUSIVE,// ..<     (exclusive range: 0..<10)
    VARIADIC,       // ...     (variadic parameters)
    BANG,           // !       (fallible type suffix, or pipeline arg pack)
    QUESTION,       // ?       (nullable type suffix)
    QUESTION_DOT,   // ?.      (nullable field access)
    QUESTION_QUESTION, // ??   (nil/err fallback)

    // ─── Access ──────────────────────────────────────────────────────────
    DOT,    // .       (field access: player.health)
    COLON,  // :       (module access: math:sqrt, or trait constraint: T : Trait)

    // ─── Delimiters ─────────────────────────────────────────────────────
    COMMA,      // ,
    SEMICOLON,  // ;        (optional, but accepted)
    LPAREN,     // (
    RPAREN,     // )
    LBRACE,     // {
    RBRACE,     // }
    LBRACKET,   // [
    RBRACKET,   // ]

    // ─── Special Symbols ────────────────────────────────────────────────
    AMPERSAND,  // &       (reference type: &T) -- same as BIT_AND but contextual
    UNDERSCORE, // _       (discard pattern: spawn _ = function())

    // ─── Literals ──────────────────────────────────────────────────────
    IDENTIFIER,         // variable, function, type names
    INT_LITERAL,        // 42
    FLOAT_LITERAL,      // 3.14
    STRING_LITERAL,     // "hello"
    RAW_STRING_LITERAL, // """raw\nno escaping"""
    CHAR_LITERAL,       // 'a'
    HEX_LITERAL,        // 0xFF
    BINARY_LITERAL,     // 0b1010

    // ─── Comments ──────────────────────────────────────────────────────
    DOC_COMMENT,        // /-- ... --/      (block documentation)
    BLOCK_COMMENT,      // /- ... -/        (block comments)
    LINE_COMMENT,       // -- text          (line comment, captured for docs)

    // ─── Error ────────────────────────────────────────────────────────
    UNKNOWN         // unrecognized character
};

/**
 * @brief A single token produced by the lexer.
 * 
 * Tokens carry:
 * - The token type (IDENTIFIER, INT_LITERAL, etc.)
 * - The raw lexeme (the actual text from source)
 * - Source location (line, column)
 * - Source file name (for error reporting)
 */
struct Token {
    TokenType type;
    std::string value;     // raw lexeme
    int line;
    int column;
    std::string filename;  // source file name for error reporting
    
    // ─── Helper Methods ──────────────────────────────────────────────────
    
    bool is_operator() const;
    bool is_literal() const;
    bool is_keyword() const;
    std::string to_string() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Token Utility Functions (implementation)
// ─────────────────────────────────────────────────────────────────────────────

inline bool Token::is_operator() const {
    switch (type) {
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
        case TokenType::POW:
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
        case TokenType::BIT_AND:
        case TokenType::BIT_OR:
        case TokenType::BIT_XOR:
        case TokenType::BIT_NOT:
        case TokenType::SHL:
        case TokenType::SHR:
        case TokenType::EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL:
        case TokenType::ARROW:
        case TokenType::COMPOSE:
        case TokenType::PIPELINE:
        case TokenType::RANGE:
        case TokenType::RANGE_EXCLUSIVE:
        case TokenType::BANG:
        case TokenType::QUESTION:
        case TokenType::QUESTION_DOT:
        case TokenType::QUESTION_QUESTION:
        case TokenType::DOT:
        case TokenType::COLON:
            return true;
        default:
            return false;
    }
}

inline bool Token::is_literal() const {
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

inline bool Token::is_keyword() const {
    switch (type) {
        case TokenType::USE:
        case TokenType::AS:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::LET:
        case TokenType::CONST:
        case TokenType::IF:
        case TokenType::ELSE:
        case TokenType::SWITCH:
        case TokenType::CASE:
        case TokenType::DEFAULT:
        case TokenType::WHILE:
        case TokenType::FOR:
        case TokenType::IN:
        case TokenType::DO:
        case TokenType::RETURN:
        case TokenType::BREAK:
        case TokenType::CONTINUE:
        case TokenType::SPAWN:
        case TokenType::JOIN:
        case TokenType::ASYNC:
        case TokenType::AWAIT:
        case TokenType::AND:
        case TokenType::OR:
        case TokenType::NOT:
        case TokenType::TRUE:
        case TokenType::FALSE:
        case TokenType::NIL:
        case TokenType::ERR:
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_INT8:
        case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:
        case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:
        case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32:
        case TokenType::TYPE_UINT64:
        case TokenType::TYPE_BYTE:
        case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:
        case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:
        case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:
        case TokenType::TYPE_ULONG:
        case TokenType::TYPE_FLOAT:
        case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING:
        case TokenType::TYPE_CHAR:
            return true;
        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Token Type Name Mapping
// ─────────────────────────────────────────────────────────────────────────────

inline std::string token_type_name(TokenType type) {
    static const std::unordered_map<TokenType, std::string> names = {
        {TokenType::EOF_TOKEN, "EOF"},
        {TokenType::USE, "use"},
        {TokenType::AS, "as"},
        {TokenType::STRUCT, "struct"},
        {TokenType::ENUM, "enum"},
        {TokenType::TRAIT, "trait"},
        {TokenType::LET, "let"},
        {TokenType::CONST, "const"},
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
        {TokenType::SPAWN, "spawn"},
        {TokenType::JOIN, "join"},
        {TokenType::ASYNC, "async"},
        {TokenType::AWAIT, "await"},
        {TokenType::AND, "and"},
        {TokenType::OR, "or"},
        {TokenType::NOT, "not"},
        {TokenType::TRUE, "true"},
        {TokenType::FALSE, "false"},
        {TokenType::NIL, "nil"},
        {TokenType::ERR, "err"},
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
        {TokenType::TYPE_FUTURE, "Future"},
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
        {TokenType::COMPOSE, "+>"},
        {TokenType::PIPELINE, "|>"},
        {TokenType::RANGE, ".."},
        {TokenType::RANGE_EXCLUSIVE, "..<"},
        {TokenType::VARIADIC, "..."},
        {TokenType::BANG, "!"},
        {TokenType::QUESTION, "?"},
        {TokenType::QUESTION_DOT, "?."},
        {TokenType::QUESTION_QUESTION, "??"},
        {TokenType::DOT, "."},
        {TokenType::COLON, ":"},
        {TokenType::COMMA, ","},
        {TokenType::SEMICOLON, ";"},
        {TokenType::LPAREN, "("},
        {TokenType::RPAREN, ")"},
        {TokenType::LBRACE, "{"},
        {TokenType::RBRACE, "}"},
        {TokenType::LBRACKET, "["},
        {TokenType::RBRACKET, "]"},
        {TokenType::AMPERSAND, "&"},
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
        {TokenType::UNKNOWN, "UNKNOWN"}
    };
    
    auto it = names.find(type);
    if (it != names.end()) {
        return it->second;
    }
    return "UNKNOWN_TOKEN";
}

inline std::string Token::to_string() const {
    std::string result = "Token(";
    result += token_type_name(type);
    result += ", '" + value + "', ";
    result += std::to_string(line);
    result += ":";
    result += std::to_string(column);
    result += ")";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyword Helpers
// ─────────────────────────────────────────────────────────────────────────────

inline bool is_keyword(const std::string& str) {
    static const std::unordered_set<std::string> keywords = {
        "use", "as", "struct", "enum", "trait",
        "let", "const",
        "if", "else", "switch", "case", "default",
        "while", "for", "in", "do",
        "return", "break", "continue",
        "spawn", "join", "async", "await",
        "and", "or", "not",
        "true", "false", "nil", "err",
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
        {"use", TokenType::USE},
        {"as", TokenType::AS},
        {"struct", TokenType::STRUCT},
        {"enum", TokenType::ENUM},
        {"trait", TokenType::TRAIT},
        {"let", TokenType::LET},
        {"const", TokenType::CONST},
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
        {"spawn", TokenType::SPAWN},
        {"join", TokenType::JOIN},
        {"async", TokenType::ASYNC},
        {"await", TokenType::AWAIT},
        {"and", TokenType::AND},
        {"or", TokenType::OR},
        {"not", TokenType::NOT},
        {"true", TokenType::TRUE},
        {"false", TokenType::FALSE},
        {"nil", TokenType::NIL},
        {"err", TokenType::ERR},
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
    if (it != keyword_map.end()) {
        return it->second;
    }
    return TokenType::IDENTIFIER;
}
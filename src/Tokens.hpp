/**
 * @file Tokens.hpp
 * 
 * @responsibility Data structures for the Lexer's output and the Parser's input.
 *
 * @fundamental This file is used by every stage of the compiler.
 * Changes here usually require updates to the Lexer AND the Parser.
 */

#pragma once
#include <string>

enum class TokenType {

    // ─── Modifiers ────────────────────────────────────────────────────────────
    PUB,    // pub       - public visibility
    EXPORT, // export    - package manifest: export math { use math.vec2 }

    // ─── Top Level ────────────────────────────────────────────────────────────
    PACKAGE, // package
    USE,     // use       - import a module: use std.io
    AS,      // as        - import alias: use math as m
    IMPL,    // impl      - bind methods to a struct (pub impl = public, impl = private)
    TYPE,    // type      - type alias: type ID = int  |  type Callback = (e Event) bool
    STRUCT,  // struct    - data structure: struct Vec2 { x float  y float }
    ENUM,    // enum      - named constant set: enum Direction { North, South, East, West }
    TRAIT,   // trait     - method contract / generic constraint: trait Drawable { draw () }
    FROM,    // from      - type conversion entry point: from (c Celsius) Fahrenheit = { ... }

    // ─── Declarations ─────────────────────────────────────────────────────────
    LET,     // let       - reassignable, mutable in place, nil allowed
    CONST,   // const     - not reassignable, not mutable in place, nil allowed

    // ─── Concurrency ──────────────────────────────────────────────────────────
    AWAIT,    // await     - await async result
    // ASYNC,    // deprecated - use ~async instead
    // PARALLEL, // deprecated - use ~parallel instead

    // ─── Primary Types ──────────────────────────────────────────────────────────
    // Boolean
    TYPE_BOOL, // bool

    // Signed integers
    TYPE_BYTE,  // byte      (int8,  -128..127)
    TYPE_SHORT, // short     (int16, -32768..32767)
    TYPE_INT,   // int       (int32)
    TYPE_LONG,  // long      (int64)

    // Unsigned integers
    TYPE_UBYTE,  // ubyte     (uint8,  0..255)
    TYPE_USHORT, // ushort    (uint16, 0..65535)
    TYPE_UINT,   // uint      (uint32)
    TYPE_ULONG,  // ulong     (uint64)

    // Fixed-width aliases (critical for Vulkan struct layouts)
    TYPE_INT8,   // int8
    TYPE_INT16,  // int16
    TYPE_INT32,  // int32
    TYPE_INT64,  // int64
    TYPE_UINT8,  // uint8
    TYPE_UINT16, // uint16
    TYPE_UINT32, // uint32
    TYPE_UINT64, // uint64

    // Floating point
    TYPE_FLOAT,   // float     (32-bit)
    TYPE_DOUBLE,  // double    (64-bit)
    TYPE_DECIMAL, // decimal   (128-bit, high precision)

    // Text
    TYPE_STRING, // string
    TYPE_CHAR,   // char

    // Special
    TYPE_ANY, // any       - dynamic type
    NIL,      // nil       - null value

    // ─── Control Flow ─────────────────────────────────────────────────────────
    IF,       // if
    ELSE,     // else
    MATCH,    // match     - expression-oriented pattern matching + struct destructuring
    SWITCH,   // switch    - statement-oriented value dispatch (multiple values/ranges per case)
    CASE,     // case
    DEFAULT,  // default
    IS,       // is        - type check with narrowing: x is int  |  v is Circle
    WHILE,    // while
    FOR,      // for
    IN,       // in        - for..in iteration
    DO,       // do
    RETURN,   // return
    BREAK,    // break
    CONTINUE, // continue

    // ─── Logical ──────────────────────────────────────────────────────────────
    AND,   // and
    OR,    // or
    NOT,   // not
    TRUE,  // true
    FALSE, // false

    // ─── Operators ────────────────────────────────────────────────────────────
    ASSIGN,         // =
    ARROW,          // ->        - function return boundary (return type separator)
    FAT_ARROW,      // =>        - match arm: case 1 =>
    COMPOSE,        // +>        - compile-time function composition: f +> g +> h
    PIPELINE,       // |>        - runtime pipeline operator: value |> fn1 |> fn2

    // Nullable operators
    QUESTION,          // ?      - nullable type suffix: int?
    QUESTION_DOT,      // ?.     - nullable field chain, propagates nil: player?.weapon?.damage
    QUESTION_QUESTION, // ??     - nil fallback, terminates a ?. chain: ?.field ?? default

    // Type operators
    PIPE,     // |         
    VARIADIC, // ...       - variadic params: args ...int
    RANGE,    // ..        - range literal: 0..10

    // ─── Access ───────────────────────────────────────────────────────────────
    DOT,              // .         - field access: mesh.vertices | module path: std.io
    COLON,            // :         - generic constraint: T : Drawable | switch case: case 1: | match field pattern: Vec2 { x: 0 }
    // DOUBLE_COLON,     // ::     - reserved for future features
    
    // ─── Math ─────────────────────────────────────────────────────────────────
    PLUS,               // +
    MINUS,              // -
    MUL,                // *
    DIV,                // /
    POW,                // ^
    MOD,                // %
    PLUS_ASSIGN,        // +=
    MINUS_ASSIGN,       // -=
    MUL_ASSIGN,         // *=
    DIV_ASSIGN,         // /=
    POW_ASSIGN,         // ^=
    MOD_ASSIGN,         // %=
    BIT_AND_ASSIGN,     // &&=
    BIT_OR_ASSIGN,      // ||=
    BIT_XOR_ASSIGN,     // ~^=
    SHL_ASSIGN,         // <<=
    SHR_ASSIGN,         // >>=

    // ─── Comparison ───────────────────────────────────────────────────────────
    LESS,                // <         - also used as generic open: Buffer<T>
    GREATER,             // >         - also used as generic close: Buffer<T>
    LESS_EQUAL,          // <=
    GREATER_EQUAL,       // >=
    EQUAL_EQUAL,         // ==        - value equality
    EQUAL_EQUAL_EQUAL,   // ===       - reference equality: same memory address
    NOT_EQUAL,           // !=
    BANG,                // !    - pipeline argument pack: scale(2.0)! means upstream injected as first arg or error type

    // ─── Bitwise ──────────────────────────────────────────────────────────────
    BIT_AND,  // &&        - bitwise AND  (integer types only)
    BIT_OR,   // ||        - bitwise OR   (integer types only)
    BIT_XOR,  // ~^
    BIT_NOT,  // ~~   -- bitwise NOT
    SHL,      // <<
    SHR,      // >>

    // ─── Delimiters ───────────────────────────────────────────────────────────
    COMMA,     // ,
    SEMICOLON, // ;
    LPAREN,    // (
    RPAREN,    // )
    LBRACE,    // {
    RBRACE,    // }
    LBRACKET,  // [
    RBRACKET,  // ]

    // ─── Literals ─────────────────────────────────────────────────────────────
    IDENTIFIER,
    WILDCARD,           // _    - match pattern wildcard: matches anything, discards value
    INT_LITERAL,        // 42
    FLOAT_LITERAL,      // 3.14
    STRING_LITERAL,     // "hello"
    RAW_STRING_LITERAL, // r"raw\nno escaping"  - backslashes are literal
    CHAR_LITERAL,       // 'a'
    HEX_LITERAL,        // 0xFF   (important for Vulkan flags/bitmasks)
    BINARY_LITERAL,     // 0b1010

    // ─── Symbols ──────────────────────────────────────────────────────────────
    AT_SIGN,   // @    - attribute prefix: @noinline, @test
    HASH,      // #    - intrinsic prefix: #malloc, #memcpy
    TILDE,     // ~    - type qualifier prefix: ~async, ~cdecl

    AMPERSAND, // &         - reference type: &T  (safe, managed)

    // ─── Error Handling ───────────────────────────────────────────────────────
    RESOLVE,   // resolve TypeA!TypeB, if succeed return TypeA else return TypeB

    // ─── Meta ─────────────────────────────────────────────────────────────────
    DOC_COMMENT,  // /-- ... --/  - block documentation comment, attached to next declaration
    LINE_COMMENT, // -- text      - single-line comment; emitted (not discarded) so the
                  //                Parser can harvest stacked and trailing doc comments.
                  //                The value holds the text after '--', trimmed of the leading space.
    UNKNOWN,      // any unrecognised character — surfaces as an error in the parser
    EOF_TOKEN
};

struct Token {
    TokenType type;
    std::string value; // holds the raw lexeme
    int line;
    int column;
};
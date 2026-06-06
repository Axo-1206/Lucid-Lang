/**
 * @file ParserLookahead.cpp
 * @brief Non‑consuming lookahead helpers for syntax disambiguation.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements pure lookahead functions that inspect the token stream
 * WITHOUT consuming any tokens. They are used to disambiguate syntax when
 * multiple grammar rules share the same prefix.
 * 
 * ## Why Lookahead is Needed
 * 
 * Luc has several ambiguous syntax patterns that require lookahead:
 * 
 *   - `let` can start a variable OR function declaration
 *   - `(` can start a grouped expression OR an anonymous function
 *   - `IDENTIFIER` can be a type name, variable name, or struct literal
 *   - `IDENTIFIER` `{` can be a struct literal OR a block (invalid)
 *   - `IDENTIFIER` `:` can be a method reference (behavior access)
 * 
 * These functions resolve the ambiguity without committing to a parse path.
 * 
 * ## Design Principles
 * 
 *   1. **Pure inspection** – Never modify `ts_.pos_`
 *   2. **Conservative** – Return `false` when uncertain (caller may try another path)
 *   3. **Comment‑aware** – Skip LINE_COMMENT and DOC_COMMENT tokens
 *   4. **Bounded** – Each function has a defined maximum lookahead distance
 * 
 * ## Implementation Pattern
 * 
 * All functions follow this pattern:
 * 
 *   const auto& tokens = ts_.getTokens();
 *   size_t tokenCount = ts_.getTokenCount();
 *   size_t i = ts_.getPos();
 *   
 *   auto skipComments = [&](size_t& idx) { ... };
 *   
 *   // Inspect tokens from position i forward
 *   // Return true if pattern matches, false otherwise
 *   // NEVER call ts_.advance() or modify parser state
 * 
 * @note TokenStream provides getTokens() for read‑only access to the token vector.
 * @see Parser.hpp for the declaration of these helpers
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// looksLikeType
// ============================================================================
// 
// Checks whether the current token stream begins with a valid type annotation.
// 
// Returns true if the current token is:
//   - A primitive type keyword (int, float, string, etc.)
//   - An identifier (named type)
//   - '[' (array type)
//   - '&' (reference type)
//   - '*' (pointer type)
//   - '~' (qualifier start, part of function type)
//   - '(' (function type)
// 
// Used in contexts where a type is expected, such as variable declarations,
// function parameters, and return type annotations.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(1) – only inspects the current token.
// ============================================================================

bool Parser::looksLikeType() const {
    if (ts_.isAtEnd()) return false;
    
    TokenType t = ts_.peekType();
    
    // Primitive type keywords
    if (isPrimitiveTypeToken(t)) return true;
    
    // Identifiers (could be named types or generic parameters)
    if (t == TokenType::IDENTIFIER) return true;
    
    // Type constructors
    if (t == TokenType::AMPERSAND) return true;  // &T (reference)
    if (t == TokenType::MUL) return true;        // *T (pointer)
    if (t == TokenType::LBRACKET) return true;   // [_, T] (array)
    if (t == TokenType::LPAREN) return true;     // (a int) -> T (function type)
    if (t == TokenType::TILDE) return true;      // ~async (a int) -> T
    
    return false;
}

// ============================================================================
// isFunctionTypeAfterParen
// ============================================================================
//
// Checks whether the current position (starting after a '(' token) looks like
// the start of a function type rather than a multi‑return list.
//
// A function type is recognised by:
//   - A parameter list (possibly empty) inside the parentheses, followed by
//     an arrow '->' after the closing ')'.
//   - If the parentheses are empty, we look for '->' immediately after.
//   - If non‑empty, we look for a parameter pattern: IDENTIFIER followed by a
//     type start, then balanced parentheses, then '->'.
//
// This function is non‑consuming – it does NOT modify the token stream.
//
// @param startPos The token index immediately after the '(' that was consumed.
// @return true if the tokens at `startPos` indicate a function type, false otherwise.
//
// ─── Complexity ────────────────────────────────────────────────────────────
// O(n) where n is the number of tokens inside the parentheses and up to '->'.
// ============================================================================

bool Parser::isFunctionTypeAfterParen(size_t startPos) const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = startPos;

    // Local helper to check if a token type can start a type
    auto isTypeStartToken = [&](TokenType tt) -> bool {
        return isPrimitiveTypeToken(tt) ||
               tt == TokenType::IDENTIFIER ||
               tt == TokenType::LBRACKET ||
               tt == TokenType::AMPERSAND ||
               tt == TokenType::MUL ||
               tt == TokenType::LPAREN ||
               tt == TokenType::TILDE;
    };

    // Helper to skip a complete parameter group and return index after its closing ')'
    auto skipParamGroup = [&](size_t pos) -> size_t {
        if (pos >= tokenCount || tokens[pos].type != TokenType::LPAREN) return pos;
        int parenDepth = 1;
        size_t j = pos + 1;
        while (j < tokenCount && parenDepth > 0) {
            j = ts_.skipCommentsFrom(j);
            if (j >= tokenCount) break;
            TokenType tt = tokens[j].type;
            if (tt == TokenType::LPAREN) ++parenDepth;
            else if (tt == TokenType::RPAREN) --parenDepth;
            ++j;
        }
        return j;
    };

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    bool isEmptyParen = (tokens[i].type == TokenType::RPAREN);

    if (isEmptyParen) {
        size_t afterParen = i + 1;
        afterParen = ts_.skipCommentsFrom(afterParen);
        return (afterParen < tokenCount && tokens[afterParen].type == TokenType::ARROW);
    }

    // Use the local type-start checker – it does not depend on parser state
    if (!isTypeStartToken(tokens[i].type)) return false;

    size_t afterFirstGroup = skipParamGroup(i);
    if (afterFirstGroup == i) return false;

    size_t afterGroup = afterFirstGroup;
    afterGroup = ts_.skipCommentsFrom(afterGroup);
    return (afterGroup < tokenCount && tokens[afterGroup].type == TokenType::ARROW);
}

// ============================================================================
// looksLikeFuncDecl
// ============================================================================
// 
// Checks whether the current position looks like a function declaration
// (as opposed to a variable declaration) after 'let' or 'const'.
// 
// Detection pattern:
//   1. IDENTIFIER (function name)
//   2. Optional generic parameters: '<' ... '>' (balanced, may be empty)
//   3. Optional qualifiers: ~async, ~nullable, ~parallel
//   4. '(' (start of first parameter group)
// 
// Returns true if the pattern matches, false otherwise.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(n) where n = number of tokens in generic parameters + qualifiers.
// Each token is inspected once, but no tokens are consumed.
// 
// ─── Example Matches ───────────────────────────────────────────────────────
//   - "add("
//   - "process<T>("
//   - "fetch ~async ("
//   - "map<T, U> ~parallel ("
// 
// ─── Example Non‑Matches ───────────────────────────────────────────────────
//   - "x int =" (variable declaration)
//   - "myStruct {" (struct literal)
// ============================================================================

bool Parser::looksLikeFuncDecl() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();

    // Skip the name IDENTIFIER
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }
    ++i;

    // Skip generic params if present: < ... >
    i = ts_.skipCommentsFrom(i);
    if (i < tokenCount && tokens[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokenCount && depth > 0) {
            i = ts_.skipCommentsFrom(i);
            if (i >= tokenCount) break;
            TokenType tt = tokens[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::SEMICOLON || tt == TokenType::RBRACE) break;
            else if (tt == TokenType::EOF_TOKEN) break;
            ++i;
        }
        if (depth != 0) return false;
    }

    // Skip type qualifiers (~async, ~noinline, etc.)
    i = ts_.skipCommentsFrom(i);
    while (i < tokenCount && tokens[i].type == TokenType::TILDE) {
        ++i;
        i = ts_.skipCommentsFrom(i);
        if (i < tokenCount && tokens[i].type == TokenType::IDENTIFIER) {
            ++i;
        } else {
            return false;
        }
        i = ts_.skipCommentsFrom(i);
    }

    // Need at least one parameter group '('
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::LPAREN) {
        return false;
    }

    return true;
}

// ============================================================================
// looksLikeAnonFunc
// ============================================================================
// 
// Checks whether the current position looks like an anonymous function
// expression (as opposed to a parenthesised grouped expression).
// 
// Detection pattern:
//   1. Optional qualifiers (~async, etc.) – though anonymous functions
//      should not have them, we still handle them.
//   2. '(' (first parameter group)
//   3. Parse the parameter group (balanced parentheses)
//   4. Optional additional curried parameter groups
//   5. After groups, must have either:
//        - '{' (void anonymous function)
//        - '->' followed by a type start
// 
// Returns true if the pattern matches, false otherwise.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(n) where n = number of tokens in parameter groups + optional return type.
// 
// ─── Distinguishing from Grouped Expression ───────────────────────────────
//   - "(x + y)"           → grouped expression (no '->' or '{' after ')')
//   - "(x int) -> int { }" → anonymous function
//   - "() { }"            → zero‑parameter anonymous function
// 
// ─── Note ──────────────────────────────────────────────────────────────────
// Anonymous functions with qualifiers are illegal (E2015), but this function
// still matches them to provide better error recovery.
// ============================================================================

bool Parser::looksLikeAnonFunc() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();

    // Skip type qualifiers (though anonymous functions shouldn't have them)
    while (i < tokenCount) {
        i = ts_.skipCommentsFrom(i);
        if (i >= tokenCount || tokens[i].type != TokenType::TILDE)
            break;
        ++i;
        i = ts_.skipCommentsFrom(i);
        if (i < tokenCount && tokens[i].type == TokenType::IDENTIFIER) {
            ++i;
        } else {
            return false;
        }
    }

    // First parameter group is required
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::LPAREN)
        return false;

    // Helper to parse a parameter group and return index after matching ')'
    auto parseOneGroup = [&](size_t start) -> size_t {
        if (start >= tokenCount || tokens[start].type != TokenType::LPAREN)
            return start;
        int parenDepth = 1;
        size_t j = start + 1;
        while (j < tokenCount && parenDepth > 0) {
            while (j < tokenCount && 
                   (tokens[j].type == TokenType::LINE_COMMENT ||
                    tokens[j].type == TokenType::DOC_COMMENT)) {
                ++j;
            }
            if (j >= tokenCount) break;
            TokenType tt = tokens[j].type;
            if (tt == TokenType::LPAREN) {
                ++parenDepth;
            } else if (tt == TokenType::RPAREN) {
                --parenDepth;
            } else if (tt == TokenType::TILDE) {
                ++j;
                if (j < tokenCount && tokens[j].type == TokenType::IDENTIFIER) ++j;
                continue;
            }
            ++j;
        }
        return j;
    };

    size_t startPos = i;
    i = parseOneGroup(i);
    if (i == startPos) return false;

    // Parse additional curried parameter groups
    while (i < tokenCount) {
        i = ts_.skipCommentsFrom(i);
        if (i >= tokenCount || tokens[i].type != TokenType::LPAREN)
            break;
        startPos = i;
        i = parseOneGroup(i);
        if (i == startPos) return false;
    }

    // Skip comments after the last ')'
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    // Immediate '{' → void anonymous function
    if (tokens[i].type == TokenType::LBRACE)
        return true;

    // Otherwise, require '->' followed by a return type
    if (tokens[i].type != TokenType::ARROW)
        return false;

    ++i;
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    // The token after '->' must start a type
    TokenType retStart = tokens[i].type;
    if (isPrimitiveTypeToken(retStart)) return true;
    switch (retStart) {
        case TokenType::IDENTIFIER:
        case TokenType::LBRACKET:
        case TokenType::AMPERSAND:
        case TokenType::MUL:
        case TokenType::LPAREN:
        case TokenType::TILDE:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// looksLikeStructLiteral
// ============================================================================
// 
// Checks whether the current position looks like a struct literal expression.
// 
// Detection pattern:
//   1. IDENTIFIER (struct type name)
//   2. Optional generic arguments: '<' ... '>'
//   3. '{' (opening brace of struct literal)
// 
// Returns true if the pattern matches, false otherwise.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(n) where n = number of tokens in generic arguments (if present).
// 
// ─── Usage Context ─────────────────────────────────────────────────────────
// Called from parsePrimaryExpr when allowStructLiteral is true. When false
// (e.g., in 'if' conditions), this pattern is disabled to avoid ambiguity
// with the following block.
// 
// ─── Example Matches ───────────────────────────────────────────────────────
//   - "Point {"
//   - "Vec2<int> {"
// 
// ─── Example Non‑Matches ───────────────────────────────────────────────────
//   - "Point(" (function call)
//   - "Vec2 :" (behavior access)
// ============================================================================

bool Parser::looksLikeStructLiteral() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }

    ++i;
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    // Optional generic args: < ... >
    if (tokens[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokenCount && depth > 0) {
            i = ts_.skipCommentsFrom(i);
            if (i >= tokenCount) break;
            TokenType tt = tokens[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::EOF_TOKEN) break;
            ++i;
        }
        if (depth != 0) return false;
        i = ts_.skipCommentsFrom(i);
        if (i >= tokenCount) return false;
    }

    return (i < tokenCount && tokens[i].type == TokenType::LBRACE);
}

// ============================================================================
// looksLikeStmtStart
// ============================================================================
// 
// Checks whether the current token can begin a statement.
// 
// Returns true for:
//   - Declaration keywords (let, const, type, struct, enum, trait, impl, from)
//   - Control flow keywords (if, for, while, do, return, break, continue, match, switch)
//   - Compiler directives (@, #)
//   - Expression starters (identifier, literal, unary operators, await, '(')
// 
// Used in parseStmt() to decide whether to parse a statement or treat the
// input as an expression statement (fallback).
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(1) – only inspects the current token (plus calls looksLikeType).
// ============================================================================

bool Parser::looksLikeStmtStart() const {
    TokenType tt = ts_.peekType();
    
    switch (tt) {
        case TokenType::LET:
        case TokenType::CONST:
        case TokenType::IF:
        case TokenType::FOR:
        case TokenType::WHILE:
        case TokenType::DO:
        case TokenType::RETURN:
        case TokenType::BREAK:
        case TokenType::CONTINUE:
        case TokenType::MATCH:
        case TokenType::SWITCH:
        case TokenType::AT_SIGN:
        case TokenType::HASH:
        case TokenType::TYPE:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::FROM:
            return true;
        default:
            return looksLikeType() || tt == TokenType::IDENTIFIER ||
                   tt == TokenType::INT_LITERAL || tt == TokenType::FLOAT_LITERAL ||
                   tt == TokenType::STRING_LITERAL || tt == TokenType::RAW_STRING_LITERAL ||
                   tt == TokenType::CHAR_LITERAL || tt == TokenType::HEX_LITERAL ||
                   tt == TokenType::BINARY_LITERAL || tt == TokenType::TRUE ||
                   tt == TokenType::FALSE || tt == TokenType::NIL ||
                   tt == TokenType::MINUS || tt == TokenType::NOT ||
                   tt == TokenType::BIT_NOT || tt == TokenType::AMPERSAND ||
                   tt == TokenType::AWAIT || tt == TokenType::LPAREN;
    }
}

// ============================================================================
// looksLikeDeclStart
// ============================================================================
// 
// Checks whether the current token can begin a top‑level or local declaration.
// 
// Returns true for:
//   - '@' (attribute, may precede any declaration)
//   - 'package', 'use'
//   - Visibility modifiers: 'pub', 'export'
//   - Declaration keywords: 'struct', 'enum', 'trait', 'impl', 'type', 'from'
//   - 'let', 'const'
// 
// Used in synchronize() to stop skipping tokens when a declaration boundary
// is reached during error recovery.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(1) – only inspects the current token.
// ============================================================================

bool Parser::looksLikeDeclStart() const {
    TokenType tt = ts_.peekType();
    
    switch (tt) {
        case TokenType::AT_SIGN:
        case TokenType::PACKAGE:
        case TokenType::USE:
        case TokenType::PUB:
        case TokenType::EXPORT:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::TYPE:
        case TokenType::FROM:
        case TokenType::LET:
        case TokenType::CONST:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// looksLikeMultiAssignStart
// ============================================================================
// 
// Checks whether the current position looks like a multi‑assignment statement
// (reassignment to multiple existing variables).
// 
// Detection pattern:
//   1. IDENTIFIER (start of first lvalue)
//   2. Optional suffixes: .field or [index]
//   3. ',' (comma separating lvalues)
//   4. Eventually '=' (assignment operator)
// 
// Returns true if the pattern matches, false otherwise.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(n) where n = distance to the next '=' or statement boundary.
// 
// ─── Example Matches ───────────────────────────────────────────────────────
//   - "a, b = f()"
//   - "arr[i], obj.field = g()"
// 
// ─── Example Non‑Matches ───────────────────────────────────────────────────
//   - "a = b" (single assignment)
//   - "let a, b = f()" (declaration, not reassignment)
// 
// Used in parseStmt() to distinguish multi‑assignment from single assignment
// and from multi‑variable declaration.
// ============================================================================

bool Parser::looksLikeMultiAssignStart() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();

    // Skip leading comments
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    // First token must be an identifier
    if (tokens[i].type != TokenType::IDENTIFIER) return false;
    ++i;

    // Parse optional suffixes for the first lvalue
    while (i < tokenCount) {
        i = ts_.skipCommentsFrom(i);
        if (i >= tokenCount) break;

        // Field access: .identifier
        if (tokens[i].type == TokenType::DOT) {
            ++i;
            i = ts_.skipCommentsFrom(i);
            if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
                return false;
            }
            ++i;
        }
        // Array index: [ expr ]
        else if (tokens[i].type == TokenType::LBRACKET) {
            ++i;
            int bracketDepth = 1;
            while (i < tokenCount && bracketDepth > 0) {
                i = ts_.skipCommentsFrom(i);
                if (i >= tokenCount) break;
                if (tokens[i].type == TokenType::LBRACKET)
                    ++bracketDepth;
                else if (tokens[i].type == TokenType::RBRACKET)
                    --bracketDepth;
                ++i;
            }
            if (bracketDepth != 0) return false;
        } else {
            break;
        }
    }

    // After the first lvalue, look for a comma
    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::COMMA) return false;

    // Found a comma, now scan for '='
    ++i;
    while (i < tokenCount) {
        i = ts_.skipCommentsFrom(i);
        if (i >= tokenCount) break;
        TokenType tt = tokens[i].type;
        if (tt == TokenType::ASSIGN) return true;
        if (tt == TokenType::SEMICOLON || tt == TokenType::LBRACE ||
            tt == TokenType::RBRACE || tt == TokenType::EOF_TOKEN) {
            return false;
        }
        ++i;
    }
    
    return false;
}

// ============================================================================
// looksLikeBehaviorAccess
// ============================================================================
// 
// Checks whether the current position looks like a behavior access expression
// (method reference) as opposed to a plain identifier or struct literal.
// 
// Detection pattern:
//   1. IDENTIFIER (type name)
//   2. Optional generic arguments: '<' ... '>'
//   3. ':' (colon separator)
//   4. IDENTIFIER (method name)
// 
// Returns true if the pattern matches, false otherwise.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(n) where n = number of tokens in generic arguments (if present).
// 
// ─── Example Matches ───────────────────────────────────────────────────────
//   - "Vec2:normalize"
//   - "Buffer<int>:create"
// 
// ─── Example Non‑Matches ───────────────────────────────────────────────────
//   - "Point { x = 0 }" (struct literal)
//   - "Matrix::identity" (static access, not supported – note '::' is not in grammar)
// 
// Used in parsePrimaryExpr() to distinguish method references from plain
// identifiers and struct literals.
// ============================================================================

bool Parser::looksLikeBehaviorAccess() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();

    i = ts_.skipCommentsFrom(i);
    
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }
    ++i;

    // Optional generic args: '<' ... '>'
    if (i < tokenCount && tokens[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokenCount && depth > 0) {
            i = ts_.skipCommentsFrom(i);
            if (i >= tokenCount) break;
            TokenType tt = tokens[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::SEMICOLON || tt == TokenType::RBRACE) return false;
            ++i;
        }
        if (depth != 0) return false;
    }

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::COLON) {
        return false;
    }
    ++i;

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }

    return true;
}

// ============================================================================
// isPrimitiveTypeToken
// ============================================================================
// 
// Static helper that returns true if a TokenType is a primitive type keyword.
// 
// Primitive types include:
//   - Boolean:   bool
//   - Signed:    byte, short, int, long, int8, int16, int32, int64
//   - Unsigned:  ubyte, ushort, uint, ulong, uint8, uint16, uint32, uint64
//   - Floating:  float, double, decimal
//   - Text:      string, char
//   - Dynamic:   any
// 
// Used by looksLikeType() and various type parsers.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(1) – switch statement on the enum value.
// 
// ─── Note ──────────────────────────────────────────────────────────────────
// This is static because it only depends on the TokenType, not on parser state.
// ============================================================================

bool Parser::isPrimitiveTypeToken(TokenType type) {
    switch (type) {
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_BYTE:
        case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:
        case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:
        case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:
        case TokenType::TYPE_ULONG:
        case TokenType::TYPE_INT8:
        case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:
        case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:
        case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32:
        case TokenType::TYPE_UINT64:
        case TokenType::TYPE_FLOAT:
        case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING:
        case TokenType::TYPE_CHAR:
        case TokenType::TYPE_ANY:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Checks whether the current position looks like a generic array target.
 *
 * Used by parseImplDecl() to distinguish between:
 *   - Concrete array: `[_, int]`
 *   - Generic array:  `[_, <T>]`
 *
 * Detection pattern:
 *   - Current token is '['
 *   - After parsing the array kind (`_`, `*`, or integer) and comma,
 *     the next non‑comment token is '<'
 *
 * @return true if the next token after array kind and comma is '<'
 *
 * @note This function is non‑consuming – it does NOT modify the token stream.
 */
bool Parser::looksLikeGenericArray() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::LBRACKET) {
        return false;
    }
    ++i; // skip '['

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    // Check array kind (_, *, or integer)
    if (tokens[i].type == TokenType::WILDCARD ||
        tokens[i].type == TokenType::MUL ||
        tokens[i].type == TokenType::INT_LITERAL) {
        ++i;
    } else {
        return false;
    }

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::COMMA) {
        return false;
    }
    ++i; // skip ','

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    // If the next token is '<', this is a generic array
    return tokens[i].type == TokenType::LESS;
}

/**
 * @brief Checks whether the current position looks like a generic type
 *        instantiation (e.g., `Wrapper<T>`).
 *
 * Detection pattern:
 *   - Current token is IDENTIFIER
 *   - The next non‑comment token is '<'
 *
 * @return true if the next token after identifier is '<'
 */
bool Parser::looksLikeGenericTypeInstantiation() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }
    ++i;

    i = ts_.skipCommentsFrom(i);
    if (i >= tokenCount) return false;

    return tokens[i].type == TokenType::LESS;
}
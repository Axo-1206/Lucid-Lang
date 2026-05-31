/**
 * @file ParserAttributes.cpp
 * @brief Attribute parsing for the Luc parser.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the parsing of compiler attributes (e.g., `@inline`,
 * `@deprecated`, `@extern`). Attributes are metadata attached to declarations
 * that influence code generation, linking, or provide compile-time information.
 * 
 * ## Attribute Syntax
 * 
 *   attribute       := '@' IDENTIFIER [ '(' attr_arg_list ')' ]
 *   attr_arg_list   := attr_arg { ',' attr_arg }
 *   attr_arg        := STRING_LITERAL | INT_LITERAL | HEX_LITERAL 
 *                    | 'true' | 'false' | IDENTIFIER
 * 
 * ## Known Attributes (from LUC_GRAMMAR.md)
 * 
 * | Attribute                | Valid on                | Purpose                                   |
 * | ------------------------ | ----------------------- | ----------------------------------------- |
 * | `@extern("sym")`         | `let`, `const` func/var | Bind to C/OS/Vulkan symbol                |
 * | `@extern("sym", "conv")` | `let`, `const` func/var | With explicit calling convention          |
 * | `@inline`                | func                    | Suggest always inline                     |
 * | `@noinline`              | func                    | Prevent inlining                          |
 * | `@packed`                | `struct`                | Remove padding — all fields byte-adjacent |
 * | `@deprecated("msg")`     | func, var, struct       | Emit warning at every use site            |
 * | `@phantom`               | `type` alias, `struct`, func | Allow unused generic parameters       |
 * | `@aot`                   | `main` only             | Ahead-of-time compilation                 |
 * | `@jit`                   | `main` only             | JIT compilation                           |
 * 
 * ## Attribute Arguments
 * 
 * Attribute arguments are intentionally limited to compile-time literals and
 * type identifiers. Runtime expressions are not valid inside attribute arguments.
 * 
 * ## Usage Flow
 * 
 *   parseAttributes() → collects all `@` tokens before a declaration
 *                    → each attribute is parsed by parseAttribute()
 *                    → arguments are parsed by parseAttributeArgLiteral()
 *                    → collected vector is attached to the declaration node
 * 
 * @see Parser.cpp for parseDeclaration() which calls parseAttributes()
 * @see AttributeAST in BaseAST.hpp for AST representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// 1. ATTRIBUTE COLLECTION
// ============================================================================

/**
 * @brief Parses a sequence of attributes preceding a declaration.
 *
 * Called from parseDeclaration() before parsing visibility and the actual
 * declaration. Consumes all consecutive '@' tokens and their arguments.
 *
 * Grammar: { '@' IDENTIFIER [ '(' attr_arg_list ')' ] }
 *
 * Example: `@inline @deprecated("Use newAPI")`
 *
 * @return std::vector<AttributePtr> – temporary collection of parsed attributes.
 *         May be empty if no attributes are present.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '@' or the first token of the declaration
 * On exit:  positioned after all attributes (at the start of the declaration)
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - If parseAttribute() fails, continues to next attribute (if any)
 * - Stops when a non-'@' token is encountered
 */
std::vector<AttributePtr> Parser::parseAttributes() {
    std::vector<AttributePtr> attrs;
    while (ts_.check(TokenType::AT_SIGN)) {
        AttributePtr attr = parseAttribute();
        if (attr) attrs.push_back(std::move(attr));
    }
    return attrs;
}

// ============================================================================
// 2. SINGLE ATTRIBUTE PARSER
// ============================================================================

/**
 * @brief Parses a single attribute and its optional arguments.
 *
 * Grammar: '@' IDENTIFIER [ '(' attr_arg_list ')' ]
 *
 * Example: `@deprecated("Use newAPI")`
 *
 * @return AttributePtr – parsed attribute node, or nullptr on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '@'
 * On exit:  positioned after the attribute (and its arguments, if any)
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing attribute name after '@': reports error, returns nullptr
 * - Missing ')' after argument list: consume() reports error
 * - Invalid argument: parseAttributeArgLiteral() returns nullptr, skipped
 */
AttributePtr Parser::parseAttribute() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AT_SIGN, "expected '@'");
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected attribute name after '@'");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    auto attr = arena_.make<AttributeAST>();
    attr->name = name;
    attr->loc = loc;

    // Parse optional argument list
    if (ts_.match(TokenType::LPAREN)) {
        std::vector<AttributeArgPtr> args;
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            if (!args.empty() && !ts_.match(TokenType::COMMA))
                break;
            AttributeArgPtr arg = parseAttributeArgLiteral();
            if (arg) args.push_back(std::move(arg));
        }
        ts_.consume(TokenType::RPAREN, "expected ')' after attribute arguments");
        
        auto builder = arena_.makeBuilder<AttributeArgPtr>();
        for (auto& a : args) builder.push_back(std::move(a));
        attr->args = builder.build();
    }
    return attr;
}

// ============================================================================
// 3. ATTRIBUTE ARGUMENT PARSER
// ============================================================================

/**
 * @brief Parses a single literal argument inside an attribute argument list.
 *
 * Grammar:
 *   attr_arg := STRING_LITERAL | INT_LITERAL | HEX_LITERAL | BINARY_LITERAL
 *             | 'true' | 'false' | IDENTIFIER
 *
 * @return AttributeArgPtr – parsed argument node, or nullptr on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the literal token
 * On exit:  positioned after the literal token
 *
 * ─── Supported Literal Types ───────────────────────────────────────────────
 * | Token Type              | AttributeArgKind | Example                 |
 * |-------------------------|------------------|-------------------------|
 * | STRING_LITERAL          | StringLit        | `"hello"`               |
 * | INT_LITERAL             | IntLit           | `42`                    |
 * | HEX_LITERAL             | IntLit           | `0xFF`                  |
 * | BINARY_LITERAL          | IntLit           | `0b1010`                |
 * | TRUE / FALSE            | BoolLit          | `true`, `false`         |
 * | IDENTIFIER              | TypeIdent        | `C` (calling convention)|
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Unrecognised token: reports error, returns nullptr
 * - Caller (parseAttribute) skips the argument and continues
 */
AttributeArgPtr Parser::parseAttributeArgLiteral() {
    // String literal
    if (ts_.check(TokenType::STRING_LITERAL)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::StringLit,
                                            pool_.intern(ts_.advance().value));
    }
    
    // Integer literals (decimal, hex, binary)
    if (ts_.check(TokenType::INT_LITERAL) || 
        ts_.check(TokenType::HEX_LITERAL) ||
        ts_.check(TokenType::BINARY_LITERAL)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::IntLit,
                                            pool_.intern(ts_.advance().value));
    }
    
    // Boolean literals
    if (ts_.check(TokenType::TRUE) || ts_.check(TokenType::FALSE)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::BoolLit,
                                            pool_.intern(ts_.advance().value));
    }
    
    // Type identifier (e.g., calling convention name)
    if (ts_.check(TokenType::IDENTIFIER)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::TypeIdent,
                                            pool_.intern(ts_.advance().value));
    }
    
    errorAt(DiagCode::E2002, 
            "expected string, integer, boolean, or type identifier in attribute argument");
    return nullptr;
}
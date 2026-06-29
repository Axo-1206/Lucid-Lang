/**
 * @file Helpers.cpp
 * @brief Implementation of parser helper functions.
 * 
 * This file implements helper functions used by the parser:
 * - harvestDocComment: Collects documentation comments attached to declarations
 * - parseAttributes: Parses attribute lists (@[attr1, attr2])
 * - parseAttribute: Parses a single attribute
 * - parseAttributeArgLiteral: Parses attribute argument literals
 */

#include "Parser.hpp"
#include "core/Tokens.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace parser {

// =============================================================================
// harvestDocComment - Collect documentation comments
// =============================================================================

/**
 * @brief Harvests documentation comments attached to the upcoming declaration.
 * 
 * This function scans backward from the current parser position to find
 * comments that should be attached to the next declaration.
 * 
 * ## Doc Comment Attachment Rules (from Grammar.md)
 * 
 * 1. Stacked `--` lines immediately above → attach as stacked doc
 * 2. `--` on the same line → trailing doc
 * 3. `/-- --/` immediately above → block doc
 * 4. Stacked above + trailing on same line → stacked wins, trailing ignored
 * 5. Blank line between comment and declaration → comment is floating, not attached
 * 6. `--` lines above a `/-- --/` block → block attaches; `--` lines above it are floating
 * 
 * ## Priority (highest to lowest)
 * 
 * 1. Block doc comment (`/-- ... --/`)
 * 2. Stacked line comments (consecutive `--` lines)
 * 3. Trailing comment (`--` on same line)
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return std::optional<DocComment> The harvested doc comment, or std::nullopt if none found
 * 
 * ## Example
 * 
 * ```lucid
 * -- normalizes the vector in place
 * -- only call after the vector has been validated
 * const normalize (v Vec2) -> Vec2 = { ... }    -- stacked attaches
 * 
 * const maxVertices int = 65536   -- Vulkan hard limit   -- trailing attaches
 * 
 * /--
 *  - Computes the dot product of two vectors.
 *  -
 *  - Returns `|a| * |b| * cos(angle)`.
 * --/
 * const dot (other Vec2) -> float = { ... }    -- block attaches
 * ```
 */
std::optional<DocComment> harvestDocComment(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("harvestDocComment: checking for doc comment");
    
    const auto& tokens = stream.getTokens();
    size_t pos = stream.getPos();
    
    if (pos == 0) return std::nullopt;
    
    int declLine = stream.peek().line;
    std::optional<std::string> trailinGREATERext;
    std::vector<std::string> stackedLines;
    int stackedTopLine = -1;
    std::optional<std::string> blockText;
    
    // Scan backward from current position
    for (size_t i = pos; i > 0; ) {
        --i;
        const Token& t = tokens[i];
        
        if (t.type == TokenType::LINE_COMMENT) {
            if (t.line <= 0) continue;
            
            // Check if it's on the same line as the declaration
            if (t.line == declLine) {
                if (!trailinGREATERext.has_value()) {
                    trailinGREATERext = t.value;
                }
                continue;
            }
            
            // Check if it's stacked (immediately above the declaration)
            if (stackedLines.empty()) {
                if (declLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            } else {
                // Check if it's part of the stacked block
                if (stackedTopLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            }
        }
        
        if (t.type == TokenType::DOC_COMMENT) {
            if (t.line <= 0) continue;
            // Doc comment must be immediately above the declaration
            if (declLine - t.line <= 1) {
                blockText = t.value;
            }
            break;
        }
        
        // Stop at any other token type
        break;
    }
    
    // Priority: Block > Stacked > Trailing
    if (blockText.has_value()) {
        LOG_PARSER_DETAIL("harvestDocComment: found block doc comment");
        return DocComment{ctx.pool.intern(*blockText), DocCommentForm::Block};
    }
    
    if (!stackedLines.empty()) {
        LOG_PARSER_DETAIL("harvestDocComment: found ", stackedLines.size(), " stacked line comments");
        // Combine stacked lines in reverse order (preserving original order)
        std::string combined;
        for (int i = static_cast<int>(stackedLines.size()) - 1; i >= 0; --i) {
            if (!combined.empty()) combined += '\n';
            combined += stackedLines[i];
        }
        return DocComment{ctx.pool.intern(combined), DocCommentForm::Stacked};
    }
    
    if (trailinGREATERext.has_value()) {
        LOG_PARSER_DETAIL("harvestDocComment: found trailing comment");
        return DocComment{ctx.pool.intern(*trailinGREATERext), DocCommentForm::Trailing};
    }
    
    return std::nullopt;
}

// =============================================================================
// parseAttributes - Parse attribute lists
// =============================================================================

/**
 * @brief Parse a list of attributes.
 * 
 * Grammar: `@[ attr_item { ',' attr_item } ]`
 * 
 * Attributes precede the declaration they annotate and use a bracket-list
 * syntax so muLESSiple items share one delimiter pair.
 * 
 * ## BuiLESS-in Attributes (from Grammar.md)
 * 
 * | Attribute              | Valid on                  | Meaning                                        |
 * | ---------------------- | ------------------------- | ---------------------------------------------- |
 * | `@[export]`            | any top-level declaration | Visible outside this module                    |
 * | `@[foreign("abi")]`    | function declaration      | Implemented in a foreign language              |
 * | `@[link("name")]`      | module or declaration     | Link against this native library               |
 * | `@[deprecated("msg")]` | any declaration           | Language warning at use sites                  |
 * | `@[inline]`            | function declaration      | Hint to inline at call sites                   |
 * | `@[noinline]`          | function declaration      | Prevent inlining                               |
 * 
 * parseAttributes()
 *   │
 *   ├── Consumes '@'
 *   ├── Consumes '['
 *   │
 *   ├── while (not ']') {
 *   │     │
 *   │     └── parseAttribute()
 *   │           │
 *   │           ├── Parses name
 *   │           ├── if '(' then consumes '('
 *   │           │   │
 *   │           │   └── while (not ')') {
 *   │           │         ├── parseAttributeArgLiteral()
 *   │           │         └── if ',' then consumes ','
 *   │           │       }
 *   │           │
 *   │           └── Consumes ')'  (owned by parseAttribute)
 *   │
 *   │     if ',' then consumes ','  (owned by parseAttributes)
 *   │
 *   └── Consumes ']'  (owned by parseAttributes)
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<AttributePtr> The parsed attributes (empty if none)
 * 
 * ## Examples
 * 
 * ```lucid
 * @[export] const main () -> int = { return 0 }
 * @[foreign("C")] const malloc (size uint64) -> ptr<byte>? = {}
 * @[deprecated("use maxConnections instead")] const max_conn int = 100
 * @[inline] const add (a int)(b int) -> int = { return a + b }
 * ```
 */
ArenaSpan<AttributePtr> parseAttributes(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseAttributes: checking for attributes");
    
    std::vector<AttributePtr> attrs;
    
    // Check if we have an attribute list
    if (!stream.check(TokenType::AT_SIGN)) {
        return ctx.arena.makeBuilder<AttributePtr>().build();
    }
    
    // Consume the '@' token
    stream.advance();
    
    // Expect '[' after '@'
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.error(stream, DiagCode::E1004, "[", "attribute list", stream.peekValue());
        synchronize(stream, ctx);
        return ctx.arena.makeBuilder<AttributePtr>().build();
    }
    stream.advance(); // Consume '['
    
    // Check for empty attribute list: @[]
    if (stream.check(TokenType::RBRACKET)) {
        stream.advance(); // Consume ']'
        return ctx.arena.makeBuilder<AttributePtr>().build();
    }
    
    // Parse attributes until we hit ']'
    while (!stream.isAtEnd()) {
        // Parse a single attribute
        AttributePtr attr = parseAttribute(stream, ctx);
        if (attr) {
            attrs.push_back(attr);
        } else {
            // Error already reported, try to recover
            synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::RBRACKET});
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
            }
            continue;
        }
        
        // Check for comma separator
        if (stream.check(TokenType::COMMA)) {
            stream.advance(); // Consume comma
            
            // Check for trailing comma: @[attr1, attr2, ]
            if (stream.check(TokenType::RBRACKET)) {
                ctx.error(stream, DiagCode::E1103); // Unexpected trailing comma
                // Consume the ']' and break
                stream.advance();
                break;
            }
            
            // Continue to parse next attribute
        } else if (stream.check(TokenType::RBRACKET)) {
            // End of attribute list
            stream.advance(); // Consume ']'
            break;
        } else {
            // Unexpected token
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or ']'");
            synchronizeTo(stream, ctx, {TokenType::RBRACKET});
            if (stream.check(TokenType::RBRACKET)) {
                stream.advance();
            }
            break;
        }
    }
    
    // Build the ArenaSpan
    auto builder = ctx.arena.makeBuilder<AttributePtr>();
    for (auto* attr : attrs) {
        builder.push_back(attr);
    }
    return builder.build();
}

// =============================================================================
// parseAttribute - Parse a single attribute
// =============================================================================

/**
 * @brief Parse a single attribute.
 * 
 * Grammar: `IDENTIFIER [ '(' attr_args ')' ]`
 * 
 * ## Examples
 * 
 * ```lucid
 * @[export]                         → name = "export", args = {}
 * @[foreign("C")]                   → name = "foreign", args = ["C"]
 * @[deprecated("use maxConnections")] → name = "deprecated", args = ["use maxConnections"]
 * @[link("opengl", "m")]            → name = "link", args = ["opengl", "m"]
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return AttributePtr The parsed attribute node, or nullptr on error
 */
AttributePtr parseAttribute(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Expect an identifier for the attribute name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "attribute name", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // Create the attribute node
    auto* attr = ctx.arena.make<AttributeAST>();
    attr->loc = loc;
    attr->name = name;
    
    // Check for arguments
    std::vector<AttributeArgPtr> args;
    if (stream.match(TokenType::LPAREN)) {
        // Check for empty arguments: @name()
        if (stream.check(TokenType::RPAREN)) {
            stream.advance(); // Consume ')'
        } else {
            // Parse arguments until we hit ')'
            while (!stream.isAtEnd()) {
                AttributeArgPtr arg = parseAttributeArgLiteral(stream, ctx);
                if (arg) {
                    args.push_back(arg);
                } else {
                    // Error already reported, try to recover
                    synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::RPAREN});
                    if (stream.check(TokenType::COMMA)) {
                        stream.advance();
                    }
                    continue;
                }
                
                // Check for comma separator
                if (stream.check(TokenType::COMMA)) {
                    stream.advance(); // Consume comma
                    
                    // Check for trailing comma: @name(arg1, )
                    if (stream.check(TokenType::RPAREN)) {
                        ctx.error(stream, DiagCode::E1103); // Unexpected trailing comma
                        stream.advance(); // Consume ')'
                        break;
                    }
                    
                    // Continue to parse next argument
                } else if (stream.check(TokenType::RPAREN)) {
                    // End of arguments
                    stream.advance(); // Consume ')'
                    break;
                } else {
                    // Unexpected token
                    ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or ')'");
                    synchronizeTo(stream, ctx, {TokenType::RPAREN});
                    if (stream.check(TokenType::RPAREN)) {
                        stream.advance();
                    }
                    break;
                }
            }
        }
    }
    
    // Build the args span
    auto builder = ctx.arena.makeBuilder<AttributeArgPtr>();
    for (auto* arg : args) {
        builder.push_back(arg);
    }
    attr->args = builder.build();
    
    LOG_PARSER("parseAttribute: parsed attribute '", 
               ctx.toString(name), "' with ", args.size(), " args");
    
    return attr;
}

// =============================================================================
// parseAttributeArgLiteral - Parse an attribute argument literal
// =============================================================================

/**
 * @brief Parse an attribute argument literal.
 * 
 * Grammar: `STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT | IDENTIFIER`
 * 
 * ## Examples
 * 
 * ```lucid
 * @[foreign("C")]           → STRING_LIT: "C"
 * @[deprecated("message")]  → STRING_LIT: "message"
 * @[link("opengl")]         → STRING_LIT: "opengl"
 * @[inline]                 → IDENTIFIER: inline (no args)
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return AttributeArgPtr The parsed argument node, or nullptr on error
 */
AttributeArgPtr parseAttributeArgLiteral(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    Token tok = stream.peek();
    
    AttributeArgKind kind;
    InternedString value;
    
    switch (tok.type) {
        case TokenType::STRING_LITERAL:
            kind = AttributeArgKind::StringLit;
            value = ctx.pool.intern(tok.value);
            stream.advance();
            break;
            
        case TokenType::INT_LITERAL:
        case TokenType::HEX_LITERAL:
        case TokenType::BINARY_LITERAL:
        case TokenType::CHAR_LITERAL:
            kind = AttributeArgKind::IntLit;
            value = ctx.pool.intern(tok.value);
            stream.advance();
            break;
            
        case TokenType::FLOAT_LITERAL:
            kind = AttributeArgKind::FloatLit;
            value = ctx.pool.intern(tok.value);
            stream.advance();
            break;
            
        case TokenType::TRUE:
        case TokenType::FALSE:
            kind = AttributeArgKind::BoolLit;
            value = ctx.pool.intern(tok.value);
            stream.advance();
            break;
            
        case TokenType::IDENTIFIER:
            kind = AttributeArgKind::TypeIdent;
            value = ctx.pool.intern(tok.value);
            stream.advance();
            break;
            
        default:
            ctx.error(stream, DiagCode::E1104, stream.peekValue());
            synchronize(stream, ctx);
            return nullptr;
    }
    
    auto* arg = ctx.arena.make<AttributeArgAST>(kind, value);
    arg->loc = loc;
    
    LOG_PARSER("parseAttributeArgLiteral: parsed argument of type ", 
               static_cast<int>(kind));
    
    return arg;
}

// =============================================================================
// parseGenericParamDecls - Parse a list of generic parameters
// =============================================================================

/**
 * @brief Parse a list of generic parameters.
 * 
 * Grammar: `'<' generic_param { ',' generic_param } '>'`
 * 
 * ## Examples
 * 
 * ```lucid
 * struct Box<T> { ... }                    // <T>
 * struct Pair<A, B> { ... }                // <A, B>
 * const process<T : Vector2, U> (v T) ...  // <T : Vector2, U>
 * ```
 * 
 * ## Error Handling
 * 
 * - Empty list `<>` → reports E1009
 * - Trailing comma `<T, >` → reports E1103
 * - Trailing plus `<T : Vector2 + >` → reports E1105
 * - Missing `>` at EOF → reports E1005
 * 
 * parseGenericParamDecls()
 *  ├── Consumes '<'
 *  ├── parseGenericParamDecl()
 *  │   ├── Parses "T"
 *  │   ├── Consumes ':'
 *  │   ├── Parses "Vector2"
 *  │   ├── Consumes '+'
 *  │   ├── Sees '>' → reports E1105, breaks (does NOT consume '>')
 *  │   └── Returns param
 *  ├── Sees '>' → consumes '>'
 *  └── Returns
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<GenericParamDeclPtr> The parsed generic parameters (empty on error)
 */
ArenaSpan<GenericParamDeclPtr> parseGenericParamDecls(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericParamDecls: parsing generic parameters");
    
    std::vector<GenericParamDeclPtr> params;
    
    // Expect '<' to start the list
    if (!stream.check(TokenType::LESS)) {
        ctx.error(stream, DiagCode::E1004, "<", "generic parameter list", stream.peekValue());
        return ctx.arena.makeBuilder<GenericParamDeclPtr>().build();
    }
    stream.advance(); // Consume '<'
    
    // Check for empty generic parameter list: <>
    if (stream.check(TokenType::GREATER)) {
        ctx.error(stream, DiagCode::E1009, "generic parameter", ">");
        stream.advance(); // Consume '>'
        return ctx.arena.makeBuilder<GenericParamDeclPtr>().build();
    }
    
    // Parse parameters until we hit '>'
    while (!stream.isAtEnd()) {
        // ─── Parse a single generic parameter ───────────────────────────
        GenericParamDeclPtr param = parseGenericParamDecl(stream, ctx);
        if (param) {
            params.push_back(param);
        } else {
            // Error already reported, try to recover
            synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::GREATER});
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
            } else if (stream.check(TokenType::GREATER)) {
                // We found the closing '>', consume it and exit
                stream.advance();
                break;
            } else {
                // No recovery token found - break to avoid infinite loop
                break;
            }
            continue;
        }
        
        // ─── Check for comma separator ──────────────────────────────────
        if (stream.check(TokenType::COMMA)) {
            stream.advance(); // Consume comma
            
            // Check for trailing comma: <T, U, >
            if (stream.check(TokenType::GREATER)) {
                ctx.error(stream, DiagCode::E1103); // Unexpected trailing comma
                stream.advance(); // Consume '>'
                break;
            }
            
            // Check if we're at EOF after comma
            if (stream.isAtEnd()) {
                ctx.error(stream, DiagCode::E1005, ">", "generic parameter list", "<EOF>");
                break;
            }
            
            // Continue to parse next parameter
        } else if (stream.check(TokenType::GREATER)) {
            // End of generic parameter list
            stream.advance(); // Consume '>'
            break;
        } else {
            // Unexpected token
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or '>'");
            synchronizeTo(stream, ctx, {TokenType::GREATER});
            if (stream.check(TokenType::GREATER)) {
                stream.advance(); // Consume '>' and exit
            }
            break;
        }
    }
    
    // ─── Check if we exited because of EOF ──────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ">", "generic parameter list", "<EOF>");
    }
    
    // Build the ArenaSpan
    auto builder = ctx.arena.makeBuilder<GenericParamDeclPtr>();
    for (auto* p : params) {
        builder.push_back(p);
    }
    
    LOG_PARSER_DETAIL("parseGenericParamDecls: parsed ", params.size(), " generic parameters");
    
    return builder.build();
}

// =============================================================================
// parseGenericParamDecl - Parse a single generic parameter declaration
// =============================================================================

/**
 * @brief Parse a single generic parameter declaration.
 * 
 * Grammar: `IDENTIFIER [ ':' trait_ref { '+' trait_ref } ]`
 * 
 * ## Examples
 * 
 * ```lucid
 * struct Box<T> { ... }                    // T (unconstrained)
 * const magnitude<T : Vector2> (v T) ...   // T constrained by Vector2
 * struct Pair<A : Named + Serializable>    // A constrained by Named and Serializable
 * ```
 * 
 * ## Error Handling
 * 
 * - Trailing '+' (e.g., `T : Vector2 +`) → reports E1105
 * - Missing trait after '+' → reports E1009
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return GenericParamDeclPtr The parsed generic parameter, or nullptr on error
 * 
 * @note This function does NOT consume `>` or `,`. That is handled by
 *       the caller (parseGenericParamDecls).
 */
GenericParamDeclPtr parseGenericParamDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Expect an identifier for the parameter name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "generic parameter name", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // Create the generic parameter node
    auto* param = ctx.arena.make<GenericParamDeclAST>(name);
    param->loc = loc;
    
    // Check for constraints (optional)
    if (stream.match(TokenType::COLON)) {
        std::vector<TraitRefPtr> constraints;
        bool hasConstraint = false;
        
        // Parse trait references separated by '+'
        while (!stream.isAtEnd()) {
            // ─── Check for trailing '+' ────────────────────────────────
            if (stream.check(TokenType::PLUS)) {
                if (!hasConstraint) {
                    // '+' before any trait - trailing plus error
                    ctx.error(stream, DiagCode::E1105);
                    stream.advance(); // Consume '+'
                    continue;
                } else {
                    // '+' after a trait - check if it's a trailing plus
                    stream.advance(); // Consume '+'
                    
                    // If we're at EOF, '>', or ',', it's a trailing plus
                    if (stream.isAtEnd()) {
                        break;
                    } else if (stream.check(TokenType::GREATER) || stream.check(TokenType::COMMA)) {
                        ctx.error(stream, DiagCode::E1105);
                        // IMPORTANT: We DO NOT consume '>' or ',' here.
                        // The caller (parseGenericParamDecls) will handle the closing '>'
                        // or the next parameter after ','.
                        break;
                    }
                    // Otherwise, continue to parse next trait
                    continue;
                }
            }
            
            // Parse a trait reference
            TraitRefPtr traitRef = parseTraitRef(stream, ctx);
            if (traitRef) {
                constraints.push_back(traitRef);
                hasConstraint = true;
            } else {
                ctx.error(stream, DiagCode::E1009, "trait reference after ':' or '+'", stream.peekValue());
                synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::GREATER});
                break;
            }
            
            // Check if we've reached the end of constraints
            if (!stream.check(TokenType::PLUS)) {
                break;
            }
        }
        
        // Build the constraints span
        auto builder = ctx.arena.makeBuilder<TraitRefPtr>();
        for (auto* tr : constraints) {
            builder.push_back(tr);
        }
        param->constraints = builder.build();
    }
    
    LOG_PARSER_DETAIL("parseGenericParamDecl: parsed parameter '", 
                       ctx.toString(name), "' with ", param->constraints.size(), " constraints");
    
    return param;
}

// =============================================================================
// parseGenericArgs - Parse a list of generic arguments
// =============================================================================

/**
 * @brief Parse a list of generic arguments.
 * 
 * Grammar: `'<' type { ',' type } '>'`
 * 
 * ## Examples
 * 
 * ```lucid
 * Box<int>                          → <int>
 * Buffer<Vec2>                      → <Vec2>
 * Map<string, int>                  → <string, int>
 * ```
 * 
 * ## Error Handling
 * 
 * - Empty list `<>` → reports E1009
 * - Trailing comma `<int, >` → reports E1103
 * - Missing `>` at EOF → reports E1005
 * - Invalid argument → parseType handles the error
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<TypePtr> The parsed generic arguments (empty on error)
 */
ArenaSpan<TypePtr> parseGenericArgs(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericArgs: parsing generic arguments");
    
    std::vector<TypePtr> args;
    
    // Expect '<' to start the list
    if (!stream.check(TokenType::LESS)) {
        ctx.error(stream, DiagCode::E1004, "<", "generic argument list", stream.peekValue());
        return ctx.arena.makeBuilder<TypePtr>().build();
    }
    stream.advance(); // Consume '<'
    
    // Check for empty generic argument list: <>
    if (stream.check(TokenType::GREATER)) {
        ctx.error(stream, DiagCode::E1009, "generic argument", ">");
        stream.advance(); // Consume '>'
        return ctx.arena.makeBuilder<TypePtr>().build();
    }
    
    // Parse arguments until we hit '>'
    while (!stream.isAtEnd()) {
        // Parse a type as the generic argument
        TypePtr arg = parseType(stream, ctx);
        if (arg) {
            args.push_back(arg);
        } else {
            // Error already reported by parseType, try to recover
            synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::GREATER});
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
            } else if (stream.check(TokenType::GREATER)) {
                stream.advance(); // Consume '>' and exit
                break;
            } else {
                // No recovery token found - break to avoid infinite loop
                break;
            }
            continue;
        }
        
        // Check for comma separator
        if (stream.check(TokenType::COMMA)) {
            stream.advance(); // Consume comma
            
            // Check for trailing comma: <int, string, >
            if (stream.check(TokenType::GREATER)) {
                ctx.error(stream, DiagCode::E1103); // Unexpected trailing comma
                stream.advance(); // Consume '>'
                break;
            }
            
            // Check if we're at EOF after comma
            if (stream.isAtEnd()) {
                ctx.error(stream, DiagCode::E1005, ">", "generic argument list", "<EOF>");
                break;
            }
            
            // Continue to parse next argument
        } else if (stream.check(TokenType::GREATER)) {
            // End of generic argument list
            stream.advance(); // Consume '>'
            break;
        } else {
            // Unexpected token
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or '>'");
            synchronizeTo(stream, ctx, {TokenType::GREATER});
            if (stream.check(TokenType::GREATER)) {
                stream.advance(); // Consume '>' and exit
            }
            break;
        }
    }
    
    // ─── Check if we exited because of EOF ──────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ">", "generic argument list", "<EOF>");
    }
    
    // Build the ArenaSpan
    auto builder = ctx.arena.makeBuilder<TypePtr>();
    for (auto* arg : args) {
        builder.push_back(arg);
    }
    
    LOG_PARSER_DETAIL("parseGenericArgs: parsed ", args.size(), " generic arguments");
    
    return builder.build();
}

// =============================================================================
// parseParamList - Parse a list of function parameters
// =============================================================================

/**
 * @brief Parse a list of function parameters.
 * 
 * Grammar: `'(' [ param { ',' param } [ ',' variadic_param ] ] ')'`
 * 
 * This function consumes the opening '(' and closing ')' parentheses.
 * 
 * ## Parameter Forms
 * 
 * - `IDENTIFIER type`                   → pass by value
 * - `const IDENTIFIER type`             → read-only reference parameter
 * - `IDENTIFIER ... type`               → variadic parameter (must be last)
 * 
 * ## Examples
 * 
 * ```lucid
 * (a int, b int)                       → [a: int, b: int]
 * (name string, age int)               → [name: string, age: int]
 * (nums ...int)                        → [nums: ...int] (variadic)
 * (const v Vector2)                    → [v: const Vector2] (read-only ref)
 * (a int, b int, rest ...string)       → [a: int, b: int, rest: ...string]
 * ()                                   → [] (empty parameter list)
 * ```
 * 
 * ## Error Handling
 * 
 * - Empty parentheses `()` → valid, returns empty list
 * - Missing type after comma → reports E1009
 * - Trailing comma `(a, )` → reports E1103
 * - Missing `)` at EOF → reports E1005
 * - Variadic not last → reports appropriate error
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return std::vector<ParamPtr> The parsed parameters (empty for void)
 *
 * @note this function consume `)`, the caller must not consume `)`
 */
std::vector<ParamPtr> parseParamList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseParamList: parsing parameter list");
    
    std::vector<ParamPtr> params;
    
    // Expect '(' to start the parameter list
    if (!stream.check(TokenType::LPAREN)) {
        ctx.error(stream, DiagCode::E1004, "(", "parameter list", stream.peekValue());
        return params;
    }
    stream.advance(); // Consume '('
    
    // Check for empty parameter list: ()
    if (stream.check(TokenType::RPAREN)) {
        stream.advance(); // Consume ')'
        return params;
    }
    
    // Parse parameters until we hit ')'
    bool hasVariadic = false;
    
    while (!stream.isAtEnd()) {
        SourceLocation loc = stream.currentLoc();
        
        // ─── 1. Check for 'const' modifier ──────────────────────────────
        bool isConst = stream.match(TokenType::CONST);
        
        // ─── 2. Parse parameter name ─────────────────────────────────────
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "parameter name", stream.peekValue());
            synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::RPAREN});
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
            } else if (stream.check(TokenType::RPAREN)) {
                stream.advance(); // Consume ')' and exit
                break;
            }
            continue;
        }
        
        Token nameTok = stream.advance();
        InternedString name = ctx.pool.intern(nameTok.value);
        
        // ─── 3. Check for variadic modifier ─────────────────────────────
        bool isVariadic = stream.match(TokenType::VARIADIC);
        
        if (isVariadic && hasVariadic) {
            ctx.error(stream, DiagCode::E1010, "parameter group", "only one variadic parameter is allowed on each group");
            synchronizeTo(stream, ctx, {TokenType::RPAREN});
            if (stream.check(TokenType::RPAREN)) {
                stream.advance();
            }
            break;
        }
        
        // ─── 4. Parse parameter type ─────────────────────────────────────
        TypePtr type = parseType(stream, ctx);
        if (!type) {
            ctx.error(stream, DiagCode::E1009, "parameter type", stream.peekValue());
            synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::RPAREN});
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
            } else if (stream.check(TokenType::RPAREN)) {
                stream.advance(); // Consume ')' and exit
                break;
            }
            continue;
        }
        
        // ─── 5. Create the parameter node ────────────────────────────────
        auto* param = ctx.arena.make<ParamAST>();
        param->loc = loc;
        param->name = name;
        param->type = type;
        param->isConst = isConst;
        param->isVariadic = isVariadic;
        params.push_back(param);
        
        if (isVariadic) {
            hasVariadic = true;
        }
        
        // ─── 6. Check for comma separator ───────────────────────────────
        if (stream.check(TokenType::COMMA)) {
            if (hasVariadic) {
                ctx.error(stream, DiagCode::E1010, "parameter group", "variadic parameter must be the last parameter");
                synchronizeTo(stream, ctx, {TokenType::RPAREN});
                if (stream.check(TokenType::RPAREN)) {
                    stream.advance();
                }
                break;
            }
            stream.advance(); // Consume comma
            
            // Check for trailing comma: (a int, b int, )
            if (stream.check(TokenType::RPAREN)) {
                ctx.error(stream, DiagCode::E1103); // Unexpected trailing comma
                stream.advance(); // Consume ')'
                break;
            }
            
            // Check if we're at EOF after comma
            if (stream.isAtEnd()) {
                ctx.error(stream, DiagCode::E1005, ")", "parameter list", "<EOF>");
                break;
            }
            
            // Continue to parse next parameter
        } else if (stream.check(TokenType::RPAREN)) {
            // End of parameter list
            stream.advance(); // Consume ')'
            break;
        } else {
            // Unexpected token
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or ')'");
            synchronizeTo(stream, ctx, {TokenType::RPAREN});
            if (stream.check(TokenType::RPAREN)) {
                stream.advance(); // Consume ')' and exit
            }
            break;
        }
    }
    
    // ─── Check if we exited because of EOF ──────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ")", "parameter list", "<EOF>");
    }
    
    LOG_PARSER_DETAIL("parseParamList: parsed ", params.size(), " parameters");
    
    return params;
}

// =============================================================================
// parseArgList - Parse a list of arguments
// =============================================================================

/**
 * @brief Parse a list of arguments for a function call.
 * @note this function consume `)`, the caller must not consume `)`
 * 
 * Grammar: `'(' [ expr { ',' expr } ] ')'`
 * 
 * This function consumes the opening '(' and closing ')' parentheses.
 * 
 * ## Examples
 * 
 * ```lucid
 * add(1, 2, 3)                    → [1, 2, 3]
 * process("hello", 42, true)      → ["hello", 42, true]
 * foo()                           → [] (empty arguments)
 * ```
 * 
 * ## Error Handling
 * 
 * - Empty parentheses `()` → valid, returns empty list
 * - Missing expression after comma → reports E1006
 * - Trailing comma `(1, )` → reports E1103
 * - Missing `)` at EOF → reports E1005
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<ExprAST*> The parsed arguments (empty for no arguments)
 */
ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseArgList: parsing argument list");
    
    std::vector<ExprPtr> args;
    
    // Expect '(' to start the argument list
    if (!stream.check(TokenType::LPAREN)) {
        ctx.error(stream, DiagCode::E1004, "(", "argument list", stream.peekValue());
        return ctx.arena.makeBuilder<ExprPtr>().build();
    }
    stream.advance(); // Consume '('
    
    // Check for empty argument list: ()
    if (stream.check(TokenType::RPAREN)) {
        stream.advance(); // Consume ')'
        return ctx.arena.makeBuilder<ExprPtr>().build();
    }
    
    // Parse arguments until we hit ')'
    while (!stream.isAtEnd()) {
        ExprPtr arg = parseExpr(stream, ctx);
        if (arg) {
            args.push_back(arg);
        } else {
            // Error already reported by parseExpr, try to recover
            ctx.error(stream, DiagCode::E1006, stream.peekValue());
            synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::RPAREN});
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
            } else if (stream.check(TokenType::RPAREN)) {
                stream.advance(); // Consume ')' and exit
                break;
            } else {
                break;
            }
            continue;
        }
        
        // Check for comma separator
        if (stream.check(TokenType::COMMA)) {
            stream.advance(); // Consume comma
            
            // Check for trailing comma: (1, 2, )
            if (stream.check(TokenType::RPAREN)) {
                ctx.error(stream, DiagCode::E1103); // Unexpected trailing comma
                stream.advance(); // Consume ')'
                break;
            }
            
            // Check if we're at EOF after comma
            if (stream.isAtEnd()) {
                ctx.error(stream, DiagCode::E1005, ")", "argument list", "<EOF>");
                break;
            }
            
            // Continue to parse next argument
        } else if (stream.check(TokenType::RPAREN)) {
            // End of argument list
            stream.advance(); // Consume ')'
            break;
        } else {
            // Unexpected token
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or ')'");
            synchronizeTo(stream, ctx, {TokenType::RPAREN});
            if (stream.check(TokenType::RPAREN)) {
                stream.advance(); // Consume ')' and exit
            }
            break;
        }
    }
    
    // ─── Check if we exited because of EOF ──────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ")", "argument list", "<EOF>");
    }
    
    // Build the ArenaSpan
    auto builder = ctx.arena.makeBuilder<ExprPtr>();
    for (auto* arg : args) {
        builder.push_back(arg);
    }
    
    LOG_PARSER_DETAIL("parseArgList: parsed ", args.size(), " arguments");
    
    return builder.build();
}

// =============================================================================
// parseReturnList - Parse a list of return types
// =============================================================================

/**
 * @brief Parse a list of return types.
 * 
 * Grammar: `type | '(' type { ',' type } ')'`
 * 
 * ## Examples
 * 
 * ```lucid
 * -> int                    → [int]          (single type, no parentheses)
 * -> (int, bool)            → [int, bool]    (multiple types, parentheses)
 * -> (string, int, float)   → [string, int, float]
 * -> ()                     → []             (void, empty parentheses)
 * ```
 * 
 * ## Error Handling
 * 
 * - Empty parentheses `()` → void (empty return list)
 * - Missing type after comma → reports E1009
 * - Trailing comma `(int, )` → reports E1103
 * - Missing `)` at EOF → reports E1005
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<TypeAST*> The parsed return types (empty for void)
 */
ArenaSpan<TypeAST*> parseReturnList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseReturnList: parsing return types");
    
    std::vector<TypePtr> returnTypes;
    
    // ─── Check if we have a parenthesized return list ────────────────────
    if (stream.check(TokenType::LPAREN)) {
        stream.advance(); // Consume '('
        
        // Check for empty parentheses (void): ()
        if (stream.check(TokenType::RPAREN)) {
            stream.advance(); // Consume ')'
            return ctx.arena.makeBuilder<TypePtr>().build();
        }
        
        // Parse types until we hit ')'
        while (!stream.isAtEnd()) {
            TypePtr type = parseType(stream, ctx);
            if (type) {
                returnTypes.push_back(type);
            } else {
                // Error already reported by parseType, try to recover
                ctx.error(stream, DiagCode::E1009, "type in return list", stream.peekValue());
                synchronizeTo(stream, ctx, {TokenType::COMMA, TokenType::RPAREN});
                if (stream.check(TokenType::COMMA)) {
                    stream.advance();
                } else if (stream.check(TokenType::RPAREN)) {
                    stream.advance(); // Consume ')' and exit
                    break;
                } else {
                    break;
                }
                continue;
            }
            
            // Check for comma separator
            if (stream.check(TokenType::COMMA)) {
                stream.advance(); // Consume comma
                
                // Check for trailing comma: (int, string, )
                if (stream.check(TokenType::RPAREN)) {
                    ctx.error(stream, DiagCode::E1103); // Unexpected trailing comma
                    stream.advance(); // Consume ')'
                    break;
                }
                
                // Check if we're at EOF after comma
                if (stream.isAtEnd()) {
                    ctx.error(stream, DiagCode::E1005, ")", "return list", "<EOF>");
                    break;
                }
                
                // Continue to parse next type
            } else if (stream.check(TokenType::RPAREN)) {
                // End of return list
                stream.advance(); // Consume ')'
                break;
            } else {
                // Unexpected token
                ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or ')'");
                synchronizeTo(stream, ctx, {TokenType::RPAREN});
                if (stream.check(TokenType::RPAREN)) {
                    stream.advance(); // Consume ')' and exit
                }
                break;
            }
        }
        
        // ─── Check if we exited because of EOF ──────────────────────────
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, ")", "return list", "<EOF>");
        }
        
    } else {
        // ─── Single return type (no parentheses) ──────────────────────────
        TypePtr type = parseType(stream, ctx);
        if (type) {
            returnTypes.push_back(type);
        } else {
            ctx.error(stream, DiagCode::E1009, "return type", stream.peekValue());
            synchronize(stream, ctx);
        }
    }
    
    // Build the ArenaSpan
    auto builder = ctx.arena.makeBuilder<TypePtr>();
    for (auto* type : returnTypes) {
        builder.push_back(type);
    }
    
    LOG_PARSER_DETAIL("parseReturnList: parsed ", returnTypes.size(), " return types");
    
    return builder.build();
}

// =============================================================================
// parseUsePath - Parse a use path
// =============================================================================

/**
 * @brief Parse a use path.
 * 
 * Grammar: `IDENTIFIER { '.' IDENTIFIER }`
 * 
 * ## Examples
 * 
 * ```lucid
 * use std.io                    → ["std", "io"]
 * use std.math as math          → ["std", "math"]
 * use graphics.gl               → ["graphics", "gl"]
 * use mylib                     → ["mylib"]
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return std::vector<InternedString> The path components
 */
std::vector<InternedString> parseUsePath(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseUsePath: parsing use path");
    
    std::vector<InternedString> pathParts;
    
    // Expect at least one identifier
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, stream.peekValue());
        return pathParts;
    }
    
    // Parse the first identifier
    Token tok = stream.advance();
    pathParts.push_back(ctx.pool.intern(tok.value));
    
    // Parse additional identifiers separated by '.'
    while (stream.match(TokenType::DOT)) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream.currentLoc(), "Expected identifier after '.' in use path");
            synchronize(stream, ctx);
            break;
        }
        
        tok = stream.advance();
        pathParts.push_back(ctx.pool.intern(tok.value));
    }
    
    // Build the full path string for logging
    std::string fullPath;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += std::string(ctx.pool.lookup(pathParts[i]));
    }
    
    LOG_PARSER_DETAIL("parseUsePath: parsed path '", fullPath, "' with ", 
                      pathParts.size(), " components");
    
    return pathParts;
}

// =============================================================================
// parseTraitRef - Parse a trait reference
// =============================================================================

/**
 * @brief Parse a trait reference.
 * 
 * Grammar: `IDENTIFIER [ '<' type { ',' type } '>' ]`
 * 
 * ## Examples
 * 
 * ```lucid
 * : Vector2          → name="Vector2",    genericArgs={}
 * : Named            → name="Named",      genericArgs={}
 * : Container<int>   → name="Container",  genericArgs=[Int]
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return TraitRefPtr The parsed trait reference, or nullptr on error
 */
TraitRefPtr parseTraitRef(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Expect an identifier for the trait name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(loc, "Expected trait name");
        synchronize(stream, ctx);
        return nullptr;
    }
    
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // Create the trait reference node
    auto* traitRef = ctx.arena.make<TraitRefAST>();
    traitRef->loc = loc;
    traitRef->name = name;
    
    // Check for generic arguments (optional)
    if (stream.check(TokenType::LESS)) {
        traitRef->genericArgs = parseGenericArgs(stream, ctx);
    }
    
    LOG_PARSER_DETAIL("parseTraitRef: parsed trait '", ctx.toString(name), 
                       "' with ", traitRef->genericArgs.size(), " generic args");
    
    return traitRef;
}

// =============================================================================
// parseLvalue - Parse an lvalue (left-hand side of assignment)
// =============================================================================

/**
 * @brief Parse an lvalue (left-hand side of an assignment).
 * 
 * An lvalue is an expression that can appear on the left side of an assignment.
 * In Lucid, lvalues are:
 * - Identifiers (variables)
 * - Field accesses (struct.field)
 * - Module accesses (module:member) - but these are NOT assignable (read-only)
 * - Index expressions (array[index])
 * 
 * ## Grammar
 * 
 * ```ebnf
 * lvalue = IDENTIFIER
 *        | lvalue '.' IDENTIFIER
 *        | lvalue '[' expr ']'
 * ```
 * 
 * ## Examples
 * 
 * ```lucid
 * x = 5                    → Identifier "x"
 * player.health = 100      → FieldAccess "player" "health"
 * items[0] = 42            → Index "items" [0]
 * math:PI = 3.0            → ModuleAccess "math" "PI" (NOT assignable - error)
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ExprPtr The lvalue expression, or nullptr on error
 */
ExprPtr parseLvalue(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Parse the base expression ────────────────────────────────────
    ExprPtr expr = nullptr;
    
    // Check if it's a module access (module:member)
    if (stream.check(TokenType::IDENTIFIER)) {
        Token moduleTok = stream.peek();
        // Look ahead to see if it's followed by ':'
        if (stream.peekNextType() == TokenType::COLON) {
            // It's a module access - parse it
            stream.advance(); // Consume module name
            InternedString moduleName = ctx.pool.intern(moduleTok.value);
            
            stream.consume(TokenType::COLON);
            
            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.error(stream.currentLoc(), "Expected member name after ':'");
                return nullptr;
            }
            
            Token memberTok = stream.advance();
            InternedString memberName = ctx.pool.intern(memberTok.value);
            
            auto* moduleAccess = ctx.arena.make<ModuleAccessExprAST>();
            moduleAccess->loc = loc;
            moduleAccess->moduleName = moduleName;
            moduleAccess->memberName = memberName;
            moduleAccess->isModuleMember = true;  // Mark as module member (read-only)
            
            expr = moduleAccess;
            
            // Module accesses are NOT assignable - this will be caught by the caller
            // when it tries to assign to this expression.
            
            LOG_PARSER_DETAIL("parseLvalue: parsed module access '", 
                              ctx.toString(moduleName), ":", ctx.toString(memberName), "'");
        } else {
            // It's a simple identifier (variable)
            stream.advance(); // Consume identifier
            InternedString varName = ctx.pool.intern(moduleTok.value);
            
            auto* identifier = ctx.arena.make<IdentifierExprAST>();
            identifier->loc = loc;
            identifier->name = varName;
            
            expr = identifier;
            
            LOG_PARSER_DETAIL("parseLvalue: parsed identifier '", ctx.toString(varName), "'");
        }
    } else {
        ctx.error(loc, "Expected lvalue (identifier, field access, or index)");
        synchronize(stream, ctx);
        return nullptr;
    }
    
    // ─── 2. Parse any trailing field accesses or index expressions ──────
    while (!stream.isAtEnd()) {
        if (stream.check(TokenType::DOT)) {
            // Field access: expr.field
            stream.advance(); // Consume '.'
            
            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.error(stream.currentLoc(), "Expected field name after '.'");
                break;
            }
            
            Token fieldTok = stream.advance();
            InternedString fieldName = ctx.pool.intern(fieldTok.value);
            
            auto* fieldAccess = ctx.arena.make<FieldAccessExprAST>();
            fieldAccess->loc = loc;
            fieldAccess->object = expr;
            fieldAccess->fieldName = fieldName;
            
            expr = fieldAccess;
            
            LOG_PARSER_DETAIL("parseLvalue: parsed field access '.", 
                              ctx.toString(fieldName), "'");
            
        } else if (stream.check(TokenType::LBRACKET)) {
            // Index expression: expr[index]
            stream.advance(); // Consume '['
            
            ExprPtr index = parseExpr(stream, ctx);
            if (!index) {
                ctx.error(stream.currentLoc(), "Expected index expression");
                synchronizeTo(stream, ctx, {TokenType::RBRACKET});
                stream.advance(); // Consume ']' to recover
                break;
            }
            
            if (!stream.check(TokenType::RBRACKET)) {
                ctx.error(stream.currentLoc(), "Expected ']' to close index");
                synchronizeTo(stream, ctx, {TokenType::RBRACKET});
            } else {
                stream.advance(); // Consume ']'
            }
            
            auto* indexExpr = ctx.arena.make<IndexExprAST>();
            indexExpr->loc = loc;
            indexExpr->target = expr;
            indexExpr->index = index;
            
            expr = indexExpr;
            
            LOG_PARSER_DETAIL("parseLvalue: parsed index expression");
            
        } else {
            // No more lvalue extensions
            break;
        }
    }
    
    return expr;
}

// =============================================================================
// parseFuncRef - Parse a function reference
// =============================================================================

/**
 * @brief Parse a function reference.
 * 
 * A function reference is an identifier that refers to a function, optionally
 * with generic arguments.
 * 
 * ## Grammar
 * 
 * ```ebnf
 * func_ref = IDENTIFIER [ '<' type { ',' type } '>' ]
 *          | module_expr
 * ```
 * 
 * ## Examples
 * 
 * ```lucid
 * add                    → Function "add"
 * math:sqrt              → ModuleAccess "math" "sqrt"
 * map<int, string>       → Generic function "map" with args [int, string]
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ExprPtr The function reference expression, or nullptr on error
 */
ExprPtr parseFuncRef(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Check for module access ─────────────────────────────────────
    if (stream.check(TokenType::IDENTIFIER)) {
        Token moduleTok = stream.peek();
        // Look ahead to see if it's followed by ':'
        if (stream.peekNextType() == TokenType::COLON) {
            // It's a module function access: module:func
            stream.advance(); // Consume module name
            InternedString moduleName = ctx.pool.intern(moduleTok.value);
            
            stream.consume(TokenType::COLON);
            
            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.error(stream.currentLoc(), "Expected function name after ':'");
                return nullptr;
            }
            
            Token funcTok = stream.advance();
            InternedString funcName = ctx.pool.intern(funcTok.value);
            
            // Check for generic arguments on the function
            ArenaSpan<TypePtr> genericArgs;
            if (stream.check(TokenType::LESS)) {
                genericArgs = parseGenericArgs(stream, ctx);
            }
            
            // Create module access expression
            auto* moduleAccess = ctx.arena.make<ModuleAccessExprAST>();
            moduleAccess->loc = loc;
            moduleAccess->moduleName = moduleName;
            moduleAccess->memberName = funcName;
            moduleAccess->genericArgs = genericArgs;
            moduleAccess->isModuleMember = true;
            
            LOG_PARSER_DETAIL("parseFuncRef: parsed module function '", 
                              ctx.toString(moduleName), ":", ctx.toString(funcName), 
                              "' with ", genericArgs.size(), " generic args");
            
            return moduleAccess;
        }
    }
    
    // ─── 2. Parse as a regular function reference ───────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(loc, "Expected function name");
        synchronize(stream, ctx);
        return nullptr;
    }
    
    Token funcTok = stream.advance();
    InternedString funcName = ctx.pool.intern(funcTok.value);
    
    // Check for generic arguments
    ArenaSpan<TypePtr> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }
    
    // Create identifier expression
    auto* identifier = ctx.arena.make<IdentifierExprAST>();
    identifier->loc = loc;
    identifier->name = funcName;
    identifier->genericArgs = genericArgs;
    
    LOG_PARSER_DETAIL("parseFuncRef: parsed function '", ctx.toString(funcName), 
                      "' with ", genericArgs.size(), " generic args");
    
    return identifier;
}

} // namespace parser
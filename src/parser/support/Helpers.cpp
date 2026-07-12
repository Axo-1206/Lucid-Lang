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

#include "../Parser.hpp"
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
 * This function parses an attribute list annotation that precedes a declaration.
 * It consumes the full `@[...]` construct and produces zero or more AttributeAST nodes.
 * 
 * ## Program Flow
 * 
 * ```
 * parseAttributes()
 *     │
 *     ├── 1. Quick Check Phase
 *     │   └── if (!stream.check(TokenType::AT_SIGN))
 *     │       └── return empty ArenaSpan<AttributePtr> immediately
 *     │
 *     ├── 2. Opening Delimiter Phase
 *     │   ├── stream.advance()  // Consume '@'
 *     │   │
 *     │   └── if (!stream.check(TokenType::LBRACKET))
 *     │       ├── ctx.error(stream, DiagCode::E1004, "[", "attribute list", got)
 *     │       ├── synchronizeToContext(stream, ctx)  // recover using enclosing context
 *     │       └── return empty ArenaSpan<AttributePtr>
 *     │   └── else
 *     │       └── stream.advance()  // Consume '['
 *     │
 *     ├── 3. Scoped Context Push
 *     │   └── ScopedContext attrGuard(ctx, SyntacticContext::Attribute, loc)
 *     │       └── Pushes Attribute context once; auto-pops on every return path
 *     │
 *     ├── 4. Leading Comma Recovery
 *     │   └── if (stream.consumeTrailing(TokenType::COMMA) > 0)
 *     │       └── ctx.error(stream, DiagCode::E1009, ",", "attribute list")
 *     │           // @[,,,, FirstAttribute ...] — reports once for all leading commas
 *     │
 *     ├── 5. Main Attribute Parsing Loop
 *     │   │
 *     │   └── while (!stream.isAtEnd() || stream.check(TokenType::RBRACKET))
 *     │       │
 *     │       ├── 5.1 Parse Single Attribute
 *     │       │   └── parseAttribute(stream, ctx) → AttributePtr
 *     │       │       │
 *     │       │       ├── if (attr != nullptr)
 *     │       │       │   └── attrs.push_back(attr)
 *     │       │       │
 *     │       │       └── else
 *     │       │           ├── Error already reported by parseAttribute
 *     │       │           ├── synchronizeToContext(stream, ctx)
 *     │       │           │   // Stops at ',' or ']' — bounded by Attribute context
 *     │       │           ├── if (stream.check(TokenType::RBRACKET))
 *     │       │           │   └── break
 *     │       │           └── else
 *     │       │               └── break
 *     │       │
 *     │       └── 5.2 Comma Separator Handling
 *     │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │               └── ctx.error(stream, DiagCode::E1009, ",", "attribute list")
 *     │                   // Reports once for multiple consecutive commas between attrs
 *     │
 *     ├── 6. Closing Bracket Phase
 *     │   │
 *     │   └── if (!stream.check(TokenType::RBRACKET))
 *     │       ├── if (stream.isAtEnd())
 *     │       │   └── ctx.error(stream, DiagCode::E1005, "]", "attribute list", "<EOF>")
 *     │       ├── else
 *     │       │   └── ctx.error(stream, DiagCode::E1005, "]", "attribute list", got)
 *     │       ├── synchronizeTo(stream, ctx, TokenType::RBRACKET)
 *     │       └── if (stream.check(TokenType::RBRACKET))
 *     │           └── stream.advance()  // consume ']' to recover
 *     │   └── else
 *     │       └── stream.advance()  // consume ']'
 *     │
 *     ├── 7. Build Result
 *     │   └── ctx.arena.makeBuilder<AttributePtr>()
 *     │       └── push_back each attr → build() → ArenaSpan<AttributePtr>
 *     │
 *     └── 8. Return
 *         └── return ArenaSpan<AttributePtr> (possibly empty)
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * ### Level 1: Missing '@[' or '@' not followed by '['
 * - Missing '@' → silent return (caller assumes no attributes)
 * - '@' without '[' → E1004, synchronizeToContext(), return empty
 * 
 * ### Level 2: Malformed Attribute Within List
 * - parseAttribute() reports its own error and returns nullptr
 * - synchronizeToContext() skips to next ',' or ']' (bounded by Attribute guard)
 * - Trailing commas after error → E1009 reported
 * 
 * ### Level 3: Missing Closing Bracket
 * - E1005 reported with actual token or "<EOF>"
 * - synchronizeTo() seeks ']' and consumes it if found
 * 
 * ### Level 4: Consecutive Commas
 * - Leading commas before first attribute → E1009 once
 * - Multiple commas between attributes → E1009 once per gap
 * - Commas after failed parse → E1009 if >1, silent if exactly 1
 * 
 * ## Context Stack Interaction
 * 
 * The ScopedContext guard ensures:
 * - synchronizeToContext() stops at ',' or ']' (Attribute follow-set)
 * - Does NOT scan past ']' into the enclosing declaration
 * - Auto-pops on every return path (including early returns)
 * 
 * ## Token Stream State
 * 
 * After this function completes:
 * - Position is AFTER ']' (normal case)
 * - Position is at recovery point (if errors occurred)
 * - Position is at EOF (if ']' never found)
 * 
 * ## Examples
 * 
 * ```lucid
 * @[export]                              → [export]
 * @[foreign("C")]                         → [foreign("C")]
 * @[deprecated("msg"), inline]          → [deprecated("msg"), inline]
 * @[,,export]                            → [export] + E1009 (leading commas)
 * @[export,,,inline]                     → [export, inline] + E1009 (gap)
 * @[export,                             → [export] + E1005 (missing ']')
 * @export                                → [] + E1004 (missing '[')
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<AttributePtr> The parsed attributes (empty if none or error)
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
        // '[' never opened, so no Attribute context to push — recover using
        // whatever context we were already in (e.g. the enclosing declaration).
        synchronizeToContext(stream, ctx);
        return ctx.arena.makeBuilder<AttributePtr>().build();
    }
    stream.advance(); // Consume '['

    // From here on the parser is inside an attribute list — push once, and
    // every return path below (including the early ones further down) pops
    // automatically when this function returns.
    ScopedContext attrGuard(ctx, SyntacticContext::Attribute, stream.currentLoc());

    // Skip initial consecutive comma ','
    // Ex: @[,,,, FirstAttribute ...]
    if (stream.consumeTrailing(TokenType::COMMA) > 0) {
        ctx.error(stream, DiagCode::E1009, ",", "attribute list"); // Unexpected trailing comma
    }
    
    // Parse attributes until we hit ']'
    while (!stream.isAtEnd() || stream.check(TokenType::RBRACKET)) {
        // Parse a single attribute
        AttributePtr attr = parseAttribute(stream, ctx);
        if (attr) {
            attrs.push_back(attr);
        } else {
            // Error already reported, try to recover. currentContext() is
            // Attribute here (attrGuard is still on the stack), so this
            // stops at ',' or ']' — it will not run past the closing ']'
            // into whatever declaration follows.
            synchronizeToContext(stream, ctx);
            if (stream.check(TokenType::RBRACKET)) {
                break;
            } else {
                break;
            }
        }

        // Consume at least 1 consecutive comma and Skip consecutive commna
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.error(stream, DiagCode::E1009, ",", "attribute list"); // Unexpected trailing comma
        }
    }
    
    // ─── Consume closing bracket ────────────────────────────────────────
    if (!stream.check(TokenType::RBRACKET)) {
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, "]", "attribute list", "<EOF>");
        } else {
            ctx.error(stream, DiagCode::E1005, "]", "attribute list", stream.peekValue());
        }
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (stream.check(TokenType::RBRACKET)) {
            stream.advance(); // Consume ']' to recover
        }
    } else {
        stream.advance(); // Consume ']'
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
 * This function parses one attribute within an attribute list. It expects to be
 * called only from inside parseAttributes()'s ScopedContext guard (SyntacticContext::Attribute).
 * 
 * ## Program Flow
 * 
 * ```
 * parseAttribute()
 *     │
 *     ├── 1. Capture Location
 *     │   └── SourceLocation loc = stream.currentLoc()
 *     │
 *     ├── 2. Parse Attribute Name
 *     │   │
 *     │   └── if (!stream.check(TokenType::IDENTIFIER))
 *     │       ├── ctx.error(stream, DiagCode::E1002, "attribute name", got)
 *     │       ├── synchronizeToContext(stream, ctx)
 *     │       │   // Bounded by Attribute context → stops at ',' or ']'
 *     │       └── return nullptr
 *     │   └── else
 *     │       ├── Token nameTok = stream.advance()
 *     │       └── InternedString name = ctx.pool.intern(nameTok.value)
 *     │
 *     ├── 3. Allocate Attribute Node
 *     │   └── auto* attr = ctx.arena.make<AttributeAST>()
 *     │       ├── attr->loc = loc
 *     │       └── attr->name = name
 *     │
 *     ├── 4. Optional Arguments Phase
 *     │   │
 *     │   └── if (stream.match(TokenType::LPAREN))
 *     │       │   // '(' was present — parse argument list
 *     │       │
 *     │       ├── 4.1 Leading Comma Recovery
 *     │       │   └── if (stream.consumeTrailing(TokenType::COMMA) > 0)
 *     │       │       └── ctx.error(stream, DiagCode::E1009, ",", "attribute arguments")
 *     │       │           // @[foo(,,,, "arg" ...)] — reports once for all leading commas
 *     │       │
 *     │       ├── 4.2 Argument Parsing Loop
 *     │       │   │
 *     │       │   └── while (!stream.isAtEnd() || stream.check(TokenType::RPAREN))
 *     │       │       │
 *     │       │       ├── 4.2.1 Parse Argument Literal
 *     │       │       │   └── parseAttributeArgLiteral(stream, ctx) → AttributeArgPtr
 *     │       │       │       │
 *     │       │       │       ├── if (arg != nullptr)
 *     │       │       │       │   └── args.push_back(arg)
 *     │       │       │       │
 *     │       │       │       └── else
 *     │       │       │           ├── Error already reported by parseAttributeArgLiteral
 *     │       │       │           ├── synchronizeTo(stream, ctx, COMMA, RPAREN)
 *     │       │       │           ├── if (stream.check(TokenType::COMMA))
 *     │       │       │           │   ├── stream.advance()
 *     │       │       │           │   └── continue
 *     │       │       │           ├── else if (stream.check(TokenType::RPAREN))
 *     │       │       │           │   └── break
 *     │       │       │           └── else
 *     │       │       │               └── break
 *     │       │       │
 *     │       │       └── 4.2.2 Comma Separator Handling
 *     │       │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │       │               └── ctx.error(stream, DiagCode::E1009, ",", "attribute arguments")
 *     │       │                   // Reports once for multiple consecutive commas between args
 *     │       │
 *     │       └── 4.3 Closing Parenthesis
 *     │           │
 *     │           └── if (!stream.check(TokenType::RPAREN))
 *     │               ├── if (stream.isAtEnd())
 *     │               │   └── ctx.error(stream, DiagCode::E1005, ")", "attribute arguments", "<EOF>")
 *     │               ├── else
 *     │               │   └── ctx.error(stream, DiagCode::E1005, ")", "attribute arguments", got)
 *     │               ├── synchronizeTo(stream, ctx, TokenType::RPAREN)
 *     │               └── if (stream.check(TokenType::RPAREN))
 *     │                   └── stream.advance()  // consume ')' to recover
 *     │           └── else
 *     │               └── stream.advance()  // consume ')'
 *     │
 *     ├── 5. Build Arguments Span
 *     │   └── ctx.arena.makeBuilder<AttributeArgPtr>()
 *     │       ├── push_back each arg
 *     │       └── build() → attr->args
 *     │
 *     ├── 6. Logging
 *     │   └── LOG_PARSER("parsed attribute 'name' with N args")
 *     │
 *     └── 7. Return
 *         └── return attr  (or nullptr if error occurred at step 2)
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * ### Level 1: Missing Attribute Name
 * - E1002 reported
 * - synchronizeToContext() skips to ',' or ']' (bounded by Attribute guard)
 * - Returns nullptr; parseAttributes handles the gap
 * 
 * ### Level 2: Malformed Argument
 * - parseAttributeArgLiteral() reports E1104 for invalid literal type
 * - synchronizeTo(COMMA, RPAREN) seeks next separator or end
 * - Single comma → skip and continue
 * - ')' → break out of loop
 * 
 * ### Level 3: Missing Closing Parenthesis
 * - E1005 reported with actual token or "<EOF>"
 * - synchronizeTo(RPAREN) seeks ')' and consumes if found
 * 
 * ### Level 4: Consecutive Commas in Arguments
 * - Leading commas before first arg → E1009 once
 * - Multiple commas between args → E1009 once per gap
 * - Handles `@[foo(,,,)]` and `@[foo(a,,,b)]`
 * 
 * ## Context Stack Interaction
 * 
 * This function relies on parseAttributes() having pushed SyntacticContext::Attribute.
 * synchronizeToContext() uses this to bound recovery:
 * - Follow-set: {COMMA, RBRACKET}
 * - Does NOT scan past ']' into declaration
 * 
 * ## Token Stream State
 * 
 * After this function completes:
 * - Position is AFTER attribute name (no args, normal case)
 * - Position is AFTER ')' (with args, normal case)
 * - Position is at recovery point (if error in name)
 * - Position is at ',' or ']' (if argument error and recovered)
 * 
 * ## Examples
 * 
 * ```lucid
 * export                               → name="export", args={}
 * foreign("C")                         → name="foreign", args=["C"]
 * deprecated("use new instead")        → name="deprecated", args=["use new instead"]
 * link("opengl", "m")                  → name="link", args=["opengl", "m"]
 * 123                                  → nullptr + E1002 (not identifier)
 * foo(,)                               → name="foo", args={} + E1009
 * foo("a",,,"b")                       → name="foo", args=["a","b"] + E1009
 * foo("a"                              → name="foo", args=["a"] + E1005
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return AttributePtr The parsed attribute node, or nullptr on error
 * 
 * @note This function is only ever called from inside parseAttributes()'s
 *       Attribute ScopedContext guard. The synchronizeToContext() call
 *       stays bounded to ',' / ']' rather than scanning for unrelated
 *       global tokens.
 * 
 * @note The closing ')' check is guarded by stream.match(LPAREN). If no
 *       '(' follows the attribute name, no arguments are parsed and no
 *       ')' is expected — the function returns immediately after step 3.
 */
AttributePtr parseAttribute(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Expect an identifier for the attribute name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "attribute name", stream.peekValue());
        // parseAttribute is only ever called from inside parseAttributes'
        // Attribute guard, so this stays bounded to ',' / ']' rather than
        // scanning for an unrelated global token set.
        synchronizeToContext(stream, ctx);
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
        // ─── Skip initial consecutive comma ',' before first arg
        // Ex: @[foo(,,,, "arg" ...)]
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, ",", "attribute arguments");
        }

        // Parse arguments until we hit ')'
        while (!stream.isAtEnd() || stream.check(TokenType::RPAREN)) {
            
            // Parse an argument
            AttributeArgPtr arg = parseAttributeArgLiteral(stream, ctx);
            if (arg) {
                args.push_back(arg);
            } else {
                // Error already reported, try to recover
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                if (stream.check(TokenType::COMMA)) {
                    stream.advance();
                    continue;
                } else if (stream.check(TokenType::RPAREN)) {
                    break;
                } else {
                    break;
                }
            }

            // Consume at least 1 comma and skip consecutive commas
            if (stream.consumeTrailing(TokenType::COMMA) > 1) {
                ctx.error(stream, DiagCode::E1009, ",", "attribute arguments");
            }
        }
        
        // ─── Consume closing parenthesis ────────────────────────────────────
        if (!stream.check(TokenType::RPAREN)) {
            if (stream.isAtEnd()) {
                ctx.error(stream, DiagCode::E1005, ")", "attribute arguments", "<EOF>");
            } else {
                ctx.error(stream, DiagCode::E1005, ")", "attribute arguments", stream.peekValue());
            }
            synchronizeTo(stream, ctx, TokenType::RPAREN);
            if (stream.check(TokenType::RPAREN)) {
                stream.advance(); // Consume ')' to recover
            }
        } else {
            stream.advance(); // Consume ')'
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
            synchronizeToContext(stream, ctx);
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
 * - Empty list `<>` → reports E1003
 * - Trailing comma `<T, >` → reports E1009
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
 *
 * @note this function consume '>' the caller must not consume '>'
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
        ctx.error(stream, DiagCode::E1003, "generic parameter", ">");
        stream.advance(); // Consume '>'
        return ctx.arena.makeBuilder<GenericParamDeclPtr>().build();
    }
    
    // Parse parameters until we hit '>'
    while (!stream.isAtEnd()) {
        // ─── Skip consecutive separators ────────────────────────────────
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, ",", "generic parameters"); // Unexpected trailing comma
        }
        
        // ─── Check if we've reached a terminator ──────────────────────
        if (stream.check(TokenType::GREATER)) {
            break; // End of generic parameter list
        }
        
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, ">", "generic parameter list", "<EOF>");
            break;
        }
        
        // ─── Parse a single generic parameter ───────────────────────────
        GenericParamDeclPtr param = parseGenericParamDecl(stream, ctx);
        if (param) {
            params.push_back(param);
        } else {
            // Error already reported, try to recover
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::GREATER)) {
                break;
            } else {
                break;
            }
        }
    }
    
    // ─── Consume closing bracket ────────────────────────────────────────
    if (!stream.check(TokenType::GREATER)) {
        ctx.error(stream, DiagCode::E1005, ">", "generic parameter list", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::GREATER);
        if (stream.check(TokenType::GREATER)) {
            stream.advance(); // Consume '>' to recover
        }
    } else {
        stream.advance(); // Consume '>'
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
 * - Missing trait after '+' → reports E1003
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
        synchronizeToContext(stream, ctx);
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
                stream.advance(); // Consume '+'
                
                // If we're at EOF, '>', or ',', it's a trailing plus
                if (stream.isAtEnd()) {
                    ctx.error(stream, DiagCode::E1009, '+', "generic constraints");
                    break;
                }
                
                if (stream.checkAny(TokenType::GREATER, TokenType::COMMA)) {
                    ctx.error(stream, DiagCode::E1009, '+', "generic constraints");
                    // IMPORTANT: We DO NOT consume '>' or ',' here.
                    // The caller (parseGenericParamDecls) will handle the closing '>'
                    // or the next parameter after ','.
                    break;
                }
                
                // Continue to parse next trait
                continue;
            }
            
            // Parse a trait reference
            TraitRefPtr traitRef = parseTraitRef(stream, ctx);
            if (traitRef) {
                constraints.push_back(traitRef);
                hasConstraint = true;
            } else {
                ctx.error(stream, DiagCode::E1003, "trait reference after ':' or '+'", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
                break;
            }
            
            // Check if we've reached the end of constraints
            if (!stream.check(TokenType::PLUS)) {
                break;
            }
        }
        
        // ─── Check if we have constraints but they're empty ──────────────
        if (!hasConstraint) {
            ctx.error(stream, DiagCode::E1003, "trait constraint after ':'", stream.peekValue());
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
 * - Empty list `<>` → reports E1003
 * - Trailing comma `<int, >` → reports E1009
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
        ctx.error(stream, DiagCode::E1003, "generic argument", ">");
        stream.advance(); // Consume '>'
        return ctx.arena.makeBuilder<TypePtr>().build();
    }
    
    // Parse arguments until we hit '>'
    while (!stream.isAtEnd()) {
        // ─── Skip consecutive separators ────────────────────────────────
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, ",", "generic arguments"); // Unexpected trailing comma
        }
        
        // ─── Check if we've reached a terminator ──────────────────────
        if (stream.check(TokenType::GREATER)) {
            break; // End of generic argument list
        }
        
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, ">", "generic argument list", "<EOF>");
            break;
        }
        
        // Parse a type as the generic argument
        TypePtr arg = parseType(stream, ctx);
        if (arg) {
            args.push_back(arg);
        } else {
            // Error already reported by parseType, try to recover
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::GREATER)) {
                break;
            } else {
                break;
            }
        }
    }
    
    // ─── Consume closing bracket ────────────────────────────────────────
    if (!stream.check(TokenType::GREATER)) {
        ctx.error(stream, DiagCode::E1005, ">", "generic argument list", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::GREATER);
        if (stream.check(TokenType::GREATER)) {
            stream.advance(); // Consume '>' to recover
        }
    } else {
        stream.advance(); // Consume '>'
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
 * - Missing type after comma → reports E1003
 * - Trailing comma `(a, )` → reports E1009
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
        // ─── Skip consecutive separators ────────────────────────────────
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, ",", "parameter list"); // Unexpected trailing comma
        }
        
        // ─── Check if we've reached a terminator ──────────────────────
        if (stream.check(TokenType::RPAREN)) {
            break; // End of parameter list
        }
        
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, ")", "parameter list", "<EOF>");
            break;
        }
        
        SourceLocation loc = stream.currentLoc();
        
        // ─── 1. Check for 'const' modifier ──────────────────────────────
        bool isConst = stream.match(TokenType::CONST);
        
        // ─── 2. Parse parameter name ─────────────────────────────────────
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "parameter name", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::RPAREN)) {
                break;
            } else {
                break;
            }
        }
        
        Token nameTok = stream.advance();
        InternedString name = ctx.pool.intern(nameTok.value);
        
        // ─── 3. Check for variadic modifier ─────────────────────────────
        bool isVariadic = stream.match(TokenType::VARIADIC);
        
        if (isVariadic && hasVariadic) {
            ctx.error(stream, DiagCode::E1010, "parameter group", "only one variadic parameter is allowed on each group");
            // Don't break here, continue to parse the type so we can recover
        }
        
        // ─── 4. Parse parameter type ─────────────────────────────────────
        TypePtr type = parseType(stream, ctx);
        if (!type) {
            ctx.error(stream, DiagCode::E1003, "parameter type", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::RPAREN)) {
                break;
            } else {
                break;
            }
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
        
        // ─── Check if variadic is last ──────────────────────────────────
        // If we have a variadic parameter, check if there's another parameter after it
        if (isVariadic && stream.check(TokenType::COMMA)) {
            ctx.error(stream, DiagCode::E1010, "parameter group", "variadic parameter must be the last parameter");
            // Skip the comma and continue to parse the next parameter
            stream.advance();
            // But we should still continue parsing to recover
            continue;
        }
    }
    
    // ─── Consume closing parenthesis ────────────────────────────────────
    if (!stream.check(TokenType::RPAREN)) {
        ctx.error(stream, DiagCode::E1005, ")", "parameter list", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RPAREN);
        if (stream.check(TokenType::RPAREN)) {
            stream.advance(); // Consume ')' to recover
        }
    } else {
        stream.advance(); // Consume ')'
    }
    
    LOG_PARSER_DETAIL("parseParamList: parsed ", params.size(), " parameters");
    
    return params;
}

// =============================================================================
// parseArgList - Parse a list of arguments
// =============================================================================

/**
 * @brief Parse a list of arguments for a function call.
 * 
 * Grammar: `'(' [ expr { ',' expr } ] ')'`
 * 
 * This function consumes the opening '(' and closing ')' parentheses.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ArenaSpan<ExprAST*> The parsed arguments (empty for no arguments)
 *
 * @note this function consume `)`, the caller must not consume `)`
 */
ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseArgList: parsing argument list");
    
    std::vector<ExprPtr> args;
    
    // ─── 1. Expect '(' ────────────────────────────────────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        ctx.error(stream, DiagCode::E1004, "(", "argument list", stream.peekValue());
        return ctx.arena.makeBuilder<ExprPtr>().build();
    }
    stream.advance(); // Consume '('
    
    // ─── 2. Check for empty argument list: () ────────────────────────────
    if (stream.check(TokenType::RPAREN)) {
        stream.advance(); // Consume ')'
        return ctx.arena.makeBuilder<ExprPtr>().build();
    }
    
    // ─── 3. Parse arguments until we hit ')' ─────────────────────────────
    while (!stream.isAtEnd()) {
        // Skip consecutive separators
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, ",", "argument list"); // Unexpected trailing comma
        }
        
        // Check if we've reached the end
        if (stream.check(TokenType::RPAREN)) {
            break;
        }
        
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, ")", "argument list", "<EOF>");
            break;
        }
        
        // Parse an argument
        ExprPtr arg = parseExpr(stream, ctx);
        if (arg) {
            args.push_back(arg);
        } else {
            // Error already reported by parseExpr, try to recover
            ctx.error(stream, DiagCode::E1006, stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::RPAREN)) {
                stream.advance();
                break;
            } else {
                break;
            }
        }
        
        // Check for comma separator
        if (stream.check(TokenType::COMMA)) {
            stream.advance();
            // Check for trailing comma
            if (stream.check(TokenType::RPAREN)) {
                ctx.error(stream, DiagCode::E1009, ",", "argument list"); // Unexpected trailing comma
                stream.advance(); // Consume ')'
                break;
            }
            // Continue to next argument
        } else if (stream.check(TokenType::RPAREN)) {
            break;
        } else {
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "',' or ')'");
            synchronizeTo(stream, ctx, TokenType::RPAREN);
            if (stream.check(TokenType::RPAREN)) {
                stream.advance();
            }
            break;
        }
    }
    
    // ─── 4. Consume closing parenthesis ───────────────────────────────────
    if (!stream.check(TokenType::RPAREN)) {
        ctx.error(stream, DiagCode::E1005, ")", "argument list", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RPAREN);
        if (stream.check(TokenType::RPAREN)) {
            stream.advance(); // Consume ')' to recover
        }
    } else {
        stream.advance(); // Consume ')'
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
// parseReturnList() - Parse a list of return types
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
 * - Missing type after comma → reports E1003 with "type in return list"
 * - Missing type after `->` → reports E1003 with "return type after '->'"
 * - Trailing comma `(int, )` → reports E1009
 * - Multiple consecutive commas `(int,,,string)` → reports E1009 once
 * - Only commas `(,,,)` → reports E1009, returns empty list
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
        
        // ─── Parse types until we hit ')' ──────────────────────────────────
        while (!stream.isAtEnd()) {
            // ─── Skip consecutive separators ────────────────────────────────
            if (stream.consumeTrailing(TokenType::COMMA) > 0) {
                ctx.error(stream, DiagCode::E1009, ",", "return list"); // Unexpected trailing comma
            }
            
            // ─── Check if we've reached a terminator ────────────────────────
            if (stream.check(TokenType::RPAREN)) {
                break; // End of return list
            }
            
            if (stream.isAtEnd()) {
                ctx.error(stream, DiagCode::E1005, ")", "return list", "<EOF>");
                break;
            }
            
            // Parse a type
            TypePtr type = parseType(stream, ctx);
            if (type) {
                returnTypes.push_back(type);
            } else {
                // Error already reported by parseType, try to recover
                // Use E1003 with context: "Expected type in return list, but found '%s'"
                ctx.error(stream, DiagCode::E1003, "in return list", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                if (stream.check(TokenType::COMMA)) {
                    stream.advance();
                    continue;
                } else if (stream.check(TokenType::RPAREN)) {
                    stream.advance(); // Consume ')' and exit
                    break;
                } else {
                    break;
                }
            }
        }
        
        // ─── Consume closing parenthesis ────────────────────────────────────
        if (!stream.check(TokenType::RPAREN)) {
            ctx.error(stream, DiagCode::E1005, ")", "return list", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::RPAREN);
            if (stream.check(TokenType::RPAREN)) {
                stream.advance(); // Consume ')' to recover
            }
        } else {
            stream.advance(); // Consume ')'
        }
        
    } else {
        // ─── Single return type (no parentheses) ──────────────────────────
        // This is the case: `-> int`
        TypePtr type = parseType(stream, ctx);
        if (type) {
            returnTypes.push_back(type);
        } else {
            // Use E1003 with specific context: "Expected return type after '->', but found '%s'"
            ctx.error(stream, DiagCode::E1003, "(return type) after '->'", stream.peekValue());
            synchronizeToContext(stream, ctx);
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
 * ## Error Handling
 * 
 * - Trailing '.' (e.g., `std.io.`) → reports E1106
 * - Missing identifier after '.' → reports E1002
 * - Empty path → reports E1002
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return std::vector<InternedString> The path components (empty on error)
 *
 * @note this function does not consume `;`, the top level function already
 *       does it
 */
std::vector<InternedString> parseUsePath(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseUsePath: parsing use path");
    
    std::vector<InternedString> pathParts;
    
    // Expect at least one identifier
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "identifier in use path", stream.peekValue());
        return pathParts;
    }
    
    // Parse the first identifier
    Token tok = stream.advance();
    pathParts.push_back(ctx.pool.intern(tok.value));
    
    // Parse additional identifiers separated by '.'
    while (!stream.isAtEnd()) {
        // ─── Check for '.' separator ─────────────────────────────────────
        if (!stream.check(TokenType::DOT)) {
            break; // No more path components
        }
        
        stream.advance(); // Consume '.'
        
        // ─── Check for trailing '.' ─────────────────────────────────────
        // This handles cases like: "std.io." or "std..io"
        if (stream.isAtEnd() || 
            stream.check(TokenType::AS) || 
            stream.check(TokenType::SEMICOLON) ||
            stream.check(TokenType::DOT) ||  // Consecutive dots
            !stream.check(TokenType::IDENTIFIER)) {
            
            // Check for consecutive dots
            if (stream.check(TokenType::DOT)) {
                ctx.error(stream, DiagCode::E1009, ".", "module path"); // Consecutive dots
                // Consume the extra dot(s) to recover
                stream.consumeTrailing(TokenType::DOT);
                // Check if we have a valid identifier after the dots
                if (stream.check(TokenType::IDENTIFIER)) {
                    tok = stream.advance();
                    pathParts.push_back(ctx.pool.intern(tok.value));
                    continue;
                }
                break;
            }
            
            // Check if we're at end of file or a terminator
            if (stream.isAtEnd()) {
                ctx.error(stream, DiagCode::E1009, ".", "module path"); // Unexpected trailing '.'
                break;
            }
            
            // Check for 'as' after dot - this means trailing dot before alias
            if (stream.check(TokenType::AS)) {
                ctx.error(stream, DiagCode::E1009, ".", "module path"); // Unexpected trailing '.'
                // Don't consume 'as' - let the caller handle it
                break;
            }
            
            // Check for semicolon after dot - this means trailing dot before semicolon
            if (stream.check(TokenType::SEMICOLON)) {
                ctx.error(stream, DiagCode::E1009, ".", "module path"); // Unexpected trailing '.'
                // Don't consume ';' - let the caller handle it
                break;
            }
            
            // If we get here, we have a token that's not an identifier, not a dot, not a terminator
            // This is an error - expected identifier after '.'
            ctx.error(stream, DiagCode::E1002, "identifier after '.'", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::AS, TokenType::SEMICOLON);
            break;
        }
        
        // We have a valid identifier after the dot
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
 * ## Important Note
 * 
 * This function does NOT consume `:`. The caller (parseGenericParamDecl)
 * already consumes the `:` before calling this function.
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return TraitRefPtr The parsed trait reference, or nullptr on error
 */
TraitRefPtr parseTraitRef(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Expect an identifier for the trait name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "trait name", stream.peekValue());
        synchronizeToContext(stream, ctx);
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
                ctx.error(stream, DiagCode::E1002, "member name after ':'", stream.peekValue());
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
        // ctx.error(loc, "Expected lvalue (identifier, field access, or index)"); TODO: Fix when diagnostic code for this is ready
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 2. Parse any trailing field accesses or index expressions ──────
    while (!stream.isAtEnd()) {
        if (stream.check(TokenType::DOT)) {
            // Field access: expr.field
            stream.advance(); // Consume '.'
            
            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.error(stream, DiagCode::E1002, "field name", stream.peekValue());
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
                // ctx.error(stream.currentLoc(), "Expected index expression"); TODO: Fix when diagnostic code for this is ready
                synchronizeTo(stream, ctx, TokenType::RBRACKET);
                stream.advance(); // Consume ']' to recover
                break;
            }
            
            if (!stream.check(TokenType::RBRACKET)) {
                ctx.error(stream, DiagCode::E1005, "]", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::RBRACKET);
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
 * math:special<int>      → Special operation
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
                ctx.error(stream, DiagCode::E1002, "function name after ':'", stream.peekValue());
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
        ctx.error(stream, DiagCode::E1002, "function name", stream.peekValue());
        synchronizeToContext(stream, ctx);
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
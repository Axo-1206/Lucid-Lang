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
 *     │   └── while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET))
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
 *     │       │           │   // Bounded by Attribute context → stops at ',' or ']'
 *     │       │           ├── if (synchronizeToContext returns SyncOutcome::Abandoned)
 *     │       │           │   └── break  // Landed on escape valve or EOF
 *     │       │           └── // SyncOutcome::Continuable: we're at ',' or ']'
 *     │       │               // The loop condition handles ']'; the comma
 *     │       │               // handling below consumes ','.
 *     │       │
 *     │       └── 5.2 Comma Separator Handling
 *     │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │               └── ctx.error(stream, DiagCode::E1009, ",", "attribute list")
 *     │                   // Reports once for multiple consecutive commas between attrs
 *     │
 *     ├── 6. Closing Bracket Phase
 *     │   │
 *     │   └── if (stream.isAtEnd())
 *     │       ├── ctx.error(stream, DiagCode::E1005, "]", "attribute list", "<EOF>")
 *     │       └── // Return with whatever attrs we have (missing ']')
 *     │   └── else
 *     │       └── stream.advance()  // Consume ']' (we know it's there from loop condition)
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
 * - synchronizeToContext() attempts to recover using the Attribute follow-set
 *   ({COMMA, RBRACKET} with semantic escape valves SEMICOLON and declaration
 *   keywords)
 * - If sync returns SyncOutcome::Continuable, we're at ',' or ']':
 *   - ']' → loop condition exits naturally
 *   - ',' → comma handling below consumes it and loop continues
 * - If sync returns SyncOutcome::Abandoned, we hit a semantic escape valve
 *   (SEMICOLON or declaration keyword) or EOF - break the loop
 * 
 * ### Level 3: Missing Closing Bracket
 * - E1005 reported at EOF
 * - No recovery needed since we're already at EOF
 * 
 * ### Level 4: Consecutive Commas
 * - Leading commas before first attribute → E1009 once
 * - Multiple commas between attributes → E1009 once per gap
 * 
 * ## SyncOutcome Interaction
 * 
 * The parser uses SyncOutcome to distinguish between two cases after recovery:
 * 
 * - **SyncOutcome::Continuable**: Recovery landed on a structural delimiter
 *   that's part of the current construct's grammar (',' or ']' for Attribute
 *   context). The caller can safely continue parsing more items.
 * 
 * - **SyncOutcome::Abandoned**: Recovery landed on a semantic escape valve
 *   (SEMICOLON or a declaration-starting keyword) or EOF. This means the
 *   current construct cannot be meaningfully continued - the caller should
 *   break out of its parsing loop.
 * 
 * This distinction is what allows parseAttributes' main loop to avoid the
 * old pattern of manually re-checking COMMA/RBRACKET after synchronizeToContext,
 * which duplicated the follow-set logic and created a maintenance burden.
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
 * - Position is at EOF (if ']' was missing)
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
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET)) {
        // Parse a single attribute
        AttributePtr attr = parseAttribute(stream, ctx);
        if (attr) {
            attrs.push_back(attr);
        } else {
            // Error already reported, try to recover. currentContext() is
            // Attribute here (attrGuard is still on the stack), so this
            // stays bounded to ',' or ']' — it will not run past the
            // closing ']' into whatever declaration follows.
            //
            // We ask synchronizeToContext what it actually landed on
            // rather than re-checking COMMA/RBRACKET ourselves — that
            // would duplicate the exact follow-set synchronizeToContext
            // already picked for Attribute, and the two would need to be
            // kept in sync by hand every time that follow-set changes.
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                // Landed on the semantic escape valve (';' or a
                // declaration keyword) or hit EOF — this attribute list
                // cannot be continued at all. The loop condition below
                // will exit naturally (isAtEnd(), or not RBRACKET so it
                // keeps trying and immediately hits this same branch
                // again — break avoids that spin).
                break;
            }
            // Continuable: we're at ',' or ']'. The loop condition
            // handles ']'; the comma handling right below consumes ','.
        }

        // Consume at least 1 consecutive comma and Skip consecutive commna
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.error(stream, DiagCode::E1009, ",", "attribute list"); // Unexpected trailing comma
        }
    }
    
    // ─── Consume closing bracket ────────────────────────────────────────
    // The loop condition guarantees we're either at ']' or at EOF.
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, "]", "attribute list", stream.peekValue());
    } else {
        // We must be at ']' (loop condition guaranteed !stream.check(RBRACKET) is false)
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
 *     │       │   └── while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN))
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
 *     │       │       │           └── // The loop condition handles ')' and EOF naturally.
 *     │       │       │               // If we're at a comma, the comma handling below will consume it.
 *     │       │       │               // If we're at some other token, break to avoid infinite loop.
 *     │       │       │
 *     │       │       └── 4.2.2 Comma Separator Handling
 *     │       │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │       │               └── ctx.error(stream, DiagCode::E1009, ",", "attribute arguments")
 *     │       │                   // Reports once for multiple consecutive commas between args
 *     │       │
 *     │       └── 4.3 Closing Parenthesis Phase
 *     │           │
 *     │           └── if (stream.isAtEnd())
 *     │               ├── ctx.error(stream, DiagCode::E1005, ")", "attribute arguments", "<EOF>")
 *     │               └── // Return with whatever args we have (missing ')')
 *     │           └── else
 *     │               └── stream.advance()  // Consume ')' (we know it's there from loop condition)
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
 * - The loop condition handles ')' and EOF naturally
 * - If we're at a comma, the comma handling below will consume it
 * - If we're at some other token, break to avoid infinite loop
 * 
 * ### Level 3: Missing Closing Parenthesis
 * - E1005 reported at EOF
 * - No recovery needed since we're already at EOF
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
 * - Position is at EOF (if ')' was missing)
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
        while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
            // Parse an argument
            AttributeArgPtr arg = parseAttributeArgLiteral(stream, ctx);
            if (arg) {
                args.push_back(arg);
            } else {
                // Error already reported, try to recover
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                // The loop condition will handle ')' and EOF naturally.
                // If we're at a comma, the comma handling below will consume it.
                // If we're at some other token, break to avoid infinite loop.
                if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN) && !stream.isAtEnd()) {
                    break;
                }
            }

            // Consume at least 1 comma and skip consecutive commas
            if (stream.consumeTrailing(TokenType::COMMA) > 1) {
                ctx.error(stream, DiagCode::E1009, ",", "attribute arguments");
            }
        }
        
        // ─── Consume closing parenthesis ────────────────────────────────────
        // The loop condition guarantees we're either at ')' or at EOF.
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, ")", "attribute arguments", "<EOF>");
        } else {
            // We must be at ')' (loop condition guaranteed !stream.check(RPAREN) is false)
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
 * This function parses a generic parameter list that follows a declaration name.
 * It consumes the full `<...>` construct and produces zero or more GenericParamDeclAST nodes.
 * 
 * ## Program Flow
 * 
 * ```
 * parseGenericParamDecls()
 *     │
 *     ├── 1. Opening Delimiter Phase
 *     │   │
 *     │   └── if (!stream.check(TokenType::LESS))
 *     │       ├── ctx.error(stream, DiagCode::E1004, "<", "generic parameter list", got)
 *     │       └── return empty ArenaSpan<GenericParamDeclPtr>
 *     │   └── else
 *     │       └── stream.advance()  // Consume '<'
 *     │
 *     ├── 2. Empty List Check
 *     │   └── if (stream.check(TokenType::GREATER))
 *     │       ├── ctx.error(stream, DiagCode::E1003, "generic parameter", ">")
 *     │       ├── stream.advance()  // Consume '>'
 *     │       └── return empty ArenaSpan<GenericParamDeclPtr>
 *     │
 *     ├── 3. Scoped Context Push
 *     │   └── ScopedContext guard(ctx, SyntacticContext::GenericParams, loc)
 *     │       └── Pushes GenericParams context once; auto-pops on every return path
 *     │
 *     ├── 4. Leading Comma Recovery
 *     │   └── if (stream.consumeTrailing(TokenType::COMMA) > 0)
 *     │       └── ctx.error(stream, DiagCode::E1009, ",", "generic parameters")
 *     │           // <,,,, T ...> — reports once for all leading commas
 *     │
 *     ├── 5. Main Parameter Parsing Loop
 *     │   │
 *     │   └── while (!stream.isAtEnd() && !stream.check(TokenType::GREATER))
 *     │       │
 *     │       ├── 5.1 Parse Single Parameter
 *     │       │   └── parseGenericParamDecl(stream, ctx) → GenericParamDeclPtr
 *     │       │       │
 *     │       │       ├── if (param != nullptr)
 *     │       │       │   └── params.push_back(param)
 *     │       │       │
 *     │       │       └── else
 *     │       │           ├── Error already reported by parseGenericParamDecl
 *     │       │           ├── synchronizeToContext(stream, ctx)
 *     │       │           │   // Bounded by GenericParams context → stops at ',' or '>'
 *     │       │           ├── if (synchronizeToContext returns SyncOutcome::Abandoned)
 *     │       │           │   └── break  // Landed on escape valve or EOF
 *     │       │           └── // SyncOutcome::Continuable: we're at ',' or '>'
 *     │       │               // The loop condition handles '>'; the comma
 *     │       │               // handling below consumes ','.
 *     │       │
 *     │       └── 5.2 Comma Separator Handling
 *     │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │               └── ctx.error(stream, DiagCode::E1009, ",", "generic parameters")
 *     │                   // Reports once for multiple consecutive commas between params
 *     │
 *     ├── 6. Closing Bracket Phase
 *     │   │
 *     │   └── if (stream.isAtEnd())
 *     │       ├── ctx.error(stream, DiagCode::E1005, ">", "generic parameter list", "<EOF>")
 *     │       └── // Return with whatever params we have (missing '>')
 *     │   └── else
 *     │       └── stream.advance()  // Consume '>' (we know it's there from loop condition)
 *     │
 *     ├── 7. Build Result
 *     │   └── ctx.arena.makeBuilder<GenericParamDeclPtr>()
 *     │       └── push_back each param → build() → ArenaSpan<GenericParamDeclPtr>
 *     │
 *     └── 8. Return
 *         └── return ArenaSpan<GenericParamDeclPtr> (possibly empty)
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * ### Level 1: Missing '<'
 * - E1004 reported, return empty
 * 
 * ### Level 2: Empty Generic Parameter List
 * - `<>` → E1003, consume '>', return empty
 * 
 * ### Level 3: Malformed Parameter Within List
 * - parseGenericParamDecl() reports its own error and returns nullptr
 * - synchronizeTo(COMMA, GREATER) attempts to recover using the GenericParams
 *   follow-set ({COMMA, GREATER} with semantic escape valves LBRACE, LPAREN,
 *   SEMICOLON, and declaration keywords)
 * - If sync returns SyncOutcome::Continuable, we're at ',' or '>':
 *   - '>' → loop condition exits naturally
 *   - ',' → comma handling below consumes it and loop continues
 * - If sync returns SyncOutcome::Abandoned, we hit a semantic escape valve
 *   (LBRACE, LPAREN, SEMICOLON, or declaration keyword) or EOF - break the loop
 * 
 * ### Level 4: Missing Closing Bracket
 * - E1005 reported at EOF
 * - No recovery needed since we're already at EOF
 * 
 * ### Level 5: Consecutive Commas
 * - Leading commas before first parameter → E1009 once
 * - Multiple commas between parameters → E1009 once per gap
 * 
 * ## SyncOutcome Interaction
 * 
 * The parser uses SyncOutcome to distinguish between two cases after recovery:
 * 
 * - **SyncOutcome::Continuable**: Recovery landed on a structural delimiter
 *   that's part of the current construct's grammar (',' or '>' for GenericParams
 *   context). The caller can safely continue parsing more items.
 * 
 * - **SyncOutcome::Abandoned**: Recovery landed on a semantic escape valve
 *   (LBRACE, LPAREN, SEMICOLON, or a declaration-starting keyword) or EOF.
 *   This means the current construct cannot be meaningfully continued - the
 *   caller should break out of its parsing loop.
 * 
 * This distinction is what allows parseGenericParamDecls' main loop to avoid
 * the old pattern of manually re-checking COMMA/GREATER after synchronizeTo,
 * which duplicated the follow-set logic and created a maintenance burden.
 * 
 * ## Context Stack Interaction
 * 
 * The ScopedContext guard ensures:
 * - synchronizeToContext() stops at ',' or '>' (GenericParams follow-set)
 * - Does NOT scan past '>' into the enclosing declaration
 * - Auto-pops on every return path (including early returns)
 * 
 * ## Token Stream State
 * 
 * After this function completes:
 * - Position is AFTER '>' (normal case)
 * - Position is at EOF (if '>' was missing)
 * 
 * ## Examples
 * 
 * ```lucid
 * <T>                                    → [T]
 * <A, B>                                 → [A, B]
 * <T : Vector2, U>                       → [T: Vector2, U]
 * <T : Named + Serializable>             → [T: Named + Serializable]
 * <,,,T>                                 → [T] + E1009 (leading commas)
 * <T,,,U>                                → [T, U] + E1009 (gap)
 * <T,                                    → [T] + E1005 (missing '>')
 * <>                                     → [] + E1003 (empty list)
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<GenericParamDeclPtr> The parsed generic parameters (empty on error)
 *
 * @note This function consumes '>'. The caller must NOT consume '>'.
 */
ArenaSpan<GenericParamDeclPtr> parseGenericParamDecls(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericParamDecls: parsing generic parameters");
    
    std::vector<GenericParamDeclPtr> params;
    
    // ─── 1. Expect '<' to start the list ─────────────────────────────────
    if (!stream.check(TokenType::LESS)) {
        ctx.error(stream, DiagCode::E1004, "<", "generic parameter list", stream.peekValue());
        return ctx.arena.makeBuilder<GenericParamDeclPtr>().build();
    }
    stream.advance(); // Consume '<'
    
    // ─── 2. Check for empty generic parameter list: <> ───────────────────
    if (stream.check(TokenType::GREATER)) {
        ctx.error(stream, DiagCode::E1003, "generic parameter", ">");
        stream.advance(); // Consume '>'
        return ctx.arena.makeBuilder<GenericParamDeclPtr>().build();
    }
    
    // ─── 3. Push GenericParams context — auto-pops on every return path ──
    ScopedContext guard(ctx, SyntacticContext::GenericParams, stream.currentLoc());
    
    // ─── 4. Skip initial consecutive commas ────────────────────────────
    // Ex: <,,,, T ...>
    if (stream.consumeTrailing(TokenType::COMMA) > 0) {
        ctx.error(stream, DiagCode::E1009, ",", "generic parameters");
    }
    
    // ─── 5. Parse parameters until we hit '>' ──────────────────────────
    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        // Parse a single generic parameter
        GenericParamDeclPtr param = parseGenericParamDecl(stream, ctx);
        if (param) {
            params.push_back(param);
        } else {
            // Error already reported, try to recover. currentContext() is
            // GenericParams here (guard is still on the stack), so this
            // stays bounded to ',' or '>' — it will not run past the
            // closing '>' into whatever declaration follows.
            //
            // We ask synchronizeTo what it actually landed on rather than
            // re-checking COMMA/GREATER ourselves — that would duplicate
            // the exact follow-set synchronizeTo already uses, and the two
            // would need to be kept in sync by hand every time that
            // follow-set changes.
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                // Landed on the semantic escape valve (LBRACE, LPAREN,
                // SEMICOLON, or a declaration keyword) or hit EOF — this
                // generic parameter list cannot be continued at all. The
                // loop condition below will exit naturally (isAtEnd(), or
                // not GREATER so it keeps trying and immediately hits this
                // same branch again — break avoids that spin).
                break;
            }
            // Continuable: we're at ',' or '>'. The loop condition
            // handles '>'; the comma handling right below consumes ','.
        }

        // ─── 5.2 Comma Separator Handling ────────────────────────────────
        // Consume at least 1 comma and skip consecutive commas
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.error(stream, DiagCode::E1009, ",", "generic parameters");
        }
    }
    
    // ─── 6. Consume closing bracket ──────────────────────────────────────
    // The loop condition guarantees we're either at '>' or at EOF.
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ">", "generic parameter list", stream.peekValue());
    } else {
        // We must be at '>' (loop condition guaranteed !stream.check(GREATER) is false)
        stream.advance(); // Consume '>'
    }
    
    // ─── 7. Build the ArenaSpan ────────────────────────────────────────
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
 * This function parses one generic parameter within a generic parameter list.
 * It expects to be called only from inside parseGenericParamDecls()'s
 * ScopedContext guard (SyntacticContext::GenericParams).
 * 
 * ## Program Flow
 * 
 * ```
 * parseGenericParamDecl()
 *     │
 *     ├── 1. Capture Location
 *     │   └── SourceLocation loc = stream.currentLoc()
 *     │
 *     ├── 2. Parse Parameter Name
 *     │   │
 *     │   └── if (!stream.check(TokenType::IDENTIFIER))
 *     │       ├── ctx.error(stream, DiagCode::E1002, "generic parameter name", got)
 *     │       ├── synchronizeToContext(stream, ctx)
 *     │       │   // Bounded by GenericParams context → stops at ',' or '>'
 *     │       └── return nullptr
 *     │   └── else
 *     │       ├── Token nameTok = stream.advance()
 *     │       └── InternedString name = ctx.pool.intern(nameTok.value)
 *     │
 *     ├── 3. Allocate Parameter Node
 *     │   └── auto* param = ctx.arena.make<GenericParamDeclAST>(name)
 *     │       ├── param->loc = loc
 *     │       └── param->name = name
 *     │
 *     ├── 4. Optional Constraints Phase
 *     │   │
 *     │   └── if (stream.match(TokenType::COLON))
 *     │       │   // ':' was present — parse constraint list
 *     │       │
 *     │       ├── 4.1 Leading Plus Recovery
 *     │       │   └── if (stream.consumeTrailing(TokenType::PLUS) > 0)
 *     │       │       └── ctx.error(stream, DiagCode::E1009, "+", "generic constraints")
 *     │       │           // <T : +++ Named> — reports once for all leading pluses
 *     │       │           // <T : + > — reports once, next iteration hits terminator
 *     │       │
 *     │       ├── 4.2 Constraint Parsing Loop
 *     │       │   │
 *     │       │   └── while (!stream.isAtEnd() && 
 *     │       │       │      !stream.check(TokenType::GREATER) &&
 *     │       │       │      !stream.check(TokenType::COMMA))
 *     │       │       │
 *     │       │       ├── 4.2.1 Parse Trait Reference
 *     │       │       │   └── parseTraitRef(stream, ctx) → TraitRefPtr
 *     │       │       │       │
 *     │       │       │       ├── if (traitRef != nullptr)
 *     │       │       │       │   ├── constraints.push_back(traitRef)
 *     │       │       │       │   └── hasConstraint = true
 *     │       │       │       │
 *     │       │       │       └── else
 *     │       │       │           ├── ctx.error(stream, DiagCode::E1003,
 *     │       │       │           │           "trait reference after ':' or '+'", got)
 *     │       │       │           ├── synchronizeTo(stream, ctx, COMMA, GREATER)
 *     │       │       │           └── break
 *     │       │       │
 *     │       │       └── 4.2.2 Trailing Plus Check
 *     │       │           ├── int plusCount = stream.consumeTrailing(TokenType::PLUS)
 *     │       │           │
 *     │       │           ├── if (plusCount == 1)
 *     │       │           │   ├── Ambiguous: could be separator (A + B) or trailing (A + >)
 *     │       │           │   ├── if (stream.check(GREATER) || stream.check(COMMA))
 *     │       │           │   │   └── ctx.error(E1009)  // Single '+' before terminator → trailing
 *     │       │           │   └── else
 *     │       │           │       └── No error  // Single '+' before trait name → valid separator
 *     │       │           │
 *     │       │           ├── else if (plusCount > 1)
 *     │       │           │   └── ctx.error(E1009)  // Multiple '+' always an error
 *     │       │           │
 *     │       │           └── else (plusCount == 0)
 *     │       │               └── No plus found, loop continues normally
 *     │       │
 *     │       │           // Loop continues; next iteration either parses next trait or hits terminator
 *     │       │
 *     │       └── 4.3 Empty Constraint Check
 *     │           └── if (!hasConstraint)
 *     │               └── ctx.error(stream, DiagCode::E1003,
 *     │                               "trait constraint after ':'", stream.peekValue())
 *     │           // Note: constraints vector may still be non-empty if only
 *     │           // trailing/leading plus errors were reported
 *     │
 *     ├── 5. Build Constraints Span
 *     │   └── ctx.arena.makeBuilder<TraitRefPtr>()
 *     │       ├── push_back each constraint
 *     │       └── build() → param->constraints
 *     │
 *     ├── 6. Logging
 *     │   └── LOG_PARSER_DETAIL("parsed parameter 'name' with N constraints")
 *     │
 *     └── 7. Return
 *         └── return param  (or nullptr if error occurred at step 2)
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * ### Level 1: Missing Parameter Name
 * - E1002 reported
 * - synchronizeToContext() skips to ',' or '>' (bounded by GenericParams guard)
 * - Returns nullptr; parseGenericParamDecls handles the gap
 * 
 * ### Level 2: Empty Constraints (T :)
 * - No trait refs parsed, hasConstraint stays false
 * - E1003 reported at step 4.3
 * 
 * ### Level 3: Leading Plus(es) (T : +++Named)
 * - All leading pluses consumed by consumeTrailing
 * - E1009 reported once
 * - Loop continues; trait ref parsed on next iteration
 * 
 * ### Level 4: Trailing Plus(es) (T : Vector2 +++)
 * - All trailing pluses consumed by consumeTrailing
 * - E1009 reported once
 * - Loop continues; if more traits follow, they're parsed; else terminator check exits
 * 
 * ### Level 5: Malformed Trait Reference
 * - parseTraitRef() returns nullptr
 * - E1003 reported, synchronizeTo(COMMA, GREATER)
 * 
 * ## Context Stack Interaction
 * 
 * This function relies on parseGenericParamDecls() having pushed
 * SyntacticContext::GenericParams. synchronizeToContext() uses this to
 * bound recovery:
 * - Follow-set: {COMMA, GREATER}
 * - Does NOT scan past '>' into declaration
 * 
 * ## Token Stream State
 * 
 * After this function completes:
 * - Position is AFTER parameter name (no constraints)
 * - Position is at ',' or '>' (constraints parsed normally)
 * - Position is at recovery point (if error in name)
 * - Position is at ',' or '>' (if constraint error and recovered)
 * 
 * ## Examples
 * 
 * ```lucid
 * T                                      → name="T", constraints={}
 * T : Vector2                            → name="T", constraints=[Vector2]
 * T : Named + Serializable               → name="T", constraints=[Named, Serializable]
 * : Vector2                              → nullptr + E1002 (missing name)
 * T :                                    → name="T", constraints={} + E1003 (empty)
 * T : +                                  → name="T", constraints={} + E1009 (leading +)
 * T : Vector2 +                          → name="T", constraints=[Vector2] + E1009 (trailing +)
 * T : Vector2 + + + Named                → name="T", constraints=[Vector2, Named] + E1009 (gap +++)
 * T : Vector2 + >                        → name="T", constraints=[Vector2] + E1009 (trailing +)
 * T : Vector2 + Serializable             → name="T", constraints=[Vector2, Serializable] (valid)
 * T : Vector2 + + + Named              → name="T", constraints=[Vector2, Named] + E1009 (multiple +)
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return GenericParamDeclPtr The parsed generic parameter, or nullptr on error
 * 
 * @note This function does NOT consume '>' or ','. That is handled by
 *       the caller (parseGenericParamDecls).
 * 
 * @note This function is only ever called from inside parseGenericParamDecls()'s
 *       GenericParams ScopedContext guard. The synchronizeToContext() call
 *       stays bounded to ',' / '>' rather than scanning for unrelated
 *       global tokens.
 */
GenericParamDeclPtr parseGenericParamDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Expect an identifier for the parameter name ──────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "generic parameter name", stream.peekValue());
        // parseGenericParamDecl is only ever called from inside
        // parseGenericParamDecls' GenericParams guard, so this stays
        // bounded to ',' / '>' rather than scanning for an unrelated
        // global token set.
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 2. Create the generic parameter node ────────────────────────────
    auto* param = ctx.arena.make<GenericParamDeclAST>(name);
    param->loc = loc;
    
    // ─── 3. Check for constraints (optional) ───────────────────────────
    if (stream.match(TokenType::COLON)) {
        std::vector<TraitRefPtr> constraints;
        bool hasConstraint = false;
        
        // ─── 3.1 Leading plus recovery ─────────────────────────────────
        // Ex: <T : +++ Named> or <T : + >
        if (stream.consumeTrailing(TokenType::PLUS) > 0) {
            ctx.error(stream, DiagCode::E1009, "+", "generic constraints");
        }
        
        // ─── 3.2 Parse constraints until terminator ──────────────────────
        while (!stream.isAtEnd() && 
               !stream.check(TokenType::GREATER) && 
               !stream.check(TokenType::COMMA)) {
            
            // ─── Parse a trait reference ─────────────────────────────────
            TraitRefPtr traitRef = parseTraitRef(stream, ctx);
            if (traitRef) {
                constraints.push_back(traitRef);
                hasConstraint = true;
            } else {
                // Error already reported by parseTraitRef
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
                break;
            }
            
            // ─── Trailing plus check ───────────────────────────────────
            // A single '+' is ambiguous: it could separate traits (Named + Serializable)
            // or be trailing (Vector2 + >). We only know it's trailing if the next
            // token after '+' is '>' or ',' (terminator). Multiple consecutive '+'
            // are always an error regardless of what follows.
            int plusCount = stream.consumeTrailing(TokenType::PLUS);
            if (plusCount == 1) {
                // Ambiguous: could be separator or trailing. Check what follows.
                if (stream.check(TokenType::GREATER) || stream.check(TokenType::COMMA)) {
                    // Single '+' before terminator → definitely trailing
                    ctx.error(stream, DiagCode::E1009, "+", "generic constraints");
                }
                // else: single '+' before a trait name → valid separator, no error
            } else if (plusCount > 1) {
                // Multiple consecutive '+' → always an error
                ctx.error(stream, DiagCode::E1009, "+", "generic constraints");
            }
        }
        
        // ─── 3.3 Check if we have constraints but they're empty ──────
        if (!hasConstraint) {
            ctx.error(stream, DiagCode::E1003, 
                      "trait constraint after ':'", stream.peekValue());
        }
        
        // ─── 3.4 Build the constraints span ──────────────────────────
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
 * This function parses a generic argument list that follows a type name
 * (e.g., `Map<string, int>`) or a function call (e.g., `foo<int>(...)`).
 * It consumes the full `<...>` construct and produces a span of type nodes.
 *
 * ## Program Flow
 *
 * ```
 * parseGenericArgs()
 *     │
 *     ├── 1. Opening Delimiter Phase
 *     │   │
 *     │   └── if (!stream.check(TokenType::LESS))
 *     │       ├── ctx.error(stream, DiagCode::E1004, "<", "generic argument list", got)
 *     │       └── return empty ArenaSpan<TypePtr>
 *     │   └── else
 *     │       └── stream.advance()  // Consume '<'
 *     │
 *     ├── 2. Empty List Check
 *     │   └── if (stream.check(TokenType::GREATER))
 *     │       ├── ctx.error(stream, DiagCode::E1003, "generic argument", ">")
 *     │       ├── stream.advance()  // Consume '>'
 *     │       └── return empty ArenaSpan<TypePtr>
 *     │
 *     ├── 3. Scoped Context Push
 *     │   └── ScopedContext guard(ctx, SyntacticContext::GenericArgs, loc)
 *     │       └── Pushes GenericArgs context once; auto-pops on every return path
 *     │
 *     ├── 4. Leading Comma Recovery
 *     │   └── if (stream.consumeTrailing(TokenType::COMMA) > 0)
 *     │       └── ctx.error(stream, DiagCode::E1009, ",", "generic arguments")
 *     │           // <,,,, int> — reports once for all leading commas
 *     │
 *     ├── 5. Main Argument Parsing Loop
 *     │   │
 *     │   └── while (!stream.isAtEnd() && !stream.check(TokenType::GREATER))
 *     │       │
 *     │       ├── 5.1 Parse Single Type
 *     │       │   └── parseType(stream, ctx) → TypePtr
 *     │       │       │
 *     │       │       ├── if (type != nullptr)
 *     │       │       │   └── args.push_back(type)
 *     │       │       │
 *     │       │       └── else
 *     │       │           ├── Error already reported by parseType
 *     │       │           ├── synchronizeToContext(stream, ctx)
 *     │       │           │   // Bounded by GenericArgs context → stops at ',' or '>'
 *     │       │           ├── if (synchronizeToContext returns SyncOutcome::Abandoned)
 *     │       │           │   └── break  // Landed on escape valve or EOF
 *     │       │           └── // SyncOutcome::Continuable: we're at ',' or '>'
 *     │       │               // The loop condition handles '>'; the comma
 *     │       │               // handling below consumes ','.
 *     │       │
 *     │       └── 5.2 Comma Separator Handling
 *     │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │               └── ctx.error(stream, DiagCode::E1009, ",", "generic arguments")
 *     │                   // Reports once for multiple consecutive commas between args
 *     │
 *     ├── 6. Closing Bracket Phase
 *     │   │
 *     │   └── if (stream.isAtEnd())
 *     │       ├── ctx.error(stream, DiagCode::E1005, ">", "generic argument list", "<EOF>")
 *     │       └── // Return with whatever args we have (missing '>')
 *     │   └── else
 *     │       └── stream.advance()  // Consume '>' (we know it's there from loop condition)
 *     │
 *     ├── 7. Build Result
 *     │   └── ctx.arena.makeBuilder<TypePtr>()
 *     │       └── push_back each arg → build() → ArenaSpan<TypePtr>
 *     │
 *     └── 8. Return
 *         └── return ArenaSpan<TypePtr> (possibly empty)
 * ```
 *
 * ## Error Recovery Strategy
 *
 * ### Level 1: Missing '<'
 * - E1004 reported, return empty
 *
 * ### Level 2: Empty Generic Argument List (`<>`)
 * - E1003 reported, consume '>', return empty
 *
 * ### Level 3: Malformed Type Argument
 * - parseType() reports its own error and returns nullptr
 * - synchronizeTo(COMMA, GREATER) attempts to recover using the GenericArgs
 *   follow-set ({COMMA, GREATER} with semantic escape valves LPAREN,
 *   SEMICOLON, and declaration keywords)
 * - If sync returns SyncOutcome::Continuable, we're at ',' or '>':
 *   - '>' → loop condition exits naturally
 *   - ',' → comma handling below consumes it and loop continues
 * - If sync returns SyncOutcome::Abandoned, we hit a semantic escape valve
 *   (LPAREN, SEMICOLON, or declaration keyword) or EOF - break the loop
 *
 * ### Level 4: Missing Closing Bracket
 * - E1005 reported at EOF
 * - No recovery needed since we're already at EOF
 *
 * ### Level 5: Consecutive Commas
 * - Leading commas before first argument → E1009 once
 * - Multiple commas between arguments → E1009 once per gap
 *
 * ## SyncOutcome Interaction
 *
 * The parser uses SyncOutcome to distinguish between two cases after recovery:
 *
 * - **SyncOutcome::Continuable**: Recovery landed on a structural delimiter
 *   that's part of the current construct's grammar (',' or '>' for GenericArgs
 *   context). The caller can safely continue parsing more items.
 *
 * - **SyncOutcome::Abandoned**: Recovery landed on a semantic escape valve
 *   (LPAREN, SEMICOLON, or a declaration-starting keyword) or EOF. This means
 *   the current construct cannot be meaningfully continued - the caller should
 *   break out of its parsing loop.
 *
 * This distinction is what allows parseGenericArgs' main loop to avoid the
 * old pattern of manually re-checking COMMA/GREATER after synchronizeTo,
 * which duplicated the follow-set logic and created a maintenance burden.
 *
 * ## Context Stack Interaction
 *
 * The ScopedContext guard ensures:
 * - synchronizeToContext() stops at ',' or '>' (GenericArgs follow-set)
 * - Does NOT scan past '>' into the enclosing construct
 * - Auto-pops on every return path (including early returns)
 *
 * ## Token Stream State
 *
 * After this function completes:
 * - Position is AFTER '>' (normal case)
 * - Position is at EOF (if '>' was missing)
 *
 * ## Examples
 *
 * ```lucid
 * <int>                                  → [int]
 * <string, int>                          → [string, int]
 * <Map<string, int>>                     → [Map<string, int>]
 * <,,,int>                               → [int] + E1009 (leading commas)
 * <int,,,string>                         → [int, string] + E1009 (gap)
 * <int,                                  → [int] + E1005 (missing '>')
 * <int, >                                → [int] + E1009 (trailing comma)
 * <int, string, >                        → [int, string] + E1009 (trailing comma)
 * <>                                     → [] + E1003 (empty list)
 * ```
 *
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<TypePtr> The parsed generic arguments (empty on error)
 *
 * @note This function consumes '>'. The caller must NOT consume '>'.
 */
ArenaSpan<TypePtr> parseGenericArgs(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericArgs: parsing generic arguments");

    std::vector<TypePtr> args;

    // ─── 1. Expect '<' to start the list ─────────────────────────────────
    if (!stream.check(TokenType::LESS)) {
        ctx.error(stream, DiagCode::E1004, "<", "generic argument list", stream.peekValue());
        return ctx.arena.makeBuilder<TypePtr>().build();
    }
    stream.advance(); // Consume '<'

    // ─── 2. Check for empty generic argument list: <> ───────────────────
    if (stream.check(TokenType::GREATER)) {
        ctx.error(stream, DiagCode::E1003, "generic argument", ">");
        stream.advance(); // Consume '>'
        return ctx.arena.makeBuilder<TypePtr>().build();
    }

    // ─── 3. Push GenericArgs context — auto-pops on every return path ──
    ScopedContext guard(ctx, SyntacticContext::GenericArgs, stream.currentLoc());

    // ─── 4. Skip initial consecutive commas ──────────────────────────────
    // Ex: <,,,, int ...>
    if (stream.consumeTrailing(TokenType::COMMA) > 0) {
        ctx.error(stream, DiagCode::E1009, ",", "generic arguments");
    }

    // ─── 5. Parse arguments until we hit '>' ────────────────────────────
    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        // ─── 5.1 Parse a type as the generic argument ──────────────────
        TypePtr type = parseType(stream, ctx);
        if (type) {
            args.push_back(type);
        } else {
            // Error already reported by parseType, try to recover
            // currentContext() is GenericArgs (guard is still on the stack),
            // so synchronizeTo(COMMA, GREATER) stays bounded.
            //
            // We ask synchronizeTo what it actually landed on rather than
            // re-checking COMMA/GREATER ourselves — that would duplicate
            // the exact follow-set synchronizeTo already uses, and the two
            // would need to be kept in sync by hand every time that
            // follow-set changes.
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                // Landed on the semantic escape valve (LPAREN, SEMICOLON,
                // or a declaration keyword) or hit EOF — this generic
                // argument list cannot be continued at all. The loop
                // condition below will exit naturally (isAtEnd(), or not
                // GREATER so it keeps trying and immediately hits this
                // same branch again — break avoids that spin).
                break;
            }
            // Continuable: we're at ',' or '>'. The loop condition
            // handles '>'; the comma handling right below consumes ','.
        }

        // ─── 5.2 Comma Separator Handling ────────────────────────────────
        // Consume at least 1 comma and skip consecutive commas
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.error(stream, DiagCode::E1009, ",", "generic arguments");
        }
    }

    // ─── 6. Consume closing bracket ──────────────────────────────────────
    // The loop condition guarantees we're either at '>' or at EOF.
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ">", "generic argument list", stream.peekValue());
    } else {
        // We must be at '>' (loop condition guaranteed !stream.check(GREATER) is false)
        stream.advance(); // Consume '>'
    }

    // ─── 7. Build the ArenaSpan ──────────────────────────────────────────
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
 * ## Program Flow
 * 
 * ```
 * parseParamList()
 *     │
 *     ├── 1. Opening Delimiter Phase
 *     │   │
 *     │   └── if (!stream.check(TokenType::LPAREN))
 *     │       ├── ctx.error(stream, DiagCode::E1004, "(", "parameter list", got)
 *     │       └── return empty vector
 *     │   └── else
 *     │       └── stream.advance()  // Consume '('
 *     │
 *     ├── 2. Empty List Check
 *     │   └── if (stream.check(TokenType::RPAREN))
 *     │       ├── stream.advance()  // Consume ')'
 *     │       └── return empty vector
 *     │
 *     ├── 3. Leading Comma Recovery
 *     │   └── if (stream.consumeTrailing(TokenType::COMMA) > 0)
 *     │       └── ctx.error(stream, DiagCode::E1009, ",", "parameter list")
 *     │           // (,,,, a int ...) — reports once for all leading commas
 *     │
 *     ├── 4. Main Parameter Parsing Loop
 *     │   │
 *     │   └── while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN))
 *     │       │
 *     │       ├── 4.1 Parse 'const' Modifier
 *     │       │   └── bool isConst = stream.match(TokenType::CONST)
 *     │       │
 *     │       ├── 4.2 Parse Parameter Name
 *     │       │   │
 *     │       │   └── if (!stream.check(TokenType::IDENTIFIER))
 *     │       │       ├── ctx.error(stream, DiagCode::E1002, "parameter name", got)
 *     │       │       ├── synchronizeTo(stream, ctx, COMMA, RPAREN)
 *     │       │       └── // The loop condition handles ')' and EOF naturally.
 *     │       │           // If we're at a comma, the comma handling below will consume it.
 *     │       │           // If we're at some other token, break to avoid infinite loop.
 *     │       │   └── else
 *     │       │       └── Token nameTok = stream.advance()
 *     │       │
 *     │       ├── 4.3 Parse Variadic Modifier
 *     │       │   └── bool isVariadic = stream.match(TokenType::VARIADIC)
 *     │       │
 *     │       ├── 4.4 Parse Parameter Type
 *     │       │   │
 *     │       │   └── TypePtr type = parseType(stream, ctx)
 *     │       │       │
 *     │       │       ├── if (type != nullptr)
 *     │       │       │   └── Create ParamAST and push to params
 *     │       │       │
 *     │       │       └── else
 *     │       │           ├── ctx.error(stream, DiagCode::E1003, "parameter type", got)
 *     │       │           ├── synchronizeTo(stream, ctx, COMMA, RPAREN)
 *     │       │           └── // The loop condition handles ')' and EOF naturally.
 *     │       │               // If we're at a comma, the comma handling below will consume it.
 *     │       │               // If we're at some other token, break to avoid infinite loop.
 *     │       │
 *     │       ├── 4.5 Variadic Last Check
 *     │       │   └── if (isVariadic && stream.check(TokenType::COMMA))
 *     │       │       ├── ctx.error(stream, DiagCode::E1010, "parameter group",
 *     │       │       │              "variadic parameter must be the last parameter")
 *     │       │       └── // The comma will be consumed by the comma handling below
 *     │       │
 *     │       └── 4.6 Comma Separator Handling
 *     │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │               └── ctx.error(stream, DiagCode::E1009, ",", "parameter list")
 *     │                   // Reports once for multiple consecutive commas between params
 *     │
 *     ├── 5. Closing Parenthesis Phase
 *     │   │
 *     │   └── if (stream.isAtEnd())
 *     │       ├── ctx.error(stream, DiagCode::E1005, ")", "parameter list", "<EOF>")
 *     │       └── // Return with whatever params we have (missing ')')
 *     │   └── else
 *     │       └── stream.advance()  // Consume ')' (we know it's there from loop condition)
 *     │
 *     └── 6. Return
 *         └── return params
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * ### Level 1: Missing '('
 * - E1004 reported, return empty
 * 
 * ### Level 2: Missing Parameter Name
 * - E1002 reported
 * - synchronizeTo(COMMA, RPAREN) skips to next separator or terminator
 * 
 * ### Level 3: Missing Parameter Type
 * - E1003 reported
 * - synchronizeTo(COMMA, RPAREN) skips to next separator or terminator
 * 
 * ### Level 4: Variadic Not Last
 * - E1010 reported when variadic is followed by a comma
 * - The comma handling below consumes the comma to allow recovery
 * 
 * ### Level 5: Missing Closing Parenthesis
 * - E1005 reported at EOF
 * - No recovery needed since we're already at EOF
 * 
 * ### Level 6: Consecutive Commas
 * - Leading commas before first parameter → E1009 once
 * - Multiple commas between parameters → E1009 once per gap
 * 
 * ## Token Stream State
 * 
 * After this function completes:
 * - Position is AFTER ')' (normal case)
 * - Position is at EOF (if ')' was missing)
 * - Position is at recovery point (if errors occurred)
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
 * (a int, b int,)                      → [a: int, b: int] + E1009 (trailing comma)
 * (a int,,,b int)                      → [a: int, b: int] + E1009 (gap)
 * (a int,                              → [a: int] + E1005 (missing ')')
 * (a int, b int, rest ...string,       → [a: int, b: int, rest: ...string] + E1010 (variadic not last) + E1005 (missing ')')
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return std::vector<ParamPtr> The parsed parameters (empty for void)
 *
 * @note This function consumes ')'. The caller must NOT consume ')'.
 */
std::vector<ParamPtr> parseParamList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseParamList: parsing parameter list");
    
    std::vector<ParamPtr> params;
    
    // ─── 1. Expect '(' to start the parameter list ──────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        ctx.error(stream, DiagCode::E1004, "(", "parameter list", stream.peekValue());
        return params;
    }
    stream.advance(); // Consume '('
    
    // ─── 2. Check for empty parameter list: () ──────────────────────────
    if (stream.check(TokenType::RPAREN)) {
        stream.advance(); // Consume ')'
        return params;
    }
    
    // ─── 3. Skip initial consecutive commas ──────────────────────────────
    // Ex: (,,,, a int ...)
    if (stream.consumeTrailing(TokenType::COMMA) > 0) {
        ctx.error(stream, DiagCode::E1009, ",", "parameter list");
    }
    
    // ─── 4. Parse parameters until we hit ')' ──────────────────────────
    bool hasVariadic = false;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        SourceLocation loc = stream.currentLoc();
        
        // ─── 4.1 Check for 'const' modifier ──────────────────────────────
        bool isConst = stream.match(TokenType::CONST);
        
        // ─── 4.2 Parse parameter name ─────────────────────────────────────
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "parameter name", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            // The loop condition will handle ')' and EOF naturally.
            // If we're at a comma, the comma handling below will consume it.
            // If we're at some other token, break to avoid infinite loop.
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN) && !stream.isAtEnd()) {
                break;
            }
            continue;
        }
        
        Token nameTok = stream.advance();
        InternedString name = ctx.pool.intern(nameTok.value);
        
        // ─── 4.3 Check for variadic modifier ─────────────────────────────
        bool isVariadic = stream.match(TokenType::VARIADIC);
        
        // ─── 4.4 Parse parameter type ─────────────────────────────────────
        TypePtr type = parseType(stream, ctx);
        if (!type) {
            ctx.error(stream, DiagCode::E1003, "parameter type", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            // The loop condition will handle ')' and EOF naturally.
            // If we're at a comma, the comma handling below will consume it.
            // If we're at some other token, break to avoid infinite loop.
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN) && !stream.isAtEnd()) {
                break;
            }
            continue;
        }
        
        // ─── 4.5 Create the parameter node ────────────────────────────────
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
        
        // ─── 4.6 Check if variadic is last ─────────────────────────────────
        // If we have a variadic parameter, check if there's another parameter after it
        if (isVariadic && stream.check(TokenType::COMMA)) {
            ctx.error(stream, DiagCode::E1010, "parameter group", "variadic parameter must be the last parameter");
            // The comma will be consumed by the comma handling below
        }
        
        // ─── 4.7 Comma Separator Handling ────────────────────────────────
        // Consume at least 1 comma and skip consecutive commas
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.error(stream, DiagCode::E1009, ",", "parameter list");
        }
    }
    
    // ─── 5. Consume closing parenthesis ────────────────────────────────────
    // The loop condition guarantees we're either at ')' or at EOF.
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ")", "parameter list", "<EOF>");
    } else {
        // We must be at ')' (loop condition guaranteed !stream.check(RPAREN) is false)
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
 * ## Program Flow
 * 
 * ```
 * parseArgList()
 *     │
 *     ├── 1. Opening Delimiter Phase
 *     │   │
 *     │   └── if (!stream.check(TokenType::LPAREN))
 *     │       ├── ctx.error(stream, DiagCode::E1004, "(", "argument list", got)
 *     │       └── return empty ArenaSpan<ExprPtr>
 *     │   └── else
 *     │       └── stream.advance()  // Consume '('
 *     │
 *     ├── 2. Empty List Check
 *     │   └── if (stream.check(TokenType::RPAREN))
 *     │       ├── stream.advance()  // Consume ')'
 *     │       └── return empty ArenaSpan<ExprPtr>
 *     │
 *     ├── 3. Leading Comma Recovery
 *     │   └── if (stream.consumeTrailing(TokenType::COMMA) > 0)
 *     │       └── ctx.error(stream, DiagCode::E1009, ",", "argument list")
 *     │           // (,,,, arg1 ...) — reports once for all leading commas
 *     │
 *     ├── 4. Main Argument Parsing Loop
 *     │   │
 *     │   └── while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN))
 *     │       │
 *     │       ├── 4.1 Parse Argument Expression
 *     │       │   │
 *     │       │   └── ExprPtr arg = parseExpr(stream, ctx)
 *     │       │       │
 *     │       │       ├── if (arg != nullptr)
 *     │       │       │   └── args.push_back(arg)
 *     │       │       │
 *     │       │       └── else
 *     │       │           ├── ctx.error(stream, DiagCode::E1006, got)
 *     │       │           ├── synchronizeTo(stream, ctx, COMMA, RPAREN)
 *     │       │           └── // The loop condition handles ')' and EOF naturally.
 *     │       │               // If we're at a comma, the comma handling below will consume it.
 *     │       │               // If we're at some other token, break to avoid infinite loop.
 *     │       │
 *     │       └── 4.2 Comma Separator Handling
 *     │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │               └── ctx.error(stream, DiagCode::E1009, ",", "argument list")
 *     │                   // Reports once for multiple consecutive commas between args
 *     │
 *     ├── 5. Closing Parenthesis Phase
 *     │   │
 *     │   └── if (stream.isAtEnd())
 *     │       ├── ctx.error(stream, DiagCode::E1005, ")", "argument list", "<EOF>")
 *     │       └── // Return with whatever args we have (missing ')')
 *     │   └── else
 *     │       └── stream.advance()  // Consume ')' (we know it's there from loop condition)
 *     │
 *     ├── 6. Build Result
 *     │   └── ctx.arena.makeBuilder<ExprPtr>()
 *     │       └── push_back each arg → build() → ArenaSpan<ExprPtr>
 *     │
 *     └── 7. Return
 *         └── return ArenaSpan<ExprPtr> (possibly empty)
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * ### Level 1: Missing '('
 * - E1004 reported, return empty
 * 
 * ### Level 2: Malformed Argument Expression
 * - parseExpr() reports its own error and returns nullptr
 * - synchronizeTo(COMMA, RPAREN) skips to next separator or terminator
 * 
 * ### Level 3: Missing Closing Parenthesis
 * - E1005 reported at EOF
 * - No recovery needed since we're already at EOF
 * 
 * ### Level 4: Consecutive Commas
 * - Leading commas before first argument → E1009 once
 * - Multiple commas between arguments → E1009 once per gap
 * 
 * ## Token Stream State
 * 
 * After this function completes:
 * - Position is AFTER ')' (normal case)
 * - Position is at EOF (if ')' was missing)
 * - Position is at recovery point (if errors occurred)
 * 
 * ## Examples
 * 
 * ```lucid
 * ()                                   → []
 * (1, 2, 3)                            → [1, 2, 3]
 * (a, b, c)                            → [a, b, c]
 * (1, 2, 3,)                           → [1, 2, 3] + E1009 (trailing comma)
 * (1,,,2)                              → [1, 2] + E1009 (gap)
 * (1, 2                               → [1, 2] + E1005 (missing ')')
 * ```
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ArenaSpan<ExprAST*> The parsed arguments (empty for no arguments)
 *
 * @note This function consumes ')'. The caller must NOT consume ')'.
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
    
    // ─── 3. Skip initial consecutive commas ──────────────────────────────
    // Ex: (,,,, arg1 ...)
    if (stream.consumeTrailing(TokenType::COMMA) > 0) {
        ctx.error(stream, DiagCode::E1009, ",", "argument list");
    }
    
    // ─── 4. Parse arguments until we hit ')' ─────────────────────────────
    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        // ─── 4.1 Parse an argument ────────────────────────────────────────
        ExprPtr arg = parseExpr(stream, ctx);
        if (arg) {
            args.push_back(arg);
        } else {
            // Error already reported by parseExpr, try to recover
            ctx.error(stream, DiagCode::E1006, stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            // The loop condition will handle ')' and EOF naturally.
            // If we're at a comma, the comma handling below will consume it.
            // If we're at some other token, break to avoid infinite loop.
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN) && !stream.isAtEnd()) {
                break;
            }
            continue;
        }
        
        // ─── 4.2 Comma Separator Handling ────────────────────────────────
        // Consume at least 1 comma and skip consecutive commas
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.error(stream, DiagCode::E1009, ",", "argument list");
        }
    }
    
    // ─── 5. Consume closing parenthesis ───────────────────────────────────
    // The loop condition guarantees we're either at ')' or at EOF.
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, ")", "argument list", "<EOF>");
    } else {
        // We must be at ')' (loop condition guaranteed !stream.check(RPAREN) is false)
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
 * ## Program Flow
 * 
 * ```
 * parseReturnList()
 *     │
 *     ├── 1. Check for Parenthesized Return List
 *     │   │
 *     │   └── if (stream.check(TokenType::LPAREN))
 *     │       │
 *     │       ├── 1.1 Opening Delimiter Phase
 *     │       │   └── stream.advance()  // Consume '('
 *     │       │
 *     │       ├── 1.2 Empty List Check: ()
 *     │       │   └── if (stream.check(TokenType::RPAREN))
 *     │       │       ├── stream.advance()  // Consume ')'
 *     │       │       └── return empty ArenaSpan<TypePtr>
 *     │       │
 *     │       ├── 1.3 Leading Comma Recovery
 *     │       │   └── if (stream.consumeTrailing(TokenType::COMMA) > 0)
 *     │       │       └── ctx.error(stream, DiagCode::E1009, ",", "return list")
 *     │       │
 *     │       ├── 1.4 Main Type Parsing Loop
 *     │       │   │
 *     │       │   └── while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN))
 *     │       │       │
 *     │       │       ├── 1.4.1 Parse Type
 *     │       │       │   │
 *     │       │       │   └── TypePtr type = parseType(stream, ctx)
 *     │       │       │       │
 *     │       │       │       ├── if (type != nullptr)
 *     │       │       │       │   └── returnTypes.push_back(type)
 *     │       │       │       │
 *     │       │       │       └── else
 *     │       │       │           ├── ctx.error(stream, DiagCode::E1003, "in return list", got)
 *     │       │       │           ├── synchronizeTo(stream, ctx, COMMA, RPAREN)
 *     │       │       │           └── // The loop condition handles ')' and EOF naturally.
 *     │       │       │               // If we're at a comma, the comma handling below will consume it.
 *     │       │       │               // If we're at some other token, break to avoid infinite loop.
 *     │       │       │
 *     │       │       └── 1.4.2 Comma Separator Handling
 *     │       │           └── if (stream.consumeTrailing(TokenType::COMMA) > 1)
 *     │       │               └── ctx.error(stream, DiagCode::E1009, ",", "return list")
 *     │       │                   // Reports once for multiple consecutive commas between types
 *     │       │
 *     │       └── 1.5 Closing Parenthesis Phase
 *     │           │
 *     │           └── if (stream.isAtEnd())
 *     │               ├── ctx.error(stream, DiagCode::E1005, ")", "return list", "<EOF>")
 *     │               └── // Return with whatever types we have (missing ')')
 *     │           └── else
 *     │               └── stream.advance()  // Consume ')' (we know it's there from loop condition)
 *     │
 *     └── 2. Single Return Type (no parentheses)
 *         │
 *         └── TypePtr type = parseType(stream, ctx)
 *             │
 *             ├── if (type != nullptr)
 *             │   └── returnTypes.push_back(type)
 *             │
 *             └── else
 *                 └── ctx.error(stream, DiagCode::E1003, "(return type) after '->'", got)
 *                     synchronizeToContext(stream, ctx)
 *     
 *     ├── 3. Build Result
 *     │   └── ctx.arena.makeBuilder<TypePtr>()
 *     │       └── push_back each type → build() → ArenaSpan<TypePtr>
 *     │
 *     └── 4. Return
 *         └── return ArenaSpan<TypePtr> (possibly empty)
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * ### Level 1: Parenthesized Return List
 * 
 * #### Level 1.1: Missing '(' (handled by caller)
 * - The caller (parseType) handles the case where '(' is expected but not found
 * 
 * #### Level 1.2: Malformed Type in Return List
 * - parseType() reports its own error and returns nullptr
 * - synchronizeTo(COMMA, RPAREN) skips to next separator or terminator
 * 
 * #### Level 1.3: Missing Closing Parenthesis
 * - E1005 reported at EOF
 * - No recovery needed since we're already at EOF
 * 
 * #### Level 1.4: Consecutive Commas
 * - Leading commas before first type → E1009 once
 * - Multiple commas between types → E1009 once per gap
 * 
 * ### Level 2: Single Return Type
 * - Missing type after `->` → E1003 reported
 * - synchronizeToContext() recovers
 * 
 * ## Token Stream State
 * 
 * After this function completes:
 * - Position is AFTER ')' (normal case for parenthesized list)
 * - Position is after the single type (normal case for single type)
 * - Position is at EOF (if ')' was missing)
 * - Position is at recovery point (if errors occurred)
 * 
 * ## Examples
 * 
 * ```lucid
 * -> int                    → [int]
 * -> (int, bool)            → [int, bool]
 * -> (string, int, float)   → [string, int, float]
 * -> ()                     → []
 * -> (int,,,bool)           → [int, bool] + E1009 (gap)
 * -> (int, )                → [int] + E1009 (trailing comma)
 * -> (int,                  → [int] + E1005 (missing ')')
 * -> (,int)                 → [int] + E1009 (leading comma)
 * -> (,,,)                  → [] + E1009 (only commas)
 * ```
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return ArenaSpan<TypeAST*> The parsed return types (empty for void)
 * 
 * @note This function consumes ')' if a parenthesized list is present.
 *       The caller must NOT consume ')'. For single return types, this
 *       function does NOT consume a closing parenthesis.
 */
ArenaSpan<TypeAST*> parseReturnList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseReturnList: parsing return types");
    
    std::vector<TypePtr> returnTypes;
    
    // ─── Check if we have a parenthesized return list ────────────────────
    if (stream.check(TokenType::LPAREN)) {
        stream.advance(); // Consume '('
        
        // ─── Check for empty parentheses (void): () ──────────────────────
        if (stream.check(TokenType::RPAREN)) {
            stream.advance(); // Consume ')'
            return ctx.arena.makeBuilder<TypePtr>().build();
        }
        
        // ─── Skip initial consecutive commas ──────────────────────────────
        // Ex: (,,,, int ...)
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, ",", "return list");
        }
        
        // ─── Parse types until we hit ')' ──────────────────────────────────
        while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
            // Parse a type
            TypePtr type = parseType(stream, ctx);
            if (type) {
                returnTypes.push_back(type);
            } else {
                // Error already reported by parseType, try to recover
                ctx.error(stream, DiagCode::E1003, "in return list", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                // The loop condition will handle ')' and EOF naturally.
                // If we're at a comma, the comma handling below will consume it.
                // If we're at some other token, break to avoid infinite loop.
                if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN) && !stream.isAtEnd()) {
                    break;
                }
                continue;
            }
            
            // ─── Comma Separator Handling ────────────────────────────────────
            // Consume at least 1 comma and skip consecutive commas
            if (stream.consumeTrailing(TokenType::COMMA) > 1) {
                ctx.error(stream, DiagCode::E1009, ",", "return list");
            }
        }
        
        // ─── Consume closing parenthesis ────────────────────────────────────
        // The loop condition guarantees we're either at ')' or at EOF.
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, ")", "return list", "<EOF>");
        } else {
            // We must be at ')' (loop condition guaranteed !stream.check(RPAREN) is false)
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

/**
 * @brief Parse a use declaration and import the module.
 * 
 * Grammar: `use path [as alias]`
 * 
 * ## Path Styles
 * 
 * ### Current: Dot-Separated (Module Style)
 * 
 * The parser currently supports **dot-separated** paths exclusively:
 * 
 * ```lucid
 * use std.io                    → path = "std.io", alias = std.io
 * use std.math as math          → path = "std.math", alias = math
 * use graphics.gl               → path = "graphics.gl", alias = graphics.gl
 * use mylib                     → path = "mylib", alias = mylib
 * ```
 * 
 * This style is inspired by Java, Python, and C# package systems where:
 * - Dots (`.`) act as namespace separators
 * - Paths are **always absolute** from the package root
 * - No relative navigation (`./`, `../`) is supported
 * - Resolution converts dots to filesystem paths internally
 *   (e.g., `std.io` → `std/io.lucid` or `std/io/__init__.luc`)
 * 
 * ### Future: Quoted Filesystem Paths
 * 
 * Support for **quoted filesystem paths** is planned as a future feature:
 * 
 * ```lucid
 * use "../folderA/moduleB" as b   // Relative filesystem path (future)
 * use "/usr/lib/mymod"            // Absolute filesystem path (future)
 * use "./local/helper" as helper  // Current directory (future)
 * ```
 * 
 * ## Why Quoted Paths?
 * 
 * Filesystem paths contain characters (`/`, `\`, `.`, `..`) that conflict with
 * the dot-separated module syntax. Wrapping them in quotes (`""`) makes them
 * unambiguous and allows:
 * 
 * - **Relative paths**: `"../folderA/moduleB"` - go up one directory
 * - **Absolute paths**: `"/usr/lib/mymod"` - from filesystem root
 * - **Path traversal**: `"./local/helper"` - current directory
 * - **Mixed usage**: `"../lib/math.vector"` - combine relative and dot style
 * 
 * ## Current Resolution Strategy
 * 
 * For dot-separated paths, the `ModuleResolver`:
 * 1. Splits the path on `.` (e.g., `"std.io"` → `["std", "io"]`)
 * 2. Replaces `.` with `/` for filesystem lookup
 * 3. Searches in the package root and additional search paths
 * 
 * ## Examples
 * 
 * ```lucid
 * // Current: Dot-separated (supported)
 * use std.io                      // Imports std/io.lucid from package root
 * use std.math as math            // Imports std/math.lucid as "math"
 * use graphics.gl                 // Imports graphics/gl.lucid
 * use mylib                       // Imports mylib.lucid from package root
 * 
 * // Future: Quoted filesystem paths (not yet implemented)
 * use "../folderA/moduleB" as b   // Imports ../folderA/moduleB.lucid
 * use "/usr/lib/mymod"            // Imports /usr/lib/mymod.lucid
 * use "./local/helper" as helper  // Imports ./local/helper.lucid
 * ```
 * 
 * ## Error Handling
 * 
 * - Missing `use` keyword → E1004
 * - Empty or malformed path → E1002, E1009
 * - Missing semicolon → E1005
 * - Module not found → E0004 (reported by ModuleResolver)
 * - Circular import → E0003
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return UseDeclAST* The parsed use declaration, or nullptr on error
 * 
 * @note This function consumes the terminating ';'. The caller must NOT
 *       consume ';'.
 * 
 * @note For dot-separated paths, `parseUsePath()` handles the parsing.
 *       The alias is optional and parsed here if the `as` keyword is present.
 * 
 * @note Filesystem paths with `"` are NOT yet supported. When implemented,
 *       they will be detected by `stream.check(TokenType::STRING_LITERAL)`
 *       and parsed as a quoted string containing the path.
 */
UseDeclAST* parseUseDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Expect 'use' keyword ────────────────────────────────────────
    if (!stream.match(TokenType::USE)) {
        ctx.error(stream, DiagCode::E1004, "use", "use declaration", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 2. Parse the path ──────────────────────────────────────────────
    // ─── 2.1 Current: Dot-separated path ───────────────────────────────
    // This is the primary path style for now.
    std::vector<InternedString> pathParts = parseUsePath(stream, ctx);
    if (pathParts.empty()) {
        // Error already reported by parseUsePath
        synchronizeTo(stream, ctx, TokenType::SEMICOLON);
        stream.consume(TokenType::SEMICOLON);
        return nullptr;
    }
    
    // Build the full path string (e.g., "std.io")
    std::string pathStr;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) pathStr += ".";
        pathStr += std::string(ctx.pool.lookup(pathParts[i]));
    }
    InternedString path = ctx.pool.intern(pathStr);
    
    // ─── 2.2 Future: Quoted filesystem path ────────────────────────────
    // TODO: Support quoted paths like use "../folder/module" as b
    // if (stream.check(TokenType::STRING_LITERAL)) {
    //     Token pathTok = stream.advance();
    //     InternedString path = ctx.pool.intern(pathTok.value);
    //     // Path is already a filesystem path - pass directly to resolver
    // }
    
    // ─── 3. Parse optional alias ────────────────────────────────────────
    InternedString alias = path;  // Default alias is the full path
    
    if (stream.match(TokenType::AS)) {
        // Expect an identifier for the alias
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "alias name after 'as'", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::SEMICOLON);
            stream.consume(TokenType::SEMICOLON);
            return nullptr;
        }
        
        Token aliasTok = stream.advance();
        alias = ctx.pool.intern(aliasTok.value);
    }
    
    // ─── 4. Consume semicolon ────────────────────────────────────────────
    if (!stream.match(TokenType::SEMICOLON)) {
        ctx.error(stream, DiagCode::E1005, ";", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::SEMICOLON);
        if (!stream.check(TokenType::SEMICOLON)) {
            // Can't recover - return nullptr
            return nullptr;
        }
        stream.advance(); // Consume semicolon
    }
    
    // ─── 5. Resolve and import the module ──────────────────────────────
    InternedString filePath = ctx.resolver ? ctx.resolver->resolveUsePath(path) : InternedString();
    
    if (ctx.resolver) {
        // ─── 5.1 Check for circular import ──────────────────────────────
        if (ctx.resolver->isParsing(filePath)) {
            ctx.error(stream, DiagCode::E0003, ctx.toString(path));
            // Return a use declaration even on error so we can continue
        } else {
            // ─── 5.2 Import the module ────────────────────────────────────
            // Always call parse() - it handles caching internally
            std::string source = ctx.resolver->readModuleSource(filePath);
            if (source.empty()) {
                // The file path exists but source is empty
                // parse() will handle this and return a module with errors
            }
            parser::parse(ctx.toString(filePath), source, ctx);
        }
    }
    
    // ─── 6. Create and return the UseDeclAST node ──────────────────────
    auto* useDecl = ctx.arena.make<UseDeclAST>();
    useDecl->loc = loc;
    useDecl->path = path;
    useDecl->alias = alias;
    
    LOG_PARSER("parseUseDecl: parsed use '", ctx.toString(path), 
               "' as '", ctx.toString(alias), "'");
    
    return useDecl;
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
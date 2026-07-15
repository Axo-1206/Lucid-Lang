/**
 * @file ParserLookahead.cpp
 * @brief Lookahead helper functions for the parser.
 * 
 * These functions peek ahead at the token stream without consuming tokens
 * to determine what syntactic construct we're looking at. They are used
 * by the parser to disambiguate between similar constructs.
 * 
 * ## Design Principles
 * 
 * 1. **Non-consuming**: None of these functions should advance the token stream
 * 2. **Fast**: They should only peek at a few tokens ahead
 * 3. **Conservative**: If uncertain, return false to let the parser try another branch
 * 
 * ## Usage
 * 
 * ```cpp
 * if (looksLikeFuncDecl(stream, ctx)) {
 *     return parseFuncDecl(stream, ctx);
 * } else {
 *     return parseVarDecl(stream, ctx);
 * }
 * ```
 */

#include "../Parser.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "core/Tokens.hpp"

namespace parser {

/**
 * @brief Check if the current position looks like a function declaration.
 * 
 * A function declaration looks like:
 *   `[let|const] IDENTIFIER [<generic>] (param) ...`
 * 
 * ## Detection Logic
 * 
 * 1. Check for `let` or `const` keyword
 * 2. Check for identifier (function name)
 * 3. Check for optional generic parameters `<...>`
 * 4. Check for `(` (parameter group start)
 * 
 * @param stream The token stream (non-consuming)
 * @param ctx The parsing context
 * @return true if it looks like a function declaration
 * 
 * ## Examples
 * 
 * ```lucid
 * const add (a int)(b int) -> int  → true
 * let x int = 42                    → false (no '(' after identifier)
 * struct Point { ... }              → false (starts with struct)
 * const process<T> (v T) -> T       → true (has generic params)
 * ```
 */
bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("looksLikeFuncDecl: checking");
    
    size_t savedPos = stream.getPos();
    bool result = false;
    
    // ─── 1. Check for `let` or `const` ──────────────────────────────
    if (!stream.checkAny(TokenType::LET, TokenType::CONST)) {
        LOG_PARSER_DETAIL("looksLikeFuncDecl: not let/const");
        stream.setPos(savedPos);
        return false;
    }
    stream.advance(); // Skip let/const (non-consuming after restore)
    
    // ─── 2. Check for identifier (function name) ─────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        LOG_PARSER_DETAIL("looksLikeFuncDecl: no identifier after let/const");
        stream.setPos(savedPos);
        return false;
    }
    stream.advance(); // Skip identifier
    
    // ─── 3. Check for optional generic parameters ──────────────────
    if (stream.check(TokenType::LESS)) {
        // Skip past generic parameters (simple scan)
        stream.advance(); // Skip '<'
        int angleDepth = 1;
        while (!stream.isAtEnd() && angleDepth > 0) {
            if (stream.check(TokenType::LESS)) angleDepth++;
            if (stream.check(TokenType::GREATER)) angleDepth--;
            stream.advance();
        }
        if (angleDepth > 0) {
            // Malformed generic params - not a function declaration
            stream.setPos(savedPos);
            return false;
        }
    }
    
    // ─── 4. Check for parameter group start ─────────────────────────
    if (stream.check(TokenType::LPAREN)) {
        result = true;
        LOG_PARSER_DETAIL("looksLikeFuncDecl: true - found parameter group");
    } else {
        LOG_PARSER_DETAIL("looksLikeFuncDecl: false - no '('");
    }
    
    stream.setPos(savedPos);
    return result;
}

/**
 * @brief Check if the current position looks like an anonymous function.
 * 
 * An anonymous function must start with one or more parameter groups `( ... )`
 * and end with a body `{ ... }`. The `->` return type is optional.
 * 
 * ## Simplified Detection
 * 
 * 1. Must start with `(`
 * 2. Must have at least one complete parameter group `( ... )`
 * 3. After all parameter groups, must have either `{` or `-> ... {`
 * 4. Rejects obvious non-functions using `is_operator()` helper
 * 
 * @param stream The token stream (non-consuming)
 * @param ctx The parsing context
 * @return true if it looks like an anonymous function
 * 
 * ## Examples
 * 
 * ```lucid
 * (a int) { ... }              → true
 * (a int) -> int { ... }       → true
 * (a int)(b int) { ... }       → true
 * ()() -> int { ... }          → true
 * +                           → false (operator)
 * (a int) + { ... }           → false (operator after param)
 * ```
 */
bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("looksLikeAnonFunc: checking");
    
    size_t savedPos = stream.getPos();
    bool result = false;
    
    // ─── 1. Must start with '(' ──────────────────────────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        stream.setPos(savedPos);
        return false;
    }
    
    // ─── 2. Parse at least one parameter group ──────────────────────────
    bool hasValidParamGroup = false;
    
    while (stream.check(TokenType::LPAREN)) {
        stream.advance(); // Skip '('
        
        // Find the matching ')'
        int parenDepth = 1;
        while (!stream.isAtEnd() && parenDepth > 0) {
            if (stream.check(TokenType::LPAREN)) parenDepth++;
            if (stream.check(TokenType::RPAREN)) parenDepth--;
            stream.advance();
        }
        
        if (parenDepth > 0) {
            stream.setPos(savedPos);
            return false;
        }
        
        hasValidParamGroup = true;
        
        // ─── 2a. After closing ')', check for operators ────────────────────
        // Use the is_operator helper from Tokens.hpp
        // This catches +, -, *, /, **, &, |, ^, <<, >>, ==, !=, <, <=, >, >=,
        // =, +=, -=, *=, /=, %=, **=, +>, |>, .., ..<
        if (stream.isAtEnd()) {
            stream.setPos(savedPos);
            return false;
        }
        
        if (stream.peek().is_operator()) {
            LOG_PARSER_DETAIL("looksLikeAnonFunc: false - operator after )");
            stream.setPos(savedPos);
            return false;
        }
    }
    
    if (!hasValidParamGroup) {
        stream.setPos(savedPos);
        return false;
    }
    
    // ─── 3. Skip optional `->` and return type ──────────────────────────
    if (stream.check(TokenType::ARROW)) {
        stream.advance(); // Skip '->'
        
        // ─── 3a. Check for immediate operator after '->' ───────────────────
        if (stream.isAtEnd()) {
            stream.setPos(savedPos);
            return false;
        }
        
        // Use is_operator helper for the check
        if (stream.peek().is_operator()) {
            LOG_PARSER_DETAIL("looksLikeAnonFunc: false - operator after ->");
            stream.setPos(savedPos);
            return false;
        }
        
        // Skip the return type (whatever it is, until we find '{')
        while (!stream.isAtEnd() && !stream.check(TokenType::LBRACE)) {
            // If we hit an operator in the return type, reject
            if (stream.peek().is_operator() && !stream.check(TokenType::ARROW)) {
                LOG_PARSER_DETAIL("looksLikeAnonFunc: false - operator in return type");
                stream.setPos(savedPos);
                return false;
            }
            
            // If we hit another `->`, it's part of the return type
            if (stream.check(TokenType::ARROW)) {
                stream.advance();
                // Check for operator after nested '->'
                if (stream.isAtEnd()) {
                    stream.setPos(savedPos);
                    return false;
                }
                if (stream.peek().is_operator()) {
                    LOG_PARSER_DETAIL("looksLikeAnonFunc: false - operator after nested ->");
                    stream.setPos(savedPos);
                    return false;
                }
                continue;
            }
            
            // If we hit `(`, skip it and its matching `)`
            if (stream.check(TokenType::LPAREN)) {
                int parenDepth = 1;
                stream.advance();
                while (!stream.isAtEnd() && parenDepth > 0) {
                    if (stream.check(TokenType::LPAREN)) parenDepth++;
                    if (stream.check(TokenType::RPAREN)) parenDepth--;
                    stream.advance();
                }
                continue;
            }
            
            // If we hit `<`, skip it and its matching `>`
            if (stream.check(TokenType::LESS)) {
                int angleDepth = 1;
                stream.advance();
                while (!stream.isAtEnd() && angleDepth > 0) {
                    if (stream.check(TokenType::LESS)) angleDepth++;
                    if (stream.check(TokenType::GREATER)) angleDepth--;
                    stream.advance();
                }
                continue;
            }
            
            // If we hit `{`, break (we found the body)
            if (stream.check(TokenType::LBRACE)) {
                break;
            }
            
            // For simple tokens, just consume them
            stream.advance();
        }
    }
    
    // ─── 4. Must end with '{' (body) ────────────────────────────────────
    if (stream.check(TokenType::LBRACE)) {
        result = true;
        LOG_PARSER_DETAIL("looksLikeAnonFunc: true - found {");
    } else {
        LOG_PARSER_DETAIL("looksLikeAnonFunc: false - no {");
    }
    
    stream.setPos(savedPos);
    return result;
}

/**
 * @brief Check if the current position looks like a multi-assignment.
 * 
 * Multi-assignments have the form:
 *   `IDENTIFIER { ',' IDENTIFIER } '=' expr`
 * 
 * ## Detection Logic
 * 
 * 1. Check for an identifier
 * 2. Check for a comma (multiple targets)
 * 3. Check for `=`
 * 
 * @param stream The token stream (non-consuming)
 * @param ctx The parsing context
 * @return true if it looks like a multi-assignment
 * 
 * ## Examples
 * 
 * ```lucid
 * x, y = 1, 2            → true
 * x, y, z = getThree()   → true
 * x = 5                  → false (single assignment)
 * let x, y = 1, 2        → false (declaration, not assignment)
 * ```
 */
bool looksLikeMultiAssignStart(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("looksLikeMultiAssignStart: checking");
    
    size_t savedPos = stream.getPos();
    bool result = false;
    
    // ─── 1. Check for first identifier ──────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.advance(); // Skip first identifier
    
    // ─── 2. Check for comma (multiple targets) ──────────────────────────
    if (!stream.check(TokenType::COMMA)) {
        // No comma → single assignment, not multi
        stream.setPos(savedPos);
        return false;
    }
    stream.advance(); // Skip comma
    
    // ─── 3. Check for second identifier ─────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.advance(); // Skip second identifier
    
    // ─── 4. Check for more commas (optional) ────────────────────────────
    while (stream.check(TokenType::COMMA)) {
        stream.advance(); // Skip comma
        if (!stream.check(TokenType::IDENTIFIER)) {
            stream.setPos(savedPos);
            return false;
        }
        stream.advance(); // Skip identifier
    }
    
    // ─── 5. Check for assignment operator ───────────────────────────────
    if (stream.check(TokenType::ASSIGN)) {
        result = true;
        LOG_PARSER_DETAIL("looksLikeMultiAssignStart: true - found multi-assignment");
    } else {
        LOG_PARSER_DETAIL("looksLikeMultiAssignStart: false - no '='");
    }
    
    stream.setPos(savedPos);
    return result;
}

/**
 * @brief Lookahead: does the statement starting at the current position
 *        look like a multi-assign target list — `lvalue { ',' lvalue } '='`?
 *
 * This is the assignment-side counterpart of `looksLikeMultiAssignStart`
 * (which gates `let`/`const` toward `parseMultiVarDecl`). Without this
 * check, a statement like `value, ok = parseInt("42");` never starts with
 * LET/CONST, so parseStmt's dispatcher would fall through to
 * parseExprStmt(), which parses a single expression and leaves the `,`
 * unconsumed — producing a spurious parse error instead of a
 * MultiAssignStmtAST.
 *
 * The scan is purely read-only: it walks the raw token buffer
 * (stream.getTokens() / stream.getPos()) without advancing the stream,
 * exactly like harvestDocComment() does. It bails out to `false` the
 * moment the shape stops matching an lvalue-list-then-'=', so ordinary
 * expression statements — including plain single-target `x = 5`, which
 * is handled entirely inside the Pratt parser via parseInfixAssign — are
 * left completely untouched and keep going through parseExprStmt().
 *
 * A single comma-free target (`x = 5`) deliberately does NOT match here
 * (see the `sawComma` gate below): parseMultiAssignStmt's own loop would
 * happily accept it too, but routing every ordinary assignment through
 * this path would bypass the Pratt parser's right-associative chaining
 * (`a = b = c`) for no benefit. Only genuinely comma-separated target
 * lists are rerouted.
 *
 * ## Grammar covered per target
 * ```
 * target  = IDENTIFIER [ ':' IDENTIFIER ]     (* plain var, or module access *)
 *           { '.' IDENTIFIER | '[' ... ']' }  (* field/index trailers *)
 * ```
 *
 * ## Examples
 * ```lucid
 * value, ok = parseInt("42");                → true
 * player.stats.hp, player.stats.mp = a, b;    → true  (single shared RHS in practice)
 * arr[i], arr[j] = arr[j], arr[i];            → true
 * x = 5;                                      → false (no comma — ordinary assign)
 * x + y;                                      → false (not an lvalue list at all)
 * f(a, b);                                    → false (never reaches '=')
 * ```
 *
 * @param stream The token stream for the current file
 * @param ctx The parsing context (unused today, kept for symmetry with
 *            looksLikeMultiAssignStart and to allow future diagnostics)
 * @return true if the upcoming tokens form a comma-separated lvalue list
 *         terminated by a bare '='; false otherwise. Never advances the
 *         stream.
 */
bool looksLikeMultiAssignTargets(TokenStream& stream, ParserContext& ctx) {
    (void)ctx;

    const auto& tokens = stream.getTokens();
    size_t i = stream.getPos();
    const size_t n = tokens.size();

    bool sawComma = false;

    while (true) {
        // Each target must start with an identifier.
        if (i >= n || tokens[i].type != TokenType::IDENTIFIER) {
            return false;
        }
        ++i;

        // Optional module access base: IDENTIFIER ':' IDENTIFIER
        // (module:member is not assignable, but that's a semantic check,
        // not a parse-time shape check — parseLvalue already tolerates it.)
        if (i + 1 < n && tokens[i].type == TokenType::COLON &&
            tokens[i + 1].type == TokenType::IDENTIFIER) {
            i += 2;
        }

        // Trailers: '.' IDENTIFIER, or '[' expr ']' (bracket-depth aware,
        // since the index expression may itself contain nested brackets).
        while (i < n) {
            if (tokens[i].type == TokenType::DOT) {
                if (i + 1 >= n || tokens[i + 1].type != TokenType::IDENTIFIER) {
                    return false; // malformed — let the real parser report it
                }
                i += 2;
                continue;
            }

            if (tokens[i].type == TokenType::LBRACKET) {
                int depth = 1;
                ++i;
                while (i < n && depth > 0) {
                    if (tokens[i].type == TokenType::LBRACKET) {
                        ++depth;
                    } else if (tokens[i].type == TokenType::RBRACKET) {
                        --depth;
                    }
                    ++i;
                }
                if (depth != 0) {
                    return false; // unbalanced — bail, don't guess
                }
                continue;
            }

            break;
        }

        // Another target follows a comma; otherwise the list is done.
        if (i < n && tokens[i].type == TokenType::COMMA) {
            sawComma = true;
            ++i;
            continue;
        }

        break;
    }

    return sawComma && i < n && tokens[i].type == TokenType::ASSIGN;
}

/**
 * @brief Check if the current position looks like a struct literal.
 * 
 * A struct literal looks like:
 *   - `TypeName { ... }`
 *   - `TypeName<T> { ... }`
 *   - `TypeName<T, U> { ... }`
 * 
 * ## Detection Logic
 * 
 * 1. Check for an identifier (the type name)
 * 2. Check for optional generic arguments `<T, ...>`
 * 3. Check for `{` (body start)
 * 
 * ## Examples
 * 
 * ```lucid
 * Point { x = 1, y = 2 }       → true
 * Box<int> { value = 42 }      → true
 * Vec2 { x = 1, y = 2 }        → true
 * add(5)                       → false (function call)
 * Point                        → false (no {)
 * ```
 * 
 * @param stream The token stream (non-consuming)
 * @param ctx The parsing context
 * @return true if it looks like a struct literal
 */
bool looksLikeStructLiteral(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("looksLikeStructLiteral: checking");
    
    size_t savedPos = stream.getPos();
    bool result = false;
    
    // ─── 1. Check for identifier (type name) ────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.advance(); // Skip identifier
    
    // ─── 2. Check for optional generic arguments ─────────────────────────
    if (stream.check(TokenType::LESS)) {
        stream.advance(); // Skip '<'
        int angleDepth = 1;
        
        // Skip through generic arguments until we find matching '>'
        // or we hit a token that can't be in generic args
        while (!stream.isAtEnd() && angleDepth > 0) {
            if (stream.check(TokenType::LESS)) {
                angleDepth++;
            } else if (stream.check(TokenType::GREATER)) {
                angleDepth--;
                if (angleDepth == 0) {
                    stream.advance(); // Skip '>'
                    break;
                }
            }
            stream.advance();
        }
        
        if (angleDepth > 0) {
            // Unterminated generic args - not a struct literal
            stream.setPos(savedPos);
            return false;
        }
        
        // ─── 3. Check for '{' after generic arguments ────────────────────
        if (stream.check(TokenType::LBRACE)) {
            result = true;
            LOG_PARSER_DETAIL("looksLikeStructLiteral: true - found generic args and {");
        } else {
            LOG_PARSER_DETAIL("looksLikeStructLiteral: false - no { after generic args");
        }
        
        stream.setPos(savedPos);
        return result;
    }
    
    // ─── 4. Check for '{' after identifier (no generic args) ────────────
    if (stream.check(TokenType::LBRACE)) {
        result = true;
        LOG_PARSER_DETAIL("looksLikeStructLiteral: true - found {");
    } else {
        LOG_PARSER_DETAIL("looksLikeStructLiteral: false - no {");
    }
    
    stream.setPos(savedPos);
    return result;
}

} // namespace parser
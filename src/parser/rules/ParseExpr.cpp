/**
 * @file ParseExpr.cpp
 * @brief Implementation of expression parsers.
 * 
 * This file implements all expression parsing functions:
 * - Pratt parser core: parseExpr(), parsePrattExpr()
 * - Prefix expressions: parsePrefixExpr(), parsePrimaryExpr()
 * - Postfix expressions: parsePostfixExpr()
 * - Calls and indexing: parseCallExpr(), parseIndexExpr(), parseSliceExpr()
 * - Pipeline and composition: parsePipelineExpr(), parseComposeExpr()
 * - Precedence helpers: infixPrec(), tokenToBinaryOp(), etc.
 * - Infix dispatch: parseInfixAssign(), parseInfixIs(), parseInfixNullCoalesce(), parseInfixBinary()
 * 
 * ## Pratt Parser Overview
 * 
 * The expression parser uses a Pratt parser (top-down operator precedence):
 * 
 * ```
 * parseExpr()
 *   └── parsePrattExpr(minPrec)
 *         ├── parsePrefixExpr()
 *         │     └── parsePrimaryExpr()
 *         │           ├── LiteralExprAST
 *         │           ├── IdentifierExprAST
 *         │           ├── ArrayLiteralExprAST
 *         │           ├── StructLiteralExprAST
 *         │           ├── AnonFuncExprAST
 *         │           ├── IfExprAST
 *         │           └── IntrinsicCallExprAST
 *         │
 *         └── while (precedence >= minPrec)
 *               └── parseInfix...() / parsePostfixExpr()
 *                     ├── parseInfixAssign()
 *                     ├── parseInfixIs()
 *                     ├── parseInfixNullCoalesce()
 *                     ├── parseInfixBinary()
 *                     └── parsePostfixExpr()
 *                           ├── parseCallExpr()
 *                           ├── parseIndexExpr()
 *                           ├── parseSliceExpr()
 *                           └── parsePipelineExpr()
 * ```
 * 
 * ## Precedence Levels
 * 
 * | Level | Operators | Associativity |
 * |-------|-----------|---------------|
 * | 8     | `+>` (composition) | left |
 * | 7     | unary `-` `not` `~` | right |
 * | 6     | `*` `/` `%` `**` | left |
 * | 5     | `+` `-` | left |
 * | 4     | `..` `..<` (range) | left |
 * | 3     | `==` `!=` `<` `<=` `>` `>=` | left |
 * | 2     | `and` | left |
 * | 1     | `or` | left |
 * | 0     | `|>` (pipeline) | left |
 * 
 * @see Parser.hpp for function declarations
 * @see Grammar.md for language grammar
 * 
 *  ## File Structure Summary
 * 
 * | Section | Functions |
 * |---------|-----------|
 * | Core Pratt Parser   | parseExpr, parsePrattExpr, parsePrefixExpr, parsePrimaryExpr |
 * | Postfix Expressions | parsePostfixExpr |
 * | Call & Index        | parseCallExpr, parseIntrinsicCallExpr, parseIndexExpr, parseSliceExpr |
 * | Pipeline & Composition | parsePipelineExpr, parseComposeExpr, parsePipelineStep, parseComposeOperand |
 * | Precedence Helpers  | infixPrec, tokenToBinaryOp, tokenToAssignOp, isAssignOp |
 * | Infix Dispatch      | parseInfixAssign, parseInfixIs, parseInfixNullCoalesce, parseInfixBinary |
 * 
 */

#include "../Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

namespace parser {

// =============================================================================
// Core Pratt Parser
// =============================================================================

/**
 * @brief Parse an expression.
 * 
 * This is the main entry point for expression parsing.
 * It calls the Pratt parser with the lowest precedence level.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ExprAST* The parsed expression, or nullptr on error
 */
ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseExpr: parsing expression");
    
    // If we're at EOF, there's nothing to parse
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1006, stream.peekValue());
        return nullptr;
    }
    
    // Parse with the lowest precedence level (0)
    return parsePrattExpr(stream, ctx, 0);
}


/**
 * @brief Parse an expression using the Pratt parser.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param minPrec The minimum precedence level to parse
 * @return ExprAST* The parsed expression, or nullptr on error
 */
ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx, int minPrec) {
    LOG_PARSER_DETAIL("parsePrattExpr: parsing expression with min precedence: ", minPrec);
    
    // ─── 1. Parse the prefix expression ──────────────────────────────────
    ExprPtr lhs = parsePrefixExpr(stream, ctx);
    if (!lhs) {
        return nullptr;
    }
    
    // ─── 2. Parse infix/postfix expressions while precedence allows ────
    while (!stream.isAtEnd()) {
        TokenType current = stream.peekType();
        int prec = infixPrec(current);
        
        // If the operator has lower precedence than our minimum, stop
        if (prec < minPrec) {
            break;
        }
        
        // ─── Handle composition operator (+>) ────────────────────────────
        if (current == TokenType::COMPOSE) {
            // Composition has higher precedence than assignment and null coalesce
            // but lower than function calls
            lhs = parseComposeExpr(stream, ctx, lhs);
            if (!lhs) {
                return nullptr;
            }
            continue;
        }
        
        // Check for assignment operators (right-associative)
        if (current == TokenType::ASSIGN) {
            // For assignment, we need to parse the RHS with lower precedence
            // to handle right-associativity: a = b = c → a = (b = c)
            stream.advance(); // Consume the assignment operator
            lhs = parseInfixAssign(stream, ctx, lhs);
            if (!lhs) {
                return nullptr;
            }
            continue;
        }
        
        // Check for `??` operator (null coalesce)
        if (current == TokenType::QUESTION_QUESTION) {
            stream.advance(); // Consume '??'
            lhs = parseInfixNullCoalesce(stream, ctx, lhs);
            if (!lhs) {
                return nullptr;
            }
            continue;
        }
        
        // Handle binary operators
        if (prec >= 0) {
            stream.advance(); // Consume the operator
            lhs = parseInfixBinary(stream, ctx, lhs, current, prec);
            if (!lhs) {
                return nullptr;
            }
            continue;
        }
        
        // Handle postfix expressions (call, index, slice, pipeline)
        if (current == TokenType::LPAREN ||
            current == TokenType::LBRACKET ||
            current == TokenType::PIPELINE) {
            lhs = parsePostfixExpr(stream, ctx, lhs);
            if (!lhs) {
                return nullptr;
            }
            continue;
        }
        
        // If we get here, we have an unexpected token
        break;
    }
    
    return lhs;
}

/**
 * @brief Parse a prefix expression.
 * 
 * Prefix expressions include:
 * - Unary operators: `-`, `not`, `~`
 * - Primary expressions
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ExprAST* The parsed prefix expression, or nullptr on error
 */
ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parsePrefixExpr: parsing prefix expression");
    
    SourceLocation loc = stream.currentLoc();
    TokenType current = stream.peekType();
    
    // ─── 1. Handle unary operators ──────────────────────────────────────
    if (current == TokenType::MINUS ||
        current == TokenType::NOT ||
        current == TokenType::BIT_NOT) {
        
        Token opTok = stream.advance();
        UnaryOp op;
        switch (opTok.type) {
            case TokenType::MINUS:   op = UnaryOp::Neg; break;
            case TokenType::NOT:     op = UnaryOp::Not; break;
            case TokenType::BIT_NOT: op = UnaryOp::BitNot; break;
            default:
                ctx.error(stream, DiagCode::E1007, "unary operator( '-', '~', 'not' )", stream.peekValue());
                return nullptr;
        }
        
        ExprPtr operand = parsePrattExpr(stream, ctx, infixPrec(current) + 1);
        if (!operand) {
            return nullptr;
        }
        
        auto* unary = ctx.arena.make<UnaryExprAST>();
        unary->op = op;
        unary->operand = operand;
        unary->loc = loc;
        return unary;
    }
    
    // ─── 2. Parse primary expression ────────────────────────────────────
    return parsePrimaryExpr(stream, ctx);
}

/**
 * @brief Parse a primary expression.
 * 
 * Primary expressions are the atoms of the language:
 * - Literals: `42`, `3.14`, `"hello"`, `true`, `false`, `nil`, `err`
 * - Identifiers: `x`, `add`, `Vec2`
 * - Parenthesized expressions: `(expr)`
 * - Anonymous functions: `(a int) -> int { ... }`
 * - Array literals: `[1, 2, 3]`
 * - Struct literals: `Point { x = 1, y = 2 }`
 * - If expressions: `if cond ?? expr else expr`
 * - Intrinsic calls: `#sizeof(T)`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ExprAST* The parsed primary expression, or nullptr on error
 */
ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parsePrimaryExpr: parsing primary expression");
    
    SourceLocation loc = stream.currentLoc();
    TokenType current = stream.peekType();
    
    // ─── 1. Literals ─────────────────────────────────────────────────────
    if (is_literal(current)) {
        return parseLiteralExpr(stream, ctx);
    }
    
    // ─── 2. Intrinsic call: #sizeof(T) ──────────────────────────────────
    if (current == TokenType::HASH) {
        return parseIntrinsicCallExpr(stream, ctx);
    }
    
    // ─── 3. Array literal: [1, 2, 3] ────────────────────────────────────
    if (current == TokenType::LBRACKET) {
        // Need to distinguish between array literal and array type
        // Array literal: `[1, 2, 3]` - contains expressions
        // Array type: `[*]int` - contains type specifiers
        // For now, try array literal first
        return parseArrayLiteralExpr(stream, ctx);
    }
    
    // ─── 4. If expression: if cond ?? expr else expr ────────────────────
    if (current == TokenType::IF) {
        return parseIfExpr(stream, ctx);
    }
    
    // ─── 5. Parenthesized expression: (expr) ────────────────────────────
    if (current == TokenType::LPAREN) {
        stream.advance(); // Consume '('
        
        // Check if it's an empty tuple or group
        if (stream.check(TokenType::RPAREN)) {
            stream.advance(); // Consume ')'
            ctx.error(stream, DiagCode::E1106, stream.peekValue());
            return nullptr;
        }
        
        ExprPtr expr = parseExpr(stream, ctx);
        if (!expr) {
            return nullptr;
        }
        
        if (!stream.check(TokenType::RPAREN)) {
            ctx.error(stream, DiagCode::E1005, ")", "parenthesized expression", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::RPAREN);
            return expr;
        }
        stream.advance(); // Consume ')'
        
        return expr;
    }
    
    // ─── 6. Anonymous function: (a int) -> int { ... } ──────────────────
    if (looksLikeAnonFunc(stream, ctx)) {
        return parseAnonFuncExpr(stream, ctx);
    }
    
    // ─── 7. Struct literal: Point { x = 1, y = 2 } ─────────────────────
    if (looksLikeStructLiteral(stream, ctx)) {
        // Parse the identifier
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "struct type name", stream.peekValue());
            return nullptr;
        }
        Token nameTok = stream.advance();
        InternedString typeName = ctx.pool.intern(nameTok.value);
        
        // Parse generic arguments if present
        ArenaSpan<TypePtr> genericArgs;
        if (stream.check(TokenType::LESS)) {
            genericArgs = parseGenericArgs(stream, ctx);
        }
        
        // Parse the struct body
        if (!stream.check(TokenType::LBRACE)) {
            ctx.error(stream, DiagCode::E1004, "{", "struct literal", stream.peekValue());
            return nullptr;
        }
        return parseStructLiteralExpr(stream, ctx, typeName, genericArgs);
    }
    
    // ─── 9. Unknown primary expression ──────────────────────────────────
    ctx.error(stream, DiagCode::E1006, stream.peekValue());
    synchronize(stream, ctx);
    return nullptr;
}

// =============================================================================
// Postfix Expressions
// =============================================================================

/**
 * @brief Parse a postfix expression.
 * 
 * Postfix expressions are applied after the left-hand side:
 * - Function calls: `f()`, `f(1, 2, 3)`
 * - Indexing: `arr[0]`
 * - Slicing: `arr[1..3]`, `arr[..<2]`
 * - Pipeline: `expr |> step`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param lhs The left-hand side expression
 * @return ExprAST* The parsed postfix expression, or nullptr on error
 */
ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lhs) {
    LOG_PARSER_DETAIL("parsePostfixExpr: parsing postfix expression");
    
    if (!lhs) {
        return nullptr;
    }
    
    TokenType current = stream.peekType();
    
    // ─── 1. Function call: f() ───────────────────────────────────────────
    if (current == TokenType::LPAREN) {
        // Generic arguments should have been parsed already in parsePrimaryExpr
        // For calls like `Buffer<int>(capacity)`, the generic args are parsed
        // before the '(' in parsePrimaryExpr when it sees `Buffer<int>`
        // 
        // However, we also need to handle the case where the callee is already
        // an IdentifierExprAST with genericArgs set (from parseFuncRef)
        //
        // We also need to handle the case where the callee is a FieldAccessExprAST
        // with genericArgs set (from parseFuncRef on a field access)
        ArenaSpan<TypePtr> genericArgs;
        
        // Check if the callee already has generic arguments (from parsePrimaryExpr)
        if (lhs->isa<IdentifierExprAST>()) {
            auto* idExpr = lhs->as<IdentifierExprAST>();
            if (idExpr->genericArgs.size() > 0) {
                genericArgs = idExpr->genericArgs;
                // Clear the genericArgs from the identifier to avoid double storage
                // We'll move them to the call expression
                // Note: ArenaSpan doesn't have a clear, so we'll just use them and
                // the identifier will keep them (they'll be ignored later)
            }
        } else if (lhs->isa<FieldAccessExprAST>()) {
            auto* fieldAccess = lhs->as<FieldAccessExprAST>();
            if (fieldAccess->genericArgs.size() > 0) {
                genericArgs = fieldAccess->genericArgs;
            }
        } else if (lhs->isa<ModuleAccessExprAST>()) {
            auto* moduleAccess = lhs->as<ModuleAccessExprAST>();
            if (moduleAccess->genericArgs.size() > 0) {
                genericArgs = moduleAccess->genericArgs;
            }
        }
        
        // Parse the call with the collected generic arguments
        return parseCallExpr(stream, ctx, lhs, genericArgs);
    }
    
    // ─── 2. Index or slice: arr[0] or arr[1..3] ─────────────────────────
    if (current == TokenType::LBRACKET) {
        // Need to look ahead to determine if it's an index or slice
        size_t savedPos = stream.getPos();
        stream.advance(); // Consume '['
        
        // Check for slice: expr [ start .. end ]
        bool isSlice = false;
        if (!stream.isAtEnd()) {
            // Skip expressions until we see '..' or '..<'
            int parenDepth = 0;
            int bracketDepth = 0;
            while (!stream.isAtEnd()) {
                if (stream.check(TokenType::LPAREN)) parenDepth++;
                if (stream.check(TokenType::RPAREN)) parenDepth--;
                if (stream.check(TokenType::LBRACKET)) bracketDepth++;
                if (stream.check(TokenType::RBRACKET)) bracketDepth--;
                if (parenDepth == 0 && bracketDepth == 0) {
                    if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
                        isSlice = true;
                        break;
                    }
                    if (stream.check(TokenType::RBRACKET)) {
                        break;
                    }
                }
                stream.advance();
            }
        }
        
        stream.setPos(savedPos);
        
        if (isSlice) {
            return parseSliceExpr(stream, ctx, lhs);
        } else {
            return parseIndexExpr(stream, ctx, lhs);
        }
    }
    
    // ─── 3. Pipeline: expr |> step ──────────────────────────────────────
    if (current == TokenType::PIPELINE) {
        return parsePipelineExpr(stream, ctx, lhs);
    }
    
    // No more postfix operators
    return lhs;
}
// =============================================================================
// Call & Index
// =============================================================================

/**
 * @brief Parse a function call expression.
 * 
 * Grammar: `'(' [ arg_list ] ')' [ '!' ]`
 * 
 * ## Examples
 * 
 * ```lucid
 * f(1, 2, 3)                              → regular call
 * Buffer<int>(capacity)                   → generic call
 * math:sqrt(x)                            → module function call
 * x |> map<int, string>(stringFromInt)!   → argument pack call
 * ```
 * 
 * ## Generic Arguments
 * 
 * Generic arguments are parsed BEFORE the call parentheses in parsePrimaryExpr:
 * - `Buffer<int>` is parsed as an IdentifierExprAST with genericArgs = [Int]
 * - The call `(capacity)` is then parsed as a CallExprAST with the genericArgs
 *   from the callee
 * 
 * ## Generic Call Resolution
 * 
 * For a call like `Buffer<int>(capacity)`:
 * 1. parsePrimaryExpr sees `Buffer` and `<int>`
 * 2. It creates an IdentifierExprAST with genericArgs = [Int]
 * 3. parsePostfixExpr sees `(` and calls parseCallExpr
 * 4. parseCallExpr extracts the genericArgs from the callee and stores them
 *    in the CallExprAST
 * 
 * ## Argument Pack (!)
 * 
 * `fn(args)!` is not a function call – `!` marks an intentionally incomplete
 * argument list. The upstream value is injected as the **first** argument when
 * `|>` fires. The semantic pass verifies that `hasArgPack` is only true when
 * the call is inside a pipeline step.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param callee The callee expression
 * @param genericArgs The generic arguments (if any)
 * @return CallExprAST* The parsed call expression, or nullptr on error
 */
CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx, 
                            ExprPtr callee, ArenaSpan<TypeAST*> genericArgs) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseCallExpr: parsing call expression");
    
    if (!callee) {
        ctx.error(stream, DiagCode::E1006, "function to call", stream.peekValue());
        return nullptr;
    }
    
    // ─── 1. Expect '(' ────────────────────────────────────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        ctx.error(stream, DiagCode::E1004, "(", "function call", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    // We don't consume '(' here - parseArgList does that
    
    // ─── 2. Parse arguments ──────────────────────────────────────────────
    // parseArgList consumes '(' and ')' and returns the arguments
    // It handles empty argument lists: f()
    ArenaSpan<ExprPtr> args = parseArgList(stream, ctx);
    
    // ─── 3. Check for argument pack (!) ──────────────────────────────────
    bool hasArgPack = stream.match(TokenType::BANG);
    
    // ─── 4. Build the call expression node ───────────────────────────────
    auto* call = ctx.arena.make<CallExprAST>();
    call->loc = loc;
    call->callee = callee;
    call->genericArgs = genericArgs;
    call->args = args;
    call->hasArgPack = hasArgPack;
    
    LOG_PARSER_DETAIL("parseCallExpr: parsed call expression with ", 
                      args.size(), " arguments",
                      genericArgs.size() > 0 ? " and " + std::to_string(genericArgs.size()) + " generic args" : "",
                      hasArgPack ? " (with argument pack)" : "");
    
    return call;
}

/**
 * @brief Parse an intrinsic call expression.
 * 
 * Grammar: `'#' IDENTIFIER '(' [ arg_list ] ')'`
 * 
 * ## Examples
 * 
 * ```lucid
 * #sizeof(T)         → intrinsicName = "sizeof", args = [T] (type argument)
 * #toRef(ptr)        → intrinsicName = "toRef", args = [ptr]
 * #toPtr(ref)        → intrinsicName = "toPtr", args = [ref]
 * #ptrOffset(ptr, 4) → intrinsicName = "ptrOffset", args = [ptr, 4]
 * #ptrDiff(p1, p2)   → intrinsicName = "ptrDiff", args = [p1, p2]
 * #memcpy(dst, src, n) → intrinsicName = "memcpy", args = [dst, src, n]
 * #sqrt(x)           → intrinsicName = "sqrt", args = [x]
 * ```
 * 
 * ## Intrinsic Categories
 * 
 * 1. **Pointer Operations**: `#toRef`, `#toPtr`, `#ptrOffset`, `#ptrDiff`
 * 2. **Memory Operations**: `#memcpy`, `#memset`, `#memmove`
 * 3. **Math Operations**: `#sqrt`, `#sin`, `#cos`, `#tan`, `#abs`, `#pow`, etc.
 * 4. **Type Operations**: `#sizeof`, `#alignof`, `#offsetof`
 * 5. **Debug/Assert**: `#assert`, `#panic`, `#print`
 * 6. **Atomic Operations**: `#atomicLoad`, `#atomicStore`, `#atomicCAS`, etc.
 * 
 * ## Argument Types
 * 
 * Intrinsic arguments can be:
 * - Expressions (value arguments)
 * - Type names (type arguments like `T` in `#sizeof(T)`)
 * 
 * The semantic pass validates the argument count and types for each intrinsic.
 * Type arguments are parsed as regular expressions (identifiers) and the
 * semantic pass will distinguish them based on the intrinsic being called.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return IntrinsicCallExprAST* The parsed intrinsic call, or nullptr on error
 */
IntrinsicCallExprAST* parseIntrinsicCallExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseIntrinsicCallExpr: parsing intrinsic call");
    
    // ─── 1. Parse '#' token ──────────────────────────────────────────────
    if (!stream.check(TokenType::HASH)) {
        ctx.error(stream, DiagCode::E1001, "#", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '#'
    
    // ─── 2. Parse intrinsic name ──────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "intrinsic name", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString intrinsicName = ctx.pool.intern(nameTok.value);
    
    // ─── 3. Parse arguments using parseArgList ───────────────────────────
    // parseArgList consumes '(' and ')' and returns the arguments as expressions
    // For type arguments like #sizeof(T), T is parsed as an IdentifierExprAST
    // The semantic pass will distinguish type arguments from value arguments
    ArenaSpan<ExprPtr> args = parseArgList(stream, ctx);
    
    // ─── 4. Build the AST node ───────────────────────────────────────────
    auto* intrinsic = ctx.arena.make<IntrinsicCallExprAST>();
    intrinsic->loc = loc;
    intrinsic->intrinsicName = intrinsicName;
    intrinsic->args = args;
    
    LOG_PARSER_DETAIL("parseIntrinsicCallExpr: parsed intrinsic '#", 
                      ctx.toString(intrinsicName), "' with ", args.size(), " arguments");
    
    return intrinsic;
}

/**
 * @brief Parse an index expression.
 * 
 * Grammar: `'[' expr ']'`
 * 
 * ## Examples
 * 
 * ```lucid
 * nums[2]         → index = 2
 * arr[i + 1]      → index = i + 1
 * matrix[row][col] → nested indexing
 * ```
 * 
 * ## Semantic Analysis Notes
 * 
 * 1. **Runtime Check**: Indexing a slice (`[_]T`) or dynamic array (`[*]T`)
 *    is always runtime-checked. A literal index does not prove in-bounds
 *    against a slice of unknown length.
 * 
 * 2. **Compile-Time Check**: Indexing a fixed-size array (`[N]T`) with a
 *    literal index that is provably less than `N` is checked at compile time.
 * 
 * 3. **Panic Handling**: Out-of-bounds access panics unless guarded with `??`.
 * 
 * 4. **Type**: The result type is the element type of the array.
 * 
 * 5. **String Indexing**: Indexing a string returns a character (char).
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param target The target expression (the array being indexed)
 * @return IndexExprAST* The parsed index expression, or nullptr on error
 */
IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseIndexExpr: parsing index expression");
    
    if (!target) {
        ctx.error(stream, DiagCode::E1006,"none for index expression");
        return nullptr;
    }
    
    // ─── 1. Expect '[' ────────────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.error(stream, DiagCode::E1004, "[", "index expression", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '['
    
    // ─── 2. Check for empty index: [] ────────────────────────────────────
    if (stream.check(TokenType::RBRACKET)) {
        ctx.error(stream, DiagCode::E1006, "none, aka no expression to index array");
        stream.advance(); // Consume ']'
        return nullptr;
    }
    
    // ─── 3. Parse the index expression ────────────────────────────────────
    ExprPtr index = parseExpr(stream, ctx);
    if (!index) {
        ctx.error(stream, DiagCode::E1006, "none, failed to parse expression for indexing");
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (stream.check(TokenType::RBRACKET)) {
            stream.advance(); // Consume ']' to recover
        }
        return nullptr;
    }
    
    // ─── 4. Expect ']' ────────────────────────────────────────────────────
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.error(stream, DiagCode::E1005, "]", "index expression", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (stream.check(TokenType::RBRACKET)) {
            stream.advance(); // Consume ']' to recover
        }
        // Return the expression anyway (error recovery)
        auto* indexExpr = ctx.arena.make<IndexExprAST>();
        indexExpr->loc = loc;
        indexExpr->target = target;
        indexExpr->index = index;
        return indexExpr;
    }
    stream.advance(); // Consume ']'
    
    // ─── 5. Build the AST node ───────────────────────────────────────────
    auto* indexExpr = ctx.arena.make<IndexExprAST>();
    indexExpr->loc = loc;
    indexExpr->target = target;
    indexExpr->index = index;
    
    LOG_PARSER_DETAIL("parseIndexExpr: parsed index expression");
    
    return indexExpr;
}

/**
 * @brief Parse a slice expression.
 * 
 * Grammar: `'[' [ expr ] range_op [ expr ] ']'`
 * 
 * ## Examples
 * 
 * ```lucid
 * nums[1..3]     → start = 1, end = 3,   isExclusive = false
 * nums[1..<3]    → start = 1, end = 3,   isExclusive = true  (end excluded)
 * nums[..<2]     → start = nullptr, end = 2, isExclusive = true
 * nums[3..]      → start = 3, end = nullptr, isExclusive = false
 * nums[..]       → start = nullptr, end = nullptr, isExclusive = false
 * nums[..<]      → start = nullptr, end = nullptr, isExclusive = true (full range exclusive)
 * ```
 * 
 * ## Range Operators
 * 
 * - `..`  : Inclusive range (end is included)
 * - `..<` : Exclusive range (end is excluded)
 * 
 * ## Slice Rules
 * 
 * 1. **Borrowed View**: A slice `[_]T` is a borrowed view – it does not own
 *    the underlying memory. The backing array must outlive the slice.
 * 
 * 2. **Bounds**: Start defaults to 0, end defaults to the array's length.
 * 
 * 3. **Runtime Check**: Slice bounds are runtime-checked. Out-of-bounds
 *    access panics unless guarded with `??`.
 * 
 * 4. **Inclusive/Exclusive**: `..` is inclusive, `..<` is exclusive.
 * 
 * ## Semantic Analysis Notes
 * 
 * 1. **Start Default**: If start is omitted, it defaults to 0.
 * 2. **End Default**: If end is omitted, it defaults to the array length.
 * 3. **Type**: The result type is a slice (`[_]T`) of the same element type.
 * 4. **String Slicing**: Slicing a string returns a string slice.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param target The target expression (the array being sliced)
 * @return SliceExprAST* The parsed slice expression, or nullptr on error
 */
SliceExprAST* parseSliceExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseSliceExpr: parsing slice expression");
    
    if (!target) {
        ctx.error(stream, DiagCode::E1006, "target", stream.peekValue());
        return nullptr;
    }
    
    // ─── 1. Expect '[' ────────────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.error(stream, DiagCode::E1004, "[", "slice expression", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '['
    
    // ─── 2. Parse the slice components ────────────────────────────────────
    ExprPtr start = nullptr;
    ExprPtr end = nullptr;
    bool isExclusive = false;
    bool hasRangeOp = false;
    
    // ─── 3. Check for empty slice: [] ────────────────────────────────────
    if (stream.check(TokenType::RBRACKET)) {
        ctx.error(stream, DiagCode::E1006, "none for slice expression");
        stream.advance(); // Consume ']'
        return nullptr;
    }
    
    // ─── 4. Parse the slice ──────────────────────────────────────────────
    // First, check if we have a range operator at the start: [..] or [..<]
    if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
        // Start is omitted: [..end] or [..<end]
        hasRangeOp = true;
        isExclusive = stream.match(TokenType::RANGE_EXCLUSIVE);
        if (!isExclusive) {
            stream.match(TokenType::RANGE); // Consume '..'
        }
        
        // Check for end expression
        if (!stream.check(TokenType::RBRACKET)) {
            end = parseExpr(stream, ctx);
            if (!end) {
                ctx.error(stream, DiagCode::E1006, "slice end expression", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::RBRACKET);
                if (stream.check(TokenType::RBRACKET)) {
                    stream.advance(); // Consume ']' to recover
                }
                // Return what we have so far
                auto* slice = ctx.arena.make<SliceExprAST>();
                slice->loc = loc;
                slice->target = target;
                slice->start = nullptr;
                slice->end = end;
                slice->isExclusive = isExclusive;
                return slice;
            }
        }
        // If we're at ']', end remains nullptr (defaults to array length)
        
    } else {
        // Parse start expression
        start = parseExpr(stream, ctx);
        if (!start) {
            ctx.error(stream, DiagCode::E1006, stream.peek());
            synchronizeTo(stream, ctx, TokenType::RANGE, TokenType::RANGE_EXCLUSIVE, TokenType::RBRACKET);
            if (stream.checkAny(TokenType::RANGE, TokenType::RANGE_EXCLUSIVE)) {
                // We have a range operator, continue parsing
                hasRangeOp = true;
            } else if (stream.check(TokenType::RBRACKET)) {
                stream.advance(); // Consume ']' to recover
                // Return what we have so far
                auto* slice = ctx.arena.make<SliceExprAST>();
                slice->loc = loc;
                slice->target = target;
                slice->start = start;
                slice->end = nullptr;
                slice->isExclusive = false;
                return slice;
            } else {
                synchronizeTo(stream, ctx, TokenType::RBRACKET);
                if (stream.check(TokenType::RBRACKET)) {
                    stream.advance();
                }
                return nullptr;
            }
        }
        
        // Check for range operator: [start..end] or [start..<end]
        if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
            hasRangeOp = true;
            isExclusive = stream.match(TokenType::RANGE_EXCLUSIVE);
            if (!isExclusive) {
                stream.match(TokenType::RANGE); // Consume '..'
            }
            
            // Check for end expression
            if (!stream.check(TokenType::RBRACKET)) {
                end = parseExpr(stream, ctx);
                if (!end) {
                    ctx.error(stream, DiagCode::E1006, stream.peekValue());
                    synchronizeTo(stream, ctx, TokenType::RBRACKET);
                    if (stream.check(TokenType::RBRACKET)) {
                        stream.advance(); // Consume ']' to recover
                    }
                    // Return what we have so far
                    auto* slice = ctx.arena.make<SliceExprAST>();
                    slice->loc = loc;
                    slice->target = target;
                    slice->start = start;
                    slice->end = nullptr;
                    slice->isExclusive = isExclusive;
                    return slice;
                }
            }
            // If we're at ']', end remains nullptr (defaults to array length)
            
        } else if (stream.check(TokenType::RBRACKET)) {
            // Just a single expression in brackets: [expr]
            // This is actually an index expression, not a slice
            // But we're in parseSliceExpr, so we should handle it gracefully
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "slice requires a range operator (.. or ..<)");
            // Consume ']' and return an index expression (treat as index)
            stream.advance(); // Consume ']'
            
            // Create an index expression instead
            auto* indexExpr = ctx.arena.make<IndexExprAST>();
            indexExpr->loc = loc;
            indexExpr->target = target;
            indexExpr->index = start;
            // We can't return an IndexExprAST from a function returning SliceExprAST*
            // So we'll create a slice with start only and let the caller handle it
            auto* slice = ctx.arena.make<SliceExprAST>();
            slice->loc = loc;
            slice->target = target;
            slice->start = start;
            slice->end = nullptr;
            slice->isExclusive = false;
            return slice;
        } else {
            // Unexpected token
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "'..' or '..<' or ]");
            synchronizeTo(stream, ctx, TokenType::RBRACKET);
            if (stream.check(TokenType::RBRACKET)) {
                stream.advance(); // Consume ']'
            }
            // Return what we have
            auto* slice = ctx.arena.make<SliceExprAST>();
            slice->loc = loc;
            slice->target = target;
            slice->start = start;
            slice->end = nullptr;
            slice->isExclusive = false;
            return slice;
        }
    }
    
    // ─── 5. Expect ']' ────────────────────────────────────────────────────
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.error(stream, DiagCode::E1005, "]", "slice expression", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (stream.check(TokenType::RBRACKET)) {
            stream.advance(); // Consume ']' to recover
        }
        // Return the expression anyway (error recovery)
        auto* slice = ctx.arena.make<SliceExprAST>();
        slice->loc = loc;
        slice->target = target;
        slice->start = start;
        slice->end = end;
        slice->isExclusive = isExclusive;
        return slice;
    }
    stream.advance(); // Consume ']'
    
    // ─── 6. Validate the slice ───────────────────────────────────────────
    // A slice must have at least one bound, or be a full range [..]
    if (!start && !end && !hasRangeOp) {
        ctx.error(stream, DiagCode::E1003, "slice bounds", stream.peekValue());
        // Return what we have
        auto* slice = ctx.arena.make<SliceExprAST>();
        slice->loc = loc;
        slice->target = target;
        slice->start = nullptr;
        slice->end = nullptr;
        slice->isExclusive = false;
        return slice;
    }
    
    // ─── 7. Build the AST node ───────────────────────────────────────────
    auto* slice = ctx.arena.make<SliceExprAST>();
    slice->loc = loc;
    slice->target = target;
    slice->start = start;
    slice->end = end;
    slice->isExclusive = isExclusive;
    
    LOG_PARSER_DETAIL("parseSliceExpr: parsed slice expression",
                      (start ? " with start" : ""),
                      (end ? " with end" : ""),
                      (isExclusive ? " (exclusive)" : " (inclusive)"));
    
    return slice;
}

// =============================================================================
// Pipeline & Composition
// =============================================================================

/**
 * @brief Parse a pipeline expression.
 * 
 * Grammar: `seed '|>' step { '|>' step }`
 * 
 * ## Examples
 * 
 * ```lucid
 * 42 |> float |> sqrt
 * getUser(id) |> validate |> save
 * v |> Vec2:normalize |> scale(2.0)!
 * x |> map<int, string>(stringFromInt)!
 * ```
 * 
 * ## Error Handling
 * 
 * - Trailing `|>` (e.g., `42 |> float |>`) → reports error and stops
 * - Missing step after `|>` → parsePipelineStep reports error
 * - Invalid step → parsePipelineStep reports error and skips
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param seed The seed expression (the initial value)
 * @return ExprAST* The parsed pipeline expression, or nullptr on error
 */
ExprAST* parsePipelineExpr(TokenStream& stream, ParserContext& ctx, ExprPtr seed) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parsePipelineExpr: parsing pipeline expression");
    
    if (!seed) {
        ctx.error(stream, DiagCode::E1107, stream.peekValue());
        return nullptr;
    }
    
    // ─── 1. Parse pipeline steps ──────────────────────────────────────────
    std::vector<PipelineStepPtr> steps;
    
    while (stream.check(TokenType::PIPELINE)) {
        SourceLocation opLoc = stream.currentLoc();
        stream.advance(); // Consume '|>'
        
        // ─── Check for trailing `|>` ─────────────────────────────────────
        // If we're at EOF after consuming '|>', we have a trailing pipeline
        if (stream.isAtEnd()) {
            ctx.errorAt(opLoc, DiagCode::E1006, "expected pipeline step after '|>'");
            ctx.error(stream, DiagCode::E1006, "missing pipeline step after '|>'", "<EOF>");
            break;
        }
        
        // ─── Check for consecutive `|>` operators ──────────────────────
        // If we see another '|>' immediately, that's a missing step
        if (stream.check(TokenType::PIPELINE)) {
            ctx.errorAt(opLoc, DiagCode::E1006, "missing pipeline step between '|>' operators");
            // Skip the extra '|>' and continue to try parsing the next step
            stream.advance();
            continue;
        }
        
        // ─── Parse the pipeline step ──────────────────────────────────────
        // parsePipelineStep handles errors and reports them
        PipelineStepPtr step = parsePipelineStep(stream, ctx);
        if (!step) {
            // Error already reported by parsePipelineStep
            // Stop parsing this pipeline - we can't recover from a bad step
            break;
        }
        steps.push_back(step);
    }
    
    // ─── 2. Check for trailing `|>` at the end ──────────────────────────
    // This is a safety check in case the loop exited unexpectedly
    if (stream.check(TokenType::PIPELINE)) {
        ctx.error(stream, DiagCode::E1006, "trailing '|>' with no following step");
        // Consume the extra '|>' to avoid infinite loops
        stream.advance();
    }
    
    // ─── 3. Validate that we have at least one step ──────────────────────
    if (steps.empty()) {
        ctx.error(stream, DiagCode::E1006, "at least one pipeline step is required");
        return seed;
    }
    
    // ─── 4. Build the pipeline expression ────────────────────────────────
    auto* pipeline = ctx.arena.make<PipelineExprAST>();
    pipeline->loc = loc;
    pipeline->seed = seed;
    
    auto builder = ctx.arena.makeBuilder<PipelineStepPtr>();
    for (auto* s : steps) {
        builder.push_back(s);
    }
    pipeline->steps = builder.build();
    
    LOG_PARSER_DETAIL("parsePipelineExpr: parsed pipeline expression with ", 
                      steps.size(), " steps");
    
    return pipeline;
}

/**
 * @brief Parse a pipeline step.
 * 
 * A pipeline step can be:
 * - A function call with argument pack: `fn(args)!`
 * - A single expression: `expr`
 * - An anonymous function: `(a int) -> int { ... }`
 * 
 * ## Grammar
 * 
 * ```ebnf
 * pipeline_step := expr [ '(' arg_list ')' '!' ] | func_literal
 * ```
 * 
 * ## Examples
 * 
 * ```lucid
 * float                    → simple expression
 * sqrt                     → function reference
 * scale(2.0)!              → function call with argument pack
 * (x int) -> int { return x * 2 }  → anonymous function
 * ```
 * 
 * ## Error Handling
 * 
 * This function handles the following errors:
 * - Invalid expression: reports error and returns nullptr
 * - Missing arguments: reports error and returns nullptr
 * - Unclosed parentheses: reports error and returns nullptr
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return PipelineStepAST* The parsed pipeline step, or nullptr on error
 */
PipelineStepAST* parsePipelineStep(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parsePipelineStep: parsing pipeline step");
    
    // ─── 1. Check for EOF ─────────────────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1006, "pipeline step", "<EOF>");
        return nullptr;
    }
    
    // ─── 2. Check for anonymous function ─────────────────────────────────
    if (looksLikeAnonFunc(stream, ctx)) {
        ExprPtr anonFunc = parseAnonFuncExpr(stream, ctx);
        if (!anonFunc) {
            return nullptr;
        }
        
        auto* step = ctx.arena.make<PipelineStepAST>();
        step->loc = loc;
        step->callable = anonFunc;
        step->packArgs = ctx.arena.makeBuilder<ExprPtr>().build();
        
        LOG_PARSER_DETAIL("parsePipelineStep: parsed anonymous function step");
        return step;
    }
    
    // ─── 3. Parse a function reference or expression ─────────────────────
    ExprPtr callable = parseExpr(stream, ctx);
    if (!callable) {
        ctx.error(stream, DiagCode::E1006, "pipeline step expression", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    // ─── 4. Check for argument pack step: fn(args)! ─────────────────────
    ArenaSpan<ExprPtr> packArgs;
    bool hasPackArgs = false;
    
    // Check if we have a function call with argument pack
    if (stream.check(TokenType::LPAREN)) {
        // Save position in case this isn't an argument pack
        size_t savedPos = stream.getPos();
        
        // Try to parse as a call expression with argument pack
        // Parse arguments
        std::vector<ExprPtr> args;
        
        // Consume '('
        stream.advance();
        
        if (!stream.check(TokenType::RPAREN)) {
            while (!stream.isAtEnd()) {
                // Skip consecutive separators
                if (stream.consumeTrailing(TokenType::COMMA) > 0) {
                    ctx.error(stream, DiagCode::E1009, ",", "pipeline arguments");
                }
                
                if (stream.check(TokenType::RPAREN)) {
                    break;
                }
                
                if (stream.isAtEnd()) {
                    ctx.error(stream, DiagCode::E1005, ")", "pipeline arguments", "<EOF>");
                    break;
                }
                
                ExprPtr arg = parseExpr(stream, ctx);
                if (arg) {
                    args.push_back(arg);
                } else {
                    ctx.error(stream, DiagCode::E1006, "pipeline argument", stream.peekValue());
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
            }
        }
        
        // Consume closing parenthesis
        if (!stream.check(TokenType::RPAREN)) {
            ctx.error(stream, DiagCode::E1005, ")", "pipeline arguments", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::RPAREN);
            if (stream.check(TokenType::RPAREN)) {
                stream.advance();
            }
        } else {
            stream.advance(); // Consume ')'
        }
        
        // Check for argument pack (!)
        if (stream.check(TokenType::BANG)) {
            stream.advance(); // Consume '!'
            hasPackArgs = true;
            
            // Build the pack args span
            auto builder = ctx.arena.makeBuilder<ExprPtr>();
            for (auto* arg : args) {
                builder.push_back(arg);
            }
            packArgs = builder.build();
        } else {
            // No '!' - this is a regular function call
            // Restore position and treat as regular expression
            stream.setPos(savedPos);
            // The callable already includes the function name
            // Just return it as a step without pack args
            auto* step = ctx.arena.make<PipelineStepAST>();
            step->loc = loc;
            step->callable = callable;
            step->packArgs = ctx.arena.makeBuilder<ExprPtr>().build();
            
            LOG_PARSER_DETAIL("parsePipelineStep: parsed regular expression step");
            return step;
        }
    }
    
    // ─── 5. Build the pipeline step ──────────────────────────────────────
    auto* step = ctx.arena.make<PipelineStepAST>();
    step->loc = loc;
    step->callable = callable;
    step->packArgs = packArgs;
    
    LOG_PARSER_DETAIL("parsePipelineStep: parsed pipeline step",
                      hasPackArgs ? " with argument pack" : "");
    
    return step;
}


/**
 * @brief Parse a composition expression.
 * 
 * Grammar: `lhs '+>' operand { '+>' operand }`
 * 
 * ## Examples
 * 
 * ```lucid
 * const process = validate +> transform +> render
 * const intToString = identity<int> +> toString<int> +> trim
 * const pipeline = normalize +> clamp(0, 1)!
 * ```
 * 
 * ## Composition Rules
 * 
 * 1. **Single Parameter Group**: Both operands must have exactly one
 *    parameter group. Curry functions are forbidden on either side.
 * 
 * 2. **Type Matching**: The output type of the left operand must exactly
 *    match the input type of the right operand.
 * 
 * 3. **Generic Instantiation**: Generic functions must be instantiated
 *    with explicit type arguments before composition.
 * 
 * 4. **Nullable/Fallible Forbidden**: `~[nullable]` and `~[fallible]`
 *    functions are forbidden as composition operands.
 * 
 * 5. **Async Composition**: When any operand is `~[async]`, the composed
 *    function must be declared `~[async]` and awaited at the call site.
 * 
 * ## Key Characteristics
 * 
 * - Compile-time: Produces a new function without executing anything.
 * - Type Matching: Strict – output type of left must exactly match input type of right.
 * - No Qualifiers: `~[async]` or `~[nullable]` operands are forbidden.
 * - Generic Instantiation: Explicit type arguments required for generic functions.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param lhs The left-hand side expression
 * @return ExprAST* The parsed composition expression, or nullptr on error
 */
ExprAST* parseComposeExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lhs) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseComposeExpr: parsing composition expression");
    
    if (!lhs) {
        ctx.error(stream, DiagCode::E1006, "left-hand side", stream.peekValue());
        return nullptr;
    }
    
    // ─── 1. Expect '+>' ──────────────────────────────────────────────────
    if (!stream.check(TokenType::COMPOSE)) {
        ctx.error(stream, DiagCode::E1004, "+>", "composition", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '+>'
    
    // ─── 2. Parse at least one operand ───────────────────────────────────
    std::vector<ComposeOperandPtr> operands;
    
    // Parse the first operand
    ComposeOperandPtr operand = parseComposeOperand(stream, ctx);
    if (!operand) {
        ctx.error(stream, DiagCode::E1006, "composition operand", stream.peekValue());
        synchronize(stream, ctx);
        return lhs;
    }
    operands.push_back(operand);
    
    // ─── 3. Parse additional operands ────────────────────────────────────
    while (stream.check(TokenType::COMPOSE)) {
        stream.advance(); // Consume '+>'
        
        operand = parseComposeOperand(stream, ctx);
        if (!operand) {
            ctx.error(stream, DiagCode::E1006, "composition operand", stream.peekValue());
            synchronize(stream, ctx);
            break;
        }
        operands.push_back(operand);
    }
    
    // ─── 4. Build the composition expression ─────────────────────────────
    auto* compose = ctx.arena.make<ComposeExprAST>();
    compose->loc = loc;
    compose->left = lhs;
    
    auto builder = ctx.arena.makeBuilder<ComposeOperandPtr>();
    for (auto* op : operands) {
        builder.push_back(op);
    }
    compose->operands = builder.build();
    
    LOG_PARSER_DETAIL("parseComposeExpr: parsed composition expression with ", 
                      operands.size(), " operands");
    
    return compose;
}

/**
 * @brief Parse a composition operand.
 * 
 * A composition operand is a function that takes the previous
 * function's output as input. Both operands must have exactly
 * one parameter group.
 * 
 * ## Grammar
 * 
 * ```ebnf
 * compose_operand := expr [ '<' type { ',' type } '>' ]
 * ```
 * 
 * The callable expression can be:
 * - IdentifierExprAST (plain function name)
 * - FieldAccessExprAST (dotted path)
 * - ModuleAccessExprAST (module:function)
 * 
 * Generic arguments are applied to the callable (e.g., `toString<int>` becomes
 * callable = IdentifierExprAST("toString") with genericArgs = [int]).
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ComposeOperandAST* The parsed composition operand, or nullptr on error
 */
ComposeOperandAST* parseComposeOperand(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseComposeOperand: parsing composition operand");
    
    // ─── 1. Parse the callable expression ────────────────────────────────
    // A composition operand is a function reference, which is an expression
    // that evaluates to a function
    ExprPtr callable = parseExpr(stream, ctx);
    if (!callable) {
        ctx.error(stream, DiagCode::E1006, "composition operand", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    // ─── 2. Check for generic arguments ──────────────────────────────────
    // Generic arguments can be attached to the callable:
    // - IdentifierExprAST: `toString<int>`
    // - FieldAccessExprAST: `myModule.toString<int>`
    // - ModuleAccessExprAST: `math:sqrt<float>`
    ArenaSpan<TypePtr> genericArgs;
    
    if (callable->isa<IdentifierExprAST>()) {
        auto* idExpr = callable->as<IdentifierExprAST>();
        if (idExpr->genericArgs.size() > 0) {
            genericArgs = idExpr->genericArgs;
        }
    } else if (callable->isa<FieldAccessExprAST>()) {
        auto* fieldAccess = callable->as<FieldAccessExprAST>();
        if (fieldAccess->genericArgs.size() > 0) {
            genericArgs = fieldAccess->genericArgs;
        }
    } else if (callable->isa<ModuleAccessExprAST>()) {
        auto* moduleAccess = callable->as<ModuleAccessExprAST>();
        if (moduleAccess->genericArgs.size() > 0) {
            genericArgs = moduleAccess->genericArgs;
        }
    }
    
    // ─── 3. Build the composition operand ────────────────────────────────
    auto* operand = ctx.arena.make<ComposeOperandAST>();
    operand->loc = loc;
    operand->callable = callable;
    operand->genericArgs = genericArgs;
    
    LOG_PARSER_DETAIL("parseComposeOperand: parsed composition operand",
                      genericArgs.size() > 0 ? " with generic args" : "");
    
    return operand;
}

// =============================================================================
// Precedence Helpers
// =============================================================================

/**
 * @brief Get the infix precedence of a token type.
 * 
 * @param type The token type
 * @return int The precedence level, or -1 if not an infix operator
 * 
 * ## Precedence Levels
 * 
 * | Level | Operators |
 * |-------|-----------|
 * | 8     | `+>` (composition) |
 * | 7     | unary `-` `not` `~` |
 * | 6     | `*` `/` `%` `**` |
 * | 5     | `+` `-` |
 * | 4     | `..` `..<` (range) |
 * | 3     | `==` `!=` `<` `<=` `>` `>=` |
 * | 2     | `and` |
 * | 1     | `or` |
 * | 0     | `|>` (pipeline) |
 */
int infixPrec(TokenType type) {
    // TODO: Implement
    return -1;
}

/**
 * @brief Convert a token type to a BinaryOp.
 * 
 * @param type The token type
 * @return BinaryOp The corresponding binary operation
 */
BinaryOp tokenToBinaryOp(TokenType type) {
    // TODO: Implement
    return BinaryOp::Add;
}

/**
 * @brief Convert a token type to an AssignOp.
 * 
 * @param type The token type
 * @return AssignOp The corresponding assignment operation
 */
AssignOp tokenToAssignOp(TokenType type) {
    // TODO: Implement
    return AssignOp::Assign;
}

// =============================================================================
// Infix Dispatch
// =============================================================================

/**
 * @brief Parse an assignment expression.
 * 
 * Grammar: `lvalue '=' expr`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param lhs The left-hand side expression
 * @return ExprPtr The parsed assignment expression, or nullptr on error
 */
ExprPtr parseInfixAssign(TokenStream& stream, ParserContext& ctx, ExprPtr lhs) {
    LOG_PARSER_DETAIL("parseInfixAssign: parsing assignment expression");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse an `is` expression.
 * 
 * Grammar: `expr 'is' type`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param lhs The left-hand side expression
 * @return ExprPtr The parsed is expression, or nullptr on error
 */
ExprPtr parseInfixIs(TokenStream& stream, ParserContext& ctx, ExprPtr lhs) {
    LOG_PARSER_DETAIL("parseInfixIs: parsing is expression");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse a null coalesce expression.
 * 
 * Grammar: `expr '??' expr`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param lhs The left-hand side expression
 * @return ExprPtr The parsed null coalesce expression, or nullptr on error
 */
ExprPtr parseInfixNullCoalesce(TokenStream& stream, ParserContext& ctx, ExprPtr lhs) {
    LOG_PARSER_DETAIL("parseInfixNullCoalesce: parsing null coalesce expression");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse a binary expression.
 * 
 * Grammar: `lhs operator rhs`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param lhs The left-hand side expression
 * @param opTok The operator token type
 * @param prec The precedence level
 * @return ExprPtr The parsed binary expression, or nullptr on error
 */
ExprPtr parseInfixBinary(TokenStream& stream, ParserContext& ctx, ExprPtr lhs, TokenType opTok, int prec) {
    LOG_PARSER_DETAIL("parseInfixBinary: parsing binary expression");
    // TODO: Implement
    return nullptr;
}

} // namespace parser

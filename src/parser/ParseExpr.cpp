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

#include "Parser.hpp"
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
        
        // Check for `is` operator (special handling)
        if (current == TokenType::IS) {
            stream.advance(); // Consume 'is'
            lhs = parseInfixIs(stream, ctx, lhs);
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
        // Check for generic arguments before the call: f<int>(42)
        // The generic args would have been parsed already in parsePrimaryExpr
        return parseCallExpr(stream, ctx, lhs, ArenaSpan<TypeAST*>());
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
 * Grammar: `'(' [ arg_list ] ')'`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param callee The callee expression
 * @param genericArgs The generic arguments (if any)
 * @return CallExprAST* The parsed call expression, or nullptr on error
 */
CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx, ExprPtr callee, ArenaSpan<TypeAST*> genericArgs) {
    LOG_PARSER_DETAIL("parseCallExpr: parsing call expression");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse an intrinsic call expression.
 * 
 * Grammar: `'#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return IntrinsicCallExprAST* The parsed intrinsic call, or nullptr on error
 */
IntrinsicCallExprAST* parseIntrinsicCallExpr(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseIntrinsicCallExpr: parsing intrinsic call");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse an index expression.
 * 
 * Grammar: `'[' expr ']'`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param target The target expression
 * @return IndexExprAST* The parsed index expression, or nullptr on error
 */
IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target) {
    LOG_PARSER_DETAIL("parseIndexExpr: parsing index expression");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse a slice expression.
 * 
 * Grammar: `'[' [ expr ] range_op [ expr ] ']'`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param target The target expression
 * @return SliceExprAST* The parsed slice expression, or nullptr on error
 */
SliceExprAST* parseSliceExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target) {
    LOG_PARSER_DETAIL("parseSliceExpr: parsing slice expression");
    // TODO: Implement
    return nullptr;
}

// =============================================================================
// Pipeline & Composition
// =============================================================================

/**
 * @brief Parse a pipeline expression.
 * 
 * Grammar: `seed '|>' step { '|>' step }`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param seed The seed expression
 * @return ExprAST* The parsed pipeline expression, or nullptr on error
 */
ExprAST* parsePipelineExpr(TokenStream& stream, ParserContext& ctx, ExprPtr seed) {
    LOG_PARSER_DETAIL("parsePipelineExpr: parsing pipeline expression");
    // TODO: Implement
    return seed;
}

/**
 * @brief Parse a composition expression.
 * 
 * Grammar: `lhs '+>' operand`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param lhs The left-hand side expression
 * @return ExprAST* The parsed composition expression, or nullptr on error
 */
ExprAST* parseComposeExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lhs) {
    LOG_PARSER_DETAIL("parseComposeExpr: parsing composition expression");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse a pipeline step.
 * 
 * A pipeline step can be:
 * - A function call with argument pack: `fn(args)!`
 * - A single expression: `expr`
 * - An anonymous function: `(a int) -> int { ... }`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return PipelineStepAST* The parsed pipeline step, or nullptr on error
 */
PipelineStepAST* parsePipelineStep(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parsePipelineStep: parsing pipeline step");
    // TODO: Implement
    return nullptr;
}

/**
 * @brief Parse a composition operand.
 * 
 * A composition operand is a function that takes the previous
 * function's output as input. Both operands must have exactly
 * one parameter group.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ComposeOperandAST* The parsed composition operand, or nullptr on error
 */
ComposeOperandAST* parseComposeOperand(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseComposeOperand: parsing composition operand");
    // TODO: Implement
    return nullptr;
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

/**
 * @file ParserExpr.cpp
 * @brief Pratt parser for Luc expressions – operators, literals, calls, pipelines.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the expression parsing subsystem using a Pratt parser
 * (top‑down operator precedence). It handles:
 *   - Literals (integers, floats, strings, booleans, nil)
 *   - Unary operators (-, not, ~~, &)
 *   - Binary operators (arithmetic, comparison, logical, bitwise)
 *   - Function calls and indexing
 *   - Field access (.) and method calls (:)
 *   - Nullable chaining (?.) and coalescing (??)
 *   - Pipeline operator (|>) and composition operator (+>)
 *   - Match expressions (patterns are in ParserPattern.cpp)
 * 
 * ## Pratt Parser Overview
 * 
 * The Pratt parser uses a precedence climbing algorithm:
 * 
 *   parseExpr() → parsePrattExpr(minPrec)
 *                      │
 *         ┌────────────┼────────────┐
 *         │            │            │
 *    parsePrefix   parseInfix   parsePostfix
 *    (unary ops)   (binary ops)  (calls, index, .)
 * 
 * Precedence levels are defined in the anonymous namespace at the top.
 * Higher number = tighter binding.
 * 
 * ## Precedence Table
 * 
 *   Level 12 : ^ (exponentiation, right‑associative)
 *   Level 11 : *, /, %
 *   Level 10 : +, -
 *   Level 8  : &&, ||, ~^, <<, >> (bitwise)
 *   Level 7  : ==, !=, <, >, <=, >=, is, ===
 *   Level 6  : and
 *   Level 5  : or
 *   Level 4  : ?? (null coalesce)
 *   Level 3  : |> (pipeline)
 *   Level 2  : +> (composition)
 *   Level 1  : =, +=, -=, etc. (assignment)
 * 
 * ## Loop Safety
 * 
 * The Pratt main loop (parsePrattExpr) checks precedence before each iteration.
 * List‑parsing functions (parseArgList) use a consecutive error counter and
 * saved position pattern to prevent infinite loops.
 * 
 * @see ParserPattern.cpp for match expression patterns
 * @see LUC_GRAMMAR.md for expression grammar
 */

#include "Parser.hpp"
#include "ast/ExprAST.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <cassert>
#include <string>

namespace {
    // Precedence levels
    constexpr int PREC_NONE = 0;
    constexpr int PREC_ASSIGN = 1;
    constexpr int PREC_COMPOSE = 2;
    constexpr int PREC_PIPE = 3;
    constexpr int PREC_NULLCOAL = 4;
    constexpr int PREC_OR = 5;
    constexpr int PREC_AND = 6;
    constexpr int PREC_CMP = 7;
    constexpr int PREC_BITWISE = 8;
    constexpr int PREC_ADD = 10;
    constexpr int PREC_MUL = 11;
    constexpr int PREC_POW = 12;
}

// ============================================================================
// Precedence Helpers
// ============================================================================
// 
// These functions map token types to precedence levels and operator enums.
// 
//   infixPrec()       : returns precedence for an infix operator
//   tokenToBinaryOp() : converts TokenType → BinaryOp (arithmetic, comparison, etc.)
//   tokenToAssignOp() : converts TokenType → AssignOp (assignment operators)
//   isAssignOp()      : true for assignment operators (lowest precedence)
// 
// The precedence values are used by parsePrattExpr to decide whether to
// consume an operator or stop.
// ============================================================================

int Parser::infixPrec(TokenType t) const {
    switch (t) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            return PREC_ASSIGN;
        case TokenType::COMPOSE:            return PREC_COMPOSE;
        case TokenType::PIPELINE:           return PREC_PIPE;
        case TokenType::QUESTION_QUESTION:  return PREC_NULLCOAL;
        case TokenType::OR:                 return PREC_OR;
        case TokenType::AND:                return PREC_AND;
        case TokenType::EQUAL_EQUAL:
        case TokenType::EQUAL_EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::GREATER:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER_EQUAL:
        case TokenType::IS:
            return PREC_CMP;
        case TokenType::BIT_AND:
        case TokenType::BIT_OR:
        case TokenType::BIT_XOR:
        case TokenType::SHL:
        case TokenType::SHR:
            return PREC_BITWISE;
        case TokenType::PLUS:
        case TokenType::MINUS:
            return PREC_ADD;
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
            return PREC_MUL;
        case TokenType::POW:
            return PREC_POW;
        default:
            return PREC_NONE;
    }
}

BinaryOp Parser::tokenToBinaryOp(TokenType t) const {
    switch (t) {
        case TokenType::PLUS:                return BinaryOp::Add;
        case TokenType::MINUS:               return BinaryOp::Sub;
        case TokenType::MUL:                 return BinaryOp::Mul;
        case TokenType::DIV:                 return BinaryOp::Div;
        case TokenType::POW:                 return BinaryOp::Pow;
        case TokenType::MOD:                 return BinaryOp::Mod;
        case TokenType::EQUAL_EQUAL:         return BinaryOp::Eq;
        case TokenType::EQUAL_EQUAL_EQUAL:   return BinaryOp::RefEq;
        case TokenType::NOT_EQUAL:           return BinaryOp::Ne;
        case TokenType::LESS:                return BinaryOp::Lt;
        case TokenType::GREATER:             return BinaryOp::Gt;
        case TokenType::LESS_EQUAL:          return BinaryOp::Le;
        case TokenType::GREATER_EQUAL:       return BinaryOp::Ge;
        case TokenType::AND:                 return BinaryOp::And;
        case TokenType::OR:                  return BinaryOp::Or;
        case TokenType::BIT_AND:             return BinaryOp::BitAnd;
        case TokenType::BIT_OR:              return BinaryOp::BitOr;
        case TokenType::BIT_XOR:             return BinaryOp::BitXor;
        case TokenType::SHL:                 return BinaryOp::Shl;
        case TokenType::SHR:                 return BinaryOp::Shr;
        default:                             return BinaryOp::Add;
    }
}

AssignOp Parser::tokenToAssignOp(TokenType t) const {
    switch (t) {
        case TokenType::ASSIGN:          return AssignOp::Assign;
        case TokenType::PLUS_ASSIGN:     return AssignOp::AddAssign;
        case TokenType::MINUS_ASSIGN:    return AssignOp::SubAssign;
        case TokenType::MUL_ASSIGN:      return AssignOp::MulAssign;
        case TokenType::DIV_ASSIGN:      return AssignOp::DivAssign;
        case TokenType::POW_ASSIGN:      return AssignOp::PowAssign;
        case TokenType::MOD_ASSIGN:      return AssignOp::ModAssign;
        case TokenType::BIT_AND_ASSIGN:  return AssignOp::BitAndAssign;
        case TokenType::BIT_OR_ASSIGN:   return AssignOp::BitOrAssign;
        case TokenType::BIT_XOR_ASSIGN:  return AssignOp::BitXorAssign;
        case TokenType::SHL_ASSIGN:      return AssignOp::ShlAssign;
        case TokenType::SHR_ASSIGN:      return AssignOp::ShrAssign;
        default:                         return AssignOp::Assign;
    }
}

bool Parser::isAssignOp(TokenType t) const {
    switch (t) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// Pratt Parser Core
// ============================================================================
// 
// The Pratt parser is a recursive‑descent operator precedence parser.
// 
//   parseExpr()          : entry point, starts at lowest precedence
//   parsePrattExpr()     : main climbing algorithm
//   parsePrefixExpr()    : handles unary operators and primary expressions
//   parsePostfixExpr()   : handles calls, indexing, field access after the prefix
// 
// Algorithm (in parsePrattExpr):
//   1. Parse a prefix expression (unary op or primary)
//   2. Apply all postfix operators (calls, indexing, ., :, ?.)
//   3. While next token is infix operator with precedence > minPrec:
//        a. If assignment operator → parseInfixAssign, break
//        b. If 'is' → parseInfixIs, continue
//        c. If '|>' → parsePipelineExpr, continue
//        d. If '+>' → parseComposeExpr, continue
//        e. If '??' → parseInfixNullCoalesce, break
//        f. Otherwise → parseInfixBinary, then re‑apply postfix
// 
// Right‑associative operators (^, ??, assignment) use `prec - 1` when recursing.
// ============================================================================

ExprPtr Parser::parseExpr(bool allowStructLiteral) {
    return parsePrattExpr(PREC_NONE, allowStructLiteral);
}

ExprPtr Parser::parsePrattExpr(int minPrec, bool allowStructLiteral) {
    ExprPtr lhs = parsePrefixExpr(allowStructLiteral);
    if (!lhs) {
        return arena_.make<UnknownExprAST>();
    }

    lhs = parsePostfixExpr(std::move(lhs));

    while (true) {
        int prec = infixPrec(ts_.peekType());
        if (prec <= minPrec) break;

        TokenType opTok = ts_.peekType();

        if (isAssignOp(opTok)) {
            lhs = parseInfixAssign(std::move(lhs), allowStructLiteral);
            break;
        }

        if (opTok == TokenType::IS) {
            lhs = parseInfixIs(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::PIPELINE) {
            lhs = parsePipelineExpr(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::COMPOSE) {
            lhs = parseComposeExpr(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::QUESTION_QUESTION) {
            lhs = parseInfixNullCoalesce(std::move(lhs), allowStructLiteral);
            break;
        }

        lhs = parseInfixBinary(std::move(lhs), opTok, prec, allowStructLiteral);
        lhs = parsePostfixExpr(std::move(lhs));
    }

    return lhs;
}

// ============================================================================
// Infix Operator Handlers
// ============================================================================
// 
// These functions are called from parsePrattExpr when an infix operator is
// encountered. Each consumes the operator token, parses the right operand,
// and builds the corresponding AST node.
// 
//   parseInfixAssign()      : =, +=, -=, *=, /=, ^=, %=, &&=, ||=, ~^=, <<=, >>=
//   parseInfixIs()          : is (type check)
//   parseInfixNullCoalesce(): ?? (null coalescing)
//   parseInfixBinary()      : all other binary operators
// 
// Chained comparison detection: parseInfixBinary reports an error when it
// sees patterns like `a < b < c` (not allowed in Luc).
// ============================================================================

ExprPtr Parser::parseInfixAssign(ExprPtr lhs, bool allowStructLiteral) {
    TokenType opTok = ts_.advance().type;
    AssignOp op = tokenToAssignOp(opTok);
    
    ExprPtr rhs = parsePrattExpr(PREC_ASSIGN - 1, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after assignment operator");
        return lhs;
    }

    auto node = arena_.make<AssignExprAST>();
    node->loc = lhs->loc;
    node->op = op;
    node->lhs = std::move(lhs);
    node->rhs = std::move(rhs);
    return node;
}

ExprPtr Parser::parseInfixIs(ExprPtr lhs) {
    ts_.advance();
    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E2005, "expected type after 'is'");
        return lhs;
    }

    auto node = arena_.make<IsExprAST>();
    node->loc = lhs->loc;
    node->expr = std::move(lhs);
    node->checkType = std::move(checkType);
    return node;
}

ExprPtr Parser::parseInfixNullCoalesce(ExprPtr lhs, bool allowStructLiteral) {
    ts_.advance();
    
    ExprPtr fallback = parsePrattExpr(PREC_NULLCOAL - 1, allowStructLiteral);
    if (!fallback) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?'");
        return lhs;
    }

    auto node = arena_.make<NullCoalesceExprAST>();
    node->loc = lhs->loc;
    node->value = std::move(lhs);
    node->fallback = std::move(fallback);
    return node;
}

ExprPtr Parser::parseInfixBinary(ExprPtr lhs, TokenType opTok, int prec, bool allowStructLiteral) {
    ts_.advance();
    
    int nextPrec = (opTok == TokenType::POW) ? prec - 1 : prec;
    ExprPtr rhs = parsePrattExpr(nextPrec, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected right-hand side of binary expression");
        return lhs;
    }

    // Chained comparison detection
    auto isComparisonOp = [](TokenType tt) {
        switch (tt) {
            case TokenType::EQUAL_EQUAL:
            case TokenType::EQUAL_EQUAL_EQUAL:
            case TokenType::NOT_EQUAL:
            case TokenType::LESS:
            case TokenType::GREATER:
            case TokenType::LESS_EQUAL:
            case TokenType::GREATER_EQUAL:
            case TokenType::IS:
                return true;
            default:
                return false;
        }
    };

    if (isComparisonOp(opTok) && isComparisonOp(ts_.peekType())) {
        errorAt(DiagCode::E2002, "chained comparisons not allowed; use 'and' explicitly");
    }

    auto node = arena_.make<BinaryExprAST>();
    node->loc = lhs->loc;
    node->op = tokenToBinaryOp(opTok);
    node->left = std::move(lhs);
    node->right = std::move(rhs);
    return node;
}

// ============================================================================
// Prefix & Primary Parsers
// ============================================================================
// 
//   parsePrefixExpr() : handles unary operators (-, not, ~~, &) then calls
//                       parsePrimaryExpr() for the operand.
//   parsePrimaryExpr(): dispatches to expression atom parsers based on the
//                       current token. Dispatch order is critical:
// 
// Dispatch Priority (highest to lowest):
//   1. match expression    : 'match'
//   2. if expression       : 'if' (expression form, requires '??' and 'else')
//   3. #intrinsic call     : '#'
//   4. await               : 'await'
//   5. array literal       : '['
//   6. bare '{' error      : suggests missing struct name or match
//   7. anonymous function  : looksLikeAnonFunc()
//   8. grouped expression  : '(' expr ')'
//   9. '*' unsafe cast     : '*T(expr)'
//   10. identifier         : IDENTIFIER (struct literal, behavior, or plain)
//   11. primitive type cast: 'float(x)'
//   12. literal            : numbers, strings, true, false, nil
// 
// The allowStructLiteral flag controls whether IDENTIFIER '{' is parsed as a
// struct literal. It is disabled in contexts where '{' belongs to something
// else (e.g., after 'if' or 'match').
// ============================================================================

ExprPtr Parser::parsePrefixExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();

    switch (ts_.peekType()) {
        case TokenType::MINUS: {
            ts_.advance();
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after '-'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Neg;
            node->operand = std::move(operand);
            return node;
        }
        case TokenType::NOT: {
            ts_.advance();
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after 'not'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Not;
            node->operand = std::move(operand);
            return node;
        }
        case TokenType::BIT_NOT: {
            ts_.advance();
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after '~'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::BitNot;
            node->operand = std::move(operand);
            return node;
        }
        case TokenType::AMPERSAND: {
            ts_.advance();
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after '&'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Ref;
            node->operand = std::move(operand);
            return node;
        }
        default:
            return parsePrimaryExpr(allowStructLiteral);
    }
}

/**
 * @brief Parses a primary expression (atom) from the token stream.
 * 
 * Dispatch Priority (highest to lowest):
 *   1. `match` expression
 *   2. `if` expression (expression form)
 *   3. `resolve` expression          // NEW
 *   4. `#intrinsic` call
 *   5. `await` expression
 *   6. array literal `[...]`
 *   7. bare `{` (error)
 *   8. anonymous function
 *   9. grouped expression `(expr)`
 *   10. unsafe cast `*T(expr)`
 *   11. identifier (struct literal, behavior access, or plain)
 *   12. primitive type cast `T(expr)`
 *   13. literal (numbers, strings, true, false, nil)
 * 
 * @param allowStructLiteral When false, `IDENTIFIER '{'` is NOT parsed as a
 *        struct literal (used in contexts like `if` conditions).
 * 
 * @return ExprPtr – the parsed expression, or UnknownExprAST on error
 */
ExprPtr Parser::parsePrimaryExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();

    // match expression
    if (ts_.check(TokenType::MATCH)) {
        return parseMatchExpr();
    }

    // if expression
    if (ts_.check(TokenType::IF)) {
        return parseIfExpr();
    }

    // resolve expression
    if (ts_.check(TokenType::RESOLVE)) {
        return parseResolveExpr();
    }

    // #intrinsic call
    if (ts_.check(TokenType::HASH)) {
        return parseIntrinsicCallExpr();
    }

    // await
    if (ts_.check(TokenType::AWAIT)) {
        return parseAwaitExpr();
    }

    // array literal
    if (ts_.check(TokenType::LBRACKET)) {
        return parseArrayLiteralExpr();
    }

    // bare '{' error
    if (ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2007, "unexpected block in expression position");
        int braceDepth = 1;
        ts_.advance();
        while (!ts_.isAtEnd() && braceDepth > 0) {
            if (ts_.match(TokenType::LBRACE)) braceDepth++;
            else if (ts_.match(TokenType::RBRACE)) braceDepth--;
            else ts_.advance();
        }
        return arena_.make<UnknownExprAST>();
    }

    // anonymous function
    if (looksLikeAnonFunc()) {
        return parseAnonFuncExpr();
    }

    // grouped expression
    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        ExprPtr inner = parsePrattExpr(PREC_NONE, allowStructLiteral);
        if (!ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2001, "expected ')' to close grouped expression");
        } else {
            ts_.advance();
        }
        return inner;
    }

    // '*' unsafe cast
    if (ts_.check(TokenType::MUL) && looksLikeType()) {
        ts_.advance();
        TypePtr targetType = parseBaseType();
        if (!targetType) {
            errorAt(DiagCode::E2005, "expected type after '*' in unsafe cast");
            return arena_.make<UnknownExprAST>();
        }
        if (!ts_.check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' after type in unsafe cast");
            return arena_.make<UnknownExprAST>();
        }
        return parseTypeConvExpr(true, std::move(targetType));
    }

    // identifier
    if (ts_.check(TokenType::IDENTIFIER)) {
        std::string name = ts_.peek().value;

        // struct literal
        if (allowStructLiteral && looksLikeStructLiteral()) {
            ts_.advance();
            ArenaSpan<TypePtr> genericArgs;
            if (ts_.check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            return parseStructLiteralExpr(name, genericArgs);
        }

        // behavior access
        if (looksLikeBehaviorAccess()) {
            std::string typeName = ts_.advance().value;
            ArenaSpan<TypePtr> genericArgs;
            if (ts_.check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            ts_.consume(TokenType::COLON, "expected ':' in behavior access");
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected method name after ':'");
                return arena_.make<UnknownExprAST>();
            }
            std::string method = ts_.advance().value;

            auto node = arena_.make<BehaviorAccessExprAST>();
            node->loc = loc;
            node->typeName = pool_.intern(typeName);
            node->genericArgs = genericArgs;
            node->method = pool_.intern(method);
            node->isBehaviorMember = true;
            return node;
        }

        // plain identifier
        ts_.advance();
        auto node = arena_.make<IdentifierExprAST>(pool_.intern(name));
        node->loc = loc;
        return node;
    }

    // primitive type cast
    if (looksLikeType() && ts_.peekNextType() == TokenType::LPAREN) {
        TypePtr targetType = parsePrimitiveType();
        if (targetType && ts_.check(TokenType::LPAREN)) {
            return parseTypeConvExpr(false, std::move(targetType));
        }
    }

    // literal
    return parseLiteralExpr();
}

// ============================================================================
// Postfix Parser
// ============================================================================
// 
// parsePostfixExpr applies postfix operators to an already‑parsed left‑hand
// side expression. It handles:
// 
//   - Function call      : lhs(args)
//   - Generic call       : lhs<T>(args)
//   - Indexing           : lhs[expr] or lhs[start..end]
//   - Field access       : lhs.field
//   - Nullable chain     : lhs?.field (collects multiple steps)
// 
// Important: This does NOT handle '|>', '+>', or ':' (method access) – those
// are handled at higher precedence levels in parsePrattExpr.
// 
// Nullable chains are collected in one go: each '?.' appends a field name to
// a single NullableChainExprAST. The grammar requires every '?.' chain to be
// terminated by '??' – this is enforced by parseInfixNullCoalesce.
// ============================================================================

ExprPtr Parser::parsePostfixExpr(ExprPtr lhs) {
    while (true) {
        if (ts_.check(TokenType::RPAREN)) break;
        if (ts_.check(TokenType::PIPELINE) || ts_.check(TokenType::COMPOSE)) break;

        // function call
        if (ts_.check(TokenType::LPAREN)) {
            lhs = parseCallExpr(std::move(lhs), ArenaSpan<TypePtr>());
            continue;
        }

        // generic call
        if (ts_.check(TokenType::LESS) && 
            (lhs->isa<IdentifierExprAST>() || lhs->isa<BehaviorAccessExprAST>())) {
            
            size_t savedPos = ts_.getPos();
            int depth = 1;
            size_t i = ts_.getPos() + 1;
            const auto& tokens = ts_.getTokens();
            size_t tokenCount = ts_.getTokenCount();
            
            while (i < tokenCount && depth > 0) {
                if (tokens[i].type == TokenType::LESS) ++depth;
                else if (tokens[i].type == TokenType::GREATER) --depth;
                else if (tokens[i].type == TokenType::EOF_TOKEN) break;
                ++i;
            }
            
            if (depth == 0 && i + 1 < tokenCount && tokens[i + 1].type == TokenType::LPAREN) {
                ArenaSpan<TypePtr> genericArgs = parseGenericArgs();
                lhs = parseCallExpr(std::move(lhs), genericArgs);
                continue;
            }
        }

        // index/slice
        if (ts_.check(TokenType::LBRACKET)) {
            lhs = parseIndexExpr(std::move(lhs));
            continue;
        }

        // field access
        if (ts_.check(TokenType::DOT)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.'");
                break;
            }
            std::string field = ts_.advance().value;
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = lhs->loc;
            node->object = std::move(lhs);
            node->field = pool_.intern(field);
            lhs = std::move(node);
            continue;
        }

        // nullable chain
        if (ts_.check(TokenType::QUESTION_DOT)) {
            // Collect all consecutive '?.' steps in one go
            std::vector<InternedString> steps;
            ExprPtr object = std::move(lhs);
            
            while (ts_.check(TokenType::QUESTION_DOT)) {
                ts_.advance(); // consume '?.'
                if (!ts_.check(TokenType::IDENTIFIER)) {
                    errorAt(DiagCode::E2003, "expected field name after '?.'");
                    break;
                }
                steps.push_back(pool_.intern(ts_.advance().value));
            }
            
            auto chain = arena_.make<NullableChainExprAST>();
            chain->loc = object->loc;
            chain->object = std::move(object);
            
            auto builder = arena_.makeBuilder<InternedString>();
            for (auto& s : steps) builder.push_back(std::move(s));
            chain->steps = builder.build();
            
            lhs = std::move(chain);
            continue;
        }

        break;
    }

    return lhs;
}

// ============================================================================
// Literal and Value Parsers (Primary Expression Factories)
// ============================================================================
// 
// These functions parse atomic expression values:
// 
//   parseLiteralExpr()       : scalar literals (int, float, string, char, bool, nil)
//   parseArrayLiteralExpr()  : [1, 2, 3] (returns temporary vector → SpanBuilder)
//   parseStructLiteralExpr() : Point { x = 0, y = 0 }
//   parseAnonFuncExpr()      : (x int) -> int { return x * 2 }
//   parseAwaitExpr()         : await expr
//   parseIfExpr()            : if cond ?? then else else
//   parseTypeConvExpr()      : type(expr) or *type(expr)
//   parseRangeExpr()         : lo..hi or lo..<hi
// 
// Struct literals use '=' for field initialisation, not ':'.
// Anonymous functions cannot have qualifiers (~async, etc.) – they are plain values.
// ============================================================================

ExprPtr Parser::parseLiteralExpr() {
    SourceLocation loc = ts_.currentLoc();
    Token tok = ts_.advance();

    LiteralKind kind;
    switch (tok.type) {
        case TokenType::INT_LITERAL:        kind = LiteralKind::Int; break;
        case TokenType::FLOAT_LITERAL:      kind = LiteralKind::Float; break;
        case TokenType::STRING_LITERAL:     kind = LiteralKind::String; break;
        case TokenType::RAW_STRING_LITERAL: kind = LiteralKind::RawString; break;
        case TokenType::CHAR_LITERAL:       kind = LiteralKind::Char; break;
        case TokenType::HEX_LITERAL:        kind = LiteralKind::Hex; break;
        case TokenType::BINARY_LITERAL:     kind = LiteralKind::Binary; break;
        case TokenType::TRUE:               kind = LiteralKind::True; break;
        case TokenType::FALSE:              kind = LiteralKind::False; break;
        case TokenType::NIL:                kind = LiteralKind::Nil; break;
        default:
            errorAt(DiagCode::E2002, "internal error: parseLiteralExpr on non-literal token");
            return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<LiteralExprAST>(kind, pool_.intern(tok.value));
    node->loc = loc;
    return node;
}

ExprPtr Parser::parseArrayLiteralExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    std::vector<ExprPtr> elements;  // temporary

    while (!ts_.check(TokenType::RBRACKET) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACKET)) break;

        size_t beforePos = ts_.getPos();
        ExprPtr elem = parseExpr();
        if (ts_.getPos() == beforePos) {
            errorAt(DiagCode::E2008, "expected expression inside array literal");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        elements.push_back(std::move(elem));
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close array literal");

    auto node = arena_.make<ArrayLiteralExprAST>();
    node->loc = loc;

    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : elements) builder.push_back(std::move(e));
    node->elements = builder.build();

    return node;
}

ExprPtr Parser::parseStructLiteralExpr(std::string typeName, ArenaSpan<TypePtr> genericArgs) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{' to open struct literal");

    auto node = arena_.make<StructLiteralExprAST>();
    node->loc = loc;
    node->typeName = pool_.intern(typeName);
    node->genericArgs = genericArgs;

    std::vector<FieldInitPtr> inits;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        SourceLocation fieldLoc = ts_.currentLoc();

        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected field name in struct literal");
            ts_.advance();
            continue;
        }
        std::string fieldName = ts_.advance().value;

        ts_.consume(TokenType::ASSIGN, "expected '=' after field name");
        ExprPtr val = parseExpr();
        if (!val) {
            errorAt(DiagCode::E2008, "expected expression for field");
            continue;
        }

        auto init = arena_.make<FieldInitAST>(pool_.intern(fieldName), std::move(val));
        init->loc = fieldLoc;
        inits.push_back(std::move(init));
    }

    auto builder = arena_.makeBuilder<FieldInitPtr>();
    for (auto& i : inits) builder.push_back(std::move(i));
    node->inits = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct literal");
    return node;
}

ExprPtr Parser::parseAnonFuncExpr() {
    SourceLocation loc = ts_.currentLoc();
    
    auto node = arena_.make<AnonFuncExprAST>();
    node->loc = loc;
    
    if (ts_.check(TokenType::TILDE)) {
        errorAt(DiagCode::E2015, "anonymous function cannot have qualifiers");
        while (ts_.check(TokenType::TILDE)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            ts_.advance();
        }
    }
    
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start anonymous function parameters");
        return arena_.make<UnknownExprAST>();
    }
    
    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    while (ts_.check(TokenType::LPAREN)) {
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    node->sig.allParams = paramsBuilder.build();
    
    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    node->sig.groupSizes = gsBuilder.build();
    
    if (ts_.check(TokenType::ARROW)) {
        ts_.advance();
        node->sig.returnTypes = parseReturnList();
        if (node->sig.returnTypes.empty() && !ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2005, "expected return type after '->' in anonymous function");
        }
    }
    
    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start anonymous function body");
        return arena_.make<UnknownExprAST>();
    }
    node->body = parseBlock();
    
    return node;
}

// ============================================================================
// Await Expression
// ============================================================================
// 
// parseAwaitExpr() parses the 'await' keyword used to suspend an async function.
// 
// Grammar: 'await' expr
// 
// Example: await httpGet(url)
// 
// ─── Semantic Restrictions (Parser Checks) ─────────────────────────────────
//   - 'await' is only valid inside a ~async function body
//   - 'await' is not allowed inside a ~parallel body (parallelDepth_ > 0)
//   - The awaited expression must be a call to a ~async function
// 
// The second restriction is checked here (parallelDepth_). The others are
// enforced by the semantic pass.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'await' keyword
// On exit:  positioned after the awaited expression
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
// None – single expression, no loops.
// ============================================================================

ExprPtr Parser::parseAwaitExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AWAIT, "expected 'await'");

    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'await' is not valid inside a 'parallel' block");
    }

    ExprPtr inner = parsePrattExpr(PREC_NONE, allowStructLiteral);
    if (!inner) {
        errorAt(DiagCode::E2008, "expected expression after 'await'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<AwaitExprAST>(std::move(inner));
    node->loc = loc;
    return node;
}

// ============================================================================
// If Expression (Expression Form)
// ============================================================================
// 
// parseIfExpr() parses the expression form of 'if', which produces a value.
// 
// Grammar: 'if' expr '??' expr 'else' expr
// 
// Example: let grade = if score >= 60 ?? "pass" else "fail"
// 
// ─── Comparison with IfStmtAST ────────────────────────────────────────────
//   IfExprAST (this)              | IfStmtAST (in ParserStmt.cpp)
//   ------------------------------|------------------------------------------
//   Expression context            | Statement context
//   'else' required               | 'else' optional
//   Produces a value              | No value produced
//   Uses '??' separator           | No '??' separator
// 
// ─── Operator Precedence ──────────────────────────────────────────────────
// The '??' here is a syntactic separator (not the null‑coalescing operator).
// The condition is parsed with PREC_NULLCOAL to stop at the first '??'.
// The expression is right‑associative: a ?? b else c ?? d else e.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'if' keyword
// On exit:  positioned after the else branch expression
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Missing condition after 'if' → returns UnknownExprAST
// - Missing '??' after condition → reports error, returns UnknownExprAST
// - Missing 'else' keyword → reports error, returns UnknownExprAST
// ============================================================================

ExprPtr Parser::parseIfExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IF, "expected 'if'");

    ExprPtr condition = parsePrattExpr(PREC_NULLCOAL, allowStructLiteral);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        return arena_.make<UnknownExprAST>();
    }

    if (!ts_.match(TokenType::QUESTION_QUESTION)) {
        errorAt(DiagCode::E2001, "expected '\?\?' after if condition in expression form");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr thenBranch = parseExpr();
    if (!thenBranch) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?'");
    }

    if (!ts_.match(TokenType::ELSE)) {
        errorAt(DiagCode::E2006, "expression-form 'if' requires an 'else' branch");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr elseBranch = parseExpr();
    if (!elseBranch) {
        errorAt(DiagCode::E2008, "expected expression after 'else'");
    }

    auto node = arena_.make<IfExprAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->thenBranch = std::move(thenBranch);
    node->elseBranch = std::move(elseBranch);
    return node;
}

// ============================================================================
// Type Conversion Expression (Cast)
// ============================================================================
// 
// parseTypeConvExpr() parses explicit type casts.
// 
// Grammar:
//   Safe cast:   type_name '(' expr ')'
//   Unsafe cast: '*' type_name '(' expr ')'
// 
// Examples:
//   float(x)      – safe cast (widening, enum→int, int→string)
//   *uint32(bits) – unsafe bit reinterpret (only in @extern)
// 
// ─── Safe vs Unsafe ────────────────────────────────────────────────────────
//   Safe (isUnsafe = false): type_name '(' expr ')'
//     - Supported casts: primitive widening, enum→int, int→string
//     - Validated by semantic pass
// 
//   Unsafe (isUnsafe = true): '*' type_name '(' expr ')'
//     - Bit reinterpretation
//     - Only allowed inside @extern functions or with --unsafe flag
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: 
//   - For unsafe cast: positioned after '*' and type name (at '(')
//   - For safe cast: positioned after type name (at '(')
// On exit: positioned after the closing ')'
// 
// ─── Preconditions ─────────────────────────────────────────────────────────
// - The caller (parsePrimaryExpr) has already consumed the type name
// - The current token is '('
// ============================================================================

ExprPtr Parser::parseTypeConvExpr(bool isUnsafe, TypePtr targetType) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LPAREN, "expected '(' for explicit type cast");

    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E2008, "expected expression inside explicit type cast");
        return arena_.make<UnknownExprAST>();
    }

    ts_.consume(TokenType::RPAREN, "expected ')' to close explicit type cast");

    auto node = arena_.make<TypeConvExprAST>(std::move(targetType), std::move(expr), isUnsafe);
    node->loc = loc;
    return node;
}

// ============================================================================
// Range Expression
// ============================================================================
// 
// parseRangeExpr() parses an inclusive or exclusive range.
// 
// Grammar: expr ( '..' | '..<' ) expr
// 
// Examples:
//   0..10    – inclusive range (0 through 10)
//   1..<5    – exclusive range (1 through 4)
// 
// ─── Usage Contexts ────────────────────────────────────────────────────────
// Ranges appear in:
//   - For loops          : for i in 0..10 { ... }
//   - Match patterns     : case 1..10 => "light"
//   - Slice indices      : nums[1..3]
// 
// ─── Preconditions ─────────────────────────────────────────────────────────
// - Called when '..' or '..<' is found after the lo expression
// - The lo expression is already parsed and passed as a parameter
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '..' or '..<' (lo already consumed)
// On exit:  positioned after the hi expression
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Missing hi expression after '..' → returns UnknownExprAST
// ============================================================================

ExprPtr Parser::parseRangeExpr(ExprPtr lo, bool allowStructLiteral) {
    SourceLocation loc = lo->loc;
    ts_.consume(TokenType::RANGE, "expected '..'");

    bool isExclusive = ts_.match(TokenType::LESS);
    ExprPtr hi = parsePrattExpr(PREC_ADD, allowStructLiteral);
    if (!hi) {
        errorAt(DiagCode::E2008, "expected upper bound after '..'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<RangeExprAST>();
    node->loc = loc;
    node->lo = std::move(lo);
    node->hi = std::move(hi);
    node->isExclusive = isExclusive;
    return node;
}

// ============================================================================
// Intrinsic Call
// ============================================================================
// 
// Intrinsic calls use the '#' prefix: #sizeof(T), #sqrt(x), #memcpy(dst, src, n)
// 
// Two categories:
//   - Type intrinsics : #sizeof, #alignof – take a type argument
//   - Value intrinsics: all others – take expression arguments
// 
// Detection is based on the intrinsic name after '#'.
// Arguments are parsed as expressions (or a single type for type intrinsics).
// ============================================================================

ExprPtr Parser::parseIntrinsicCallExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::HASH, "expected '#'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected intrinsic name after '#'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IntrinsicCallExprAST>();
    node->loc = loc;
    node->intrinsicName = pool_.intern(ts_.advance().value);

    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' after intrinsic name");
        return arena_.make<UnknownExprAST>();
    }
    ts_.advance();

    std::string intrinsicStr = std::string(pool_.lookup(node->intrinsicName));
    bool isTypeIntrinsic = (intrinsicStr == "sizeof" || intrinsicStr == "alignof");

    if (isTypeIntrinsic) {
        if (ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2005, "expected type argument");
        } else {
            TypePtr typeArg = parseType();
            if (!typeArg) errorAt(DiagCode::E2005, "invalid type argument");
            else node->typeArg = std::move(typeArg);
        }
        ts_.consume(TokenType::RPAREN, "expected ')' after type argument");
    } else {
        std::vector<ExprPtr> args;
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            size_t savedPos = ts_.getPos();
            ExprPtr arg = parseExpr();
            if (ts_.getPos() == savedPos) {
                errorAt(DiagCode::E2008, "expected argument expression");
                if (!ts_.isAtEnd()) ts_.advance();
                break;
            }
            args.push_back(std::move(arg));
            if (ts_.check(TokenType::RPAREN)) break;
            if (!ts_.match(TokenType::COMMA)) {
                errorAt(DiagCode::E2001, "expected ',' or ')' in intrinsic argument list");
                break;
            }
        }
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : args) builder.push_back(std::move(a));
        node->args = builder.build();
        ts_.consume(TokenType::RPAREN, "expected ')' to close intrinsic call");
    }

    return node;
}

// ============================================================================
// Call and Index Parsers
// ============================================================================
// 
//   parseCallExpr()   : callee(args) with optional generic arguments
//   parseIndexExpr()  : target[idx] or target[start..end] (slice)
//   parseArgList()    : comma‑separated argument list until ')'
// 
// Generic call detection uses lookahead to distinguish from comparison '<'.
// 
// parseArgList includes a consecutive error counter (max 5) to prevent
// infinite loops on malformed argument lists (e.g., missing expression after comma).
// ============================================================================

ExprPtr Parser::parseCallExpr(ExprPtr callee, ArenaSpan<TypePtr> genericArgs) {
    SourceLocation loc = callee->loc;
    ts_.consume(TokenType::LPAREN, "expected '('");

    auto node = arena_.make<CallExprAST>();
    node->loc = loc;
    node->callee = std::move(callee);
    node->genericArgs = genericArgs;

    if (!ts_.check(TokenType::RPAREN)) {
        node->args = parseArgList();
    }

    ts_.consume(TokenType::RPAREN, "expected ')' to close argument list");
    node->isArgPack = ts_.match(TokenType::BANG);

    return node;
}

ExprPtr Parser::parseIndexExpr(ExprPtr target) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    ExprPtr startExpr = parseExpr();
    if (!startExpr) {
        errorAt(DiagCode::E2008, "expected index expression");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IndexExprAST>();
    node->loc = loc;
    node->target = std::move(target);

    if (ts_.check(TokenType::RANGE)) {
        ts_.advance();
        bool isExclusive = ts_.match(TokenType::LESS);
        ExprPtr endExpr = parseExpr();
        if (!endExpr) {
            errorAt(DiagCode::E2008, "expected end of slice range after '..'");
            return arena_.make<UnknownExprAST>();
        }
        node->index = std::move(startExpr);
        node->sliceEnd = std::move(endExpr);
        node->kind = IndexKind::Slice;
        node->isExclusive = isExclusive;
    } else {
        node->index = std::move(startExpr);
        node->kind = IndexKind::Element;
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close index expression");
    return node;
}

ArenaSpan<ExprPtr> Parser::parseArgList() {
    std::vector<ExprPtr> args;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            errorAt(DiagCode::E2002, "too many consecutive errors in argument list; skipping to ')'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) ts_.advance();
            break;
        }

        size_t savedPos = ts_.getPos();
        ExprPtr arg = parseExpr();

        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2008, "expected argument expression");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            continue;
        }

        consecutiveErrors = 0;
        args.push_back(std::move(arg));

        if (ts_.check(TokenType::RPAREN)) break;
        if (!ts_.match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "expected ',' after argument");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            break;
        }
    }
    
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}

// ============================================================================
// Pipeline and Composition
// ============================================================================
// 
//   parsePipelineExpr() : seed |> step |> step (runtime, left‑associative)
//   parseComposeExpr()  : f +> g +> h (compile‑time, left‑associative)
// 
// Pipeline steps can be:
//   - Function name          : fn
//   - Method reference       : Type:method
//   - Field reference        : obj.field (must be function type)
//   - Index reference        : arr[idx]
//   - Argument pack          : fn(args)! (upstream injected as first arg)
//   - Anonymous function     : (x int) -> int { ... }
// 
// The '!' suffix marks an intentionally incomplete argument list.
// 
// Composition operands are more restricted: no '!', no anonymous functions.
// ============================================================================

ExprPtr Parser::parsePipelineExpr(ExprPtr seed) {
    if (!seed) {
        errorAt(DiagCode::E2008, "expected pipeline seed before '|>'");
        return arena_.make<UnknownExprAST>();
    }

    std::vector<PipelineStepPtr> steps;  // temporary

    while (ts_.check(TokenType::PIPELINE)) {
        ts_.advance();
        PipelineStepPtr step = parsePipelineStep();
        if (step) {
            steps.push_back(std::move(step));
        } else {
            break;
        }
    }

    if (steps.empty()) {
        errorAt(DiagCode::E2006, "pipeline '|>' requires at least one step");
        return seed;
    }

    auto node = arena_.make<PipelineExprAST>();
    node->loc = seed->loc;
    node->seed = std::move(seed);
    
    auto builder = arena_.makeBuilder<PipelineStepPtr>();
    for (auto& s : steps) builder.push_back(std::move(s));
    node->steps = builder.build();

    return node;
}

ExprPtr Parser::parseComposeExpr(ExprPtr lhs) {
    auto node = arena_.make<ComposeExprAST>();
    node->loc = lhs->loc;
    node->left = std::move(lhs);

    std::vector<ComposeOperandPtr> operands;

    while (ts_.check(TokenType::COMPOSE)) {
        ts_.advance();
        ComposeOperandPtr op = parseComposeOperand();
        if (!op) {
            errorAt(DiagCode::E2002, "expected function name after '+>'");
            break;
        }
        operands.push_back(std::move(op));
    }

    if (operands.empty()) {
        return std::move(node->left);
    }

    // Build ArenaSpan
    auto builder = arena_.makeBuilder<ComposeOperandPtr>();
    for (auto& op : operands) builder.push_back(std::move(op));
    node->operands = builder.build();

    return node;
}

PipelineStepPtr Parser::parsePipelineStep() {
    if (looksLikeAnonFunc()) {
        return parseAnonFuncPipelineStep();
    }

    bool isPrimitiveType = isPrimitiveTypeToken(ts_.peekType());
    if (!ts_.check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        errorAt(DiagCode::E2002, "expected function name, method reference, or anonymous function");
        auto step = arena_.make<PipelineStepAST>();
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        ts_.advance();
        return step;
    }

    std::string name = ts_.advance().value;
    ArenaSpan<TypePtr> genericArgs;
    if (ts_.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs();
    }

    if (ts_.check(TokenType::COLON)) {
        return parseBehaviorPipelineStep(name, genericArgs);
    }
    
    if (ts_.check(TokenType::DOT)) {
        return parseFieldPipelineStep(name, genericArgs);
    }

    if (ts_.check(TokenType::LBRACKET)) {
        return parseIndexPipelineStep(name, genericArgs);
    }

    if (ts_.check(TokenType::LPAREN)) {
        return parseArgPackPipelineStep(name, genericArgs);
    }

    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::Ident;
    step->ident = pool_.intern(name);
    step->genericArgs = genericArgs;
    return step;
}

PipelineStepPtr Parser::parseAnonFuncPipelineStep() {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::AnonFunc;
    step->anonFunc = parseAnonFuncExpr();
    return step;
}

PipelineStepPtr Parser::parseBehaviorPipelineStep(const std::string& typeName, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::BehaviorRef;
    step->typeName = pool_.intern(typeName);
    step->genericArgs = genericArgs;

    ts_.consume(TokenType::COLON, "expected ':'");
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name after ':'");
        step->method = pool_.intern("<error>");
        return step;
    }
    step->method = pool_.intern(ts_.advance().value);

    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        std::vector<ExprPtr> packArgs;
        if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
        ts_.consume(TokenType::RPAREN, "expected ')'");
        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for method argument pack");
            return step;
        }
        step->kind = PipelineStepKind::BehaviorArgPack;
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(std::move(a));
        step->packArgs = builder.build();
    }

    return step;
}

PipelineStepPtr Parser::parseFieldPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::FieldRef;
    step->ident = pool_.intern(ident);
    step->genericArgs = genericArgs;

    ts_.consume(TokenType::DOT, "expected '.'");
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name after '.'");
        step->field = pool_.intern("<error>");
        return step;
    }
    step->field = pool_.intern(ts_.advance().value);

    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        std::vector<ExprPtr> packArgs;
        if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
        ts_.consume(TokenType::RPAREN, "expected ')'");
        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for field argument pack");
            return step;
        }
        step->kind = PipelineStepKind::FieldArgPack;
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(std::move(a));
        step->packArgs = builder.build();
    }

    return step;
}

PipelineStepPtr Parser::parseIndexPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::IndexRef;
    step->ident = pool_.intern(ident);
    step->genericArgs = genericArgs;

    auto addIndex = [&](ExprPtr target, ExprPtr idx) -> ExprPtr {
        auto node = arena_.make<IndexExprAST>();
        node->target = std::move(target);
        node->index = std::move(idx);
        node->kind = IndexKind::Element;
        return node;
    };

    ts_.consume(TokenType::LBRACKET, "expected '['");
    ExprPtr idx = parseExpr();
    if (!idx) {
        errorAt(DiagCode::E2008, "expected index expression");
        int bracketDepth = 1;
        while (!ts_.isAtEnd() && bracketDepth > 0) {
            if (ts_.check(TokenType::LBRACKET)) ++bracketDepth;
            else if (ts_.check(TokenType::RBRACKET)) --bracketDepth;
            ts_.advance();
        }
        step->index = arena_.make<UnknownExprAST>();
        return step;
    }
    ts_.consume(TokenType::RBRACKET, "expected ']' after index");
    
    auto baseIdent = arena_.make<IdentifierExprAST>(pool_.intern(ident));
    baseIdent->loc = ts_.currentLoc();
    ExprPtr indexChain = addIndex(std::move(baseIdent), std::move(idx));

    while (ts_.check(TokenType::LBRACKET)) {
        ts_.advance();
        ExprPtr nextIdx = parseExpr();
        if (!nextIdx) {
            errorAt(DiagCode::E2008, "expected index expression");
            int bracketDepth = 1;
            while (!ts_.isAtEnd() && bracketDepth > 0) {
                if (ts_.check(TokenType::LBRACKET)) ++bracketDepth;
                else if (ts_.check(TokenType::RBRACKET)) --bracketDepth;
                ts_.advance();
            }
            break;
        }
        ts_.consume(TokenType::RBRACKET, "expected ']' after index");
        indexChain = addIndex(std::move(indexChain), std::move(nextIdx));
    }

    step->index = std::move(indexChain);

    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        std::vector<ExprPtr> packArgs;
        if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
        ts_.consume(TokenType::RPAREN, "expected ')'");
        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for array index argument pack");
            return step;
        }
        step->kind = PipelineStepKind::IndexArgPack;
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(std::move(a));
        step->packArgs = builder.build();
    }

    return step;
}

PipelineStepPtr Parser::parseArgPackPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::ArgPack;
    step->ident = pool_.intern(ident);
    step->genericArgs = genericArgs;

    ts_.consume(TokenType::LPAREN, "expected '('");
    std::vector<ExprPtr> packArgs;
    if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
    ts_.consume(TokenType::RPAREN, "expected ')'");
    
    if (!ts_.match(TokenType::BANG)) {
        errorAt(DiagCode::E2001, "expected '!' after arguments for function argument pack");
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        return step;
    }
    
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& a : packArgs) builder.push_back(std::move(a));
    step->packArgs = builder.build();
    return step;
}

ComposeOperandPtr Parser::parseComposeOperand() {
    auto op = arena_.make<ComposeOperandAST>();

    bool isPrimitiveType = isPrimitiveTypeToken(ts_.peekType());
    if (!ts_.check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        errorAt(DiagCode::E2002, "expected function name or method reference");
        return nullptr;
    }

    std::string name = ts_.advance().value;

    if (ts_.check(TokenType::COLON) && ts_.peekNextType() == TokenType::IDENTIFIER) {
        ts_.advance();
        std::string method = ts_.advance().value;
        op->kind = ComposeOperandKind::BehaviorRef;
        op->typeName = pool_.intern(name);
        op->method = pool_.intern(method);
    } else if (ts_.check(TokenType::DOT) && ts_.peekNextType() == TokenType::IDENTIFIER) {
        ts_.advance();
        std::string field = ts_.advance().value;
        op->kind = ComposeOperandKind::FieldRef;
        op->ident = pool_.intern(name);
        op->field = pool_.intern(field);
    } else {
        op->kind = ComposeOperandKind::Ident;
        op->ident = pool_.intern(name);
    }

    return op;
}

// ============================================================================
// Match Expression (Stub)
// ============================================================================
// 
// parseMatchExpr() parses the pattern‑matching expression.
// 
// Grammar: match expr { pattern [if guard] => expr [, expr], default => expr }
// 
// The actual pattern parsing (bind, wildcard, type, struct, literal, range)
// is implemented in ParserPattern.cpp. This function only:
//   1. Parses the subject expression
//   2. Collects match arms (calls parseMatchArm)
//   3. Enforces that a 'default' arm is present and last
//   4. Builds ArenaSpan for arms
// ============================================================================

ExprPtr Parser::parseMatchExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::MATCH, "expected 'match'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'match'");
        return arena_.make<UnknownExprAST>();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' after match subject");

    auto node = arena_.make<MatchExprAST>();
    node->loc = loc;
    node->subject = std::move(subject);

    std::vector<MatchArmPtr> arms;  // temporary
    bool hasDefault = false;

    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        if (ts_.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                errorAt(DiagCode::E2007, "duplicate 'default' arm in match expression");
            }
            node->defaultBody = parseDefaultArm();
            if (node->defaultBody) {
                node->defaultLoc = node->defaultBody->loc;
            }
            hasDefault = true;
            break;
        }

        size_t beforePos = ts_.getPos();
        MatchArmPtr arm = parseMatchArm();
        if (ts_.getPos() == beforePos) {
            errorAt(DiagCode::E2007, "failed to parse match arm, skipping");
            ts_.advance();
            continue;
        }
        if (!arm) {
            errorAt(DiagCode::E2007, "invalid match arm, skipping");
            continue;
        }
        arms.push_back(std::move(arm));
    }

    ts_.consume(TokenType::RBRACE, "expected '}' to close match expression");

    if (!hasDefault) {
        error(loc, DiagCode::E2006, "match expression must have a 'default' arm");
    }

    auto armsBuilder = arena_.makeBuilder<MatchArmPtr>();
    for (auto& a : arms) armsBuilder.push_back(std::move(a));
    node->arms = armsBuilder.build();

    return node;
}

// ============================================================================
// Lvalue Parsing (for Assignments)
// ============================================================================
// 
// parseLvalue() parses an assignable left‑hand side expression for multi‑assignment.
// 
// Grammar: IDENTIFIER { ( '.' IDENTIFIER ) | ( '[' expr ']' ) }
// 
// Examples: x, point.x, arr[i], matrix[row][col]
// 
// This is distinct from parseExpr() because it stops before operators like '='
// and does not allow behavior access (':') or function calls.
// ============================================================================

ExprPtr Parser::parseLvalue() {
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected identifier for lvalue");
        return nullptr;
    }
    std::string name = ts_.advance().value;
    ExprPtr expr = arena_.make<IdentifierExprAST>(pool_.intern(name));
    expr->loc = ts_.currentLoc();

    while (true) {
        if (ts_.check(TokenType::DOT)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.'");
                return expr;
            }
            std::string field = ts_.advance().value;
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = expr->loc;
            node->object = std::move(expr);
            node->field = pool_.intern(field);
            expr = std::move(node);
        } 
        else if (ts_.check(TokenType::LBRACKET)) {
            ts_.advance();
            ExprPtr index = parseExpr();
            if (!index) {
                errorAt(DiagCode::E2008, "expected index expression");
                return expr;
            }
            ts_.consume(TokenType::RBRACKET, "expected ']' after index");
            auto node = arena_.make<IndexExprAST>();
            node->loc = expr->loc;
            node->target = std::move(expr);
            node->index = std::move(index);
            node->kind = IndexKind::Element;
            expr = std::move(node);
        }
        else if (ts_.check(TokenType::COLON)) {
            break;
        }
        else {
            break;
        }
    }
    return expr;
}

/**
 * @brief Parses a `resolve` expression for structured error handling.
 * 
 * Grammar:
 *   resolve_expr := 'resolve' expr '{' ok_arm err_arm '}'
 * 
 * Example:
 *   resolve divide(10, 0) {
 *       ok  (v int)    { return v  }
 *       err (e string) { return -1 }
 *   }
 * 
 * ─── Semantics ─────────────────────────────────────────────────────────────
 *   - The `subject` must evaluate to a `T!E` type
 *   - The `ok` arm receives the unwrapped success value (plain T)
 *   - The `err` arm receives the error value (type E, or nothing for bare '!')
 *   - Both arms are required and must return the same type
 *   - After the `resolve` block, the `!` is consumed; the expression result
 *     is plain T (the type returned by the arms)
 * 
 * ─── Comparison with `??` ──────────────────────────────────────────────────
 *   `??` provides a fallback value and discards the error.
 *   `resolve` allows inspection of the error and full control flow.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'resolve' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing subject: returns UnknownExprAST
 * - Missing '{' after subject: reports error, returns UnknownExprAST
 * - Missing 'ok' arm: reports error, continues with null okArm
 * - Missing 'err' arm: reports error, continues with null errArm
 * - Missing '}': consume() reports error, returns node with whatever was parsed
 * 
 * @return ExprPtr – ResolveExprAST on success, UnknownExprAST on error
 */
ExprPtr Parser::parseResolveExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::RESOLVE, "expected 'resolve'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'resolve'");
        return arena_.make<UnknownExprAST>();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' after resolve subject");

    OkArmPtr okArm = parseOkArm();
    if (!okArm) {
        errorAt(DiagCode::E2006, "expected 'ok' arm in resolve expression");
    }

    ErrArmPtr errArm = parseErrArm();
    if (!errArm) {
        errorAt(DiagCode::E2006, "expected 'err' arm in resolve expression");
    }

    ts_.consume(TokenType::RBRACE, "expected '}' to close resolve expression");

    auto node = arena_.make<ResolveExprAST>();
    node->loc = loc;
    node->subject = std::move(subject);
    node->okArm = std::move(okArm);
    node->errArm = std::move(errArm);
    return node;
}

/**
 * @brief Parses the `ok` arm of a `resolve` expression.
 * 
 * Grammar:
 *   ok_arm := 'ok' '(' IDENTIFIER type ')' block
 * 
 * Example:
 *   ok (v int) { return v * 2 }
 * 
 * ─── Semantics ─────────────────────────────────────────────────────────────
 *   - The identifier is bound to the unwrapped success value
 *   - The type annotation is required and must be the success type T
 *   - The block is executed when the subject succeeded
 *   - The block's return type determines the type of the entire `resolve`
 *     expression
 * 
 * ─── Important Restriction ─────────────────────────────────────────────────
 *   The `bindType` is always plain T (never T!E). The `!` is consumed at the
 *   resolve boundary before the arm is entered.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'ok' keyword
 * On exit:  positioned after the closing '}' of the block
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '(' after 'ok': reports error, returns nullptr
 * - Missing identifier: reports error, returns nullptr
 * - Missing type after identifier: reports error, returns nullptr
 * - Missing ')' after type: reports error, returns nullptr
 * - Missing block: reports error, returns arm with null body
 * 
 * @return OkArmPtr – OkArmAST on success, nullptr on error
 */
OkArmPtr Parser::parseOkArm() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::OK, "expected 'ok'");
    ts_.consume(TokenType::LPAREN, "expected '(' after 'ok'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected identifier for ok arm binding");
        return nullptr;
    }
    InternedString bindName = pool_.intern(ts_.advance().value);

    TypePtr bindType = parseType();
    if (!bindType) {
        errorAt(DiagCode::E2005, "expected type for ok arm binding");
        return nullptr;
    }

    ts_.consume(TokenType::RPAREN, "expected ')' after ok arm type");

    StmtPtr body = parseBlock();
    if (!body) {
        errorAt(DiagCode::E2001, "expected block for ok arm");
        return nullptr;
    }

    auto arm = arena_.make<OkArmAST>();
    arm->loc = loc;
    arm->bindName = bindName;
    arm->bindType = std::move(bindType);
    arm->body = std::move(body);
    return arm;
}

/**
 * @brief Parses the `err` arm of a `resolve` expression.
 * 
 * Grammar:
 *   err_arm := 'err' '(' [ IDENTIFIER type ] ')' block
 * 
 * Examples:
 *   err (e string) { return -1 }    -- typed error
 *   err () { return 0 }             -- bare '!' (no error payload)
 * 
 * ─── Semantics ─────────────────────────────────────────────────────────────
 *   - For typed errors (E): the identifier is bound to the error value
 *   - For bare '!': the parentheses are empty; no binding is introduced
 *   - The block is executed when the subject failed
 *   - The block's return type must match the `ok` arm's return type
 * 
 * ─── Detection ─────────────────────────────────────────────────────────────
 *   The presence of an identifier before the ')' determines whether this is a
 *   typed error or a bare '!':
 *     - err (e string) → typed error (bindName = "e", bindType = string)
 *     - err ()         → bare '!' (bindName = "", bindType = nullptr)
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'err' keyword
 * On exit:  positioned after the closing '}' of the block
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '(' after 'err': reports error, returns nullptr
 * - If identifier present but type missing: reports error, returns nullptr
 * - Missing ')' after (optional) type: reports error, returns nullptr
 * - Missing block: reports error, returns arm with null body
 * 
 * @return ErrArmPtr – ErrArmAST on success, nullptr on error
 */
ErrArmPtr Parser::parseErrArm() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::ERR, "expected 'err'");
    ts_.consume(TokenType::LPAREN, "expected '(' after 'err'");

    InternedString bindName;
    TypePtr bindType;

    // Check for optional identifier and type (bare '!' case)
    if (!ts_.check(TokenType::RPAREN)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier for err arm binding");
        } else {
            bindName = pool_.intern(ts_.advance().value);
            bindType = parseType();
            if (!bindType) {
                errorAt(DiagCode::E2005, "expected type for err arm binding");
            }
        }
    }

    ts_.consume(TokenType::RPAREN, "expected ')' after err arm");

    StmtPtr body = parseBlock();
    if (!body) {
        errorAt(DiagCode::E2001, "expected block for err arm");
        return nullptr;
    }

    auto arm = arena_.make<ErrArmAST>();
    arm->loc = loc;
    arm->bindName = bindName;
    arm->bindType = std::move(bindType);
    arm->body = std::move(body);
    return arm;
}
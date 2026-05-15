/**
 * @file ParserExpr.cpp
 *
 * @responsibility Implements the Pratt Parser for all LUC expressions.
 *
 * @grammar_rules Literals, Binary Ops, Calls, Indexing, Match Expr, Pipelines, Composition.
 *
 * @related src/diagnostics/DiagnosticEngine.hpp, DiagnosticCodes.hpp
 *
 * @note This is the "high-traffic" area for logic changes.
 *       Operator precedence is defined in `infixPrec()`.
 */

#include "Parser.hpp"
#include "ast/BaseAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <cassert>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ParserExpr.cpp
//
// Implements the full expression parser (Pratt / top-down operator precedence)
// and all pattern parsing (used exclusively inside match expressions).
//
// Precedence table (from LUC_GRAMMAR.md §Operator Precedence), encoded as
// integer levels used by parsePrattExpr:
//
//   PREC_ASSIGN   = 1   =  +=  -=  *=  /=  ^=  %=  &&=  ||=  ~^=  <<=  >>=  (right-assoc)
//   PREC_COMPOSE  = 2   +>                                   (left-assoc)
//   PREC_PIPELINE = 3   |>                                   (left-assoc)
//   PREC_NULLCOAL = 4   ??                                   (right-assoc)
//   PREC_OR       = 5   or
//   PREC_AND      = 6   and
//   PREC_CMP      = 7   == != < > <= >=  is
//   PREC_BITWISE  = 8   & | ~^ ~  <<  >>
//   PREC_SHIFT    = 9   << >>              (sub-level of BITWISE)
//   PREC_ADD      = 10  + -
//   PREC_MUL      = 11  * / %
//   PREC_POW      = 12  ^                                    (right-assoc)
//
// Levels are deliberately spaced so a "one higher than current" right-recursive
// call for right-associative operators is just minPrec + 1.
//
// Postfix operations (call, index, '.', ':', '?.') are handled at the top of
// parsePrattExpr via parsePostfixExpr rather than as infix operators in the
// precedence table — they are always left-associative and bind tighter than
// any binary op.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
    // Precedence levels — private to this TU.
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
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Precedence helpers
// ─────────────────────────────────────────────────────────────────────────────

int Parser::infixPrec(TokenType t) const {
    switch (t) {
        // Assignment — lowest, right-associative, handled separately.
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
        case TokenType::ARROW:              return PREC_PIPE;   // for '->'
        case TokenType::PIPELINE:           return PREC_PIPE;   // for '|>'
        case TokenType::QUESTION_QUESTION:  return PREC_NULLCOAL;
        case TokenType::OR:                 return PREC_OR;
        case TokenType::AND:                return PREC_AND;

        // Comparison operators — value equality, reference equality, ordering, type check
        case TokenType::EQUAL_EQUAL:        // ==   value equality
        case TokenType::EQUAL_EQUAL_EQUAL:  // ===  reference equality
        case TokenType::NOT_EQUAL:          // !=
        case TokenType::LESS:
        case TokenType::GREATER:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER_EQUAL:
        case TokenType::IS:
            return PREC_CMP;

        // Bitwise operators — && and || instead of & and | to avoid ambiguity
        case TokenType::BIT_AND:   // &&
        case TokenType::BIT_OR:    // ||
        case TokenType::BIT_XOR:   // ~^
        case TokenType::SHL:       // <<
        case TokenType::SHR:       // >>
            return PREC_BITWISE;

        // BIT_NOT (~) is unary only — not an infix operator, no precedence here.
        // AMPERSAND (&) is the reference operator in expression context — not bitwise.

        case TokenType::PLUS:
        case TokenType::MINUS:
            return PREC_ADD;

        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
            return PREC_MUL;

        case TokenType::POW:
            return PREC_POW;

        // RANGE '..' — handled inside parsePostfixExpr, not as an infix op.
        // Returning PREC_NONE here prevents the Pratt loop from consuming it,
        // which is correct: ranges only appear as standalone expressions in
        // for/match/index contexts, not as general infix operators.
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
        case TokenType::BIT_AND:             return BinaryOp::BitAnd;  // &&
        case TokenType::BIT_OR:              return BinaryOp::BitOr;   // ||
        case TokenType::BIT_XOR:             return BinaryOp::BitXor;  // ~^
        case TokenType::SHL:                 return BinaryOp::Shl;
        case TokenType::SHR:                 return BinaryOp::Shr;
        default:
            // BIT_NOT is unary only — should never reach here.
            // AMPERSAND is reference operator in expression context — not bitwise.
            return BinaryOp::Add; // unreachable, satisfy compiler
    }
}

AssignOp Parser::tokenToAssignOp(TokenType t) const {
    switch (t) {
        case TokenType::ASSIGN:
            return AssignOp::Assign;
        case TokenType::PLUS_ASSIGN:
            return AssignOp::AddAssign;
        case TokenType::MINUS_ASSIGN:
            return AssignOp::SubAssign;
        case TokenType::MUL_ASSIGN:
            return AssignOp::MulAssign;
        case TokenType::DIV_ASSIGN:
            return AssignOp::DivAssign;
        case TokenType::POW_ASSIGN:
            return AssignOp::PowAssign;
        case TokenType::MOD_ASSIGN:
            return AssignOp::ModAssign;
        case TokenType::BIT_AND_ASSIGN:
            return AssignOp::BitAndAssign;
        case TokenType::BIT_OR_ASSIGN:
            return AssignOp::BitOrAssign;
        case TokenType::BIT_XOR_ASSIGN:
            return AssignOp::BitXorAssign;
        case TokenType::SHL_ASSIGN:
            return AssignOp::ShlAssign;
        case TokenType::SHR_ASSIGN:
            return AssignOp::ShrAssign;
        default:
            return AssignOp::Assign;
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

// ─────────────────────────────────────────────────────────────────────────────
// parseExpr  — root entry point
//
// Parses a full expression including assignment operators (the lowest
// precedence level).  Starts the Pratt loop at PREC_NONE so everything
// binds.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseExpr(bool allowStructLiteral) {
    LUC_LOG_EXPR("=== parseExpr START (allowStructLiteral=" << allowStructLiteral << ") ===");
    ExprPtr result = parsePrattExpr(PREC_NONE, allowStructLiteral);
    LUC_LOG_EXPR("=== parseExpr END ===");
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePrattExpr  — Pratt / top-down operator precedence loop
//
// Algorithm:
//   1. Parse a prefix/primary expression as the initial lhs.
//   2. Apply all postfix operations (calls, indexing, field/behavior access,
//      nullable chains) to get the fully-decorated lhs.
//   3. While the current token is an infix operator with precedence > minPrec:
//        a. If it is an assignment op → build AssignExprAST (right-assoc).
//        b. If it is 'is'            → build IsExprAST.
//        c. If it is '->'            → build PipelineExprAST.
//        d. If it is '+>'            → build ComposeExprAST.
//        e. If it is '??'            → build NullableChainExprAST or wrap lhs.
//        f. Otherwise                → build BinaryExprAST.
//   4. Return lhs.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parsePrattExpr(int minPrec, bool allowStructLiteral) {
    LUC_LOG_EXPR_VERBOSE("parsePrattExpr: minPrec=" << minPrec << ", token='" << peek().value << "'");

    ExprPtr lhs = parsePrefixExpr(allowStructLiteral);
    if (!lhs) {
        LUC_LOG_EXPR("parsePrattExpr: prefix parsing failed");
        return arena_.make<UnknownExprAST>();
    }

    // Apply postfix operators before entering the infix loop.
    LUC_LOG_EXPR_VERBOSE("parsePrattExpr: lhs kind=" << LucDebug::kindToString(lhs->kind));
    lhs = parsePostfixExpr(std::move(lhs));

    while (true) {
        int prec = infixPrec(peek().type);
        if (prec <= minPrec) {
            LUC_LOG_EXPR_EXTREME("parsePrattExpr: stopping - prec=" << prec << " <= minPrec=" << minPrec);
            break;
        }

        TokenType opTok = peek().type;
        LUC_LOG_EXPR_VERBOSE("parsePrattExpr: found infix operator '" 
                             << LucDebug::tokenTypeToString(opTok) 
                             << "' with prec=" << prec);

        // ── Assignment (right-associative) ────────────────────────────────────
        if (isAssignOp(opTok)) {
            AssignOp op = tokenToAssignOp(opTok);
            advance();
            // Right-associative: recurse at the same precedence level.
            ExprPtr rhs = parsePrattExpr(PREC_ASSIGN - 1, allowStructLiteral);
            if (!rhs) {
                errorAt(DiagCode::E2008, "expected expression after assignment operator");
                break;
            }

            SourceLocation loc = lhs->loc;
            auto node = arena_.make<AssignExprAST>();
            node->loc = loc;
            node->op = op;
            node->lhs = std::move(lhs);
            node->rhs = std::move(rhs);
            lhs = std::move(node);
            // Assignment is a statement-level expression — stop the loop.
            LUC_LOG_EXPR_VERBOSE("parsePrattExpr: returning with kind=" << LucDebug::kindToString(lhs->kind));
            break;
        }

        // ── 'is' type check ───────────────────────────────────────────────────
        if (opTok == TokenType::IS) {
            advance(); // consume 'is'
            TypePtr checkType = parseType();
            if (!checkType) {
                errorAt(DiagCode::E2005, "expected type after 'is'");
                break;
            }

            SourceLocation loc = lhs->loc;
            auto node = arena_.make<IsExprAST>();
            node->loc = loc;
            node->expr = std::move(lhs);
            node->checkType = std::move(checkType);
            lhs = std::move(node);
            // 'is' is a comparison — continue at PREC_CMP level.
            continue;
        }

        // ── '|>' pipeline ─────────────────────────────────────────────────────
        if (opTok == TokenType::PIPELINE) {
            lhs = parsePipelineExpr(std::move(lhs));
            // parsePipelineExpr consumed all '|>' steps; continue the outer loop.
            continue;
        }

        // ── '+>' composition ──────────────────────────────────────────────────
        if (opTok == TokenType::COMPOSE) {
            lhs = parseComposeExpr(std::move(lhs));
            continue;
        }

        // ── '??' null coalescing ──────────────────────────────────────────────
        if (opTok == TokenType::QUESTION_QUESTION) {
            advance(); // consume '??'
            // Right-associative.
            ExprPtr fallback = parsePrattExpr(PREC_NULLCOAL - 1, allowStructLiteral);
            if (!fallback) {
                errorAt(DiagCode::E2008, "expected expression after '\?\?'");
                break;
            }

            SourceLocation loc = lhs->loc;
            auto node = arena_.make<NullCoalesceExprAST>();
            node->loc = loc;
            node->value = std::move(lhs);
            node->fallback = std::move(fallback);
            lhs = std::move(node);
            
            break; // '??' terminates the chain — nothing binds tighter
        }

        // ── Standard binary operators ─────────────────────────────────────────
        advance(); // consume the operator

        // Right-associative: POW (^)
        int nextPrec = (opTok == TokenType::POW) ? prec - 1 : prec;

        ExprPtr rhs = parsePrattExpr(nextPrec, allowStructLiteral);
        if (!rhs) {
            errorAt(DiagCode::E2008, "expected right-hand side of binary expression");
            break;
        }

        // ── Chained comparison detection ──────────────────────────────────────
        // After building  lhs op rhs, if the next token is ALSO a comparison
        // operator, this is a chained comparison: 0 < x < 10.
        // In Luc this is always an error — the developer must use 'and':
        //   if 0 < x and x < 10 { ... }
        //
        // We detect it here (parse time) so the error message is specific.
        // The semantic pass would also catch it via bool-on-left-of-comparison,
        // but a parser-level message is clearer.
        //
        // Comparison tokens: == === != < > <= >= is
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

        bool currentIsComparison = isComparisonOp(opTok);
        bool nextIsComparison    = isComparisonOp(peek().type);

        if (currentIsComparison && nextIsComparison) {
            errorAt(DiagCode::E3014,
                    "chained comparisons are not allowed; "
                    "use 'and' explicitly, e.g. '0 < x and x < 10'");
            // Continue parsing to surface any further errors, but the AST
            // produced here is semantically invalid. The semantic pass will
            // also catch this via type checking (bool on left of comparison).
        }

        SourceLocation loc = lhs->loc;
        auto node = arena_.make<BinaryExprAST>();
        node->loc = loc;
        node->op = tokenToBinaryOp(opTok);
        node->left = std::move(lhs);
        node->right = std::move(rhs);
        lhs = std::move(node);

        // After a binary op, apply postfix again (e.g. a + b.x)
        lhs = parsePostfixExpr(std::move(lhs));
    }

    return lhs;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePrefixExpr  — unary prefix operators and primary expressions
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parsePrefixExpr(bool allowStructLiteral) {
    LUC_LOG_EXPR_VERBOSE("parsePrefixExpr: token='" << peek().value << "'");
    SourceLocation loc = currentLoc();

    switch (peek().type) {
        case TokenType::MINUS: {
            advance();
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
            advance();
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
            advance();
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
            // '&' in expression position is always the unary reference operator (&x).
            // Bitwise AND uses '&&' (BIT_AND token) to avoid ambiguity with &T.
            advance();
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

// ─────────────────────────────────────────────────────────────────────────────
// parsePrimaryExpr  — atoms: literals, identifiers, grouped, special forms
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parsePrimaryExpr(bool allowStructLiteral) {
    LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: allowStructLiteral=" << allowStructLiteral 
                         << ", token='" << peek().value << "'");
    SourceLocation loc = currentLoc();

    // ── match expression ──────────────────────────────────────────────────────
    if (check(TokenType::MATCH)) {
        LUC_LOG_EXPR("parsePrimaryExpr: parsing match expression");
        return parseMatchExpr();
    }

    // ── if expression ─────────────────────────────────────────────────────────
    // 'if' in expression position (after '=' in a body or inside any expression).
    // The statement version (IfStmtAST) is produced by parseIfStmt() when 'if'
    // appears at the start of a statement.  Here we always produce IfExprAST.
    if (check(TokenType::IF)) {
        LUC_LOG_EXPR("parsePrimaryExpr: parsing if expression");
        return parseIfExpr();
    }

    // ── #intrinsic call ───────────────────────────────────────────────────────
    // '#' IDENTIFIER '(' args ')'  — compiler-builtin call.
    // Examples:  #sizeof(Vec2)   #memcpy(dst, src, n)   #sqrt(x)
    if (check(TokenType::HASH)) {
        LUC_LOG_EXPR("parsePrimaryExpr: parsing # intrinsic");
        return parseIntrinsicCallExpr();
    }

    // ── await ─────────────────────────────────────────────────────────────────
    if (check(TokenType::AWAIT)) {
        LUC_LOG_EXPR("parsePrimaryExpr: await");
        return parseAwaitExpr();
    }

    // ── array literal ─────────────────────────────────────────────────────────
    if (check(TokenType::LBRACKET))
        return parseArrayLiteralExpr();

    // Bare '{' in expression position - this is invalid Luc syntax
    // Valid blocks always have a prefix: TypeName, 'match', '(', or 'async'
    if (check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2007, 
                "unexpected block in expression position. "
                "Did you mean a struct literal (TypeName { ... }), "
                "match expression (match ... { ... }), "
                "or anonymous function ((params) { ... })?");

        // Error recovery: consume the entire block to avoid cascading errors
        consume(TokenType::LBRACE, "expected '{'");
        int braceDepth = 1;
        while (!isAtEnd() && braceDepth > 0) {
            if (match(TokenType::LBRACE)) {
                braceDepth++;
            } else if (match(TokenType::RBRACE)) {
                braceDepth--;
            } else {
                advance();
            }
        }
        
        // Return nullptr - caller will handle the error
        return arena_.make<UnknownExprAST>();
    }

    // ── anonymous function (non-async) ────────────────────────────────────────
    // A '(' that is immediately followed by ')' or 'IDENTIFIER type' is an anon
    // func. Distinguish from a grouped expression by lookahead.
    // Heuristic: if current is '(' and:
    //   - next is ')' (empty params)
    //   - next is IDENTIFIER and the token after that looks like a type start
    // then it's an anonymous function.
    if (check(TokenType::LPAREN)) {
        auto isTypeStart = [&](TokenType tt) {
            if (Parser::isPrimitiveTypeToken(tt))
                return true;
            switch (tt) {
                case TokenType::IDENTIFIER:
                case TokenType::LBRACKET:
                case TokenType::AMPERSAND:
                case TokenType::MUL:
                case TokenType::LPAREN:
                case TokenType::VARIADIC:
                    return true;
                default:
                    return false;
            }
        };

        // Lookahead to distinguish anon func from grouped expr.
        // An anon func must have ')' as its only next token (empty), or
        // IDENTIFIER followed by something that looks like a type.
        TokenType n1 = peekNext().type;
        if (n1 == TokenType::RPAREN) {
            // Could be  ()  — could be empty anon func or empty group.
            // Peek further: after ')' comes a type start or '{' → anon func.
            TokenType n2 = peekAt(2).type;
            bool anonFunc = (n2 == TokenType::LBRACE) || // () { ... }
                            isTypeStart(n2);             // () RetType { ... } — approximate
            
            if (n2 == TokenType::LBRACE || isTypeStart(n2)) {
                return parseAnonFuncExpr();
            }
            // Otherwise it is  ()  as a grouped unit (unusual but legal).
            // Fall through to grouped expression.
        } else if (n1 == TokenType::IDENTIFIER) {
            // Could be  (name type ...)  — anon func if token after name is a type.
            TokenType n2 = peekAt(2).type;

            if (isTypeStart(n2)) {
                return parseAnonFuncExpr();
            }
        }

        // ── grouped expression: '(' expr ')' ──────────────────────────────────
        advance(); // consume '('

        // Parse the inner expression - use parsePrattExpr directly to get full expression
        ExprPtr inner = parsePrattExpr(PREC_NONE);

        // IMPORTANT: Consume the closing ')' and VERIFY it's consumed
        if (!check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2001, "expected ')' to close grouped expression");
        } else {
            LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: about to consume ')', current token='" << peek().value << "'");
            advance(); // consume ')'
            LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: after consuming ')', new token='" << peek().value << "'");
        }
        // Return the inner expression directly - do NOT call parsePostfixExpr here
        return inner;
    }

    // ── '*' unsafe explicit cast  *T(expr) ────────────────────────────────────
    if (check(TokenType::MUL)) {
        advance(); // consume '*'
        TypePtr targetType = parseBaseType();
        if (!targetType) {
            errorAt(DiagCode::E2005, "expected type after '*' in unsafe cast");
            return arena_.make<UnknownExprAST>();
        }
        if (!check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' after type in unsafe cast '*T(expr)'");
            return arena_.make<UnknownExprAST>();
        }
        return parseTypeConvExpr(/*isUnsafe=*/true, std::move(targetType));
    }

    // ── Identifier — struct literal, behavior access, or plain name ─────────
    if (check(TokenType::IDENTIFIER)) {
        std::string name = peek().value;

        // Struct literal: IDENTIFIER [ '<' ... '>' ] '{'
        if (allowStructLiteral && looksLikeStructLiteral()) {
            advance(); // consume IDENTIFIER
            std::vector<TypePtr> genericArgs;
            if (check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            return parseStructLiteralExpr(std::move(name), std::move(genericArgs));
        }

        // ── Behavior access: IDENTIFIER [ '<' type { ',' type } '>' ] ':' IDENTIFIER ──
        // Perform a non‑destructive lookahead to verify the pattern.
        std::size_t savedPos = pos_;
        bool isBehavior = false;
        std::vector<TypePtr> genericArgs;
        std::string methodName;

        // Manual scan: we need to check if there is an optional '<' ... '>' followed by ':' and an identifier.
        // We'll use a temporary index `scan` that starts after the current token (the identifier).
        std::size_t scan = savedPos + 1; // position after the identifier

        // Skip any comments before checking for '<'
        while (scan < tokens_.size() && (tokens_[scan].type == TokenType::LINE_COMMENT ||
                                        tokens_[scan].type == TokenType::DOC_COMMENT))
            ++scan;

        // If the next token is '<', try to parse generic arguments.
        if (scan < tokens_.size() && tokens_[scan].type == TokenType::LESS) {
            // Count brackets to find the matching '>'
            int depth = 1;
            ++scan; // move past '<'
            while (scan < tokens_.size() && depth > 0) {
                // Skip comments inside the generic argument list
                while (scan < tokens_.size() && (tokens_[scan].type == TokenType::LINE_COMMENT ||
                                                tokens_[scan].type == TokenType::DOC_COMMENT))
                    ++scan;
                if (scan >= tokens_.size()) break;
                TokenType tt = tokens_[scan].type;
                if (tt == TokenType::LESS) ++depth;
                else if (tt == TokenType::GREATER) --depth;
                ++scan;
            }
            if (depth == 0) {
                // Now after the '>', skip comments and check for ':'
                while (scan < tokens_.size() && (tokens_[scan].type == TokenType::LINE_COMMENT ||
                                                tokens_[scan].type == TokenType::DOC_COMMENT))
                    ++scan;
                if (scan < tokens_.size() && tokens_[scan].type == TokenType::COLON) {
                    ++scan; // move past ':'
                    while (scan < tokens_.size() && (tokens_[scan].type == TokenType::LINE_COMMENT ||
                                                    tokens_[scan].type == TokenType::DOC_COMMENT))
                        ++scan;
                    if (scan < tokens_.size() && tokens_[scan].type == TokenType::IDENTIFIER) {
                        isBehavior = true;
                        methodName = tokens_[scan].value;
                    }
                }
            }
        } else {
            // No generic args: check for ':' directly
            if (scan < tokens_.size() && tokens_[scan].type == TokenType::COLON) {
                ++scan; // move past ':'
                while (scan < tokens_.size() && (tokens_[scan].type == TokenType::LINE_COMMENT ||
                                                tokens_[scan].type == TokenType::DOC_COMMENT))
                    ++scan;
                if (scan < tokens_.size() && tokens_[scan].type == TokenType::IDENTIFIER) {
                    isBehavior = true;
                    methodName = tokens_[scan].value;
                }
            }
        }

        if (isBehavior) {
            // Commit: consume the identifier, then optional generic args, then ':', then method name.
            advance(); // consume the identifier (name)
            if (check(TokenType::LESS)) {
                genericArgs = parseGenericArgs(); // consumes '<' ... '>'
            }
            // Consume ':'
            if (!check(TokenType::COLON)) {
                errorAt(DiagCode::E2001, "expected ':' in behavior access");
            } else {
                advance();
            }
            // Consume method name
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected method name after ':'");
                return arena_.make<UnknownExprAST>();
            }
            methodName = advance().value;

            auto node = arena_.make<BehaviorAccessExprAST>();
            node->loc = loc;
            node->typeName = pool_.intern(name);
            node->genericArgs = std::move(genericArgs);
            node->method = pool_.intern(methodName);
            node->isBehaviorMember = true;
            return node;
        }

        // ── Fallback: plain identifier ──────────────────────────────────────
        advance();
        auto node = arena_.make<IdentifierExprAST>(pool_.intern(name));
        node->loc = loc;
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: returning identifier");
        return node;
    }

    // ── Primitive type keywords used as casting functions: float(x) ────────────
    if (looksLikeType() && peekNext().type == TokenType::LPAREN) {
        // e.g. float(x), int(x), string(n)
        TypePtr targetType = parsePrimitiveType();
        if (targetType && check(TokenType::LPAREN)) {
            return parseTypeConvExpr(/*isUnsafe=*/false, std::move(targetType));
        }
        // If parsePrimitiveType consumed the token but no '(' follows,
        // this was a standalone type keyword in expression position —
        // unusual; fall through will produce an error.
    }

    // ── Scalar literals ───────────────────────────────────────────────────────
    switch (peek().type) {
        case TokenType::INT_LITERAL:
        case TokenType::FLOAT_LITERAL:
        case TokenType::STRING_LITERAL:
        case TokenType::RAW_STRING_LITERAL:
        case TokenType::CHAR_LITERAL:
        case TokenType::HEX_LITERAL:
        case TokenType::BINARY_LITERAL:
        case TokenType::TRUE:
        case TokenType::FALSE:
        case TokenType::NIL:
            return parseLiteralExpr();
        default:
            break;
    }

    // ── Nothing matched ───────────────────────────────────────────────────────
    errorAt(DiagCode::E2002, "expected expression, got '" + peek().value + "'");
    return arena_.make<UnknownExprAST>();
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePostfixExpr  — apply all postfix operators to an already-parsed lhs
//
// Postfix operators (highest precedence, left-associative):
//   '(' args ')'        — function call
//   '<' types '>' '(' ) — generic call
//   '[' expr ']'        — element index
//   '[' expr '..' expr ']' — slice index
//   '.' IDENTIFIER      — field access (data)
//   ':' IDENTIFIER      — NOT handled here (already parsed in parsePrimaryExpr
//                          via BehaviorAccessExprAST when lhs is IDENTIFIER)
//   '?.' IDENTIFIER     — nullable chain step
//   '!!'                — not valid here (only inside pipeline steps)
//
// IMPORTANT: Does NOT handle '|>' (pipeline) or '+>' (composition) - those
// are handled at a higher precedence level in parsePrattExpr.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parsePostfixExpr(ExprPtr lhs) {
    LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: lhs kind=" << LucDebug::kindToString(lhs->kind));
    while (true) {

        // Stop at closing parenthesis - they are handled by the grouped expression parser
        if (check(TokenType::RPAREN)) {
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: stopping at ')'");
            break;
        }


        // Stop at pipeline or composition operators - they are handled by parsePrattExpr
        if (check(TokenType::ARROW) || check(TokenType::COMPOSE)) {
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: stopping at pipeline/compose operator");
            break;
        }
        
        // ── Function call: lhs '(' args ')' ──────────────────────────────────
        if (check(TokenType::LPAREN)) {
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: function call");
            lhs = parseCallExpr(std::move(lhs), {});
            continue;
        }

        // ── Generic call: lhs '<' types '>' '(' args ')' ─────────────────────
        if (check(TokenType::LESS) &&
            (lhs->isa<IdentifierExprAST>() || lhs->isa<BehaviorAccessExprAST>())) {
            // Save position so we can roll back if this is actually '<' comparison.
            std::size_t savedPos = pos_;
            std::vector<TypePtr> genericArgs;
            bool ok = false;
            // Attempt to parse generic args.
            int depth = 1;
            std::size_t i = pos_ + 1;
            while (i < tokens_.size() && depth > 0) {
                if (tokens_[i].type == TokenType::LESS)
                    ++depth;
                else if (tokens_[i].type == TokenType::GREATER) {
                    --depth;
                    if (depth == 0)
                        break;
                } else if (tokens_[i].type == TokenType::EOF_TOKEN)
                    break;
                ++i;
            }
            // If after the closing '>' we see '(', commit to generic call.
            if (depth == 0 && i + 1 < tokens_.size() &&
                tokens_[i + 1].type == TokenType::LPAREN) {
                genericArgs = parseGenericArgs();
                ok = true;
            }
            if (ok) {
                lhs = parseCallExpr(std::move(lhs), std::move(genericArgs));
                continue;
            }
            // Not a generic call — leave '<' for the binary op loop.
        }

        // ── Index / slice: lhs '[' expr ']' or lhs '[' expr '..' expr ']' ────
        if (check(TokenType::LBRACKET)) {
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: index/slice");
            lhs = parseIndexExpr(std::move(lhs));
            continue;
        }

        // ── Field access: lhs '.' IDENTIFIER ─────────────────────────────────
        if (check(TokenType::DOT)) {
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: field access");
            advance(); // consume '.'
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.'");
                break;
            }
            std::string field = advance().value;
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = lhs->loc;
            node->object = std::move(lhs);
            node->field = std::move(pool_.intern(field));
            lhs = std::move(node);
            continue;
        }

        // ── Nullable chain step: lhs '?.' IDENTIFIER ─────────────────────────
        if (check(TokenType::QUESTION_DOT)) {
            // Start or extend a NullableChainExprAST.
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: nullable chain");
            NullableChainExprAST* existing =
                lhs->isa<NullableChainExprAST>() ? lhs->as<NullableChainExprAST>() : nullptr;

            if (!existing) {
                // Start a new chain.
                auto chain = arena_.make<NullableChainExprAST>();
                chain->loc = lhs->loc;
                chain->object = std::move(lhs);
                lhs = std::move(chain);
                existing = static_cast<NullableChainExprAST *>(lhs.get());
            }

            advance(); // consume '?.'
            
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '?.'");
                break;
            }
            existing->steps.push_back(pool_.intern(advance().value));
            continue;
        }

        // ── '..' range — only valid in specific contexts, stop here ───────────
        // (for loops, match patterns, slice index — handled by their own parsers)
        break;
    }

    LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: returning with " << LucDebug::kindToString(lhs->kind));
    return lhs;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseLiteralExpr
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseLiteralExpr() {
    LUC_LOG_EXPR_VERBOSE("parseLiteralExpr: token='" << peek().value << "'");
    SourceLocation loc = currentLoc();
    Token tok = advance();

    LiteralKind kind;
    switch (tok.type) {
        case TokenType::INT_LITERAL:
            kind = LiteralKind::Int;
            break;
        case TokenType::FLOAT_LITERAL:
            kind = LiteralKind::Float;
            break;
        case TokenType::STRING_LITERAL:
            kind = LiteralKind::String;
            break;
        case TokenType::RAW_STRING_LITERAL:
            kind = LiteralKind::RawString;
            break;
        case TokenType::CHAR_LITERAL:
            kind = LiteralKind::Char;
            break;
        case TokenType::HEX_LITERAL:
            kind = LiteralKind::Hex;
            break;
        case TokenType::BINARY_LITERAL:
            kind = LiteralKind::Binary;
            break;
        case TokenType::TRUE:
            kind = LiteralKind::True;
            break;
        case TokenType::FALSE:
            kind = LiteralKind::False;
            break;
        case TokenType::NIL:
            kind = LiteralKind::Nil;
            break;
        default:
            errorAt(DiagCode::E2002, "internal error: parseLiteralExpr on non-literal token");
            return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<LiteralExprAST>(kind, pool_.intern(tok.value));
    node->loc = loc;
    LUC_LOG_EXPR_VERBOSE("parseLiteralExpr: created " << LucDebug::kindToString(static_cast<ASTKind>(node->kind)));
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseArrayLiteralExpr
//
// Grammar:  '[' [ expr { ',' expr } ] ']'
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseArrayLiteralExpr() {
    LUC_LOG_EXPR("parseArrayLiteralExpr: parsing array literal");
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACKET, "expected '['");

    auto node = arena_.make<ArrayLiteralExprAST>();
    node->loc = loc;

    while (!check(TokenType::RBRACKET) && !isAtEnd()) {
        match(TokenType::COMMA);
        if (check(TokenType::RBRACKET)) break;

        size_t beforePos = pos_;
        ExprPtr elem = parseExpr();
        if (pos_ == beforePos) {
            // parseExpr made no progress
            errorAt(DiagCode::E2008, "expected expression inside array literal");
            // Skip the current token to avoid infinite loop
            advance();
            break;
        }
        node->elements.push_back(std::move(elem));
    }

    consume(TokenType::RBRACKET, "expected ']' to close array literal");
    LUC_LOG_EXPR_VERBOSE("parseArrayLiteralExpr: parsed " << node->elements.size() << " elements");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseStructLiteralExpr
//
// Grammar:  IDENTIFIER [genericArgs] '{' { IDENTIFIER '=' expr } '}'
//
// Called after the type name (and optional generic args) have already been read.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseStructLiteralExpr(std::string typeName, std::vector<TypePtr> genericArgs) {
    LUC_LOG_EXPR("parseStructLiteralExpr: type='" << typeName << "', genericArgs=" << genericArgs.size());
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACE, "expected '{' to open struct literal");

    auto node = arena_.make<StructLiteralExprAST>();
    node->loc = loc;
    node->typeName = std::move(pool_.intern(typeName));
    node->genericArgs = std::move(genericArgs);

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::COMMA);
        match(TokenType::SEMICOLON);
        if (check(TokenType::RBRACE))
            break;

        SourceLocation fieldLoc = currentLoc();

        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected field name in struct literal");
            synchronize();
            if (check(TokenType::RBRACE) || isAtEnd())
                break;
            continue;
        }
        std::string fieldName = advance().value;

        consume(TokenType::ASSIGN, "expected '=' after field name '" + fieldName + "' in struct literal");

        // Progress tracking
        std::size_t savedPos = pos_;
        ExprPtr val = parseExpr();
        if (pos_ == savedPos) {
            errorAt(DiagCode::E2008, "expected expression for field '" + fieldName + "' in struct literal");
            if (!isAtEnd()) advance(); // consume the offending token to avoid infinite loop
            continue;
        }

        // parseExpr never returns null, but keep a defensive check
        if (!val) {
            errorAt(DiagCode::E2008, "expected expression for field '" + fieldName + "' in struct literal");
            continue;
        }

        auto init = arena_.make<FieldInitAST>(pool_.intern(fieldName), std::move(val));
        init->loc = fieldLoc;
        node->inits.push_back(std::move(init));
    }

    consume(TokenType::RBRACE, "expected '}' to close struct literal");
    LUC_LOG_EXPR_VERBOSE("parseStructLiteralExpr: parsed " << node->inits.size() << " field inits");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseAnonFuncExpr
//
// Grammar:
//   anon_func := param_group { param_group } [ '->' return_list ] block
//
// Notes:
//   - Anonymous functions CANNOT have qualifiers (~async, ~nullable). They are
//     plain values. Qualifiers belong on declarations or parameter types.
//   - Multiple parameter groups = curried anonymous function.
//   - Return list after '->' can contain multiple types (comma separated).
//   - No nullable suffix '?' – anonymous functions are never nil.
//
// Examples:
//   (x int) -> int { return x * 2 }
//   (a int)(b int) -> int { return a + b }
//   (src string) -> int, string { ... }
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseAnonFuncExpr() {
    SourceLocation loc = currentLoc();
    
    auto node = arena_.make<AnonFuncExprAST>();
    node->loc = loc;
    
    // ── Anonymous functions cannot have qualifiers ─────────────────────────
    if (check(TokenType::TILDE)) {
        errorAt(DiagCode::E2002,
                "anonymous function cannot have qualifiers (e.g., ~async, ~nullable). "
                "Qualifiers belong on declarations, not values.");
        // Skip the qualifier(s) to recover
        while (check(TokenType::TILDE)) {
            advance(); // consume '~'
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            advance(); // consume qualifier name
        }
    }
    
    // ── Parse parameter groups (curried) ──────────────────────────────────
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start anonymous function parameters");
        return arena_.make<UnknownExprAST>();
    }
    
    while (check(TokenType::LPAREN)) {
        node->sig.paramGroups.push_back(parseParamGroup());
        LUC_LOG_EXPR_VERBOSE("\tparsed param group with " 
                            << node->sig.paramGroups.back().size() << " params");
    }
    
    // ── '->' and return list (multiple returns) ──────────────────
    if (check(TokenType::ARROW)) {
        advance(); // consume '->' manually to guarantee progress
        node->sig.returnTypes = parseReturnList();
        if (node->sig.returnTypes.empty() && !check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2005, "expected return type after '->' in anonymous function");
        }
    } else {
        // void anonymous function (no return types)
        LUC_LOG_EXPR_VERBOSE("\tvoid anonymous function (no return types)");
    }
    
    // ── Body block must follow ────────────────────────────────────────────
    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start anonymous function body");
        return arena_.make<UnknownExprAST>();
    }
    node->body = parseBlock();
    
    LUC_LOG_EXPR_VERBOSE("parseAnonFuncExpr: paramGroups=" << node->sig.paramGroups.size() 
                        << ", returnTypes=" << node->sig.returnTypes.size());
    
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseIntrinsicCallExpr
//
// Grammar:
//   intrinsic_call := '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'
//   intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }
//   intrinsic_arg  := type_name            -- for #sizeof(T), #alignof(T)
//                   | expr                 -- for #sqrt(x), #memcpy(dst,src,n)
//
// The parser uses a simple disambiguation:
//   If the first argument after '(' is a bare IDENTIFIER that looks like a
//   named type (not followed by an infix operator), and the intrinsic is a
//   type-parameter intrinsic (#sizeof / #alignof), we parse it as typeArg.
//   Otherwise all arguments are parsed as regular expressions.
//
// Type-parameter intrinsics:  sizeof, alignof
// Value-argument intrinsics:  sqrt, abs, min, max, memcpy, memset, ...
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseIntrinsicCallExpr() {
    LUC_LOG_EXPR("parseIntrinsicCallExpr: parsing # intrinsic");
    SourceLocation loc = currentLoc();
    consume(TokenType::HASH, "expected '#'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected intrinsic name after '#'");
        if (!isAtEnd()) advance();
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IntrinsicCallExprAST>();
    node->loc = loc;
    node->intrinsicName = pool_.intern(advance().value);

    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' after intrinsic '#" + std::string(pool_.lookup(node->intrinsicName)) + "'");
        return arena_.make<UnknownExprAST>();
    }
    LUC_LOG_EXPR("parseIntrinsicCallExpr: name='" << pool_.lookup(node->intrinsicName) << "'");
    consume(TokenType::LPAREN, "expected '('");

    std::string intrinsicStr = std::string(pool_.lookup(node->intrinsicName));
    bool isTypeIntrinsic = (intrinsicStr == "sizeof" || intrinsicStr == "alignof");

    if (isTypeIntrinsic) {
        if (check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2005, "expected type argument");
        } else {
            TypePtr typeArg = parseType();
            if (!typeArg) errorAt(DiagCode::E2005, "invalid type argument");
            else node->typeArg = std::move(typeArg);
        }
        consume(TokenType::RPAREN, "expected ')' after type argument");
    } else {
        while (!check(TokenType::RPAREN) && !isAtEnd()) {
            std::size_t savedPos = pos_;
            ExprPtr arg = parseExpr();
            if (pos_ == savedPos) {
                errorAt(DiagCode::E2008, "expected argument expression in '#" + intrinsicStr + "'");
                // Skip the offending token
                if (!isAtEnd()) advance();
                // Skip to the next comma or closing parenthesis
                while (!isAtEnd() && !check(TokenType::COMMA) && !check(TokenType::RPAREN)) {
                    advance();
                }
                if (check(TokenType::COMMA)) {
                    advance(); // consume comma and continue
                    continue;
                }
                break;
            }
            node->args.push_back(std::move(arg));
            if (check(TokenType::RPAREN)) break;
            if (!match(TokenType::COMMA)) {
                errorAt(DiagCode::E2001, "expected ',' or ')' in intrinsic argument list");
                // Skip to the closing parenthesis
                while (!isAtEnd() && !check(TokenType::RPAREN)) advance();
                break;
            }
        }
        consume(TokenType::RPAREN, "expected ')' to close intrinsic call");
    }

    LUC_LOG_EXPR_VERBOSE("parseIntrinsicCallExpr: typeArg=" << (node->typeArg != nullptr) 
                         << ", args=" << node->args.size());
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseAwaitExpr
//
// await is only valid inside a function that has the ~async type qualifier.
// The semantic pass will enforce this by checking the function's type.
// The parser only performs basic syntax checks.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseAwaitExpr() {
    LUC_LOG_EXPR("parseAwaitExpr");
    SourceLocation loc = currentLoc();
    consume(TokenType::AWAIT, "expected 'await'");

    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'await' is not valid inside a 'parallel' block");
    }

    ExprPtr inner = parsePrattExpr(PREC_NONE);
    if (!inner) {
        errorAt(DiagCode::E2008, "expected expression after 'await'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<AwaitExprAST>(std::move(inner));
    node->loc = loc;
    LUC_LOG_EXPR_VERBOSE("parseAwaitExpr: returning await expression");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseMatchExpr
//
// Grammar:
//   match_expr := 'match' expr '{' { match_arm } default_arm '}'
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseMatchExpr() {
    LUC_LOG_EXPR("parseMatchExpr");
    SourceLocation loc = currentLoc();
    consume(TokenType::MATCH, "expected 'match'");

    // Parse subject — disable struct literal because '{' belongs to the match arms
    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'match'");
        return arena_.make<UnknownExprAST>();
    }
    LUC_LOG_EXPR_VERBOSE("parseMatchExpr: subject parsed");

    consume(TokenType::LBRACE, "expected '{' after match subject");

    auto node = arena_.make<MatchExprAST>();
    node->loc = loc;
    node->subject = std::move(subject);

    bool hasDefault = false;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        if (check(TokenType::DEFAULT)) {
            if (hasDefault) {
                errorAt(DiagCode::E2007, "duplicate 'default' arm in match expression");
            }
            LUC_LOG_EXPR_VERBOSE("parseMatchExpr: parsing default arm");
            node->defaultBody = parseDefaultArm();
            if (node->defaultBody) {
                node->defaultLoc = node->defaultBody->loc;
            }
            hasDefault = true;
            break;
        }

        size_t beforePos = pos_;
        MatchArmPtr arm = parseMatchArm();
        if (pos_ == beforePos) {
            errorAt(DiagCode::E2007, "failed to parse match arm, skipping");
            synchronize();
            if (check(TokenType::RBRACE) || isAtEnd())
                break;
            continue;
        }
        // Check if parseMatchArm failed (returned nullptr) even though progress was made
        if (!arm) {
            errorAt(DiagCode::E2007, "invalid match arm, skipping");
            synchronize();
            if (check(TokenType::RBRACE) || isAtEnd())
                break;
            continue;
        }
        node->arms.push_back(std::move(arm));
    }

    consume(TokenType::RBRACE, "expected '}' to close match expression");

    if (!hasDefault) {
        error(loc, DiagCode::E2006, "match expression must have a 'default' arm");
    }

    LUC_LOG_EXPR_VERBOSE("parseMatchExpr: " << node->arms.size() << " arms, hasDefault=" << hasDefault);
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseIfExpr
//
// Grammar:
//   if_expr := 'if' expr '??' expr 'else' expr
//
// Expression form requires 'else'. Both branches must return the same type
// (enforced by the semantic pass).
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseIfExpr() {
    LUC_LOG_EXPR("parseIfExpr");
    SourceLocation loc = currentLoc();
    consume(TokenType::IF, "expected 'if'");

    // Parse condition, stop at '??' (precedence 4)
    ExprPtr condition = parsePrattExpr(PREC_NULLCOAL); 
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        return arena_.make<UnknownExprAST>();
    }
    LUC_LOG_EXPR_VERBOSE("parseIfExpr: condition parsed");

    // Inline if expression: if cond ?? thenExpr else elseExpr
    if (!match(TokenType::QUESTION_QUESTION)) {
        errorAt(DiagCode::E2001, "expected '\?\?' after if condition in expression form");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr thenBranch = parseExpr();
    if (!thenBranch) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?' in 'if' expression");
    }
    LUC_LOG_EXPR_VERBOSE("parseIfExpr: then branch parsed");

    if (!match(TokenType::ELSE)) {
        errorAt(DiagCode::E2006, "expression-form 'if' requires an 'else' branch");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr elseBranch = parseExpr();
    if (!elseBranch) {
        errorAt(DiagCode::E2008, "expected expression after 'else' in 'if' expression");
    }
    LUC_LOG_EXPR_VERBOSE("parseIfExpr: else branch parsed");

    auto node = arena_.make<IfExprAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->thenBranch = std::move(thenBranch);
    node->elseBranch = std::move(elseBranch);
    LUC_LOG_EXPR_VERBOSE("parseIfExpr: returning IfExprAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTypeConvExpr
//
// Grammar:
//   type_conv := type_name '(' expr ')'     -- safe conversion
//              | '*' type_name '(' expr ')' -- unsafe bit reinterpret
//
// Called after the target type has already been parsed.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseTypeConvExpr(bool isUnsafe, TypePtr targetType) {
    LUC_LOG_EXPR("parseTypeConvExpr: isUnsafe=" << (isUnsafe ? "true" : "false"));
    SourceLocation loc = currentLoc();
    consume(TokenType::LPAREN, "expected '(' for explicit type cast");
    LUC_LOG_EXPR_VERBOSE("parseTypeConvExpr: target type parsed");

    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E2008, "expected expression inside explicit type cast");
        return arena_.make<UnknownExprAST>();
    }

    consume(TokenType::RPAREN, "expected ')' to close explicit type cast");
    LUC_LOG_EXPR_VERBOSE("parseTypeConvExpr: expression parsed");

    auto node = arena_.make<TypeConvExprAST>(
        std::move(targetType), std::move(expr), isUnsafe);
    node->loc = loc;
    LUC_LOG_EXPR_VERBOSE("parseTypeConvExpr: returning TypeConvExprAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseRangeExpr
//
// Called when '..' is found after lo has been parsed.
// Grammar:  lo '..' hi
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseRangeExpr(ExprPtr lo) {
    LUC_LOG_EXPR("parseRangeExpr");
    LUC_LOG_EXPR_VERBOSE("parseRangeExpr: lo kind=" << LucDebug::kindToString(lo->kind));
    SourceLocation loc = lo->loc;
    consume(TokenType::RANGE, "expected '..'");

    bool isExclusive = match(TokenType::LESS);
    LUC_LOG_EXPR_VERBOSE("parseRangeExpr: isExclusive=" << (isExclusive ? "true" : "false"));

    ExprPtr hi = parsePrattExpr(PREC_ADD); // stop before low-prec operators
    if (!hi) {
        errorAt(DiagCode::E2008, "expected upper bound after '..'");
        return arena_.make<UnknownExprAST>();
    }
    LUC_LOG_EXPR_VERBOSE("parseRangeExpr: hi parsed");

    auto node = arena_.make<RangeExprAST>();
    node->loc = loc;
    node->lo = std::move(lo);
    node->hi = std::move(hi);
    node->isExclusive = isExclusive;
    LUC_LOG_EXPR_VERBOSE("parseRangeExpr: returning RangeExprAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseCallExpr
//
// Grammar:  callee '(' [ arg_list ] ')'
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseCallExpr(ExprPtr callee, std::vector<TypePtr> genericArgs) {
    LUC_LOG_EXPR("parseCallExpr");
    LUC_LOG_EXPR_VERBOSE("parseCallExpr: callee kind=" << LucDebug::kindToString(callee->kind));
    if (!genericArgs.empty()) {
        LUC_LOG_EXPR("parseCallExpr: generic args count=" << genericArgs.size());
    }
    
    SourceLocation loc = callee->loc;
    consume(TokenType::LPAREN, "expected '('");

    auto node = arena_.make<CallExprAST>();
    node->loc = loc;
    node->callee = std::move(callee);
    node->genericArgs = std::move(genericArgs);

    if (!check(TokenType::RPAREN)) {
        node->args = parseArgList();
        LUC_LOG_EXPR_VERBOSE("parseCallExpr: parsed " << node->args.size() << " arguments");
    }

    consume(TokenType::RPAREN, "expected ')' to close argument list");

    // Check for argument-pack suffix '!' — only valid inside pipeline steps,
    // but we parse it here and set isArgPack; the semantic pass enforces context.
    node->isArgPack = match(TokenType::BANG);
    if (node->isArgPack) {
        LUC_LOG_EXPR("parseCallExpr: argument pack '!' detected");
    }

    LUC_LOG_EXPR_VERBOSE("parseCallExpr: returning CallExprAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseIndexExpr
//
// Grammar:
//   '[' expr ']'          — element index
//   '[' expr '..' expr ']' — inclusive slice
//   '[' expr '..<' expr ']' — exclusive slice
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseIndexExpr(ExprPtr target) {
    LUC_LOG_EXPR("parseIndexExpr");
    LUC_LOG_EXPR_VERBOSE("parseIndexExpr: target kind=" << LucDebug::kindToString(target->kind));
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACKET, "expected '['");

    ExprPtr startExpr = parseExpr();
    if (!startExpr) {
        errorAt(DiagCode::E2008, "expected index expression");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IndexExprAST>();
    node->loc = loc;
    node->target = std::move(target);

    if (check(TokenType::RANGE)) {
        // Slice: start '..' end or start '..<' end
        advance(); // consume '..'
        bool isExclusive = match(TokenType::LESS);
        LUC_LOG_EXPR_VERBOSE("parseIndexExpr: slice, isExclusive=" << (isExclusive ? "true" : "false"));
        
        ExprPtr endExpr = parseExpr();
        if (!endExpr) {
            errorAt(DiagCode::E2008, "expected end of slice range after '..'");
            return arena_.make<UnknownExprAST>();
        }
        node->index = std::move(startExpr);
        node->sliceEnd = std::move(endExpr);
        node->kind = IndexKind::Slice;
        node->isExclusive = isExclusive;
        LUC_LOG_EXPR("parseIndexExpr: slice from index to end");
    } else {
        node->index = std::move(startExpr);
        node->kind = IndexKind::Element;
        node->isExclusive = false;
        LUC_LOG_EXPR_VERBOSE("parseIndexExpr: element access");
    }

    consume(TokenType::RBRACKET, "expected ']' to close index expression");
    LUC_LOG_EXPR_VERBOSE("parseIndexExpr: returning IndexExprAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseArgList
//
// Parses comma-separated expressions until ')'. Does not consume the ')'.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<ExprPtr> Parser::parseArgList() {
    LUC_LOG_EXPR_VERBOSE("parseArgList");
    std::vector<ExprPtr> args;

    while (!check(TokenType::RPAREN) && !isAtEnd()) {
        std::size_t savedPos = pos_;
        ExprPtr arg = parseExpr();

        if (pos_ == savedPos) {
            // No progress – skip the offending token
            errorAt(DiagCode::E2008, "expected argument expression");
            if (!isAtEnd()) advance();
            // If the next token is a comma, consume it and continue
            if (check(TokenType::COMMA)) advance();
            // Continue to try next argument (or exit)
            continue;
        }

        args.push_back(std::move(arg));

        if (check(TokenType::RPAREN)) break;

        if (!match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "expected ',' after argument");
            // Skip tokens until we find a comma or closing parenthesis
            while (!isAtEnd() && !check(TokenType::COMMA) && !check(TokenType::RPAREN)) {
                advance();
            }
            if (check(TokenType::COMMA)) {
                advance(); // consume the comma
                // After consuming a comma, continue to parse the next argument
                continue;
            }
            // If we reached ')' or EOF, break out of the loop
            break;
        }

        // Check for consecutive commas (empty argument)
        if (check(TokenType::COMMA)) {
            errorAt(DiagCode::E2008, "empty argument in call (consecutive commas)");
            // Skip the extra comma
            advance();
        }
    }

    LUC_LOG_EXPR_VERBOSE("parseArgList: parsed " << args.size() << " arguments");
    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePipelineExpr
//
// Grammar:
//   pipeline_expr := seed { '|>' pipeline_step }
//
// Called from parsePrattExpr when '|>' is seen. lhs is already parsed as seed.
// Consumes ALL '|>' steps greedily.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parsePipelineExpr(ExprPtr seed) {
    LUC_LOG_EXPR("parsePipelineExpr: building pipeline");
    if (!seed) {
        errorAt(DiagCode::E2008, "expected pipeline seed before '|>'");
        auto unknown = arena_.make<UnknownExprAST>();
        unknown->loc = currentLoc();
        return unknown;
    }
    
    LUC_LOG_EXPR_VERBOSE("parsePipelineExpr: seed kind=" << LucDebug::kindToString(seed->kind));
    SourceLocation loc = seed->loc;

    auto node = arena_.make<PipelineExprAST>();
    node->loc = loc;
    node->seed = std::move(seed);

    while (check(TokenType::PIPELINE)) {
        advance(); // consume '|>'
        
        PipelineStepPtr step = parsePipelineStep();
        if (step) {
            node->steps.push_back(std::move(step));
        } else {
            LUC_LOG_EXPR("parsePipelineExpr: parsePipelineStep returned nullptr, breaking");
            break;
        }
    }

    if (node->steps.empty()) {
        errorAt(DiagCode::E2006, "pipeline '->' requires at least one step");
        // node->seed is still valid (was moved from seed, not null)
        return std::move(node->seed);
    }

    LUC_LOG_EXPR("parsePipelineExpr: " << node->steps.size() << " pipeline steps");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePipelineStep
//
// Five forms:
//   Ident       fn (IDENTIFIER or PRIMITIVE_TYPE)
//   BehaviorRef Type:method
//   FieldRef    obj.field       (IDENTIFIER '.' IDENTIFIER)
//   ArgPack     fn(args)!       (IDENTIFIER or PRIMITIVE_TYPE)
//   AnonFunc    (params) { ... } or ~async (params) { ... }
// ─────────────────────────────────────────────────────────────────────────────
PipelineStepPtr Parser::parsePipelineStep() {
    LUC_LOG_EXPR_VERBOSE("parsePipelineStep: token='" << peek().value << "', type=" << static_cast<int>(peek().type));
    SourceLocation loc = currentLoc();
    auto step = arena_.make<PipelineStepAST>();
    step->loc = loc;

    // ═══════════════════════════════════════════════════════════════════════
    // 1. ANONYMOUS FUNCTION DETECTION (lookahead)
    // ═══════════════════════════════════════════════════════════════════════
    std::size_t savedPos = pos_;
    std::size_t testPos = savedPos;
    bool isAnonFunc = false;

    // Skip qualifiers (error if present)
    if (testPos < tokens_.size() && tokens_[testPos].type == TokenType::TILDE) {
        errorAt(DiagCode::E2002,
                "anonymous function cannot have qualifiers (e.g., ~async, ~nullable). "
                "Qualifiers belong on declarations, not values.");
        while (testPos < tokens_.size() && tokens_[testPos].type == TokenType::TILDE) {
            ++testPos;
            if (testPos < tokens_.size() && tokens_[testPos].type == TokenType::IDENTIFIER)
                ++testPos;
            while (testPos < tokens_.size() && (tokens_[testPos].type == TokenType::LINE_COMMENT ||
                                                tokens_[testPos].type == TokenType::DOC_COMMENT))
                ++testPos;
        }
    }

    // Parse parameter groups
    if (testPos < tokens_.size() && tokens_[testPos].type == TokenType::LPAREN) {
        auto skipParamGroup = [&](std::size_t& pos) -> bool {
            if (pos >= tokens_.size() || tokens_[pos].type != TokenType::LPAREN) return false;
            int parenDepth = 1;
            ++pos;
            while (pos < tokens_.size() && parenDepth > 0) {
                while (pos < tokens_.size() && (tokens_[pos].type == TokenType::LINE_COMMENT ||
                                                tokens_[pos].type == TokenType::DOC_COMMENT))
                    ++pos;
                if (pos >= tokens_.size()) break;
                TokenType tt = tokens_[pos].type;
                if (tt == TokenType::LPAREN) ++parenDepth;
                else if (tt == TokenType::RPAREN) --parenDepth;
                else if (tt == TokenType::TILDE) {
                    ++pos;
                    if (pos < tokens_.size() && tokens_[pos].type == TokenType::IDENTIFIER) ++pos;
                    continue;
                }
                ++pos;
            }
            return true;
        };

        bool hasParamGroups = false;
        while (testPos < tokens_.size() && tokens_[testPos].type == TokenType::LPAREN) {
            hasParamGroups = true;
            if (!skipParamGroup(testPos)) break;
            while (testPos < tokens_.size() && (tokens_[testPos].type == TokenType::LINE_COMMENT ||
                                                tokens_[testPos].type == TokenType::DOC_COMMENT))
                ++testPos;
        }

        if (hasParamGroups) {
            // Optional '->' and return type
            if (testPos < tokens_.size() && tokens_[testPos].type == TokenType::ARROW) {
                ++testPos;
                while (testPos < tokens_.size() && (tokens_[testPos].type == TokenType::LINE_COMMENT ||
                                                    tokens_[testPos].type == TokenType::DOC_COMMENT))
                    ++testPos;
                if (testPos < tokens_.size()) {
                    TokenType retStart = tokens_[testPos].type;
                    if (isPrimitiveTypeToken(retStart) || retStart == TokenType::IDENTIFIER) {
                        ++testPos;
                        if (testPos < tokens_.size() && tokens_[testPos].type == TokenType::LESS) {
                            int depth = 1;
                            ++testPos;
                            while (testPos < tokens_.size() && depth > 0) {
                                if (tokens_[testPos].type == TokenType::LESS) ++depth;
                                else if (tokens_[testPos].type == TokenType::GREATER) --depth;
                                ++testPos;
                            }
                        }
                    }
                }
            }

            while (testPos < tokens_.size() && (tokens_[testPos].type == TokenType::LINE_COMMENT ||
                                                tokens_[testPos].type == TokenType::DOC_COMMENT))
                ++testPos;

            if (testPos < tokens_.size() && tokens_[testPos].type == TokenType::LBRACE)
                isAnonFunc = true;
        }
    }

    pos_ = savedPos;  // restore position

    if (isAnonFunc) {
        LUC_LOG_EXPR_VERBOSE("parsePipelineStep: parsing anonymous function");
        ExprPtr anonFuncExpr = parseAnonFuncExpr();
        if (!anonFuncExpr || anonFuncExpr->isa<UnknownExprAST>()) {
            errorAt(DiagCode::E2002, "expected valid anonymous function as pipeline step");
            // Recover: skip to next pipeline or brace
            while (!isAtEnd() && !check(TokenType::PIPELINE) && !check(TokenType::RBRACE) &&
                   !check(TokenType::SEMICOLON))
                advance();
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            return step;
        }
        step->kind = PipelineStepKind::AnonFunc;
        step->anonFunc = std::move(anonFuncExpr);
        return step;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 2. OTHER STEP FORMS (identifier‑based)
    // ═══════════════════════════════════════════════════════════════════════
    bool isPrimitiveType = Parser::isPrimitiveTypeToken(peek().type);
    if (!check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        errorAt(DiagCode::E2002,
                "expected function name, method reference, array access, or anonymous function as pipeline step, got '" +
                peek().value + "'");
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        advance();
        return step;
    }

    std::string name;
    if (isPrimitiveType) {
        Token tok = advance();
        name = tok.value;
    } else {
        name = advance().value;
    }

    // ── Process possible generic arguments ─────────────────────────────────
    std::vector<TypePtr> genericArgs;
    if (check(TokenType::LESS)) {
        genericArgs = parseGenericArgs();
    }

    // ── Type:method [ (args)! ] ──────────────────────────────────────────
    if (check(TokenType::COLON)) {
        if (peekNext().type != TokenType::IDENTIFIER) {
            errorAt(DiagCode::E2003, "expected method name after ':'");
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            advance();
            return step;
        }
        advance(); // consume ':'
        std::string method = advance().value;

        if (check(TokenType::LPAREN)) {
            consume(TokenType::LPAREN, "expected '('");
            std::vector<ExprPtr> packArgs;
            if (!check(TokenType::RPAREN)) packArgs = parseArgList();
            consume(TokenType::RPAREN, "expected ')'");
            if (!match(TokenType::BANG)) {
                errorAt(DiagCode::E2001, "expected '!' after arguments for method argument pack");
                step->kind = PipelineStepKind::Ident;
                step->ident = pool_.intern("<error>");
                return step;
            }
            step->kind = PipelineStepKind::BehaviorArgPack;
            step->typeName = pool_.intern(name);
            step->method = pool_.intern(method);
            step->packArgs = std::move(packArgs);
            // genericArgs are not allowed on BehaviorRef – ignore if present
            if (!genericArgs.empty()) {
                errorAt(DiagCode::E2002, "generic arguments not allowed on method reference");
            }
            return step;
        }

        step->kind = PipelineStepKind::BehaviorRef;
        step->typeName = pool_.intern(name);
        step->method = pool_.intern(method);
        if (!genericArgs.empty()) {
            errorAt(DiagCode::E2002, "generic arguments not allowed on method reference");
        }
        return step;
    }

    // ── obj.field or obj.field(args)! ────────────────────────────────────────
    if (check(TokenType::DOT)) {
        if (peekNext().type != TokenType::IDENTIFIER) {
            errorAt(DiagCode::E2003, "expected field name after '.'");
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            advance(); // consume '.'
            return step;
        }
        advance(); // consume '.'
        std::string field = advance().value;

        // Check for argument pack call: (args)!
        if (check(TokenType::LPAREN)) {
            consume(TokenType::LPAREN, "expected '('");
            std::vector<ExprPtr> packArgs;
            if (!check(TokenType::RPAREN)) {
                packArgs = parseArgList();
            }
            consume(TokenType::RPAREN, "expected ')'");
            if (!match(TokenType::BANG)) {
                errorAt(DiagCode::E2001, "expected '!' after arguments for field argument pack");
                step->kind = PipelineStepKind::Ident;
                step->ident = pool_.intern("<error>");
                return step;
            }
            step->kind = PipelineStepKind::FieldArgPack;
            step->ident = pool_.intern(name);
            step->field = pool_.intern(field);
            step->packArgs = std::move(packArgs);
            if (!genericArgs.empty()) {
                errorAt(DiagCode::E2002, "generic arguments not allowed on field reference");
            }
            return step;
        }

        // Plain field reference (no call)
        step->kind = PipelineStepKind::FieldRef;
        step->ident = pool_.intern(name);
        step->field = pool_.intern(field);
        if (!genericArgs.empty()) {
            errorAt(DiagCode::E2002, "generic arguments not allowed on field reference");
        }
        return step;
    }

    // ── Array indexing: arr[0] or arr[0][1]... ────────────────────────────
    if (check(TokenType::LBRACKET)) {
        auto addIndex = [&](ExprPtr target, ExprPtr idx) -> ExprPtr {
            auto node = arena_.make<IndexExprAST>();
            node->target = std::move(target);
            node->index = std::move(idx);
            node->kind = IndexKind::Element;
            return node;
        };

        ExprPtr indexChain = nullptr;
        // first bracket
        advance();
        ExprPtr idx = parseExpr();
        if (!idx) {
            errorAt(DiagCode::E2008, "expected index expression");
            // skip to matching ']'
            int bracketDepth = 1;
            while (!isAtEnd() && bracketDepth > 0) {
                if (check(TokenType::LBRACKET)) ++bracketDepth;
                else if (check(TokenType::RBRACKET)) --bracketDepth;
                advance();
            }
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            return step;
        }
        consume(TokenType::RBRACKET, "expected ']' after index");
        ExprPtr base = arena_.make<IdentifierExprAST>(pool_.intern(name));
        base->loc = loc;
        indexChain = addIndex(std::move(base), std::move(idx));

        // additional brackets
        while (check(TokenType::LBRACKET)) {
            advance();
            ExprPtr nextIdx = parseExpr();
            if (!nextIdx) {
                errorAt(DiagCode::E2008, "expected index expression");
                int bracketDepth = 1;
                while (!isAtEnd() && bracketDepth > 0) {
                    if (check(TokenType::LBRACKET)) ++bracketDepth;
                    else if (check(TokenType::RBRACKET)) --bracketDepth;
                    advance();
                }
                break;
            }
            consume(TokenType::RBRACKET, "expected ']' after index");
            indexChain = addIndex(std::move(indexChain), std::move(nextIdx));
        }

        // optional argument pack call
        if (check(TokenType::LPAREN)) {
            consume(TokenType::LPAREN, "expected '('");
            std::vector<ExprPtr> packArgs;
            if (!check(TokenType::RPAREN)) packArgs = parseArgList();
            consume(TokenType::RPAREN, "expected ')'");
            if (!match(TokenType::BANG)) {
                errorAt(DiagCode::E2001, "expected '!' after arguments for array index argument pack");
                step->kind = PipelineStepKind::Ident;
                step->ident = pool_.intern("<error>");
                return step;
            }
            step->kind = PipelineStepKind::IndexArgPack;
            step->ident = pool_.intern(name);
            step->index = std::move(indexChain);
            step->packArgs = std::move(packArgs);
            return step;
        }

        if (!genericArgs.empty()) {
            errorAt(DiagCode::E2002, "generic arguments not allowed on array index step");
        }

        step->kind = PipelineStepKind::IndexRef;
        step->ident = pool_.intern(name);
        step->index = std::move(indexChain);
        return step;
    }

    // ── Function call with argument pack (or plain generic function name) ──
    if (check(TokenType::LPAREN)) {
        consume(TokenType::LPAREN, "expected '('");
        std::vector<ExprPtr> packArgs;
        if (!check(TokenType::RPAREN)) packArgs = parseArgList();
        consume(TokenType::RPAREN, "expected ')'");
        if (!match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for function argument pack");
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            return step;
        }
        step->kind = PipelineStepKind::ArgPack;
        step->ident = pool_.intern(name);
        step->genericArgs = std::move(genericArgs);
        step->packArgs = std::move(packArgs);
        return step;
    }

    // ── Plain identifier (function reference) with optional generic arguments ──
    step->kind = PipelineStepKind::Ident;
    step->ident = pool_.intern(name);
    step->genericArgs = std::move(genericArgs);
    return step;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseComposeExpr
//
// Grammar:
//   compose_expr := lhs { '+>' compose_operand }
//
// Called from parsePrattExpr when '+>' is seen.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseComposeExpr(ExprPtr lhs) {
    LUC_LOG_EXPR("parseComposeExpr");
    LUC_LOG_EXPR_VERBOSE("parseComposeExpr: lhs kind=" << LucDebug::kindToString(lhs->kind));
    SourceLocation loc = lhs->loc;

    auto node = arena_.make<ComposeExprAST>();
    node->loc = loc;
    node->left = std::move(lhs);
    int operandCount = 0;

    while (check(TokenType::COMPOSE)) {
        advance(); // consume '+>'
        operandCount++;
        LUC_LOG_EXPR_VERBOSE("parseComposeExpr: parsing operand " << operandCount);
        
        ComposeOperandPtr op = parseComposeOperand();
        if (!op) {
            errorAt(DiagCode::E2002, "expected function name after '+>'");
            break;
        }
        node->operands.push_back(std::move(op));
    }

    if (node->operands.empty()) {
        LUC_LOG_EXPR_VERBOSE("parseComposeExpr: no operands, returning left");
        return std::move(node->left);
    }

    LUC_LOG_EXPR("parseComposeExpr: " << operandCount << " operands");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseComposeOperand
//
// Three forms (no AnonFunc, no ArgPack — compile-time only):
//   Ident       fn (IDENTIFIER or PRIMITIVE_TYPE)
//   BehaviorRef Type:method
//   FieldRef    obj.field
// ─────────────────────────────────────────────────────────────────────────────
ComposeOperandPtr Parser::parseComposeOperand() {
    LUC_LOG_EXPR_VERBOSE("parseComposeOperand: token='" << peek().value << "'");
    SourceLocation loc = currentLoc();
    auto op = arena_.make<ComposeOperandAST>();
    op->loc = loc;

    // Check if current token is a primitive type keyword
    bool isPrimitiveType = Parser::isPrimitiveTypeToken(peek().type);

    if (!check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        errorAt(DiagCode::E2002, "expected function name or method reference as composition operand");
        return nullptr;
    }

    // Get the name
    std::string name;
    if (isPrimitiveType) {
        Token tok = advance();
        name = tok.value;
    } else {
        name = advance().value;
    }

    // BehaviorRef
    if (check(TokenType::COLON) && peekNext().type == TokenType::IDENTIFIER) {
        advance();
        std::string method = advance().value;
        op->kind = ComposeOperandKind::BehaviorRef;
        op->typeName = std::move(pool_.intern(name));
        op->method = std::move(pool_.intern(method));
        LUC_LOG_EXPR_VERBOSE("parseComposeOperand: BehaviorRef " << pool_.lookup(op->typeName) << ":" << pool_.lookup(op->method));
        return op;
    }

    // FieldRef
    if (check(TokenType::DOT) && peekNext().type == TokenType::IDENTIFIER) {
        advance();
        std::string field = advance().value;
        op->kind = ComposeOperandKind::FieldRef;
        op->ident = std::move(pool_.intern(name));
        op->field = std::move(pool_.intern(field));
        LUC_LOG_EXPR_VERBOSE("parseComposeOperand: FieldRef " << pool_.lookup(op->ident) << "." << pool_.lookup(op->field));
        return op;
    }

    // Ident
    op->kind = ComposeOperandKind::Ident;
    op->ident = std::move(pool_.intern(name));
    LUC_LOG_EXPR_VERBOSE("parseComposeOperand: Ident " << pool_.lookup(op->ident));
    return op;
}

// ═════════════════════════════════════════════════════════════════════════════
// PATTERN PARSING
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// parseMatchArm
//
// Grammar:
//   match_arm := pattern { ',' pattern } [ 'if' guard_expr ] '=>' arm_body
// ─────────────────────────────────────────────────────────────────────────────
MatchArmPtr Parser::parseMatchArm() {
    SourceLocation loc = currentLoc();

    auto arm = arena_.make<MatchArmAST>();
    arm->loc = loc;

    // Parse comma-separated pattern list
    // First pattern (required)
    ASTPtr<PatternAST> pat = parsePattern();
    if (!pat) {
        // No pattern – arm is invalid; caller will handle.
        return nullptr;
    }
    arm->patterns.push_back(std::move(pat));

    // Additional patterns after commas
    while (check(TokenType::COMMA)) {
        // Peek ahead – is the next token a valid pattern start?
        TokenType nextType = peekNext().type;
        bool isPatternStart = (nextType == TokenType::WILDCARD ||
                            nextType == TokenType::INT_LITERAL ||
                            nextType == TokenType::FLOAT_LITERAL ||
                            nextType == TokenType::STRING_LITERAL ||
                            nextType == TokenType::RAW_STRING_LITERAL ||
                            nextType == TokenType::CHAR_LITERAL ||
                            nextType == TokenType::HEX_LITERAL ||
                            nextType == TokenType::BINARY_LITERAL ||
                            nextType == TokenType::TRUE ||
                            nextType == TokenType::FALSE ||
                            nextType == TokenType::NIL ||
                            nextType == TokenType::MINUS ||     // negative literal
                            nextType == TokenType::IDENTIFIER);
        if (!isPatternStart) {
            errorAt(DiagCode::E2007, "expected pattern after ',' in match arm");
            break;   // do NOT consume the comma – leave it for error recovery
        }
        advance(); // consume the comma
        pat = parsePattern();
        if (!pat) {
            // parsePattern failed – error already reported.
            // The comma was already consumed, but no pattern added.
            break;
        }
        arm->patterns.push_back(std::move(pat));
    }

    // Optional guard: 'if' expr
    if (check(TokenType::IF)) {
        advance(); // consume 'if'
        std::size_t savedPos = pos_;
        ExprPtr guard = parseExpr();
        if (pos_ == savedPos) {
            errorAt(DiagCode::E2008, "expected guard expression after 'if' in match arm");
            // arm->guard remains nullptr (already default-initialised)
        } else {
            arm->guard = std::move(guard);
        }
    }

    // Use FAT_ARROW (=>) instead of ARROW (->)
    consume(TokenType::FAT_ARROW, "expected '=>' after match pattern");

    // Parse result expressions: at least one, at most two, no trailing commas
    // First expression (required)
    size_t beforePos = pos_;
    ExprPtr first = parseExpr();
    if (pos_ == beforePos || !first) {
        errorAt(DiagCode::E2008, "expected result expression after '=>' in match arm");
    } else {
        arm->exprs.push_back(std::move(first));
    }

    // Optional second expression after comma
    if (match(TokenType::COMMA)) {
        // If there's a comma, there must be a second expression
        if (check(TokenType::COMMA) || check(TokenType::RBRACE) || check(TokenType::FAT_ARROW) || isAtEnd()) {
            errorAt(DiagCode::E2001, "expected expression after ',' in match arm");
        } else {
            size_t beforePos2 = pos_;
            ExprPtr second = parseExpr();
            if (pos_ == beforePos2 || !second) {
                errorAt(DiagCode::E2008, "expected second result expression after ',' in match arm");
            } else {
                arm->exprs.push_back(std::move(second));
            }
        }
        // No more commas allowed – if another comma appears, skip the rest of the arm body
        if (match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "match arm cannot have more than two expressions");
            // Skip tokens until the start of the next arm or the closing brace
            while (!isAtEnd() && !check(TokenType::RBRACE) && !check(TokenType::DEFAULT)) {
                TokenType t = peek().type;
                // Break if we see a token that can begin a match pattern or the default keyword
                if (t == TokenType::WILDCARD ||
                    t == TokenType::IDENTIFIER ||
                    t == TokenType::INT_LITERAL ||
                    t == TokenType::FLOAT_LITERAL ||
                    t == TokenType::STRING_LITERAL ||
                    t == TokenType::RAW_STRING_LITERAL ||
                    t == TokenType::CHAR_LITERAL ||
                    t == TokenType::HEX_LITERAL ||
                    t == TokenType::BINARY_LITERAL ||
                    t == TokenType::TRUE ||
                    t == TokenType::FALSE ||
                    t == TokenType::NIL ||
                    t == TokenType::MINUS) {
                    break;
                }
                advance();
            }
        }
    }
    
    return arm;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseDefaultArm
//
// Grammar:  'default' '=>' arm_body
// ─────────────────────────────────────────────────────────────────────────────
DefaultArmPtr Parser::parseDefaultArm() {
    SourceLocation loc = currentLoc();
    consume(TokenType::DEFAULT, "expected 'default'");
    consume(TokenType::FAT_ARROW, "expected '=>' after 'default'");

    auto arm = arena_.make<DefaultArmAST>();
    arm->loc = loc;

    // First expression (required)
    std::size_t savedPos = pos_;
    ExprPtr exp = parseExpr();
    if (pos_ == savedPos || !exp) {
        errorAt(DiagCode::E2008, "expected expression after '=>' in default arm");
        return arm;
    }
    arm->exprs.push_back(std::move(exp));

    // Optional second expression after comma
    if (match(TokenType::COMMA)) {
        if (check(TokenType::COMMA) || check(TokenType::RBRACE) || check(TokenType::FAT_ARROW) || isAtEnd()) {
            errorAt(DiagCode::E2001, "expected expression after ',' in default arm");
        } else {
            std::size_t savedPos2 = pos_;
            ExprPtr second = parseExpr();
            if (pos_ == savedPos2 || !second) {
                errorAt(DiagCode::E2008, "expected second expression after ',' in default arm");
            } else {
                arm->exprs.push_back(std::move(second));
            }
        }
        // No more commas allowed
        if (match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "default arm cannot have more than two expressions");
            // Skip any further tokens until a safe boundary (optional)
            while (!isAtEnd() && !check(TokenType::RBRACE) && !check(TokenType::DEFAULT)) {
                advance();
            }
        }
    }

    return arm;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePattern  — dispatch to the correct pattern sub-parser
//
// Decision tree:
//   WILDCARD                 → WildcardPatternAST
//   literal tokens           → LiteralPatternAST (or RangePatternAST if '..' follows)
//   IDENTIFIER 'is' type     → TypePatternAST
//   IDENTIFIER '{'           → StructPatternAST
//   IDENTIFIER               → BindPatternAST (or RangePatternAST if '..' follows)
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<PatternAST> Parser::parsePattern() {
    LUC_LOG_EXPR_VERBOSE("parsePattern: token='" << peek().value << "'");
    
    // Wildcard
    if (check(TokenType::WILDCARD)) {
        LUC_LOG_EXPR_VERBOSE("parsePattern: wildcard pattern");
        return parseWildcardPattern();
    }

    // Literal patterns (and ranges)
    switch (peek().type) {
        case TokenType::INT_LITERAL:
        case TokenType::FLOAT_LITERAL:
        case TokenType::STRING_LITERAL:
        case TokenType::RAW_STRING_LITERAL:
        case TokenType::CHAR_LITERAL:
        case TokenType::HEX_LITERAL:
        case TokenType::BINARY_LITERAL:
        case TokenType::TRUE:
        case TokenType::FALSE:
        case TokenType::NIL:
        case TokenType::MINUS:
            return parseLiteralOrRangePattern();
        default:
            break;
    }

    // IDENTIFIER-based patterns
    if (check(TokenType::IDENTIFIER)) {
        std::string name = peek().value;

        // Type pattern: IDENTIFIER 'is' type
        if (peekNext().type == TokenType::IS) {
            LUC_LOG_EXPR_VERBOSE("parsePattern: type pattern (is)");
            advance(); // consume IDENTIFIER
            return parseTypePattern(std::move(pool_.intern(name)));
        }

        // Struct pattern: IDENTIFIER '{'
        if (peekNext().type == TokenType::LBRACE) {
            advance(); // consume IDENTIFIER
            return parseStructPattern(std::move(pool_.intern(name)));
        }

        // Qualified constant pattern: IDENTIFIER '.' ...
        if (peekNext().type == TokenType::DOT) {
            // Parse the entire expression (e.g., Direction.North)
            ExprPtr expr = parseExpr();
            if (!expr) {
                errorAt(DiagCode::E2007, "expected expression after '.' in pattern");
                return nullptr;
            }
            return arena_.make<PatternExprAST>(std::move(expr));
        }

        // Simple bind pattern
        advance(); // consume IDENTIFIER
        if (check(TokenType::RANGE)) {
            errorAt(DiagCode::E2007, "bind patterns cannot be used as range bounds");
            advance(); // consume '..'
            parseLiteralOrRangePattern(); // recover
        }
        return parseBindPattern(pool_.intern(name));
    }

    errorAt(DiagCode::E2007, "expected pattern");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseLiteralOrRangePattern
//
// Parses a literal token (possibly prefixed with '-' for negatives) and
// checks if '..' follows to build a RangePatternAST.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<PatternAST> Parser::parseLiteralOrRangePattern() {
    SourceLocation loc = currentLoc();
 
    // Handle unary minus for negative literals
    bool negative = false;
    if (check(TokenType::MINUS)) {
        negative = true;
        advance();
    }
 
    Token tok = advance();
    LiteralKind kind;
    switch (tok.type) {
        case TokenType::INT_LITERAL:
            kind = LiteralKind::Int;
            break;
        case TokenType::FLOAT_LITERAL:
            kind = LiteralKind::Float;
            break;
        case TokenType::STRING_LITERAL:
            kind = LiteralKind::String;
            break;
        case TokenType::RAW_STRING_LITERAL:
            kind = LiteralKind::RawString;
            break;
        case TokenType::CHAR_LITERAL:
            kind = LiteralKind::Char;
            break;
        case TokenType::HEX_LITERAL:
            kind = LiteralKind::Hex;
            break;
        case TokenType::BINARY_LITERAL:
            kind = LiteralKind::Binary;
            break;
        case TokenType::TRUE:
            kind = LiteralKind::True;
            break;
        case TokenType::FALSE:
            kind = LiteralKind::False;
            break;
        case TokenType::NIL:
            kind = LiteralKind::Nil;
            break;
        default:
            errorAt(DiagCode::E2009, "expected literal value in pattern");
            return nullptr;
    }
 
    std::string rawValue = negative ? ("-" + tok.value) : tok.value;
    InternedString internedValue = pool_.intern(rawValue);
 
    // Check for range: lo '..' [ '<' ] hi
    if (check(TokenType::RANGE)) {
        advance(); // consume '..'
        bool isExclusive = match(TokenType::LESS);
 
        bool negHi = false;
        if (check(TokenType::MINUS)) {
            negHi = true;
            advance();
        }
 
        if (!checkAny({TokenType::INT_LITERAL, TokenType::HEX_LITERAL,
                       TokenType::FLOAT_LITERAL})) {
            errorAt(DiagCode::E2009, "expected literal after '..' in range pattern");
            return nullptr;
        }
        Token hiTok = advance();
        std::string hiRaw = negHi ? ("-" + hiTok.value) : hiTok.value;
        InternedString hiInterned = pool_.intern(hiRaw);
 
        LiteralKind hiKind;
        switch (hiTok.type) {
            case TokenType::INT_LITERAL:
                hiKind = LiteralKind::Int;
                break;
            case TokenType::HEX_LITERAL:
                hiKind = LiteralKind::Hex;
                break;
            default:
                hiKind = LiteralKind::Float;
                break;
        }
 
        auto loExpr = arena_.make<LiteralExprAST>(kind, internedValue);
        loExpr->loc = loc;
        auto hiExpr = arena_.make<LiteralExprAST>(hiKind, hiInterned);
        hiExpr->loc = locOf(hiTok);
 
        auto range = arena_.make<RangeExprAST>();
        range->loc = loc;
        range->lo = std::move(loExpr);
        range->hi = std::move(hiExpr);
        range->isExclusive = isExclusive;
        
        // Wrap RangeExprAST in PatternExprAST
        return arena_.make<PatternExprAST>(std::move(range));
    }
 
    // Simple literal pattern
    auto lit = arena_.make<LiteralExprAST>(kind, internedValue);
    lit->loc = loc;
    return arena_.make<PatternExprAST>(std::move(lit));
}

// ─────────────────────────────────────────────────────────────────────────────
// parseBindPattern
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<BindPatternAST> Parser::parseBindPattern(InternedString name) {
    SourceLocation loc = currentLoc();
    auto pat = arena_.make<BindPatternAST>(std::move(name));
    pat->loc = loc;
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTypePattern
//
// Grammar:  IDENTIFIER 'is' type
// Called after the IDENTIFIER has been consumed.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<TypePatternAST> Parser::parseTypePattern(InternedString bindName) {
    SourceLocation loc = currentLoc();
    consume(TokenType::IS, "expected 'is' in type pattern");

    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E2005, "expected type after 'is' in type pattern");
        return nullptr;
    }

    auto pat = arena_.make<TypePatternAST>();
    pat->loc = loc;
    pat->bindName = std::move(bindName);
    pat->checkType = std::move(checkType);
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseWildcardPattern
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<WildcardPatternAST> Parser::parseWildcardPattern() {
    SourceLocation loc = currentLoc();
    consume(TokenType::WILDCARD, "expected '_'");
    auto pat = arena_.make<WildcardPatternAST>();
    pat->loc = loc;
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseStructPattern
//
// Grammar:  IDENTIFIER '{' { field_pattern } '}'
// Called after the type name has been consumed.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<StructPatternAST> Parser::parseStructPattern(InternedString typeName) {
    LUC_LOG_EXPR("parseStructPattern: type='" << pool_.lookup(typeName) << "'");
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACE, "expected '{' in struct pattern");

    auto pat = arena_.make<StructPatternAST>();
    pat->loc = loc;
    pat->typeName = std::move(typeName);

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        std::size_t savedPos = pos_;
        FieldPatternPtr fp = parseFieldPattern();
        if (fp) {
            pat->fields.push_back(std::move(fp));
        } else {
            // No progress? Skip the offending token to avoid infinite loop.
            if (pos_ == savedPos && !isAtEnd()) {
                errorAt(DiagCode::E2003, "expected field name in struct pattern");
                advance(); // consume the unexpected token
            }
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close struct pattern");
    LUC_LOG_EXPR_VERBOSE("parseStructPattern: " << pat->fields.size() << " fields");
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFieldPattern
//
// Grammar:
//   field_pattern := IDENTIFIER
//                  | IDENTIFIER ':' pattern
// ─────────────────────────────────────────────────────────────────────────────
FieldPatternPtr Parser::parseFieldPattern() {
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name in struct pattern");
        return nullptr;
    }
    std::string fieldName = advance().value;

    auto fp = arena_.make<FieldPatternAST>();
    fp->loc = loc;
    fp->field = std::move(pool_.intern(fieldName));

    // Full form: 'fieldName : sub_pattern'
    if (check(TokenType::COLON)) {
        advance(); // consume ':'
        fp->subPattern = parsePattern();
        if (!fp->subPattern) {
            errorAt(DiagCode::E2007, "expected sub-pattern after ':' in field pattern");
        }
    }
    // else: shorthand — subPattern is nullptr, bind by field name

    return fp;
}

/**
* @brief Parses an assignable left‑hand side (lvalue) expression.
*
* Grammar of an lvalue:
*   lvalue := IDENTIFIER { ( '.' IDENTIFIER ) | ( '[' expr ']' ) }
*
* Examples:
*   x
*   point.x
*   arr[i]
*   matrix[row][col]
*   p.x.y
*
* This function stops at the first token that is not part of a valid lvalue
* construct. It does NOT consume any operator that follows the lvalue,
* such as '=', '+=', '?', ':', etc.
*
* Why not use parseExpr()?
*   parseExpr() would treat '=' as a binary operator and prematurely consume
*   the assignment token, breaking multi‑assignment parsing. This function
*   is specifically for multi‑assignment left‑hand sides where the '=' is
*   the separator between the LHS list and the RHS expression.
*
* @return An expression tree that represents an assignable location,
*         or nullptr on error.
*
* @note The caller (parseMultiAssignStmt) uses this function to parse the
*       comma‑separated list of lvalues before consuming the '=' token.
*/
ExprPtr Parser::parseLvalue() {
    // Start with an identifier (required)
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected identifier for lvalue");
        return nullptr;
    }
    std::string name = advance().value;
    ExprPtr expr = arena_.make<IdentifierExprAST>(pool_.intern(name));
    expr->loc = currentLoc();

    // Apply postfix operators that produce an assignable location: '.' and '[' only.
    while (true) {
        if (check(TokenType::DOT)) {
            // Field access: expr.field
            advance(); // consume '.'
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.'");
                return expr; // return what we have so far
            }
            std::string field = advance().value;
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = expr->loc;
            node->object = std::move(expr);
            node->field = pool_.intern(field);
            expr = std::move(node);
        } 
        else if (check(TokenType::LBRACKET)) {
            // Indexing: expr[index]
            advance(); // consume '['
            ExprPtr index = parseExpr();
            if (!index) {
                errorAt(DiagCode::E2008, "expected index expression");
                return expr;
            }
            consume(TokenType::RBRACKET, "expected ']' after index");
            auto node = arena_.make<IndexExprAST>();
            node->loc = expr->loc;
            node->target = std::move(expr);
            node->index = std::move(index);
            node->kind = IndexKind::Element;
            expr = std::move(node);
        }
        else if (check(TokenType::COLON)) {
            // Behavior access (Type:method) is NOT assignable – stop.
            break;
        }
        else {
            break;
        }
    }
    return expr;
}
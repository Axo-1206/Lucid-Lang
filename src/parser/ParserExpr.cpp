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
//   PREC_PIPE     = 3   ->                                   (left-assoc)
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
        case TokenType::ARROW:              return PREC_PIPE;
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

        // ── '->' pipeline ─────────────────────────────────────────────────────
        if (opTok == TokenType::ARROW) {
            lhs = parsePipelineExpr(std::move(lhs));
            // parsePipelineExpr consumed all '->' steps; continue the outer loop.
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

    // ── @intrinsic call ───────────────────────────────────────────────────────
    // '@' IDENTIFIER '(' args ')'  — compiler-builtin call.
    // Examples:  @sizeof(Vec2)   @memcpy(dst, src, n)   @sqrt(x)
    if (check(TokenType::AT_SIGN)) {
        LUC_LOG_EXPR("parsePrimaryExpr: parsing @ intrinsic");
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

    // ── Identifier — struct literal, explicit type cast, behavior access, or name ─
    if (check(TokenType::IDENTIFIER)) {
        std::string name = peek().value;

        // Safe explicit cast: primitive name or named type immediately followed by '('
        // e.g.  float(x)  string(n)  Fahrenheit(boiling)
        // The key: IDENTIFIER '(' where the IDENTIFIER is used as a type name.
        // We distinguish this from a regular call f(args) by checking whether
        // the name is a type keyword (primitives) or if this looks like a struct
        // constructor (from() dispatch) — both are handled as TypeConvExprAST.
        // For simplicity, all IDENTIFIER '(' cases go through parseCallExpr
        // first; the semantic pass recognises explicit type casts by resolving the
        // callee name to a type rather than a function.
        // Exception: primitives with type-keyword tokens are handled separately
        // below.

        // Struct literal: IDENTIFIER [ '<' ... '>' ] '{'
        if (allowStructLiteral && looksLikeStructLiteral()) {
            advance(); // consume IDENTIFIER
            std::vector<TypePtr> genericArgs;
            if (check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            return parseStructLiteralExpr(std::move(name), std::move(genericArgs));
        }

        // Behavior access: IDENTIFIER ':' IDENTIFIER  (not ':' used in switch/match)
        // Peek: if after consuming the ident we see ':', and after that an IDENTIFIER,
        // this is a behavior member reference.
        if (peekNext().type == TokenType::COLON &&
            peekAt(2).type == TokenType::IDENTIFIER) {
            advance();                            // consume type name
            advance();                            // consume ':'
            std::string method = advance().value; // consume method name

            auto node = arena_.make<BehaviorAccessExprAST>();
            node->loc = loc;
            node->typeName = std::move(pool_.intern(name));
            node->method = std::move(pool_.intern(method));
            node->isBehaviorMember = true;
            return node;
        }

        // Plain identifier
        advance();
        auto node = arena_.make<IdentifierExprAST>(std::move(pool_.intern(name)));
        node->loc = loc;

        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: returning identifier/struct literal");
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
// IMPORTANT: Does NOT handle '->' (pipeline) or '+>' (composition) - those
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

        ExprPtr val = parseExpr();
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
// Grammar (updated to match func_decl — multiple param groups allowed):
//   anon_func       := [ qualifier_list ] '(' [ param_list ] ')' [ return_type ] block
//
// Single-group:   (x int) int { return x * 2 }
// Multi-group:    (a int) (b int) int { return a + b }
// Async:          async (url string) string { return await httpGet(url) }
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseAnonFuncExpr() {
    SourceLocation loc = currentLoc();
    
    auto node = arena_.make<AnonFuncExprAST>();
    node->loc = loc;
    
    // ── Parse type qualifiers into FuncSignature (~async, ~noinline, etc.) ──────
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        
        node->sig.rawQualifiers.push_back(pool_.intern(advance().value));
        LUC_LOG_EXPR_VERBOSE("\tqualifier: '~" << pool_.lookup(node->sig.rawQualifiers.back()) << "'");
    }
    
    // Parse parameter groups into FuncSignature
    while (check(TokenType::LPAREN)) {
        node->sig.paramGroups.push_back(parseParamGroup());
        LUC_LOG_EXPR_VERBOSE("\tparsed param group with " 
                            << node->sig.paramGroups.back().size() << " params");
    }
    
    // Optional return type
    if (looksLikeType() && !check(TokenType::LBRACE)) {
        node->sig.returnType = parseType();
        LUC_LOG_EXPR_VERBOSE("\treturn type parsed");
    }
    
    // Optional nullable function suffix '?'
    node->sig.isNullable = match(TokenType::QUESTION);
    
    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start anonymous function body");
    } else {
        node->body = parseBlock();
    }
    
    LUC_LOG_EXPR_VERBOSE("parseAnonFuncExpr: paramGroups=" << node->sig.paramGroups.size() 
                        << ", returnType=" << (node->sig.returnType != nullptr)
                        << ", isNullable=" << node->sig.isNullable);
    
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseIntrinsicCallExpr
//
// Grammar:
//   intrinsic_call := '@' IDENTIFIER '(' [ intrinsic_arg_list ] ')'
//   intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }
//   intrinsic_arg  := type_name            -- for @sizeof(T), @alignof(T)
//                   | expr                 -- for @sqrt(x), @memcpy(dst,src,n)
//
// The parser uses a simple disambiguation:
//   If the first argument after '(' is a bare IDENTIFIER that looks like a
//   named type (not followed by an infix operator), and the intrinsic is a
//   type-parameter intrinsic (@sizeof / @alignof), we parse it as typeArg.
//   Otherwise all arguments are parsed as regular expressions.
//
// Type-parameter intrinsics:  sizeof, alignof
// Value-argument intrinsics:  sqrt, abs, min, max, memcpy, memset, ...
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseIntrinsicCallExpr() {
    LUC_LOG_EXPR("parseIntrinsicCallExpr: parsing @ intrinsic");
    SourceLocation loc = currentLoc();
    consume(TokenType::AT_SIGN, "expected '@'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected intrinsic name after '@'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IntrinsicCallExprAST>();
    node->loc           = loc;
    node->intrinsicName = pool_.intern(advance().value);

    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001,
                "expected '(' after intrinsic '@" + std::string(pool_.lookup(node->intrinsicName)) + "'");
        return arena_.make<UnknownExprAST>();
    }
    LUC_LOG_EXPR("parseIntrinsicCallExpr: name='" << pool_.lookup(node->intrinsicName) << "'");
    consume(TokenType::LPAREN, "expected '('");

    // Check if this intrinsic takes a type argument (sizeof/alignof)
    bool isTypeParam = (node->intrinsicName == kw_sizeof) ||
                       (node->intrinsicName == kw_alignof);

    if (isTypeParam) {
        // Parse a type argument.
        if (!check(TokenType::RPAREN)) {
            TypePtr typeArg = parseType();
            if (!typeArg) {
                errorAt(DiagCode::E2005,
                        "expected type argument for '@" + std::string(pool_.lookup(node->intrinsicName)) + "'");
            } else {
                node->typeArg = std::move(typeArg);
            }
        }
    } else {
        // Parse regular expression arguments.
        while (!check(TokenType::RPAREN) && !isAtEnd()) {
            ExprPtr arg = parseExpr();
            if (!arg) {
                errorAt(DiagCode::E2008,
                        "expected argument expression in '@" + std::string(pool_.lookup(node->intrinsicName)) + "'");
                break;
            }
            node->args.push_back(std::move(arg));

            if (check(TokenType::RPAREN)) break;
            if (!match(TokenType::COMMA)) {
                errorAt(DiagCode::E2001,
                        "expected ',' or ')' in intrinsic argument list");
                break;
            }
        }
    }

    consume(TokenType::RPAREN, "expected ')' to close intrinsic call");
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

    ExprPtr subject = parseExpr();
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
            // No progress – abort this arm and skip to next sync point
            errorAt(DiagCode::E2007, "failed to parse match arm, skipping");
            synchronize();
            // If we hit the closing brace or EOF, stop entirely
            if (check(TokenType::RBRACE) || isAtEnd())
                break;
            continue;
        }
        if (arm) {
            node->arms.push_back(std::move(arm));
        }
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
    int argCount = 0;

    while (!check(TokenType::RPAREN) && !isAtEnd()) {
        ExprPtr arg = parseExpr();
        if (!arg) {
            errorAt(DiagCode::E2008, "expected argument expression");
            break;
        }
        args.push_back(std::move(arg));
        argCount++;
        LUC_LOG_EXPR_EXTREME("parseArgList: parsed argument " << argCount);

        if (check(TokenType::RPAREN)) break;

        if (!match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "expected ',' after argument");
            break;
        }

        if (check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2001, "unexpected trailing ',' in argument list");
            break;
        }
    }

    LUC_LOG_EXPR_VERBOSE("parseArgList: parsed " << argCount << " arguments");
    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePipelineExpr
//
// Grammar:
//   pipeline_expr := seed { '->' pipeline_step }
//
// Called from parsePrattExpr when '->' is seen. lhs is already parsed as seed.
// Consumes ALL '->' steps greedily.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parsePipelineExpr(ExprPtr seed) {
    LUC_LOG_EXPR("parsePipelineExpr: building pipeline");
    if (!seed) {
        errorAt(DiagCode::E2008, "expected pipeline seed before '->'");
        auto unknown = arena_.make<UnknownExprAST>();
        unknown->loc = currentLoc();
        return unknown;
    }
    
    LUC_LOG_EXPR_VERBOSE("parsePipelineExpr: seed kind=" << LucDebug::kindToString(seed->kind));
    SourceLocation loc = seed->loc;

    auto node = arena_.make<PipelineExprAST>();
    node->loc = loc;
    node->seed = std::move(seed);

    while (check(TokenType::ARROW)) {
        advance(); // consume '->'
        
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

    // ── Anonymous function detection using precise lookahead ─────────────────
    if (looksLikeAnonFunc()) {
        LUC_LOG_EXPR_VERBOSE("parsePipelineStep: parsing anonymous function");
        ExprPtr anonFuncExpr = parseAnonFuncExpr();
        if (!anonFuncExpr) {
            errorAt(DiagCode::E2002, "expected anonymous function as pipeline step");
            // Return a step with error marker
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            return step;
        }
        step->kind = PipelineStepKind::AnonFunc;
        step->anonFunc = std::move(anonFuncExpr);
        return step;
    }

    // ── Check for primitive type keywords (valid conversion functions) ────────
    bool isPrimitiveType = Parser::isPrimitiveTypeToken(peek().type);

    // Must be either IDENTIFIER or primitive type
    if (!check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        errorAt(DiagCode::E2002, 
                "expected function name, method reference, or anonymous function as pipeline step, got '" + 
                peek().value + "'");
        // Return an error marker step and advance to avoid infinite loop
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        advance();
        return step;
    }

    // Get the name - handle both IDENTIFIER and primitive type tokens
    std::string name;
    if (isPrimitiveType) {
        Token tok = advance();
        name = tok.value;
        LUC_LOG_EXPR_VERBOSE("parsePipelineStep: primitive type conversion '" << name << "'");
    } else {
        name = advance().value;
        LUC_LOG_EXPR_VERBOSE("parsePipelineStep: identifier '" << name << "'");
    }

    // ── BehaviorRef: IDENTIFIER ':' IDENTIFIER ────────────────────────────────
    if (check(TokenType::COLON)) {
        // Need to check that after ':' there's an IDENTIFIER
        if (peekNext().type == TokenType::IDENTIFIER) {
            advance(); // consume ':'
            std::string method = advance().value;
            step->kind = PipelineStepKind::BehaviorRef;
            step->typeName = std::move(pool_.intern(name));
            step->method = std::move(pool_.intern(method));
            LUC_LOG_EXPR_VERBOSE("parsePipelineStep: BehaviorRef " << pool_.lookup(step->typeName) << ":" << pool_.lookup(step->method));
            return step;
        }
        // ':' but not followed by IDENTIFIER - treat as regular identifier
        LUC_LOG_EXPR_VERBOSE("parsePipelineStep: colon without identifier, treating as regular ident");
    }

    // ── FieldRef: IDENTIFIER '.' IDENTIFIER ──────────────────────────────────
    if (check(TokenType::DOT)) {
        if (peekNext().type == TokenType::IDENTIFIER) {
            advance(); // consume '.'
            std::string field = advance().value;
            step->kind = PipelineStepKind::FieldRef;
            step->ident = std::move(pool_.intern(name));
            step->field = std::move(pool_.intern(field));
            LUC_LOG_EXPR_VERBOSE("parsePipelineStep: FieldRef " << pool_.lookup(step->ident) << "." << pool_.lookup(step->field));
            return step;
        }
        // '.' but not followed by IDENTIFIER - error
        errorAt(DiagCode::E2003, "expected field name after '.'");
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        return step;
    }

    // ── ArgPack: IDENTIFIER '(' args ')' '!' ─────────────────────────────────
    if (check(TokenType::LPAREN)) {
        consume(TokenType::LPAREN, "expected '('");
        std::vector<ExprPtr> packArgs;
        if (!check(TokenType::RPAREN)) {
            packArgs = parseArgList();
        }
        consume(TokenType::RPAREN, "expected ')'");
        
        if (!check(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' to mark argument pack in pipeline step");
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            return step;
        }
        advance(); // consume '!'
        
        step->kind = PipelineStepKind::ArgPack;
        step->ident = std::move(pool_.intern(name));
        step->packArgs = std::move(packArgs);
        LUC_LOG_EXPR_VERBOSE("parsePipelineStep: ArgPack " << pool_.lookup(step->ident) << " with " << step->packArgs.size() << " args");
        return step;
    }

    // ── Ident: bare function name (IDENTIFIER or primitive type) ──────────────
    step->kind = PipelineStepKind::Ident;
    step->ident = std::move(pool_.intern(name));
    LUC_LOG_EXPR_VERBOSE("parsePipelineStep: Ident '" << pool_.lookup(step->ident) << "'");
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
    do {
        auto pat = parsePattern();
        if (!pat) {
            break;
        }
        arm->patterns.push_back(std::move(pat));

    } while (match(TokenType::COMMA));

    // Optional guard: 'if' expr
    if (check(TokenType::IF)) {
        advance(); // consume 'if'
        arm->guard = parseExpr();
        if (!arm->guard) {
            errorAt(DiagCode::E2008, "expected guard expression after 'if' in match arm");
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
        // No more commas allowed
        if (match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "match arm cannot have more than two expressions");
        }
    }
    
    return arm;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseDefaultArm
//
// Grammar:  'default' '->' arm_body
// ─────────────────────────────────────────────────────────────────────────────
DefaultArmPtr Parser::parseDefaultArm() {
    SourceLocation loc = currentLoc();
    consume(TokenType::DEFAULT, "expected 'default'");
    consume(TokenType::FAT_ARROW, "expected '=>' after 'default'");

    auto arm = arena_.make<DefaultArmAST>();
    arm->loc = loc;

    // Parse one or more result expressions
    do {
        ExprPtr exp = parseExpr();
        if (!exp) break;
        arm->exprs.push_back(std::move(exp));
    } while (match(TokenType::COMMA));

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

    // Literal patterns (and possibly ranges starting with a literal)
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
        case TokenType::MINUS: // negative numeric literals: -42
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

        // Bind pattern
        advance(); // consume IDENTIFIER

        // Check for range from bind (invalid but parsed defensively)
        if (check(TokenType::RANGE)) {
             errorAt(DiagCode::E2007, "bind patterns cannot be used as range bounds");
             advance(); // consume '..'
             parseLiteralOrRangePattern(); // consume hi to recover
        }

        return parseBindPattern(std::move(pool_.intern(name)));
    }

    errorAt(DiagCode::E2007, "expected pattern");
    LUC_LOG_EXPR_VERBOSE("parsePattern: returning nullptr (error)");
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

        FieldPatternPtr fp = parseFieldPattern();
        if (fp)
            pat->fields.push_back(std::move(fp));
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
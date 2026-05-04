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
#include "diagnostics/DiagnosticCodes.hpp"

#include <cassert>

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
// Postfix operations (call, index, '.', ':', '.?') are handled at the top of
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
        // with the reference operator &T and union type separator |
        case TokenType::BIT_AND:   // &&
        case TokenType::BIT_OR:    // ||
        case TokenType::BIT_XOR:   // ~^
        case TokenType::SHL:       // <<
        case TokenType::SHR:       // >>
            return PREC_BITWISE;

        // BIT_NOT (~) is unary only — not an infix operator, no precedence here.
        // AMPERSAND (&) is the reference operator in expression context — not bitwise.
        // PIPE (|) is the union type separator in type context — not bitwise.

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
            // PIPE is union type separator in type context — not bitwise.
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
        return nullptr;
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
            auto node = std::make_unique<AssignExprAST>();
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
            auto node = std::make_unique<IsExprAST>();
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

            // If lhs is already a NullableChainExprAST, attach the fallback.
            if (lhs->isa<NullableChainExprAST>()) {
                auto* chain = lhs->as<NullableChainExprAST>();
                if (chain->fallback) {
                    errorAt(DiagCode::E2007, "duplicate '\?\?' in nullable chain");
                } else {
                    chain->fallback = std::move(fallback);
                }
            } else {
                // Standalone ?? not part of a .? chain — still valid as a
                // general nil-coalescing expression.
                SourceLocation loc = lhs->loc;
                auto node = std::make_unique<NullableChainExprAST>();
                node->loc = loc;
                node->object = std::move(lhs);
                // steps is empty — this is just  expr ?? fallback
                node->fallback = std::move(fallback);
                lhs = std::move(node);
            }
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
        auto node = std::make_unique<BinaryExprAST>();
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
                return nullptr;
            }
            auto node = std::make_unique<UnaryExprAST>();
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
                return nullptr;
            }
            auto node = std::make_unique<UnaryExprAST>();
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
                return nullptr;
            }
            auto node = std::make_unique<UnaryExprAST>();
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
                return nullptr;
            }
            auto node = std::make_unique<UnaryExprAST>();
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

    // ── async function body / async anonymous function ────────────────────────
    // Two forms:
    //   async { ... }             — block body form (no param list repeated)
    //   async (params) ret { ... } — explicit anon func form
    //
    // Both map to AnonFuncExprAST with isAsync = true.
    // The block body form has empty params and nullptr returnType — the outer
    // FuncDeclAST already carries the signature; this node is just the body.
    if (check(TokenType::ASYNC)) {
        // Peek past 'async': if the very next meaningful token is '{', this is
        // the short block-body form.  Otherwise it is the explicit anon func
        // form that starts with '(' — delegate to parseAnonFuncExpr() which
        // consumes the param group and optional return type itself.
        TokenType afterAsync = peekNext().type;
        if (afterAsync == TokenType::LBRACE) {
            // async { ... }  — block body, no param repetition
            SourceLocation asyncLoc = currentLoc();
            advance(); // consume 'async'
            ++asyncDepth_;

            auto node = std::make_unique<AnonFuncExprAST>();
            node->loc     = asyncLoc;
            node->isAsync = true;
            // paramGroups intentionally left empty (no groups) and returnType
            // nullptr — the enclosing FuncDeclAST owns the real signature.
            node->body = parseBlock();

            --asyncDepth_;
            return node;
        }
        // async (params) ret { ... } — fall through to parseAnonFuncExpr
        return parseAnonFuncExpr();
    }

    // ── array literal ─────────────────────────────────────────────────────────
    if (check(TokenType::LBRACKET))
        return parseArrayLiteralExpr();

    // ── bare block body  { stmts }  in expression position ───────────────────
    // Handles two cases from the grammar:
    //
    //   func_body := block      — e.g.  let f (x int) int = { return x + 1 }
    //                              or   f = { return 42 }   (reassignment)
    //
    // When the expression parser reaches a lone '{', it means the RHS of a
    // function declaration or reassignment is a plain block body — not a struct
    // literal (those are always preceded by IDENTIFIER) and not a block
    // statement (those are parsed by parseStmt, never by parseExpr).
    //
    // We produce an AnonFuncExprAST with empty params and nullptr returnType.
    // The enclosing FuncDeclAST already owns the real signature; this node
    // carries only the body.  The semantic pass treats this form identically
    // to the explicit anon-func form.
    if (check(TokenType::LBRACE)) {
        SourceLocation blockLoc = currentLoc();
        auto node = std::make_unique<AnonFuncExprAST>();
        node->loc     = blockLoc;
        node->isAsync = false;
        // params intentionally empty, returnType intentionally nullptr
        node->body = parseBlock();
        return node;
    }

    // ── anonymous function (non-async) ────────────────────────────────────────
    // A '(' that is immediately followed by ')' or 'IDENTIFIER type' is an anon
    // func. Distinguish from a grouped expression by lookahead.
    // Heuristic: if current is '(' and:
    //   - next is ')' (empty params)
    //   - next is IDENTIFIER and the token after that looks like a type start
    // then it's an anonymous function.
    if (check(TokenType::LPAREN)) {
        // Lookahead to distinguish anon func from grouped expr.
        // An anon func must have ')' as its only next token (empty), or
        // IDENTIFIER followed by something that looks like a type.
        TokenType n1 = peekNext().type;
        if (n1 == TokenType::RPAREN) {
            // Could be  ()  — could be empty anon func or empty group.
            // Peek further: after ')' comes a type start or '{' → anon func.
            TokenType n2 = peekAt(2).type;
            bool anonFunc = (n2 == TokenType::LBRACE) || // () { ... }
                            looksLikeType();             // () RetType { ... } — approximate
            // More precise: after '(' ')' check if token at offset 2 looks like type start
            // We check if offset 2 is looksLikeType by inspecting tokens directly.
            auto isTypeStart = [&](TokenType tt) {
                switch (tt) {
                    case TokenType::TYPE_BOOL:
                    case TokenType::TYPE_BYTE:
                    case TokenType::TYPE_SHORT:
                    case TokenType::TYPE_INT:
                    case TokenType::TYPE_LONG:
                    case TokenType::TYPE_UBYTE:
                    case TokenType::TYPE_USHORT:
                    case TokenType::TYPE_UINT:
                    case TokenType::TYPE_ULONG:
                    case TokenType::TYPE_INT8:
                    case TokenType::TYPE_INT16:
                    case TokenType::TYPE_INT32:
                    case TokenType::TYPE_INT64:
                    case TokenType::TYPE_UINT8:
                    case TokenType::TYPE_UINT16:
                    case TokenType::TYPE_UINT32:
                    case TokenType::TYPE_UINT64:
                    case TokenType::TYPE_FLOAT:
                    case TokenType::TYPE_DOUBLE:
                    case TokenType::TYPE_DECIMAL:
                    case TokenType::TYPE_STRING:
                    case TokenType::TYPE_CHAR:
                    case TokenType::TYPE_ANY:
                    case TokenType::IDENTIFIER:
                    case TokenType::LBRACKET:
                    case TokenType::AMPERSAND:
                    case TokenType::MUL:
                    case TokenType::LPAREN:
                        return true;
                    default:
                        return false;
                }
            };
            if (n2 == TokenType::LBRACE || isTypeStart(n2)) {
                return parseAnonFuncExpr();
            }
            // Otherwise it is  ()  as a grouped unit (unusual but legal).
            // Fall through to grouped expression.
        } else if (n1 == TokenType::IDENTIFIER) {
            // Could be  (name type ...)  — anon func if token after name is a type.
            TokenType n2 = peekAt(2).type;
            auto isTypeStart = [&](TokenType tt) {
                switch (tt) {
                    case TokenType::TYPE_BOOL:
                    case TokenType::TYPE_BYTE:
                    case TokenType::TYPE_SHORT:
                    case TokenType::TYPE_INT:
                    case TokenType::TYPE_LONG:
                    case TokenType::TYPE_UBYTE:
                    case TokenType::TYPE_USHORT:
                    case TokenType::TYPE_UINT:
                    case TokenType::TYPE_ULONG:
                    case TokenType::TYPE_INT8:
                    case TokenType::TYPE_INT16:
                    case TokenType::TYPE_INT32:
                    case TokenType::TYPE_INT64:
                    case TokenType::TYPE_UINT8:
                    case TokenType::TYPE_UINT16:
                    case TokenType::TYPE_UINT32:
                    case TokenType::TYPE_UINT64:
                    case TokenType::TYPE_FLOAT:
                    case TokenType::TYPE_DOUBLE:
                    case TokenType::TYPE_DECIMAL:
                    case TokenType::TYPE_STRING:
                    case TokenType::TYPE_CHAR:
                    case TokenType::TYPE_ANY:
                    case TokenType::IDENTIFIER:
                    case TokenType::LBRACKET:
                    case TokenType::AMPERSAND:
                    case TokenType::MUL:
                    case TokenType::VARIADIC:
                        return true;
                    default:
                        return false;
                }
            };
            if (isTypeStart(n2)) {
                return parseAnonFuncExpr();
            }
        }

        // ── grouped expression: '(' expr ')' ──────────────────────────────────
        advance(); // consume '('
        ExprPtr inner = parseExpr();
        consume(TokenType::RPAREN, DiagCode::E2001, "expected ')' to close grouped expression");
        return inner;
    }

    // ── '*' unsafe explicit cast  *T(expr) ────────────────────────────────────
    if (check(TokenType::MUL)) {
        advance(); // consume '*'
        TypePtr targetType = parseBaseType();
        if (!targetType) {
            errorAt(DiagCode::E2005, "expected type after '*' in unsafe cast");
            return nullptr;
        }
        if (!check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' after type in unsafe cast '*T(expr)'");
            return nullptr;
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

            auto node = std::make_unique<BehaviorAccessExprAST>();
            node->loc = loc;
            node->typeName = std::move(name);
            node->method = std::move(method);
            node->isBehaviorMember = true;
            return node;
        }

        // Plain identifier
        advance();
        auto node = std::make_unique<IdentifierExprAST>(std::move(name));
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
    return nullptr;
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
//   '.?' IDENTIFIER     — nullable chain step
//   '!!'                — not valid here (only inside pipeline steps)
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parsePostfixExpr(ExprPtr lhs) {
    LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: lhs kind=" << LucDebug::kindToString(lhs->kind));
    while (true) {
        // ── Function call: lhs '(' args ')' ──────────────────────────────────
        if (check(TokenType::LPAREN)) {
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: function call");
            lhs = parseCallExpr(std::move(lhs), {});
            continue;
        }

        // ── Generic call: lhs '<' types '>' '(' args ')' ─────────────────────
        // This is ambiguous with less-than comparisons. We use a simple
        // heuristic: only treat '<' as a generic open when the lhs is an
        // IdentifierExprAST or BehaviorAccessExprAST (i.e. a name), and when the
        // content between '<' and '>' looks like a type list.
        // Full disambiguation would require unbounded lookahead; we handle the
        // common cases and let the semantic pass catch the rest.
        if (check(TokenType::LESS) &&
            (lhs->isa<IdentifierExprAST>() || lhs->isa<BehaviorAccessExprAST>())) {
            // Save position so we can roll back if this is actually '<' comparison.
            std::size_t savedPos = pos_;
            std::vector<TypePtr> genericArgs;
            bool ok = false;
            // Attempt to parse generic args.
            // We do a simple bracket-balanced scan to detect '>'.
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
            auto node = std::make_unique<FieldAccessExprAST>();
            node->loc = lhs->loc;
            node->object = std::move(lhs);
            node->field = std::move(field);
            lhs = std::move(node);
            continue;
        }

        // ── Nullable chain step: lhs '.?' IDENTIFIER ─────────────────────────
        if (check(TokenType::DOT_QUESTION)) {
            // Start or extend a NullableChainExprAST.
            LUC_LOG_EXPR_VERBOSE("parsePostfixExpr: nullable chain");
            NullableChainExprAST* existing =
                lhs->isa<NullableChainExprAST>() ? lhs->as<NullableChainExprAST>() : nullptr;

            if (!existing) {
                // Start a new chain.
                auto chain = std::make_unique<NullableChainExprAST>();
                chain->loc = lhs->loc;
                chain->object = std::move(lhs);
                lhs = std::move(chain);
                existing = static_cast<NullableChainExprAST *>(lhs.get());
            }

            advance(); // consume '.?'

            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.?'");
                break;
            }
            existing->steps.push_back(advance().value);
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
            return nullptr;
    }

    auto node = std::make_unique<LiteralExprAST>(kind, tok.value);
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

    auto node = std::make_unique<ArrayLiteralExprAST>();
    node->loc = loc;

    while (!check(TokenType::RBRACKET) && !isAtEnd()) {
        match(TokenType::COMMA); // optional separator / trailing comma
        if (check(TokenType::RBRACKET))
            break;

        ExprPtr elem = parseExpr();
        if (!elem) {
            errorAt(DiagCode::E2008, "expected expression inside array literal");
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

    auto node = std::make_unique<StructLiteralExprAST>();
    node->loc = loc;
    node->typeName = std::move(typeName);
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
            continue;
        }
        std::string fieldName = advance().value;

        consume(TokenType::ASSIGN, "expected '=' after field name '" + fieldName + "' in struct literal");

        ExprPtr val = parseExpr();
        if (!val) {
            errorAt(DiagCode::E2008, "expected expression for field '" + fieldName + "' in struct literal");
            continue;
        }

        node->inits.push_back({std::move(fieldName), std::move(val), fieldLoc});
    }

    consume(TokenType::RBRACE, "expected '}' to close struct literal");
    LUC_LOG_EXPR_VERBOSE("parseStructLiteralExpr: parsed " << node->inits.size() << " field inits");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseAnonFuncExpr
//
// Grammar (updated to match func_decl — multiple param groups allowed):
//   anon_func := [ 'async' ] param_group { param_group } [ return_type ] block
//
// Single-group:   (x int) int { return x * 2 }
// Multi-group:    (a int) (b int) int { return a + b }
// Async:          async (url string) string { return await httpGet(url) }
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseAnonFuncExpr() {
    SourceLocation loc = currentLoc();
    bool isAsync = match(TokenType::ASYNC);
    LUC_LOG_EXPR("parseAnonFuncExpr: isAsync=" << (isAsync ? "true" : "false"));

    if (isAsync)
        ++asyncDepth_;

    auto node = std::make_unique<AnonFuncExprAST>();
    node->loc = loc;
    node->isAsync = isAsync;

    // Parse one or more parameter groups — same loop as parseFuncDecl.
    // At least one '(' is guaranteed here because the caller already
    // verified the current token is '(' (or 'async' followed by '(').
    while (check(TokenType::LPAREN)) {
        node->paramGroups.push_back(parseParamGroup());
    }

    // Optional return type — present if current token looks like a type
    // but is not '{' (which would start the body).
    if (looksLikeType() && !check(TokenType::LBRACE)) {
        node->returnType = parseType();
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start anonymous function body");
    } else {
        node->body = parseBlock();
    }

    if (isAsync)
        --asyncDepth_;

    LUC_LOG_EXPR_VERBOSE("parseAnonFuncExpr: paramGroups=" << node->paramGroups.size() 
                        << ", hasReturnType=" << (node->returnType != nullptr));
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
        return nullptr;
    }

    auto node = std::make_unique<IntrinsicCallExprAST>();
    node->loc           = loc;
    node->intrinsicName = advance().value; // e.g. "sizeof", "sqrt"

    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001,
                "expected '(' after intrinsic '@" + node->intrinsicName + "'");
        return nullptr;
    }
    LUC_LOG_EXPR("parseIntrinsicCallExpr: name='" << node->intrinsicName << "'");
    consume(TokenType::LPAREN, "expected '('");

    // ── Decide if the first argument is a type ────────────────────────────────
    // Type-parameter intrinsics only take one argument and it is a type name.
    static const std::initializer_list<std::string> typeParamIntrinsics = {
        "sizeof", "alignof"
    };
    bool isTypeParam = false;
    for (const auto& n : typeParamIntrinsics)
        if (n == node->intrinsicName) { isTypeParam = true; break; }

    if (isTypeParam) {
        // Parse a type argument.
        if (!check(TokenType::RPAREN)) {
            TypePtr typeArg = parseType();
            if (!typeArg) {
                errorAt(DiagCode::E2005,
                        "expected type argument for '@" + node->intrinsicName + "'");
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
                        "expected argument expression in '@" + node->intrinsicName + "'");
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
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseAwaitExpr() {
    LUC_LOG_EXPR("parseAwaitExpr");
    SourceLocation loc = currentLoc();
    consume(TokenType::AWAIT, "expected 'await'");

    if (asyncDepth_ == 0) {
        error(loc, DiagCode::E2006, "'await' is only valid inside an 'async' function");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'await' is not valid inside a 'parallel' block");
    }

    ExprPtr inner = parsePrattExpr(PREC_NONE);
    if (!inner) {
        errorAt(DiagCode::E2008, "expected expression after 'await'");
        return nullptr;
    }

    auto node = std::make_unique<AwaitExprAST>(std::move(inner));
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
        return nullptr;
    }
    LUC_LOG_EXPR_VERBOSE("parseMatchExpr: subject parsed");

    consume(TokenType::LBRACE, "expected '{' after match subject");

    auto node = std::make_unique<MatchExprAST>();
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
            continue;
        }

        MatchArmPtr arm = parseMatchArm();
        if (arm)
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
        return nullptr;
    }
    LUC_LOG_EXPR_VERBOSE("parseIfExpr: condition parsed");

    // Inline if expression: if cond ?? thenExpr else elseExpr
    if (!match(TokenType::QUESTION_QUESTION)) {
        errorAt(DiagCode::E2001, "expected '\?\?' after if condition in expression form");
        return nullptr;
    }

    ExprPtr thenBranch = parseExpr();
    if (!thenBranch) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?' in 'if' expression");
    }
    LUC_LOG_EXPR_VERBOSE("parseIfExpr: then branch parsed");

    if (!match(TokenType::ELSE)) {
        errorAt(DiagCode::E2006, "expression-form 'if' requires an 'else' branch");
        return nullptr;
    }

    ExprPtr elseBranch = parseExpr();
    if (!elseBranch) {
        errorAt(DiagCode::E2008, "expected expression after 'else' in 'if' expression");
    }
    LUC_LOG_EXPR_VERBOSE("parseIfExpr: else branch parsed");

    auto node = std::make_unique<IfExprAST>();
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
        return nullptr;
    }

    consume(TokenType::RPAREN, "expected ')' to close explicit type cast");
    LUC_LOG_EXPR_VERBOSE("parseTypeConvExpr: expression parsed");

    auto node = std::make_unique<TypeConvExprAST>(
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
        return nullptr;
    }
    LUC_LOG_EXPR_VERBOSE("parseRangeExpr: hi parsed");

    auto node = std::make_unique<RangeExprAST>();
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

    auto node = std::make_unique<CallExprAST>();
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
        return nullptr;
    }

    auto node = std::make_unique<IndexExprAST>();
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
            return nullptr;
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
    LUC_LOG_EXPR_VERBOSE("parsePipelineExpr: seed kind=" << LucDebug::kindToString(seed->kind));
    SourceLocation loc = seed->loc;

    auto node = std::make_unique<PipelineExprAST>();
    node->loc = loc;
    node->seed = std::move(seed);

    while (check(TokenType::ARROW)) {
        advance(); // consume '->'
        PipelineStepPtr step = parsePipelineStep();
        if (!step) {
            errorAt(DiagCode::E2002, "expected pipeline step after '->'");
            break;
        }
        node->steps.push_back(std::move(step));
    }

    if (node->steps.empty()) {
        errorAt(DiagCode::E2006, "pipeline '->' requires at least one step");
        return node->seed ? std::move(node->seed) : nullptr;
    }

    LUC_LOG_EXPR("parsePipelineExpr: " << node->steps.size() << " pipeline steps");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePipelineStep
//
// Five forms:
//   Ident       fn
//   BehaviorRef Type:method
//   FieldRef    obj.field       (IDENTIFIER '.' IDENTIFIER, non-callable obj)
//   ArgPack     fn(args)!
//   AnonFunc    [ async ] '(' params ')' [ ret ] block
// ─────────────────────────────────────────────────────────────────────────────
PipelineStepPtr Parser::parsePipelineStep() {
    LUC_LOG_EXPR_VERBOSE("parsePipelineStep: token='" << peek().value << "'");
    SourceLocation loc = currentLoc();
    auto step = std::make_unique<PipelineStepAST>();
    step->loc = loc;

    // ── AnonFunc: '(' ... or 'async' '(' ─────────────────────────────────────
    if (check(TokenType::LPAREN) || check(TokenType::ASYNC)) {
        step->kind = PipelineStepKind::AnonFunc;
        ExprPtr af = parseAnonFuncExpr();
        step->anonFunc = std::unique_ptr<AnonFuncExprAST>(
            static_cast<AnonFuncExprAST *>(af.release()));
        return step;
    }

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2002, "expected function name, method reference, or anonymous function as pipeline step");
        return nullptr;
    }

    std::string name = advance().value;

    // ── BehaviorRef: IDENTIFIER ':' IDENTIFIER ────────────────────────────────
    if (check(TokenType::COLON) && peekNext().type == TokenType::IDENTIFIER) {
        advance(); // consume ':'
        std::string method = advance().value;
        step->kind = PipelineStepKind::BehaviorRef;
        step->typeName = std::move(name);
        step->method = std::move(method);
        return step;
    }

    // ── FieldRef: IDENTIFIER '.' IDENTIFIER ──────────────────────────────────
    if (check(TokenType::DOT) && peekNext().type == TokenType::IDENTIFIER) {
        advance(); // consume '.'
        std::string field = advance().value;
        step->kind = PipelineStepKind::FieldRef;
        step->ident = std::move(name);
        step->field = std::move(field);
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
        consume(TokenType::BANG, "expected '!' to mark argument pack in pipeline step");
        step->kind = PipelineStepKind::ArgPack;
        step->ident = std::move(name);
        step->packArgs = std::move(packArgs);
        return step;
    }

    // ── Ident: bare function name ─────────────────────────────────────────────
    step->kind = PipelineStepKind::Ident;
    step->ident = std::move(name);
    LUC_LOG_EXPR_VERBOSE("parsePipelineStep: kind=" << static_cast<int>(step->kind));
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

    auto node = std::make_unique<ComposeExprAST>();
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
//   Ident       fn
//   BehaviorRef Type:method
//   FieldRef    obj.field
// ─────────────────────────────────────────────────────────────────────────────
ComposeOperandPtr Parser::parseComposeOperand() {
    LUC_LOG_EXPR_VERBOSE("parseComposeOperand: token='" << peek().value << "'");
    SourceLocation loc = currentLoc();
    auto op = std::make_unique<ComposeOperandAST>();
    op->loc = loc;

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2002, "expected function name or method reference as composition operand");
        return nullptr;
    }

    std::string name = advance().value;

    // BehaviorRef
    if (check(TokenType::COLON) && peekNext().type == TokenType::IDENTIFIER) {
        advance();
        std::string method = advance().value;
        op->kind = ComposeOperandKind::BehaviorRef;
        op->typeName = std::move(name);
        op->method = std::move(method);
        LUC_LOG_EXPR_VERBOSE("parseComposeOperand: BehaviorRef " << op->typeName << ":" << op->method);
        return op;
    }

    // FieldRef
    if (check(TokenType::DOT) && peekNext().type == TokenType::IDENTIFIER) {
        advance();
        std::string field = advance().value;
        op->kind = ComposeOperandKind::FieldRef;
        op->ident = std::move(name);
        op->field = std::move(field);
        LUC_LOG_EXPR_VERBOSE("parseComposeOperand: FieldRef " << op->ident << "." << op->field);
        return op;
    }

    // Ident
    op->kind = ComposeOperandKind::Ident;
    op->ident = std::move(name);
    LUC_LOG_EXPR_VERBOSE("parseComposeOperand: Ident " << op->ident);
    return op;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseNullCoalesceExpr  — handled inline in parsePrattExpr
// (separate function kept for symmetry; currently unused)
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseNullCoalesceExpr(ExprPtr lhs) {
    // This path is taken by the parsePrattExpr infix loop directly.
    // Kept as a named function for future refactoring.
    (void)lhs;
    return nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// PATTERN PARSING
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// parseMatchArm
//
// Grammar:
//   match_arm := pattern { ',' pattern } [ 'if' guard_expr ] '->' arm_body
// ─────────────────────────────────────────────────────────────────────────────
MatchArmPtr Parser::parseMatchArm() {
    SourceLocation loc = currentLoc();

    auto arm = std::make_unique<MatchArmAST>();
    arm->loc = loc;

    // Parse comma-separated pattern list
    do {
        auto pat = parsePattern();
        if (!pat) {
            // Error already recorded by parsePattern() if it returned nullptr,
            // but we ensure we don't proceed with an empty or broken arm.
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

    consume(TokenType::ARROW, "expected '->' after match pattern");

    // Parse one or more result expressions
    do {
        ExprPtr exp = parseExpr();
        if (!exp) break;
        arm->exprs.push_back(std::move(exp));
    } while (match(TokenType::COMMA));
    
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
    consume(TokenType::ARROW, "expected '->' after 'default'");

    auto arm = std::make_unique<DefaultArmAST>();
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
std::unique_ptr<BaseAST> Parser::parsePattern() {
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
            return parseTypePattern(std::move(name));
        }

        // Struct pattern: IDENTIFIER '{'
        if (peekNext().type == TokenType::LBRACE) {
            advance(); // consume IDENTIFIER
            return parseStructPattern(std::move(name));
        }

        // Bind pattern (may be followed by '..' for range)
        advance(); // consume IDENTIFIER

        // Range from bind: n..m — unusual but supported
        if (check(TokenType::RANGE)) {
            // Wrap name as a literal-like expression and build a range pattern.
            // Bind names cannot appear in range patterns per the grammar, but
            // we parse defensively and let the semantic pass reject it.
            advance(); // consume '..'
            auto hi = parseLiteralOrRangePattern();
            // Fall back: treat as a bind pattern.
            (void)hi;
        }

        return parseBindPattern(std::move(name));
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
std::unique_ptr<BaseAST> Parser::parseLiteralOrRangePattern() {
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
 
        auto loExpr = std::make_unique<LiteralExprAST>(kind, std::move(rawValue));
        loExpr->loc = loc;
        auto hiExpr = std::make_unique<LiteralExprAST>(hiKind, std::move(hiRaw));
        hiExpr->loc = locOf(hiTok);
 
        auto range = std::make_unique<RangeExprAST>();
        range->loc = loc;
        range->lo = std::move(loExpr);
        range->hi = std::move(hiExpr);
        range->isExclusive = isExclusive;
        return range;
    }
 
    auto pat = std::make_unique<LiteralExprAST>(kind, std::move(rawValue));
    pat->loc = loc;
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseBindPattern
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<BindPatternAST> Parser::parseBindPattern(std::string name) {
    SourceLocation loc = currentLoc();
    auto pat = std::make_unique<BindPatternAST>(std::move(name));
    pat->loc = loc;
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTypePattern
//
// Grammar:  IDENTIFIER 'is' type
// Called after the IDENTIFIER has been consumed.
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<TypePatternAST> Parser::parseTypePattern(std::string bindName) {
    SourceLocation loc = currentLoc();
    consume(TokenType::IS, "expected 'is' in type pattern");

    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E2005, "expected type after 'is' in type pattern");
        return nullptr;
    }

    auto pat = std::make_unique<TypePatternAST>();
    pat->loc = loc;
    pat->bindName = std::move(bindName);
    pat->checkType = std::move(checkType);
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseWildcardPattern
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<WildcardPatternAST> Parser::parseWildcardPattern() {
    SourceLocation loc = currentLoc();
    consume(TokenType::WILDCARD, "expected '_'");
    auto pat = std::make_unique<WildcardPatternAST>();
    pat->loc = loc;
    return pat;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseStructPattern
//
// Grammar:  IDENTIFIER '{' { field_pattern } '}'
// Called after the type name has been consumed.
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<StructPatternAST> Parser::parseStructPattern(std::string typeName) {
    LUC_LOG_EXPR("parseStructPattern: type='" << typeName << "'");
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACE, "expected '{' in struct pattern");

    auto pat = std::make_unique<StructPatternAST>();
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

    auto fp = std::make_unique<FieldPatternAST>();
    fp->loc = loc;
    fp->field = std::move(fieldName);

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
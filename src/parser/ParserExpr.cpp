/**
 * @file ParserExpr.cpp
 *
 * @responsibility Implements the Pratt parser for all LUC expressions and pattern matching.
 *
 * This file contains the heart of the expression parsing logic:
 *   - Pratt parser with precedence climbing (parsePrattExpr)
 *   - Prefix, infix, and postfix operator handling
 *   - Literals, identifiers, array literals, struct literals
 *   - Function calls, indexing, field access, behavior access
 *   - Pipeline operator (|>) and composition operator (+>)
 *   - Match expressions with pattern matching
 *   - If expressions, await expressions, intrinsic calls
 *   - Pattern parsing for match arms (bind, wildcard, type, struct patterns)
 *
 * All expression parsers consume tokens from the parser's stream and build
 * corresponding ExprAST nodes. The Pratt parser uses a precedence table
 * defined in the anonymous namespace below.
 *
 * @related
 *   - Parser.hpp – class declaration and shared utilities
 *   - Parser.cpp – core token stream primitives
 *   - ParserDecl.cpp – declaration parsing (called from expressions in some contexts)
 *   - ParserStmt.cpp – statement parsing (expressions can appear in statements)
 *   - ParserType.cpp – type parsing (used in type casts and is-expressions)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Precedence Helpers
 *   infixPrec()                     – precedence of infix operators
 *   tokenToBinaryOp()               – TokenType → BinaryOp
 *   tokenToAssignOp()               – TokenType → AssignOp
 *   isAssignOp()                    – true for assignment operators
 *
 * ██ Pratt Parser Core
 *   parseExpr()                     – root entry point
 *   parsePrattExpr()                – Pratt climbing main loop
 *   parseInfixAssign()              – handles assignment operators
 *   parseInfixIs()                  – handles 'is' type check
 *   parseInfixNullCoalesce()        – handles '??' operator
 *   parseInfixBinary()              – generic binary operator
 *
 * ██ Prefix & Primary Parsers
 *   parsePrefixExpr()               – unary operators (-, not, ~, &)
 *   parsePrimaryExpr()              – atoms: literals, identifiers, grouped, special forms
 *
 * ██ Postfix Parser
 *   parsePostfixExpr()              – calls, indexing, field access, nullable chain
 *
 * ██ Literal & Value Parsers
 *   parseLiteralExpr()              – scalar literals
 *   parseArrayLiteralExpr()         – [ ... ] array literals
 *   parseStructLiteralExpr()        – Type { field = value, ... }
 *   parseAnonFuncExpr()             – (params) -> ret { ... }
 *   parseAwaitExpr()                – await expr
 *   parseTypeConvExpr()             – type(expr) or *type(expr)
 *   parseRangeExpr()                – lo .. hi
 *
 * ██ Call & Index Parsers
 *   parseCallExpr()                 – callee(args)
 *   parseIndexExpr()                – target[idx] or target[start..end]
 *   parseIntrinsicCallExpr()        – #name(args)
 *   parseArgList()                  – comma‑separated argument list
 *
 * ██ Pipeline & Composition
 *   parsePipelineExpr()             – seed |> step |> step
 *   parsePipelineStep()             – one pipeline step (dispatcher)
 *   parseAnonFuncPipelineStep()     – anonymous function as step
 *   parseBehaviorPipelineStep()     – Type:method as step
 *   parseFieldPipelineStep()        – obj.field as step
 *   parseIndexPipelineStep()        – arr[idx] as step
 *   parseArgPackPipelineStep()      – fn(args)! as step
 *   parseComposeExpr()              – f +> g +> h
 *   parseComposeOperand()           – one composition operand
 *
 * ██ Match Expression & Patterns
 *   parseMatchExpr()                – match subject { arms }
 *   parseMatchArm()                 – pattern [if guard] => expr [, expr]
 *   parseDefaultArm()               – default => expr [, expr]
 *   parsePattern()                  – dispatch to pattern sub‑parsers
 *   parseLiteralOrRangePattern()    – literal or lo..hi range
 *   parseBindPattern()              – identifier
 *   parseTypePattern()              – identifier is type
 *   parseWildcardPattern()          – _
 *   parseStructPattern()            – Type { field, field: pattern, ... }
 *   parseFieldPattern()             – field or field: pattern
 *
 * ██ Lvalue Parser (for multi‑assignment)
 *   parseLvalue()                   – assignable left‑hand side
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * PRECEDENCE TABLE (from LUC_GRAMMAR.md)
 *
 *   PREC_ASSIGN   = 1   =  +=  -=  *=  /=  ^=  %=  &&=  ||=  ~^=  <<=  >>=
 *   PREC_COMPOSE  = 2   +>
 *   PREC_PIPELINE = 3   |>
 *   PREC_NULLCOAL = 4   ??
 *   PREC_OR       = 5   or
 *   PREC_AND      = 6   and
 *   PREC_CMP      = 7   == != < > <= >= is
 *   PREC_BITWISE  = 8   && || ~^ << >>
 *   PREC_ADD      = 10  + -
 *   PREC_MUL      = 11  * / %
 *   PREC_POW      = 12  ^ (right‑associative)
 *
 * Postfix operators (calls, indexing, '.', ':', '?.') bind tighter than any
 * binary operator and are handled in parsePostfixExpr.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "Parser.hpp"
#include "ast/BaseAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <cassert>
#include <string>

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

// ─────────────────────────────────────────────────────────────────────────────
// infixPrec
//
// Returns the precedence level of an infix operator token, or PREC_NONE (0) if
// the token is not an infix operator.
//
// Precedence levels (higher = binds tighter):
//   PREC_ASSIGN   = 1   – assignment and compound assignment (right‑associative)
//   PREC_COMPOSE  = 2   – composition '+>'
//   PREC_PIPELINE = 3   – pipeline '|>'
//   PREC_NULLCOAL = 4   – null coalesce '??' (right‑associative)
//   PREC_OR       = 5   – logical OR
//   PREC_AND      = 6   – logical AND
//   PREC_CMP      = 7   – comparison: ==, !=, <, >, <=, >=, ===, is
//   PREC_BITWISE  = 8   – bitwise: &&, ||, ~^, <<, >>
//   PREC_ADD      = 10  – addition: +, -
//   PREC_MUL      = 11  – multiplication: *, /, %
//   PREC_POW      = 12  – exponentiation: ^ (right‑associative)
//
// Note: RANGE ('..') returns PREC_NONE – it is handled by parsePostfixExpr
//       and specialised parsers (for loops, match patterns, slice indices).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Does NOT consume any tokens; pure inspection via peek().
// - The caller (parsePrattExpr) uses the returned precedence to decide whether
//   to consume the operator and recurse.
//
// ─── Error Handling ──────────────────────────────────────────────────────────
// - Returns PREC_NONE for any token that is not a recognised infix operator.
// - No error reporting; the caller handles unexpected tokens.
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

// ─────────────────────────────────────────────────────────────────────────────
// tokenToBinaryOp
//
// Converts a TokenType to the corresponding BinaryOp enum value.
//
// Mapping:
//   TokenType::PLUS          → BinaryOp::Add
//   TokenType::MINUS         → BinaryOp::Sub
//   TokenType::MUL           → BinaryOp::Mul
//   TokenType::DIV           → BinaryOp::Div
//   TokenType::POW           → BinaryOp::Pow
//   TokenType::MOD           → BinaryOp::Mod
//   TokenType::EQUAL_EQUAL   → BinaryOp::Eq        (value equality)
//   TokenType::EQUAL_EQUAL_EQUAL → BinaryOp::RefEq (reference equality)
//   TokenType::NOT_EQUAL     → BinaryOp::Ne
//   TokenType::LESS          → BinaryOp::Lt
//   TokenType::GREATER       → BinaryOp::Gt
//   TokenType::LESS_EQUAL    → BinaryOp::Le
//   TokenType::GREATER_EQUAL → BinaryOp::Ge
//   TokenType::AND           → BinaryOp::And       (logical AND)
//   TokenType::OR            → BinaryOp::Or        (logical OR)
//   TokenType::BIT_AND       → BinaryOp::BitAnd    (bitwise AND, token '&&')
//   TokenType::BIT_OR        → BinaryOp::BitOr     (bitwise OR,  token '||')
//   TokenType::BIT_XOR       → BinaryOp::BitXor    (bitwise XOR, token '~^')
//   TokenType::SHL           → BinaryOp::Shl       (left shift)
//   TokenType::SHR           → BinaryOp::Shr       (right shift)
//
// ─── Preconditions ───────────────────────────────────────────────────────────
// - The caller must ensure the TokenType is a valid binary operator.
// - BIT_NOT ('~') and AMPERSAND ('&') are unary operators – never passed here.
//
// ─── Error Handling ──────────────────────────────────────────────────────────
// - The default case returns BinaryOp::Add (should never be reached in correct
//   parsing). This satisfies the compiler but is a logic error if triggered.
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// tokenToAssignOp
//
// Converts a TokenType to the corresponding AssignOp enum value.
//
// Mapping:
//   TokenType::ASSIGN         → AssignOp::Assign      (=)
//   TokenType::PLUS_ASSIGN    → AssignOp::AddAssign   (+=)
//   TokenType::MINUS_ASSIGN   → AssignOp::SubAssign   (-=)
//   TokenType::MUL_ASSIGN     → AssignOp::MulAssign   (*=)
//   TokenType::DIV_ASSIGN     → AssignOp::DivAssign   (/=)
//   TokenType::POW_ASSIGN     → AssignOp::PowAssign   (^=)
//   TokenType::MOD_ASSIGN     → AssignOp::ModAssign   (%=)
//   TokenType::BIT_AND_ASSIGN → AssignOp::BitAndAssign (&&=)
//   TokenType::BIT_OR_ASSIGN  → AssignOp::BitOrAssign  (||=)
//   TokenType::BIT_XOR_ASSIGN → AssignOp::BitXorAssign (~^=)
//   TokenType::SHL_ASSIGN     → AssignOp::ShlAssign    (<<=)
//   TokenType::SHR_ASSIGN     → AssignOp::ShrAssign    (>>=)
//
// ─── Preconditions ───────────────────────────────────────────────────────────
// - The caller must ensure the TokenType is a valid assignment operator.
// - Typically called only after isAssignOp() returns true.
//
// ─── Error Handling ──────────────────────────────────────────────────────────
// - The default case returns AssignOp::Assign (should never be reached in
//   correct parsing). This satisfies the compiler but is a logic error if triggered.
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// isAssignOp
//
// Returns true if the given TokenType is an assignment operator.
//
// Assignment operators include:
//   =, +=, -=, *=, /=, ^=, %=, &&=, ||=, ~^=, <<=, >>=
//
// ─── Usage ───────────────────────────────────────────────────────────────────
// - Used in parsePrattExpr to detect assignment operators before the generic
//   binary operator path.
// - Assignment operators have the lowest precedence (PREC_ASSIGN = 1) and are
//   right‑associative.
// - When an assignment operator is encountered, the Pratt loop breaks after
//   parsing it (assignments are statement‑level expressions).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Does NOT consume any tokens; pure inspection.
// - The caller (parsePrattExpr) consumes the token after checking this predicate.
// ─────────────────────────────────────────────────────────────────────────────
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
// Pratt Parser Core
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseExpr
//
// Root entry point for expression parsing.
//
// Grammar:
//   expr := assign_expr
//
// ─── Overview ────────────────────────────────────────────────────────────────
// - Starts the Pratt parser at the lowest precedence level (PREC_NONE = 0),
//   which ensures all operators (including assignment) are consumed.
// - Delegates to parsePrattExpr() which handles the full precedence climbing.
// - After parsing, the expression may be followed by a semicolon or another
//   token – the caller is responsible for consuming separators.
//
// ─── Parameters ──────────────────────────────────────────────────────────────
//   allowStructLiteral – When false, prevents an IDENTIFIER followed by '{'
//                        from being parsed as a StructLiteralExprAST. This is
//                        used in control‑flow headers (if, for, while) to avoid
//                        greedily consuming the following block.
//
// ─── Return Value ───────────────────────────────────────────────────────────
//   Returns an ExprPtr (never nullptr; on error returns UnknownExprAST).
//   The caller should check the diagnostic engine for errors.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes tokens until the expression is fully parsed (stops when the next
//   token cannot be part of the expression, e.g., ';', ')', '}', or a statement
//   keyword).
// - Does NOT consume trailing semicolons or separators – that is the caller's
//   responsibility.
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseExpr(bool allowStructLiteral) {
    LUC_LOG_EXPR("=== parseExpr START (allowStructLiteral=" << allowStructLiteral << ") ===");
    ExprPtr result = parsePrattExpr(PREC_NONE, allowStructLiteral);
    LUC_LOG_EXPR("=== parseExpr END ===");
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePrattExpr
//
// Core Pratt parser (top‑down operator precedence) for expressions.
//
// Algorithm:
//   1. Parse a prefix/primary expression as the initial lhs.
//   2. Apply all postfix operations (calls, indexing, field/behavior access,
//      nullable chains) to get the fully‑decorated lhs.
//   3. While the current token is an infix operator with precedence > minPrec:
//        a. If it is an assignment op → build AssignExprAST (right‑assoc), break.
//        b. If it is 'is'            → build IsExprAST, continue.
//        c. If it is '|>'            → build PipelineExprAST, continue.
//        d. If it is '+>'            → build ComposeExprAST, continue.
//        e. If it is '??'            → build NullCoalesceExprAST, break.
//        f. Otherwise                → build BinaryExprAST, continue.
//   4. Return lhs.
//
// ─── Parameters ──────────────────────────────────────────────────────────────
//   minPrec            – Minimum precedence level to consume. The loop stops
//                        when the current operator's precedence <= minPrec.
//   allowStructLiteral – Passed down to parsePrefixExpr to control struct
//                        literal detection in ambiguous contexts.
//
// ─── Right‑Associative Operators ────────────────────────────────────────────
//   For right‑associative operators (assignment, '??', '^'), the recursion
//   uses `minPrec` (or `minPrec - 1`) to allow the same operator to bind more
//   tightly on the right side.
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - Each iteration consumes at least one token (the infix operator).
// - The loop terminates when the current operator's precedence <= minPrec,
//   which is guaranteed at EOF (prec = PREC_NONE).
// - No unbounded recursion; each recursive call reduces the precedence level
//   or consumes an operator.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the prefix/primary expression (via parsePrefixExpr).
// - Then consumes postfix operators (via parsePostfixExpr).
// - Then consumes infix operators (via the dispatch branches) and recurses.
// - Returns the fully parsed expression, with pos_ positioned at the first
//   token that is not part of the expression.
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

        // ── Infix operator dispatch (REFACTOR-2) ───────────────────────────────
        if (isAssignOp(opTok)) {
            lhs = parseInfixAssign(std::move(lhs), allowStructLiteral);
            // Assignment is a statement-level expression — stop the loop.
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
            break; // '??' terminates the chain
        }

        // Generic binary operator path
        lhs = parseInfixBinary(std::move(lhs), opTok, prec, allowStructLiteral);
        
        // After any binary op, apply postfix again (e.g. a + b.x)
        lhs = parsePostfixExpr(std::move(lhs));
    }

    return lhs;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseInfixAssign
//
// Parses an assignment expression (plain or compound) in the Pratt infix loop.
//
// Grammar:
//   assign_expr := lhs assign_op rhs
//   assign_op   := '=' | '+=' | '-=' | '*=' | '/=' | '^=' | '%='
//                | '&&=' | '||=' | '~^=' | '<<=' | '>>='
//
// Examples:
//   x = 5               → AssignOp::Assign
//   x += 1              → AssignOp::AddAssign
//   arr[i] *= 2         → AssignOp::MulAssign
//
// ─── Operator Precedence & Associativity ────────────────────────────────────
// - Assignment operators have the lowest precedence (PREC_ASSIGN = 1).
// - They are right‑associative: a = b = c  →  a = (b = c)
// - This function recurses with `minPrec = PREC_ASSIGN - 1` to achieve
//   right‑associative parsing.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the assignment operator token.
// - Recursively parses the right‑hand side expression (with precedence lower
//   than PREC_ASSIGN).
// - After returning, the caller (parsePrattExpr) breaks the infix loop because
//   assignment is a statement‑level expression that cannot be followed by
//   another operator at the same precedence.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If the right‑hand side expression fails to parse, reports an error and
//   returns the original lhs (the caller may continue parsing).
// - The resulting AssignExprAST node is still constructed (with unknown RHS)
//   to avoid returning nullptr and stalling the parser.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   AssignExprAST {
//       op:  AssignOp (from tokenToAssignOp)
//       lhs: the left‑hand side expression (must be an assignable lvalue)
//       rhs: the right‑hand side expression
//   }
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseInfixAssign(ExprPtr lhs, bool allowStructLiteral) {
    LUC_LOG_EXPR_VERBOSE("parseInfixAssign");
    TokenType opTok = advance().type;
    AssignOp op = tokenToAssignOp(opTok);
    
    // Right-associative: recurse at the same precedence level.
    ExprPtr rhs = parsePrattExpr(PREC_ASSIGN - 1, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after assignment operator");
        return lhs;
    }

    SourceLocation loc = lhs->loc;
    auto node = arena_.make<AssignExprAST>();
    node->loc = loc;
    node->op = op;
    node->lhs = std::move(lhs);
    node->rhs = std::move(rhs);
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseInfixIs
//
// Parses an 'is' type check expression in the Pratt infix loop.
//
// Grammar:
//   is_expr := lhs 'is' type
//
// Example:
//   x is int          → returns IsExprAST with x as expr, int as checkType
//   shape is Circle   → returns IsExprAST with shape as expr, Circle as checkType
//
// ─── Runtime Behaviour ───────────────────────────────────────────────────────
// - Produces a boolean value: true if the runtime type of lhs matches the
//   specified type (including nullability distinctions).
// - Inside the then‑branch of an if statement, the type of lhs is narrowed
//   to the checked type (semantic pass).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'is' keyword.
// - Parses the type annotation via parseType() (consumes the type tokens).
// - Does NOT consume any tokens beyond the type.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If parseType() fails or returns UnknownTypeAST, reports an error and returns
//   the original lhs (the IsExprAST node is still constructed with a null
//   checkType to avoid returning nullptr and stalling the parser).
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   IsExprAST {
//       expr:      the left‑hand side expression
//       checkType: the type being tested against
//   }
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseInfixIs(ExprPtr lhs) {
    LUC_LOG_EXPR_VERBOSE("parseInfixIs");
    advance(); // consume 'is'
    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E2005, "expected type after 'is'");
        return lhs;
    }

    SourceLocation loc = lhs->loc;
    auto node = arena_.make<IsExprAST>();
    node->loc = loc;
    node->expr = std::move(lhs);
    node->checkType = std::move(checkType);
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseInfixNullCoalesce
//
// Parses a null coalescing expression '??' in the Pratt infix loop.
//
// Grammar:
//   null_coalesce_expr := lhs '??' rhs
//
// Example:
//   getValue() ?? defaultValue
//
// ─── Semantics ───────────────────────────────────────────────────────────────
// - If lhs evaluates to a non‑nil value (for nullable types) or non‑error
//   (for Error types), the result is lhs.
// - If lhs is nil or an Error, the result is rhs.
// - The rhs is evaluated only when lhs is nil/Error (short‑circuit evaluation).
//
// ─── Operator Precedence & Associativity ────────────────────────────────────
// - Precedence: PREC_NULLCOAL = 4
// - Right‑associative: a ?? b ?? c  →  a ?? (b ?? c)
// - This function recurses with `minPrec = PREC_NULLCOAL - 1` to achieve
//   right‑associative parsing.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '??' token.
// - Recursively parses the right‑hand side expression (with precedence lower
//   than PREC_NULLCOAL).
// - After parsing, the caller (parsePrattExpr) typically breaks the infix loop
//   because '??' terminates the chain (cannot be followed by higher‑prec ops).
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If the right‑hand side expression fails to parse, reports an error and
//   returns the original lhs (the node is still constructed with null fallback).
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   NullCoalesceExprAST {
//       value:    the left‑hand side expression (nullable)
//       fallback: the right‑hand side expression (evaluated if lhs is nil/Error)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseInfixNullCoalesce(ExprPtr lhs, bool allowStructLiteral) {
    LUC_LOG_EXPR_VERBOSE("parseInfixNullCoalesce");
    advance(); // consume '??'
    
    // Right-associative.
    ExprPtr fallback = parsePrattExpr(PREC_NULLCOAL - 1, allowStructLiteral);
    if (!fallback) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?'");
        return lhs;
    }

    SourceLocation loc = lhs->loc;
    auto node = arena_.make<NullCoalesceExprAST>();
    node->loc = loc;
    node->value = std::move(lhs);
    node->fallback = std::move(fallback);
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseInfixBinary
//
// Parses a generic binary operator expression (arithmetic, comparison,
// logical, bitwise) in the Pratt infix loop.
//
// Grammar:
//   binary_expr := lhs operator rhs
//
// Operators covered (see tokenToBinaryOp for full list):
//   Arithmetic:  +, -, *, /, %, ^
//   Comparison:  ==, !=, ===, <, >, <=, >=
//   Logical:     and, or
//   Bitwise:     &&, ||, ~^, <<, >>
//
// ─── Operator Precedence & Associativity ────────────────────────────────────
// - Left‑associative operators: most binary operators (+, -, *, /, %, and, or,
//   comparison, bitwise). Recurses with `nextPrec = prec` (same precedence)
//   which correctly handles left associativity because the loop condition
//   checks `prec > minPrec` – the newly parsed RHS will not consume operators
//   at the same precedence.
// - Right‑associative operator: '^' (exponentiation). Uses `nextPrec = prec - 1`
//   so that the right side binds more tightly.
//
// ─── Chained Comparison Detection ───────────────────────────────────────────
// - Detects patterns like `a < b < c` and reports an error (chained comparisons
//   are not allowed in Luc). The user must write `a < b and b < c`.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the operator token.
// - Recursively parses the right‑hand side expression with the appropriate
//   next precedence level.
// - After returning, the caller (parsePrattExpr) applies postfix operators
//   again (e.g., `a + b.c`).
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If the right‑hand side expression fails to parse, reports an error and
//   returns the original lhs (the BinaryExprAST node is still constructed
//   with null RHS to avoid returning nullptr).
// - Chained comparison detection reports an error but continues parsing to
//   avoid crashing.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   BinaryExprAST {
//       op:    BinaryOp (from tokenToBinaryOp)
//       left:  the left‑hand side expression
//       right: the right‑hand side expression
//   }
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseInfixBinary(ExprPtr lhs, TokenType opTok, int prec, bool allowStructLiteral) {
    LUC_LOG_EXPR_VERBOSE("parseInfixBinary: " << LucDebug::tokenTypeToString(opTok));
    advance(); // consume the operator

    // Right-associative: POW (^)
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

    if (isComparisonOp(opTok) && isComparisonOp(peek().type)) {
        errorAt(DiagCode::E3014,
                "chained comparisons are not allowed; "
                "use 'and' explicitly, e.g. '0 < x and x < 10'");
    }

    SourceLocation loc = lhs->loc;
    auto node = arena_.make<BinaryExprAST>();
    node->loc = loc;
    node->op = tokenToBinaryOp(opTok);
    node->left = std::move(lhs);
    node->right = std::move(rhs);
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// Prefix & Primary Parsers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parsePrefixExpr
//
// Parses a unary prefix expression or dispatches to parsePrimaryExpr for atoms.
//
// Grammar:
//   unary_expr := ( '-' | 'not' | '~~' | '&' ) unary_expr
//               | primary_expr
//
// Operators:
//   -    → UnaryOp::Neg      (arithmetic negation)
//   not  → UnaryOp::Not      (logical negation, works on bool and nullable)
//   ~~   → UnaryOp::BitNot   (bitwise NOT, integer types only)
//   &    → UnaryOp::Ref      (take a reference)
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - If a unary operator is found: consumes the operator token, then recursively
//   calls parsePrefixExpr() to parse the operand.
// - If no unary operator: calls parsePrimaryExpr() to parse an atom.
// - The operand is parsed with the same allowStructLiteral flag (passed down).
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If a unary operator is consumed but the operand fails to parse, reports an
//   error and returns an UnknownExprAST (the node is still constructed with
//   null operand to avoid returning nullptr).
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   UnaryExprAST {
//       op:      UnaryOp (Neg, Not, BitNot, or Ref)
//       operand: the inner expression
//   }
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
// parsePrimaryExpr
//
// Parses an atomic (primary) expression – the leaves of the expression tree.
//
// Grammar (primary_expr):
//   literal | IDENTIFIER | struct_literal | '(' expr ')' | anon_func
//   | match_expr | if_expr | array_literal | await_expr
//   | 'nil' | 'true' | 'false' | '#' intrinsic_call
//   | '*' type '(' expr ')'          (unsafe bit reinterpret cast)
//   | type_name '(' expr ')'         (safe type conversion cast)
//
// ─── Dispatch Order (priority from highest to lowest) ───────────────────────
//   1. match_expr         – 'match' keyword
//   2. if_expr            – 'if' keyword (expression form, requires '??' and 'else')
//   3. #intrinsic_call    – '#' prefix (compiler builtins)
//   4. await_expr         – 'await' keyword
//   5. array_literal      – '[' ... ']'
//   6. block recovery     – bare '{' (error, suggests struct literal or match)
//   7. anonymous function – '(' followed by parameter pattern (lookahead)
//   8. grouped expr       – '(' expr ')' (fallback when not an anonymous function)
//   9. unsafe cast        – '*' type '(' expr ')'
//   10. identifier        – IDENTIFIER (struct literal, behavior access, or plain name)
//   11. type cast         – primitive_type '(' expr ')' (e.g., int(x))
//   12. literal           – scalar literals, true, false, nil
//
// ─── Struct Literal Detection ───────────────────────────────────────────────
// - When allowStructLiteral is true and looksLikeStructLiteral() returns true,
//   an IDENTIFIER followed by '{' is parsed as a struct literal.
// - When false (e.g., in if/for/while headers), struct literals are disabled
//   to avoid ambiguity with the following block.
//
// ─── Behavior Access Detection ──────────────────────────────────────────────
// - Pattern: IDENTIFIER [ '<' type_args '>' ] ':' IDENTIFIER
// - Example: Vec2:normalize, Buffer<int>:create
// - Uses a non‑destructive lookahead scan to verify the pattern before committing.
//
// ─── Anonymous Function vs Grouped Expression ───────────────────────────────
// - Lookahead determines whether '(' starts an anonymous function or a grouped expr.
// - Anonymous function requires: '(' param_list ')' [ '->' type ] '{' ... '}'
// - Grouped expression: '(' expr ')'
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Bare '{' in expression position reports a helpful error and consumes the
//   entire block to avoid cascading errors.
// - Unknown tokens report "expected expression" and return UnknownExprAST.
// - Most sub‑parsers have their own error recovery; this function ensures
//   that at least one token is consumed on error paths.
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - No long loops; each branch either consumes tokens and returns, or reports
//   an error and returns.
// - The behavior access lookahead uses a local index and does not modify pos_
//   until the pattern is confirmed.
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

        // ── Behavior access: IDENTIFIER [ '<' generic_args '>' ] ':' IDENTIFIER ──
        if (looksLikeBehaviorAccess()) {
            std::string typeName = advance().value;
            std::vector<TypePtr> genericArgs;
            if (check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            consume(TokenType::COLON, "expected ':' in behavior access");
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected method name after ':'");
                return arena_.make<UnknownExprAST>();
            }
            std::string method = advance().value;

            auto node = arena_.make<BehaviorAccessExprAST>();
            node->loc = loc;
            node->typeName = pool_.intern(typeName);
            node->genericArgs = std::move(genericArgs);
            node->method = pool_.intern(method);
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
// Postfix Parser
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parsePostfixExpr
//
// Applies all postfix operators to an already‑parsed left‑hand side expression.
//
// Postfix operators (highest precedence, left‑associative):
//   '(' args ')'                    → function call
//   '<' types '>' '(' args ')'      → generic function call
//   '[' expr ']'                    → element index
//   '[' expr '..' expr ']'          → slice index (inclusive/exclusive)
//   '.' IDENTIFIER                  → field access (data member)
//   '?.' IDENTIFIER                 → nullable chain step
//
// IMPORTANT: Does NOT handle:
//   - '|>' (pipeline) – handled at a higher precedence level in parsePrattExpr
//   - '+>' (composition) – handled at a higher precedence level in parsePrattExpr
//   - ':' (behavior access) – handled in parsePrimaryExpr when lhs is IDENTIFIER
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes postfix operators one at a time, modifying the lhs expression.
// - Returns after processing all consecutive postfix operators (stops when the
//   next token is not a postfix operator).
//
// ─── Generic Call Detection ─────────────────────────────────────────────────
// - When the lhs is an IdentifierExprAST or BehaviorAccessExprAST and the next
//   token is '<', attempts to parse generic arguments.
// - Uses lookahead to verify that a '(' follows the closing '>' before committing.
// - If not a generic call (e.g., a '<' comparison operator), leaves '<' for the
//   binary operator loop and returns.
//
// ─── Nullable Chain Processing ──────────────────────────────────────────────
// - When '?.' is encountered:
//     * If the current lhs is not already a NullableChainExprAST, creates a new
//       chain node with the current lhs as the object.
//     * Otherwise, appends the field name to the existing chain.
//   The grammar requires that every '?.' chain is terminated by '??' (null
//   coalesce), which is handled in parsePrattExpr.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - Missing field name after '.' or '?.': reports error and stops processing
//   further postfix operators.
// - Missing closing ']' in index/slice: reports error, returns current lhs.
// - Missing closing ')' in call: consume() reports error and recovers.
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The loop consumes at least one token per iteration (the postfix operator).
// - Terminates when no postfix operator is found or when a syntax error occurs.
// - The generic call lookahead uses a local index and does not modify pos_
//   until the pattern is confirmed.
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
        if (check(TokenType::PIPELINE) || check(TokenType::COMPOSE)) {
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
// Literal & Value Parsers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseLiteralExpr
//
// Parses a scalar literal expression.
//
// Grammar:
//   literal := INT_LITERAL | FLOAT_LITERAL | STRING_LITERAL | RAW_STRING_LITERAL
//            | CHAR_LITERAL | HEX_LITERAL | BINARY_LITERAL
//            | 'true' | 'false' | 'nil'
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the literal token.
// - Does NOT consume any tokens beyond the literal.
//
// ─── LiteralKind Mapping ────────────────────────────────────────────────────
//   TokenType::INT_LITERAL        → LiteralKind::Int
//   TokenType::FLOAT_LITERAL      → LiteralKind::Float
//   TokenType::STRING_LITERAL     → LiteralKind::String
//   TokenType::RAW_STRING_LITERAL → LiteralKind::RawString
//   TokenType::CHAR_LITERAL       → LiteralKind::Char
//   TokenType::HEX_LITERAL        → LiteralKind::Hex
//   TokenType::BINARY_LITERAL     → LiteralKind::Binary
//   TokenType::TRUE               → LiteralKind::True
//   TokenType::FALSE              → LiteralKind::False
//   TokenType::NIL                → LiteralKind::Nil
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If called on a non‑literal token, reports an internal error and returns
//   an UnknownExprAST (should never happen in correct parsing).
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   LiteralExprAST {
//       kind:  LiteralKind (Int, Float, String, etc.)
//       value: InternedString of the raw token text
//   }
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
// Parses an array literal expression.
//
// Grammar:
//   array_literal := '[' [ expr { ',' expr } ] ']'
//
// Examples:
//   [1, 2, 3]
//   ["hello", "world"]
//   []   — empty array literal
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '['.
// - Repeatedly parses expressions (each element) until the closing ']'.
// - Consumes optional commas between elements.
// - Consumes the closing ']'.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - Uses a progress guard: saves pos_ before each parseExpr() call.
// - If parseExpr() makes no progress, reports an error, consumes one token,
//   and breaks out of the loop (prevents infinite loop on malformed input).
// - Optional commas are consumed without stalling.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing closing ']': consume() reports error and recovers.
// - Empty array literal `[]` is valid (produces an ArrayLiteralExprAST with
//   an empty elements vector).
// - Invalid element expressions are skipped; the loop continues to parse
//   remaining elements if possible.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   ArrayLiteralExprAST {
//       elements: vector of ExprPtr (may be empty)
//   }
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
// Parses a struct literal expression.
//
// Grammar:
//   struct_literal := IDENTIFIER [ generic_args ] '{' { field_init } '}'
//   field_init     := IDENTIFIER '=' expr
//
// Examples:
//   Vec2 { x = 0.0, y = 0.0 }
//   Color {}   (all fields take defaults)
//   Pair<int, string> { first = 1, second = "one" }
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called after the type name (and optional generic args) have already been
//   consumed by parsePrimaryExpr().
// - The caller has verified looksLikeStructLiteral() or equivalent.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '{'.
// - Repeatedly parses field initialisers: IDENTIFIER '=' expr.
// - Consumes optional commas/semicolons between field inits.
// - Consumes the closing '}'.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - Uses a progress guard: saves pos_ before each parseExpr() for the field value.
// - If parseExpr() makes no progress, reports an error, consumes one token,
//   and continues to the next field (prevents infinite loop).
// - The outer loop consumes at least one token per field (the field name and '=').
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing field name: reports error, calls synchronize() to skip to next field.
// - Missing '=' after field name: reports error, recovers.
// - Missing expression after '=': reports error, continues to next field.
// - Missing closing '}': consume() reports error and recovers.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   StructLiteralExprAST {
//       typeName:      InternedString (e.g., "Vec2", "Color")
//       genericArgs:   vector of TypePtr (empty if non‑generic)
//       inits:         vector of FieldInitPtr (field = expression pairs)
//   }
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
// Parses an anonymous function expression (closure / lambda).
//
// Grammar:
//   anon_func := param_group { param_group } [ '->' return_list ] block
//
// Notes:
//   - Anonymous functions CANNOT have qualifiers (~async, ~nullable, ~parallel).
//     They are plain values. Qualifiers belong on declarations or parameter types.
//   - Multiple parameter groups = curried anonymous function.
//   - Return list after '->' can contain multiple types (comma separated).
//   - No nullable suffix '?' – anonymous functions are never nil.
//
// Examples:
//   (x int) -> int { return x * 2 }
//   (a int)(b int) -> int { return a + b }
//   (src string) -> (int, string) { ... }
//   () -> int { return 42 }      — zero parameters
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Parses one or more parameter groups via parseParamGroup().
// - Optionally consumes '->' and parses return list via parseReturnList().
// - Consumes the body block (always a BlockStmtAST).
// - Does NOT consume any tokens beyond the closing '}' of the block.
//
// ─── Rejecting Qualifiers ───────────────────────────────────────────────────
// - If a '~' is found at the start, reports an error and consumes the qualifier
//   token(s) to recover (the anonymous function is still parsed).
// - Anonymous functions are always plain values; the caller's binding provides
//   any necessary qualifiers.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '(' after optional qualifiers: reports error, returns UnknownExprAST.
// - Missing return type after '->' (if present): reports error, continues.
// - Missing body block: reports error, returns UnknownExprAST.
// - All sub‑parsers handle their own internal error recovery.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   AnonFuncExprAST {
//       sig:    FuncSignature (paramGroups, returnTypes, qualifiers=0)
//       body:   StmtPtr (always a BlockStmtAST)
//   }
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
// parseAwaitExpr
//
// Parses an 'await' expression that suspends the current async function until
// the awaited future resolves.
//
// Grammar:
//   await_expr := 'await' expr
//
// Example:
//   await httpGet(url)
//   await fetchAll(items)
//
// ─── Semantic Restrictions (Enforced by Semantic Pass) ──────────────────────
// - 'await' is only valid inside a function whose binding carries the '~async'
//   qualifier.
// - 'await' is not valid inside a '~parallel' body.
// - The expression after 'await' must resolve to a call to a '~async'‑qualified
//   function.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'await' keyword.
// - Parses the inner expression (any expression) using parsePrattExpr at the
//   lowest precedence level (PREC_NONE).
// - Does NOT consume any tokens beyond the inner expression.
//
// ─── Parse‑Time Checks ──────────────────────────────────────────────────────
// - If parallelDepth_ > 0 (inside a parallel block), reports an error.
// - The semantic pass performs the remaining restrictions.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - Missing inner expression after 'await': reports error, returns UnknownExprAST.
// - Nested await is allowed (await await f()) – the semantic pass will validate.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   AwaitExprAST {
//       inner: ExprPtr (the expression being awaited)
//   }
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
// Parses a match expression – pattern matching that produces a value.
//
// Grammar:
//   match_expr := 'match' expr '{' { match_arm } default_arm '}'
//   match_arm  := pattern_list [ 'if' guard_expr ] '=>' arm_body
//   default_arm := 'default' '=>' arm_body
//   arm_body   := expr [ ',' expr ]
//
// Examples:
//   match status {
//       200      => "ok"
//       404      => "not found"
//       default  => "unknown"
//   }
//
//   match point {
//       Vec2 { x: 0.0, y: 0.0 } => "origin"
//       Vec2 { x, y }            => "at " + string(x) + ", " + string(y)
//       default                  => "unknown"
//   }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'match' keyword.
// - Parses the subject expression (with struct literals disabled because '{'
//   belongs to the match arms).
// - Consumes the opening '{'.
// - Repeatedly parses match arms (via parseMatchArm()) until 'default' or '}'.
// - Parses the required default arm via parseDefaultArm().
// - Consumes the closing '}'.
//
// ─── Default Arm Requirement ─────────────────────────────────────────────────
// - The grammar requires a 'default' arm as the last arm. The semantic pass
//   reports an error if default is missing.
// - Duplicate 'default' arms are reported as an error.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing subject after 'match': reports error, returns UnknownExprAST.
// - Missing '{' after subject: reports error, returns UnknownExprAST.
// - If parseMatchArm() makes no progress, calls synchronize() to skip to the
//   next arm or closing brace.
// - Missing closing '}': consume() reports error and recovers.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - The arm loop uses a progress guard: saves pos_ before parseMatchArm().
// - If no progress is made, synchronize() is called (which consumes tokens
//   until a statement/declaration boundary), guaranteeing forward progress.
// - The loop terminates when '}' or 'default' is found, or EOF is reached.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   MatchExprAST {
//       subject:     ExprPtr
//       arms:        vector<MatchArmPtr> (non‑default arms)
//       defaultBody: DefaultArmPtr (required)
//       defaultLoc:  SourceLocation of the 'default' keyword
//   }
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
// Parses the expression form of 'if' – an inline conditional expression that
// produces a value.
//
// Grammar:
//   if_expr := 'if' expr '??' expr 'else' expr
//
// Example:
//   let grade string = if score >= 60 ?? "pass" else "fail"
//   let label string = if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"
//
// ─── Comparison with IfStmtAST ──────────────────────────────────────────────
//   IfExprAST (this function)          | IfStmtAST (in ParserStmt.cpp)
//   -----------------------------------|--------------------------------------
//   Expression context (after '=', etc) | Statement context (standalone)
//   'else' required                     | 'else' optional
//   Both branches produce a value       | No value produced (statements)
//   Uses '??' as separator              | No '??' separator
//
// ─── Operator Precedence ─────────────────────────────────────────────────────
// - The '??' here is a syntactic separator, not the null‑coalescing operator.
// - The condition is parsed with precedence PREC_NULLCOAL (4) to stop at the
//   first '??' that belongs to the if expression, not a nested null coalesce.
// - The expression is right‑associative: `if a ?? b else if c ?? d else e`
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'if' keyword.
// - Parses the condition (stops at '??').
// - Consumes the '??' separator.
// - Parses the then‑branch expression.
// - Consumes the 'else' keyword.
// - Parses the else‑branch expression.
// - Does NOT consume any tokens beyond the else‑branch.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing condition after 'if': reports error, returns UnknownExprAST.
// - Missing '??' after condition: reports error, returns UnknownExprAST.
// - Missing then‑branch after '??': reports error.
// - Missing 'else' keyword: reports error, returns UnknownExprAST.
// - Missing else‑branch after 'else': reports error.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   IfExprAST {
//       condition:  ExprPtr
//       thenBranch: ExprPtr
//       elseBranch: ExprPtr
//   }
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
// Parses an explicit type conversion (cast) expression.
//
// Grammar:
//   type_conv := type_name '(' expr ')'     -- safe conversion
//              | '*' type_name '(' expr ')' -- unsafe bit reinterpret
//
// Examples:
//   float(x)          -- int → float (safe)
//   string(n)         -- int → string formatting (safe)
//   *uint32(bits)     -- reinterpret bits as uint32 (unsafe)
//
// ─── Safe vs Unsafe ─────────────────────────────────────────────────────────
//   Safe (isUnsafe = false):  type_name '(' expr ')'
//     - Supported casts: primitive widening (int→float), enum→int, int→string
//     - Enforced by the semantic pass; invalid casts produce errors.
//
//   Unsafe (isUnsafe = true): '*' type_name '(' expr ')'
//     - Bit reinterpretation: reinterprets the bits of expr as the target type.
//     - Valid only inside @extern‑decorated functions or when --unsafe is enabled.
//     - Target and source sizes must match – enforced by the semantic pass.
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - For unsafe casts: The '*' and target type have already been consumed by
//   parsePrimaryExpr() before this function is called.
// - For safe casts: The target type has already been parsed (e.g., by
//   parsePrimitiveType() when a type keyword is followed by '(').
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '(' token.
// - Parses the inner expression (the value being cast).
// - Consumes the closing ')'.
// - Does NOT consume any tokens beyond the ')'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '(' after type: reports error, returns UnknownExprAST.
// - Missing inner expression: reports error, returns UnknownExprAST.
// - Missing closing ')': consume() reports error and recovers.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   TypeConvExprAST {
//       targetType: TypePtr (the type to convert to)
//       expr:       ExprPtr (the value being converted)
//       isUnsafe:   bool (true for '*T(expr)' reinterpret casts)
//   }
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
// Parses a range expression: lo '..' hi or lo '..<' hi
//
// Grammar:
//   range_expr := expr ( '..' | '..<' ) expr
//
// Examples:
//   0..10     — inclusive range (0 through 10)
//   1..<10    — exclusive range (1 through 9)
//   start..end — generic bounds (used in for loops, match patterns, slice indices)
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called when '..' or '..<' is found after the lo expression has been parsed.
// - The lo expression is passed as a parameter (already consumed).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '..' token (RANGE).
// - Optionally consumes a '<' token (makes the range exclusive).
// - Parses the hi expression using parsePrattExpr with minPrec = PREC_ADD,
//   which stops before low‑precedence operators like comparison or logical.
// - Does NOT consume any tokens beyond the hi expression.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing hi expression after '..' or '..<': reports error, returns UnknownExprAST.
// - The hi expression is required; no default value is provided.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   RangeExprAST {
//       lo:         ExprPtr (start bound, inclusive)
//       hi:         ExprPtr (end bound)
//       isExclusive: bool (true for '..<', false for '..')
//   }
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
// Call & Index Parsers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseCallExpr
//
// Parses a function call: callee '(' [ arg_list ] ')' [ '!' ]
//
// Grammar:
//   call_expr := callee '(' [ arg_list ] ')' [ '!' ]
//
// Examples:
//   f()                     — no arguments
//   add(10, 20)             — multiple arguments
//   process<int>(42)        — generic arguments
//   handle(args)!           — argument pack (for pipeline steps)
//
// ─── Generic Arguments ───────────────────────────────────────────────────────
// - genericArgs may be non‑empty when the call is prefixed with '<' types '>'
//   (parsed in parsePostfixExpr before calling parseCallExpr).
// - Example: process<int>(42) → genericArgs = [Int]
//
// ─── Argument Pack '!' ──────────────────────────────────────────────────────
// - The '!' suffix marks this call as an argument pack for a pipeline step.
// - The upstream value will be injected as the first argument when the pipeline
//   executes.
// - Only valid inside a pipeline step – the semantic pass enforces this.
// - Syntax: fn(args)!  (the '!' is parsed after the closing ')')
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes '('.
// - If the next token is not ')', calls parseArgList() to parse arguments.
// - Consumes the closing ')'.
// - Optionally consumes a '!' token (argument pack suffix).
// - Does NOT consume any tokens beyond the '!' (if present).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '(' after callee: reports error, returns UnknownExprAST.
// - Argument list parsing errors are handled by parseArgList().
// - Missing closing ')': consume() reports error and recovers.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   CallExprAST {
//       callee:      ExprPtr (the function being called)
//       genericArgs: vector<TypePtr> (explicit type arguments, may be empty)
//       args:        vector<ExprPtr> (call arguments in order)
//       isArgPack:   bool (true if '!' suffix was present)
//       isAsyncCall: bool (set by semantic pass, not parser)
//   }
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
// Parses an array/slice indexing or slicing expression.
//
// Grammar:
//   '[' expr ']'               — element index
//   '[' expr '..' expr ']'     — inclusive slice
//   '[' expr '..<' expr ']'    — exclusive slice
//
// Examples:
//   nums[2]        → element access (IndexKind::Element)
//   nums[1..3]     → slice (inclusive end, IndexKind::Slice)
//   nums[1..<3]    → slice (exclusive end, IndexKind::Slice, isExclusive=true)
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '['.
// - Parses the start expression.
// - If the next token is '..' (RANGE) or '..<' (RANGE followed by LESS):
//     * Consumes the range operator.
//     * Parses the end expression.
//     * Sets kind = IndexKind::Slice.
// - Otherwise:
//     * Sets kind = IndexKind::Element.
// - Consumes the closing ']'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing start expression after '[': reports error, returns UnknownExprAST.
// - Missing end expression after '..' or '..<': reports error.
// - Missing closing ']': consume() reports error and recovers.
//
// ─── Semantic Slice Type Handling ───────────────────────────────────────────
// - The AST node has a mutable `sliceType` field that the semantic pass populates
//   with a synthesized SliceTypeAST when kind == IndexKind::Slice.
// - This allows codegen to know the result type without re‑parsing.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   IndexExprAST {
//       target:     ExprPtr (the array/slice being indexed)
//       index:      ExprPtr (element index or slice start)
//       sliceEnd:   ExprPtr (nullptr for Element, end expression for Slice)
//       kind:       IndexKind (Element or Slice)
//       isExclusive: bool (true for '..<', only meaningful when kind == Slice)
//   }
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
// parseIntrinsicCallExpr
//
// Parses a compiler intrinsic call (prefixed with '#').
//
// Grammar:
//   intrinsic_call := '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'
//   intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }
//   intrinsic_arg  := type_name        -- for #sizeof(T), #alignof(T)
//                   | expr             -- for #sqrt(x), #memcpy(dst,src,n)
//
// Examples:
//   #sizeof(Vec2)          — type argument
//   #alignof(Vertex)       — type argument
//   #sqrt(x)               — value argument
//   #memcpy(dst, src, len) — multiple value arguments
//   #bitcast(float32, bits)— type + value arguments (special case)
//
// ─── Type‑Parameter Intrinsics ──────────────────────────────────────────────
// - Intrinsics that take a type argument: sizeof, alignof
// - These are parsed with typeArg (TypePtr) and no value args.
// - The parser detects these by name (intrinsicStr == "sizeof" || "alignof").
//
// ─── Value‑Parameter Intrinsics ─────────────────────────────────────────────
// - All other intrinsics (sqrt, abs, min, max, memcpy, memset, etc.)
// - Arguments are parsed as expressions via parseExpr().
// - The semantic pass validates argument counts and types.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '#' token.
// - Consumes an IDENTIFIER (intrinsic name).
// - Consumes '('.
// - For type intrinsics: parses a single type argument.
// - For value intrinsics: parses zero or more comma‑separated expressions.
// - Consumes the closing ')'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing intrinsic name after '#': reports error, returns UnknownExprAST.
// - Missing '(' after name: reports error, returns UnknownExprAST.
// - For type intrinsics with no argument: reports error.
// - For value intrinsics, if parseExpr() makes no progress, consumes one token,
//   then skips to the next comma or closing parenthesis.
// - Missing closing ')': consume() reports error and recovers.
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The value argument loop uses a progress guard; if parseExpr() makes no
//   progress, consumes the offending token and continues.
// - The loop terminates when ')' is reached or EOF is found.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   IntrinsicCallExprAST {
//       intrinsicName: InternedString (e.g., "sizeof", "sqrt")
//       typeArg:       TypePtr (non‑null for sizeof/alignof)
//       args:          vector<ExprPtr> (value arguments)
//   }
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
// parseArgList
//
// Parses a comma‑separated list of argument expressions for a function call.
//
// Grammar:
//   arg_list := expr { ',' expr }
//
// Does NOT consume the closing ')'. The caller is responsible for consuming it.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Repeatedly parses expressions as arguments while the next token is not ')'.
// - Consumes commas between arguments.
// - Stops when ')' is encountered or EOF is reached.
// - Does NOT consume the closing ')'.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - Uses a consecutive error counter (MAX_CONSECUTIVE_ERRORS = 5).
// - If parseExpr() makes no progress:
//     * Reports an error.
//     * Consumes one token (the offending token).
//     * If a comma follows, consumes it to keep the loop moving.
//     * Increments consecutiveErrors.
// - If consecutiveErrors reaches the limit:
//     * Reports "too many consecutive errors".
//     * Skips all tokens until the closing ')' or EOF.
//     * Breaks out of the loop.
// - On success, resets consecutiveErrors to 0.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing expression after comma: reports error, skips the comma, continues.
// - Consecutive commas (empty argument): reports error, skips the extra comma.
// - Missing comma between arguments: reports error, skips tokens until a comma
//   or ')' is found, then continues if a comma was found.
// - Returns a vector of parsed expressions (may contain UnknownExprAST on error).
//
// ─── Result ─────────────────────────────────────────────────────────────────
//   std::vector<ExprPtr> – the parsed arguments in order.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<ExprPtr> Parser::parseArgList() {
    LUC_LOG_EXPR_VERBOSE("parseArgList");
    std::vector<ExprPtr> args;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    while (!check(TokenType::RPAREN) && !isAtEnd()) {
        // Safety: if we've hit too many errors in a row, force exit
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            errorAt(DiagCode::E2002, "too many consecutive errors in argument list; skipping to ')'");
            // Skip everything until the closing parenthesis or end
            while (!isAtEnd() && !check(TokenType::RPAREN)) {
                advance();
            }
            break;
        }

        std::size_t savedPos = pos_;
        ExprPtr arg = parseExpr();

        if (pos_ == savedPos) {
            // parseExpr made zero progress → consume one token to avoid infinite loop
            errorAt(DiagCode::E2008, "expected argument expression");
            if (!isAtEnd()) {
                advance(); // consume the offending token
            }
            consecutiveErrors++;
            // Do NOT push an argument
            // If the next token is a comma, consume it to keep loop moving
            if (check(TokenType::COMMA)) {
                advance();
            }
            continue;
        }

        // Progress was made; reset error counter
        consecutiveErrors = 0;

        // Even if arg is an UnknownExprAST (error recovery node), we still push it
        // because the parser produced a node and advanced.
        args.push_back(std::move(arg));

        if (check(TokenType::RPAREN)) break;

        if (!match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "expected ',' after argument");
            // Skip tokens until we find a comma or closing parenthesis
            while (!isAtEnd() && !check(TokenType::COMMA) && !check(TokenType::RPAREN)) {
                advance();
            }
            if (check(TokenType::COMMA)) {
                advance(); // consume the comma and continue
                continue;
            }
            // If we reached ')' or EOF, break out of the loop
            break;
        }

        // Check for consecutive commas (empty argument)
        if (check(TokenType::COMMA)) {
            errorAt(DiagCode::E2008, "empty argument in call (consecutive commas)");
            advance(); // skip the extra comma
            consecutiveErrors++; // count this as an error
            // Do not push an argument
            continue;
        }
    }

    LUC_LOG_EXPR_VERBOSE("parseArgList: parsed " << args.size() << " arguments");
    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline & Composition
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parsePipelineExpr
//
// Parses a pipeline expression: seed |> step |> step |> ...
//
// Grammar:
//   pipeline_expr := seed { '|>' pipeline_step }
//   pipeline_step := IDENTIFIER | IDENTIFIER ':' IDENTIFIER | IDENTIFIER '.' IDENTIFIER
//                  | IDENTIFIER '(' arg_list ')' '!' | anon_func
//
// Examples:
//   42 |> float |> sqrt
//   getUser(id) |> validate |> save
//   v |> Vec2:normalize |> scale(2.0)!
//
// ─── Operator Precedence ────────────────────────────────────────────────────
// - Precedence: PREC_PIPELINE = 3 (higher than assignment, lower than comparison)
// - Left‑associative: a |> b |> c  →  (a |> b) |> c
// - Called from parsePrattExpr when '|>' is encountered.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - The seed expression (lhs) is already parsed by the caller.
// - While the current token is '|>', consumes it, then parses one pipeline step.
// - After parsing all steps, returns a PipelineExprAST node.
// - If no steps are parsed (e.g., '|>' with nothing after), reports an error
//   and returns the original seed (no pipeline node).
//
// ─── Pipeline Step Parsing ──────────────────────────────────────────────────
// - Delegates to parsePipelineStep() which handles the various step forms.
// - Each step may consume generic arguments, parentheses, and the '!' suffix.
// - Anonymous function steps are parsed via parseAnonFuncExpr().
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If the seed is null (should not happen), reports error and returns UnknownExprAST.
// - If parsePipelineStep() returns nullptr, breaks out of the loop.
// - If no steps are parsed after at least one '|>', reports an error.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   PipelineExprAST {
//       seed:  ExprPtr (the initial value)
//       steps: vector<PipelineStepPtr> (at least one step)
//   }
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
        errorAt(DiagCode::E2006, "pipeline '|>' requires at least one step");
        // node->seed is still valid (was moved from seed, not null)
        return std::move(node->seed);
    }

    LUC_LOG_EXPR("parsePipelineExpr: " << node->steps.size() << " pipeline steps");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePipelineStep
//
// Parses a single step in a pipeline expression (after '|>').
//
// Grammar:
//   pipeline_step := anon_func
//                  | IDENTIFIER [ generic_args ] [ step_suffix ]
//
//   step_suffix := ':' IDENTIFIER [ '(' arg_list ')' '!' ]   -- method reference
//                | '.' IDENTIFIER [ '(' arg_list ')' '!' ]   -- field reference
//                | '[' expr { '[' expr ']' } ']' [ '(' arg_list ')' '!' ] -- index
//                | '(' arg_list ')' '!'                      -- argument pack
//                | (nothing)                                 -- plain identifier
//
// ─── Step Kinds (PipelineStepKind) ──────────────────────────────────────────
//   Ident            – Plain function name: fn
//   BehaviorRef      – Method reference: Type:method
//   FieldRef         – Field reference: obj.field
//   IndexRef         – Array index: arr[idx]
//   ArgPack          – Argument pack: fn(args)!
//   BehaviorArgPack  – Method with argument pack: Type:method(args)!
//   FieldArgPack     – Field with argument pack: obj.field(args)!
//   IndexArgPack     – Index with argument pack: arr[idx](args)!
//   AnonFunc         – Anonymous function as step: (x int) -> int { ... }
//
// ─── Dispatch Order ─────────────────────────────────────────────────────────
//   1. looksLikeAnonFunc() → parseAnonFuncPipelineStep()
//   2. IDENTIFIER or primitive type:
//        a. Parse name and optional generic args
//        b. If next is ':' → parseBehaviorPipelineStep()
//        c. If next is '.' → parseFieldPipelineStep()
//        d. If next is '[' → parseIndexPipelineStep()
//        e. If next is '(' → parseArgPackPipelineStep()
//        f. Otherwise → plain Ident step
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the step name (IDENTIFIER or primitive type keyword).
// - Optionally consumes generic arguments (if '<' is present).
// - Consumes the appropriate suffix tokens (':', '.', '[', '(') based on the
//   step kind.
// - For ArgPack steps, consumes '(' arg_list ')' followed by '!'.
// - For anonymous function steps, parseAnonFuncExpr() consumes the entire
//   function (including parameter groups, return type, and body block).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If no valid step form is matched (not anon func and not identifier),
//   reports an error, creates an error Ident step with name "<error>", consumes
//   one token, and returns the step (prevents infinite loop).
// - Sub‑parsers (parseBehaviorPipelineStep, etc.) report their own errors.
// - Generic arguments on step forms that don't support them (method ref, field ref)
//   are rejected with an error.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - No loops in the main dispatcher; each branch consumes tokens deterministically.
// - The generic argument parser (parseGenericArgs) has its own progress guards.
// - Anonymous function detection (looksLikeAnonFunc) is a pure lookahead.
//
// ─── Result ─────────────────────────────────────────────────────────────────
//   PipelineStepPtr – always non‑null (on error, returns an error step with
//                     kind = Ident and name = "<error>")
// ─────────────────────────────────────────────────────────────────────────────
PipelineStepPtr Parser::parsePipelineStep() {
    
    // 1. ANONYMOUS FUNCTION DETECTION
    if (looksLikeAnonFunc()) {
        return parseAnonFuncPipelineStep();
    }

    // 2. OTHER STEP FORMS (identifier-based)
    bool isPrimitiveType = Parser::isPrimitiveTypeToken(peek().type);
    if (!check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        SourceLocation loc = currentLoc();
        errorAt(DiagCode::E2002,
                "expected function name, method reference, array access, or anonymous function as pipeline step, got '" +
                peek().value + "'");
        auto step = arena_.make<PipelineStepAST>();
        step->loc = loc;
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

    // Process possible generic arguments
    std::vector<TypePtr> genericArgs;
    if (check(TokenType::LESS)) {
        genericArgs = parseGenericArgs();
    }

    if (check(TokenType::COLON)) {
        return parseBehaviorPipelineStep(name, std::move(genericArgs));
    }
    
    if (check(TokenType::DOT)) {
        return parseFieldPipelineStep(name, std::move(genericArgs));
    }

    if (check(TokenType::LBRACKET)) {
        return parseIndexPipelineStep(name, std::move(genericArgs));
    }

    if (check(TokenType::LPAREN)) {
        return parseArgPackPipelineStep(name, std::move(genericArgs));
    }

    // Plain identifier (function reference)
    SourceLocation loc = currentLoc(); // approximate
    auto step = arena_.make<PipelineStepAST>();
    step->loc = loc;
    step->kind = PipelineStepKind::Ident;
    step->ident = pool_.intern(name);
    step->genericArgs = std::move(genericArgs);
    return step;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseAnonFuncPipelineStep
//
// Parses an anonymous function as a pipeline step.
//
// Grammar:
//   anon_func_step := anon_func
//
// Example:
//   42 |> (x int) -> int { return x * 2 }
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called when looksLikeAnonFunc() returns true (the current token stream
//   matches the anonymous function pattern).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Delegates to parseAnonFuncExpr() which consumes the entire anonymous
//   function (parameter groups, optional return type, and body block).
// - Does NOT consume any tokens beyond the anonymous function's closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If parseAnonFuncExpr() returns nullptr or an UnknownExprAST, reports an error.
// - On error, skips tokens until the next pipeline operator, brace, or semicolon
//   to recover, and returns an error step (kind = Ident, name = "<error>").
//
// ─── Resulting PipelineStep ─────────────────────────────────────────────────
//   PipelineStepAST {
//       kind:     PipelineStepKind::AnonFunc
//       anonFunc: ExprPtr (the anonymous function expression)
//   }
// ─────────────────────────────────────────────────────────────────────────────s
PipelineStepPtr Parser::parseAnonFuncPipelineStep() {
    LUC_LOG_EXPR_VERBOSE("parseAnonFuncPipelineStep");
    SourceLocation loc = currentLoc();
    auto step = arena_.make<PipelineStepAST>();
    step->loc = loc;
    
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

// ─────────────────────────────────────────────────────────────────────────────
// parseBehaviorPipelineStep
//
// Parses a method reference (behavior access) as a pipeline step.
//
// Grammar:
//   behavior_step := IDENTIFIER [ generic_args ] ':' IDENTIFIER [ '(' arg_list ')' '!' ]
//
// Examples:
//   Vec2:normalize           → BehaviorRef
//   Vec2:scale(2.0)!         → BehaviorArgPack
//
// ─── Two Forms ──────────────────────────────────────────────────────────────
//   1. BehaviorRef (plain method reference):
//        Type:method
//      - The method function is passed to the pipeline.
//      - Upstream value becomes the first argument (receiver).
//
//   2. BehaviorArgPack (method with argument pack):
//        Type:method(args)!
//      - The '!' suffix marks that the argument list is intentionally incomplete.
//      - Upstream value is injected as the first argument, followed by args.
//      - Example: v |> Vec2:scale(2.0)!  →  Vec2:scale(v, 2.0)
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called after the type name (and optional generic args) have been consumed.
// - The current token is ':' (already checked by parsePipelineStep).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the ':' token.
// - Consumes the method name (IDENTIFIER).
// - If '(' follows:
//     * Consumes '('
//     * Parses argument list via parseArgList()
//     * Consumes ')'
//     * Consumes '!' (required for argument pack form)
//     * Sets kind = PipelineStepKind::BehaviorArgPack
// - Otherwise:
//     * Sets kind = PipelineStepKind::BehaviorRef
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing method name after ':': reports error, returns error step.
// - If '(' is present but '!' is missing after ')': reports error, returns error step.
// - Generic arguments are not allowed on method references; if present, reports
//   an error and ignores them.
//
// ─── Resulting PipelineStep ─────────────────────────────────────────────────
//   For BehaviorRef:
//       kind:     PipelineStepKind::BehaviorRef
//       typeName: InternedString (the struct/type name)
//       method:   InternedString (the method name)
//
//   For BehaviorArgPack:
//       kind:     PipelineStepKind::BehaviorArgPack
//       typeName: InternedString
//       method:   InternedString
//       packArgs: vector<ExprPtr> (arguments to pass after upstream)
// ─────────────────────────────────────────────────────────────────────────────
PipelineStepPtr Parser::parseBehaviorPipelineStep(const std::string& typeName, std::vector<TypePtr> genericArgs) {
    LUC_LOG_EXPR_VERBOSE("parseBehaviorPipelineStep: " << typeName);
    SourceLocation loc = currentLoc();
    auto step = arena_.make<PipelineStepAST>();
    step->loc = loc;

    if (peekNext().type != TokenType::IDENTIFIER) {
        errorAt(DiagCode::E2003, "expected method name after ':'");
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        advance(); // consume ':'
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
        step->typeName = pool_.intern(typeName);
        step->method = pool_.intern(method);
        step->packArgs = std::move(packArgs);
        if (!genericArgs.empty()) {
            errorAt(DiagCode::E2002, "generic arguments not allowed on method reference");
        }
        return step;
    }

    step->kind = PipelineStepKind::BehaviorRef;
    step->typeName = pool_.intern(typeName);
    step->method = pool_.intern(method);
    if (!genericArgs.empty()) {
        errorAt(DiagCode::E2002, "generic arguments not allowed on method reference");
    }
    return step;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFieldPipelineStep
//
// Parses a field access as a pipeline step (field must be of function type).
//
// Grammar:
//   field_step := IDENTIFIER '.' IDENTIFIER [ '(' arg_list ')' '!' ]
//
// Examples:
//   obj.transform           → FieldRef
//   obj.process(2.0, 3.0)!  → FieldArgPack
//
// ─── Two Forms ──────────────────────────────────────────────────────────────
//   1. FieldRef (plain field reference):
//        obj.field
//      - The field must be of function type (non‑nullable).
//      - Upstream value is passed as the first argument to that function.
//
//   2. FieldArgPack (field with argument pack):
//        obj.field(args)!
//      - The '!' suffix marks that the argument list is intentionally incomplete.
//      - Upstream value is injected as the first argument, followed by args.
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called after the object name and optional generic args have been consumed.
// - The current token is '.' (already checked by parsePipelineStep).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '.' token.
// - Consumes the field name (IDENTIFIER).
// - If '(' follows:
//     * Consumes '('
//     * Parses argument list via parseArgList()
//     * Consumes ')'
//     * Consumes '!' (required for argument pack form)
//     * Sets kind = PipelineStepKind::FieldArgPack
// - Otherwise:
//     * Sets kind = PipelineStepKind::FieldRef
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing field name after '.': reports error, returns error step.
// - If '(' is present but '!' is missing after ')': reports error, returns error step.
// - Generic arguments are not allowed on field references; if present, reports
//   an error and ignores them.
//
// ─── Resulting PipelineStep ─────────────────────────────────────────────────
//   For FieldRef:
//       kind:  PipelineStepKind::FieldRef
//       ident: InternedString (the object name)
//       field: InternedString (the field name)
//
//   For FieldArgPack:
//       kind:     PipelineStepKind::FieldArgPack
//       ident:    InternedString
//       field:    InternedString
//       packArgs: vector<ExprPtr> (arguments to pass after upstream)
// ─────────────────────────────────────────────────────────────────────────────
PipelineStepPtr Parser::parseFieldPipelineStep(const std::string& ident, std::vector<TypePtr> genericArgs) {
    LUC_LOG_EXPR_VERBOSE("parseFieldPipelineStep: " << ident);
    SourceLocation loc = currentLoc();
    auto step = arena_.make<PipelineStepAST>();
    step->loc = loc;

    if (peekNext().type != TokenType::IDENTIFIER) {
        errorAt(DiagCode::E2003, "expected field name after '.'");
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        advance(); // consume '.'
        return step;
    }
    advance(); // consume '.'
    std::string field = advance().value;

    if (check(TokenType::LPAREN)) {
        consume(TokenType::LPAREN, "expected '('");
        std::vector<ExprPtr> packArgs;
        if (!check(TokenType::RPAREN)) packArgs = parseArgList();
        consume(TokenType::RPAREN, "expected ')'");
        if (!match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for field argument pack");
            step->kind = PipelineStepKind::Ident;
            step->ident = pool_.intern("<error>");
            return step;
        }
        step->kind = PipelineStepKind::FieldArgPack;
        step->ident = pool_.intern(ident);
        step->field = pool_.intern(field);
        step->packArgs = std::move(packArgs);
        if (!genericArgs.empty()) {
            errorAt(DiagCode::E2002, "generic arguments not allowed on field reference");
        }
        return step;
    }

    step->kind = PipelineStepKind::FieldRef;
    step->ident = pool_.intern(ident);
    step->field = pool_.intern(field);
    if (!genericArgs.empty()) {
        errorAt(DiagCode::E2002, "generic arguments not allowed on field reference");
    }
    return step;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseIndexPipelineStep
//
// Parses an array/slice index as a pipeline step (indexed element must be of
// function type).
//
// Grammar:
//   index_step := IDENTIFIER '[' expr { '[' expr ']' } ']' [ '(' arg_list ')' '!' ]
//
// Examples:
//   handlers[0]                    → IndexRef
//   callbacks[i](extra)!           → IndexArgPack
//   matrix[row][col]               → IndexRef (nested indexing)
//
// ─── Two Forms ──────────────────────────────────────────────────────────────
//   1. IndexRef (plain index reference):
//        arr[idx]
//      - The indexed element must be of function type (non‑nullable).
//      - Upstream value is passed as the first argument to that function.
//
//   2. IndexArgPack (index with argument pack):
//        arr[idx](args)!
//      - The '!' suffix marks that the argument list is intentionally incomplete.
//      - Upstream value is injected as the first argument, followed by args.
//
// ─── Nested Indexing Support ────────────────────────────────────────────────
// - Multiple index brackets are supported: arr[i][j][k]
// - Each nested index builds an IndexExprAST chain.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the first '['.
// - Parses the index expression.
// - Consumes the matching ']'.
// - Repeats for any additional index brackets (nested indexing).
// - If '(' follows:
//     * Consumes '('
//     * Parses argument list via parseArgList()
//     * Consumes ')'
//     * Consumes '!' (required for argument pack form)
//     * Sets kind = PipelineStepKind::IndexArgPack
// - Otherwise:
//     * Sets kind = PipelineStepKind::IndexRef
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing index expression after '[': reports error, consumes tokens until
//   the matching ']' is found, returns error step.
// - Missing closing ']': reports error, recovers by consuming tokens until
//   a ']' or safe boundary is found.
// - If '(' is present but '!' is missing after ')': reports error, returns error step.
// - Generic arguments are not allowed on index steps; if present, reports error.
//
// ─── Resulting PipelineStep ─────────────────────────────────────────────────
//   For IndexRef:
//       kind:  PipelineStepKind::IndexRef
//       ident: InternedString (the array name)
//       index: ExprPtr (the index expression chain)
//
//   For IndexArgPack:
//       kind:     PipelineStepKind::IndexArgPack
//       ident:    InternedString
//       index:    ExprPtr (the index expression chain)
//       packArgs: vector<ExprPtr> (arguments to pass after upstream)
// ─────────────────────────────────────────────────────────────────────────────
PipelineStepPtr Parser::parseIndexPipelineStep(const std::string& ident, std::vector<TypePtr> genericArgs) {
    LUC_LOG_EXPR_VERBOSE("parseIndexPipelineStep: " << ident);
    SourceLocation loc = currentLoc();
    auto step = arena_.make<PipelineStepAST>();
    step->loc = loc;

    auto addIndex = [&](ExprPtr target, ExprPtr idx) -> ExprPtr {
        auto node = arena_.make<IndexExprAST>();
        node->target = std::move(target);
        node->index = std::move(idx);
        node->kind = IndexKind::Element;
        return node;
    };

    ExprPtr indexChain = nullptr;
    advance(); // consume '['
    ExprPtr idx = parseExpr();
    if (!idx) {
        errorAt(DiagCode::E2008, "expected index expression");
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
    
    auto baseIdent = arena_.make<IdentifierExprAST>(pool_.intern(ident));
    baseIdent->loc = loc;
    indexChain = addIndex(std::move(baseIdent), std::move(idx));

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
        step->ident = pool_.intern(ident);
        step->index = std::move(indexChain);
        step->packArgs = std::move(packArgs);
        return step;
    }

    if (!genericArgs.empty()) {
        errorAt(DiagCode::E2002, "generic arguments not allowed on array index step");
    }

    step->kind = PipelineStepKind::IndexRef;
    step->ident = pool_.intern(ident);
    step->index = std::move(indexChain);
    return step;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseArgPackPipelineStep
//
// Parses an argument pack step: fn(args)! where the function name appears
// directly as a pipeline step with an argument pack.
//
// Grammar:
//   arg_pack_step := IDENTIFIER [ generic_args ] '(' arg_list ')' '!'
//
// Example:
//   42 |> scale(2.0)!   → scale(42, 2.0)
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called after the function name (and optional generic args) have been consumed.
// - The current token is '(' (already checked by parsePipelineStep).
// - This is distinct from a regular function call because the '!' suffix is
//   required, and the call is only valid inside a pipeline step.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes '('.
// - Parses the argument list via parseArgList().
// - Consumes ')'.
// - Consumes '!' (required).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '(': consume() reports error.
// - Missing ')' after arguments: consume() reports error.
// - Missing '!' after ')': reports error, returns error step.
//
// ─── Resulting PipelineStep ─────────────────────────────────────────────────
//   PipelineStepAST {
//       kind:       PipelineStepKind::ArgPack
//       ident:      InternedString (the function name)
//       genericArgs: vector<TypePtr> (optional generic arguments)
//       packArgs:    vector<ExprPtr> (arguments to pass after upstream)
//   }
// ─────────────────────────────────────────────────────────────────────────────
PipelineStepPtr Parser::parseArgPackPipelineStep(const std::string& ident, std::vector<TypePtr> genericArgs) {
    LUC_LOG_EXPR_VERBOSE("parseArgPackPipelineStep: " << ident);
    SourceLocation loc = currentLoc();
    auto step = arena_.make<PipelineStepAST>();
    step->loc = loc;

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
    step->ident = pool_.intern(ident);
    step->genericArgs = std::move(genericArgs);
    step->packArgs = std::move(packArgs);
    return step;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseComposeExpr
//
// Parses a function composition expression: lhs '+>' operand { '+>' operand }
//
// Grammar:
//   compose_expr := lhs { '+>' compose_operand }
//
// Examples:
//   f +> g               → composes f then g
//   validate +> transform +> render
//   (a int -> string) +> (s string -> bool)
//
// ─── Operator Precedence ────────────────────────────────────────────────────
// - Precedence: PREC_COMPOSE = 2 (higher than assignment, lower than pipeline)
// - Left‑associative: a +> b +> c  →  (a +> b) +> c
// - Called from parsePrattExpr when '+>' is encountered.
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - Composition is compile‑time: the output type of the left must exactly match
//   the input type of the right operand.
// - The result is a plain function (no qualifiers). Qualifiers belong on the
//   binding, not the composition result.
// - Generics must be explicitly instantiated before composing; type inference
//   across '+>' is not supported.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - The left expression (lhs) is already parsed by the caller.
// - While the current token is '+>', consumes it, then parses one compose operand.
// - After parsing all operands, returns a ComposeExprAST node.
// - If no operands are parsed, returns the original lhs (no composition).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If parseComposeOperand() returns nullptr, reports an error and breaks.
// - Missing operand after '+>': error reported by parseComposeOperand.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   ComposeExprAST {
//       left:     ExprPtr (the left‑hand side expression)
//       operands: vector<ComposeOperandPtr> (right‑hand operands in order)
//   }
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
// Parses a single operand in a composition expression (after '+>').
//
// Grammar:
//   compose_operand := IDENTIFIER
//                    | IDENTIFIER ':' IDENTIFIER   (method reference)
//                    | IDENTIFIER '.' IDENTIFIER   (field reference)
//
// Examples:
//   validate                 → Ident
//   Vec2:normalize           → BehaviorRef
//   processor.transform      → FieldRef
//
// ─── Restrictions ────────────────────────────────────────────────────────────
// - Anonymous functions are NOT allowed as compose operands (compile‑time only).
// - Argument pack '!' is NOT allowed (no runtime injection).
// - Only plain identifiers, method references, and field references are valid.
// - Field references must be non‑nullable (semantic pass enforces this).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the operand name (IDENTIFIER or primitive type keyword).
// - If ':' follows, consumes ':' and the method name → BehaviorRef.
// - If '.' follows, consumes '.' and the field name → FieldRef.
// - Otherwise → Ident.
// - Does NOT consume any tokens beyond the operand (stops after the name or
//   after the method/field name).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If the current token is not an identifier or primitive type, reports error
//   and returns nullptr.
// - Missing method name after ':': reports error, returns nullptr.
// - Missing field name after '.': reports error, returns nullptr.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   For Ident:
//       kind:  ComposeOperandKind::Ident
//       ident: InternedString (function name)
//
//   For BehaviorRef:
//       kind:     ComposeOperandKind::BehaviorRef
//       typeName: InternedString
//       method:   InternedString
//
//   For FieldRef:
//       kind:  ComposeOperandKind::FieldRef
//       ident: InternedString (object name)
//       field: InternedString (field name)
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

// ─────────────────────────────────────────────────────────────────────────────
// Match Expression & Patterns
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseMatchArm
//
// Parses a single non‑default arm in a match expression.
//
// Grammar:
//   match_arm := pattern { ',' pattern } [ 'if' guard_expr ] '=>' arm_body
//   arm_body  := expr [ ',' expr ]
//
// Examples:
//   200 => "ok"
//   200, 201, 202 => "success"
//   1..10 => "light"
//   n if n < 0 => "negative"
//   Vec2 { x, y } => "at " + string(x) + ", " + string(y)
//   200 => "ok", "request succeeded"
//
// ─── Pattern List ───────────────────────────────────────────────────────────
// - One or more patterns, comma‑separated.
// - Each pattern is parsed via parsePattern().
// - All patterns in the list must bind the same set of names (semantic pass).
// - The arm fires if any pattern matches.
//
// ─── Guard Expression ───────────────────────────────────────────────────────
// - Optional 'if' followed by an expression.
// - Only valid after a bind or wildcard pattern.
// - The guard expression may reference names introduced by bind patterns.
// - If the guard evaluates to false, the arm is skipped.
//
// ─── Arm Body (Result Expressions) ──────────────────────────────────────────
// - One required expression, optionally followed by a comma and a second.
// - At most two expressions per arm (primary and optional secondary).
// - Secondary value rules:
//     * If no arm supplies secondary → match produces one value
//     * If every arm supplies secondary → second value is non‑nullable
//     * If only some arms supply secondary → second value must be nullable
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Parses the pattern list (first pattern required, additional after commas).
// - Optionally consumes 'if' and parses the guard expression.
// - Consumes '=>' (FAT_ARROW).
// - Parses the first result expression.
// - If a comma follows, parses the second result expression.
// - Does NOT consume any tokens beyond the second expression (stops before the
//   next arm or the closing '}').
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If no pattern can be parsed, returns nullptr.
// - Missing pattern after comma: reports error, breaks out of pattern loop.
// - Missing guard expression after 'if': reports error.
// - Missing result expression after '=>': reports error.
// - More than two expressions (extra commas): reports error, skips to next arm.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - The pattern list loop checks the next token before consuming a comma to
//   ensure a valid pattern follows, preventing stalls on malformed input.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   MatchArmAST {
//       patterns: vector<PatternPtr> (at least one)
//       guard:    ExprPtr (nullptr if no guard)
//       exprs:    vector<ExprPtr> (1 or 2 expressions)
//   }
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
// Parses the required default arm in a match expression.
//
// Grammar:
//   default_arm := 'default' '=>' arm_body
//   arm_body    := expr [ ',' expr ]
//
// Examples:
//   default => "unknown"
//   default => "unknown", "no detail available"
//
// ─── Position Requirement ───────────────────────────────────────────────────
// - The default arm must be the last arm in the match expression.
// - The semantic pass reports an error if default is not last or if multiple
//   default arms exist.
//
// ─── Arm Body (Result Expressions) ──────────────────────────────────────────
// - One required expression, optionally followed by a comma and a second.
// - At most two expressions per arm.
// - The secondary value presence must be consistent with all other arms:
//     * If no arm supplies secondary → match produces one value
//     * If every arm supplies secondary → second value is non‑nullable
//     * If only some arms supply secondary → second value must be nullable
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'default' keyword.
// - Consumes '=>' (FAT_ARROW).
// - Parses the first result expression.
// - If a comma follows, parses the second result expression.
// - Does NOT consume any tokens beyond the second expression (stops before the
//   closing '}').
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '=>' after 'default': reports error (consume recovers).
// - Missing result expression after '=>': reports error, returns arm with empty
//   exprs (caller may still accept it).
// - More than two expressions (extra commas): reports error, skips to closing
//   brace or next arm.
// - Consecutive commas in body: reports error, skips the extra comma.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   DefaultArmAST {
//       exprs: vector<ExprPtr> (1 or 2 expressions)
//   }
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
// parsePattern
//
// Dispatches to the appropriate pattern sub‑parser based on the current token.
//
// Grammar (pattern):
//   pattern := wildcard_pattern
//            | literal_pattern
//            | range_pattern
//            | bind_pattern
//            | type_pattern
//            | struct_pattern
//            | qualified_constant_pattern
//
// Decision tree:
//   WILDCARD ('_')                    → WildcardPatternAST
//   literal tokens (INT, FLOAT, etc.) → parseLiteralOrRangePattern()
//   IDENTIFIER followed by 'is'       → TypePatternAST
//   IDENTIFIER followed by '{'        → StructPatternAST
//   IDENTIFIER followed by '.'        → Qualified constant pattern (wrapped in PatternExprAST)
//   IDENTIFIER (alone)                → BindPatternAST
//   (default)                         → error
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the pattern token(s) via the appropriate sub‑parser.
// - Does NOT consume any tokens beyond the pattern (stops at the next comma,
//   'if', '=>', or closing brace).
//
// ─── Qualified Constant Pattern ─────────────────────────────────────────────
// - Example: Direction.North
// - Parsed as a full expression (parseExpr()) then wrapped in PatternExprAST.
// - The expression must evaluate to a constant value (enum variant, const, etc.)
//   – enforced by the semantic pass.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If no pattern is recognised, reports "expected pattern" and returns nullptr.
// - Range pattern detection inside bind pattern: if IDENTIFIER is followed by
//   '..', reports an error (bind patterns cannot be used as range bounds) and
//   recovers by consuming the range and calling parseLiteralOrRangePattern().
//
// ─── Result ─────────────────────────────────────────────────────────────────
//   ASTPtr<PatternAST> – never nullptr on success; nullptr on error.
//   Concrete pattern types: WildcardPatternAST, BindPatternAST, TypePatternAST,
//   StructPatternAST, PatternExprAST (wrapping LiteralExprAST or RangeExprAST).
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
// Parses a literal pattern or a range pattern (literal '..' literal).
//
// Grammar:
//   literal_pattern := literal
//   range_pattern   := literal '..' [ '<' ] literal
//
// Examples:
//   42                 → literal pattern
//   "ok"               → literal pattern
//   1..10              → inclusive range pattern
//   1..<10             → exclusive range pattern
//   -5..5              → negative literal as lower bound
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Optionally consumes a '-' prefix (for negative literals).
// - Consumes the first literal token (INT, FLOAT, STRING, etc.).
// - If '..' (RANGE) follows:
//     * Consumes '..'
//     * Optionally consumes '<' (makes range exclusive)
//     * Optionally consumes a '-' prefix for negative upper bound
//     * Consumes the second literal token
//     * Returns a RangeExprAST wrapped in PatternExprAST
// - Otherwise:
//     * Returns a LiteralExprAST wrapped in PatternExprAST
//
// ─── Supported Literal Types ────────────────────────────────────────────────
//   INT_LITERAL, FLOAT_LITERAL, STRING_LITERAL, RAW_STRING_LITERAL,
//   CHAR_LITERAL, HEX_LITERAL, BINARY_LITERAL, TRUE, FALSE, NIL
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If the first token is not a valid literal, reports error, returns nullptr.
// - If '..' is present but no second literal follows, reports error, returns nullptr.
// - Invalid second literal type (e.g., string in a numeric range) is reported
//   and recovery attempts to consume the token.
//
// ─── Result ─────────────────────────────────────────────────────────────────
//   PatternExprAST wrapping either:
//       - LiteralExprAST (single literal)
//       - RangeExprAST (lo..hi or lo..<hi)
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
//
// Parses a bind pattern: an identifier that binds the matched value to a name.
//
// Grammar:
//   bind_pattern := IDENTIFIER
//
// Example:
//   n     → binds the matched value to 'n'
//   item  → binds the matched value to 'item'
//   _     → not a bind pattern (handled by parseWildcardPattern)
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called after the IDENTIFIER token has already been consumed.
// - The name is passed as a parameter (already interned).
//
// ─── Scope Introduction ─────────────────────────────────────────────────────
// - The bind pattern introduces a new variable in the arm's scope.
// - The variable's type is the type of the matched value (narrowed by the
//   pattern context).
// - The variable is accessible in the guard expression and the arm body.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes no additional tokens (the identifier was already consumed).
// - The caller (parsePattern) consumes the identifier before calling this.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - This function does not report errors; it assumes the identifier is already
//   validated by the caller.
// - Always returns a valid BindPatternAST.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   BindPatternAST {
//       name: InternedString (the variable name to bind)
//   }
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
// Parses a type pattern: IDENTIFIER 'is' type
//
// Grammar:
//   type_pattern := IDENTIFIER 'is' type
//
// Example:
//   s is Circle   → matches if subject is a Circle, binds as 's' typed Circle
//   v is Rect     → matches if subject is a Rect, binds as 'v' typed Rect
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - Combines a runtime type check with a name binding.
// - If the subject's runtime type matches checkType, the value is bound to
//   bindName (with the narrowed type).
// - After a successful match, the bindName's type is narrowed to checkType
//   for the duration of the arm body.
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called after the IDENTIFIER has been consumed by parsePattern().
// - The bindName is passed as a parameter (already interned).
// - The current token should be 'is'.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'is' keyword.
// - Parses the type annotation via parseType().
// - Does NOT consume any tokens beyond the type.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing 'is' after identifier: consume() reports error, returns nullptr.
// - Missing or invalid type after 'is': parseType() reports error, returns nullptr.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   TypePatternAST {
//       bindName:  InternedString (the variable name to bind)
//       checkType: TypePtr (the type to check against)
//   }
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
//
// Parses a wildcard pattern: '_' which matches any value and discards it.
//
// Grammar:
//   wildcard_pattern := '_'
//
// Example:
//   _ => "anything"
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - Matches any value (like a bind pattern) but does NOT introduce a variable
//   name into the arm's scope.
// - The matched value is discarded and cannot be referenced in the guard or body.
// - May appear with a guard, but the guard cannot reference the matched value
//   (since there's no name bound).
//
// ─── Distinction from 'default' ─────────────────────────────────────────────
//   '_'      – pattern that matches anything; may appear in any arm position
//   default  – required final fallback arm keyword (not a pattern)
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '_' token (WILDCARD).
// - Does NOT consume any tokens beyond the wildcard.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - This function does not report errors; it assumes the WILDCARD token is
//   already validated by the caller (parsePattern).
// - Always returns a valid WildcardPatternAST.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   WildcardPatternAST (no fields)
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
// Parses a struct destructuring pattern: IDENTIFIER '{' { field_pattern } '}'
//
// Grammar:
//   struct_pattern := IDENTIFIER '{' { field_pattern } '}'
//   field_pattern  := IDENTIFIER [ ':' pattern ]
//
// Examples:
//   Vec2 { x, y }                     → shorthand: binds x and y from subject
//   Vec2 { x: 0.0, y: 0.0 }          → exact match on field values
//   Player { health: 0, name }       → mixed: exact match on health, bind name
//   Vec2 { x: 0.0, y: v }            → nested: bind y's value to variable v
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - Matches when the subject is a struct of the named type.
// - Fields not listed in the pattern are ignored (match succeeds regardless).
// - For each field pattern:
//     * If only field name is given (no ':'): binds the field's value to a
//       variable with the same name.
//     * If ':' and a sub‑pattern are given: the field's value must match the
//       sub‑pattern (which may be a literal, range, bind, type, or nested struct).
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called after the type name has been consumed by parsePattern().
// - The current token is '{' (already checked).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '{'.
// - Repeatedly parses field patterns via parseFieldPattern() until '}'.
// - Consumes optional commas between field patterns.
// - Consumes the closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '{' after type name: consume() reports error.
// - If parseFieldPattern() returns nullptr and no progress was made, consumes
//   one token and continues (prevents infinite loop).
// - Missing closing '}': consume() reports error and recovers.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - The field pattern loop uses a progress guard: saves pos_ before
//   parseFieldPattern().
// - If parseFieldPattern() returns nullptr and pos_ == savedPos, consumes one
//   token to guarantee progress.
// - Optional commas are consumed without stalling.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   StructPatternAST {
//       typeName: InternedString (the struct type name)
//       fields:   vector<FieldPatternPtr> (field patterns in source order)
//   }
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
// Parses a single field entry inside a struct pattern.
//
// Grammar:
//   field_pattern := IDENTIFIER
//                  | IDENTIFIER ':' pattern
//
// Examples:
//   x               → shorthand: bind field 'x' to variable 'x'
//   x: 0.0          → full form: match field 'x' against literal 0.0
//   pos: Vec2 { ... } → nested: match field 'pos' against a struct pattern
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - Shorthand form (no ':'): equivalent to field_name: bind_pattern(field_name)
//   The field's value is bound to a variable with the same name.
// - Full form (with ':'): the field's value must match the given sub‑pattern.
// - The sub‑pattern can be any valid pattern (literal, range, bind, type,
//   wildcard, or nested struct pattern).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the field name (IDENTIFIER).
// - If ':' follows, consumes it and parses the sub‑pattern via parsePattern().
// - Does NOT consume any tokens beyond the sub‑pattern (or beyond the field name
//   if no sub‑pattern).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing field name: reports error, returns nullptr.
// - If ':' is present but sub‑pattern parsing fails: reports error, returns
//   field node with null subPattern (the caller may still accept it).
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   FieldPatternAST {
//       field:      InternedString (the struct field name)
//       subPattern: PatternPtr (nullptr for shorthand form, sub‑pattern for full)
//   }
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

// ─────────────────────────────────────────────────────────────────────────────
// Lvalue Parser (for multi‑assignment)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseLvalue
//
// Parses an assignable left‑hand side (lvalue) expression for multi‑assignment.
//
// Grammar:
//   lvalue := IDENTIFIER { ( '.' IDENTIFIER ) | ( '[' expr ']' ) }
//
// Examples:
//   x
//   point.x
//   arr[i]
//   matrix[row][col]
//   p.x.y
//
// ─── Difference from parseExpr() ────────────────────────────────────────────
//   parseExpr() would treat '=' as a binary operator and prematurely consume
//   the assignment token, breaking multi‑assignment parsing.
//   parseLvalue() stops at the first token that is not part of a valid lvalue,
//   leaving the '=' for the caller (parseMultiAssignStmt) to consume.
//
// ─── Allowed Lvalue Suffixes ────────────────────────────────────────────────
//   - '.' IDENTIFIER  – field access (e.g., point.x)
//   - '[' expr ']'    – array/slice index (e.g., arr[i])
//
// ─── Not Allowed (stops before these) ───────────────────────────────────────
//   - ':'             – behavior access (not assignable)
//   - '()'            – function call (not assignable)
//   - '??', '|>', '+>' – operators (not part of lvalue)
//   - '='             – assignment operator (stopped before it)
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the first IDENTIFIER (required).
// - While the next token is '.' or '[', consumes the suffix and builds the
//   corresponding AST node (FieldAccessExprAST or IndexExprAST).
// - Stops when a token that cannot be part of an lvalue is encountered.
// - Does NOT consume the '=' token or any operator following the lvalue.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing identifier at start: reports error, returns nullptr.
// - Missing field name after '.': reports error, returns current expression.
// - Missing index expression after '[': reports error, returns current expression.
// - Missing closing ']' after index: consume() reports error.
//
// ─── Result ─────────────────────────────────────────────────────────────────
//   ExprPtr – an expression tree representing an assignable location:
//       - IdentifierExprAST (simple variable)
//       - FieldAccessExprAST (field access chain)
//       - IndexExprAST (array/slice index chain)
//   Returns nullptr on error.
// ─────────────────────────────────────────────────────────────────────────────
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
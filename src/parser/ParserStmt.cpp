/**
 * @file ParserStmt.cpp
 * 
 * @responsibility Parses control-flow, blocks, and parallel statements.
 * 
 * @grammar_rules If, Switch, For, While, Do, Return, Parallel.
 * 
 * @related src/diagnostics/DiagnosticEngine.hpp, DiagnosticCodes.hpp
 */

#include "Parser.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <cassert>
#include <algorithm>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// ParserStmt.cpp
//
// Implements every statement parse function declared in Parser.hpp.
// This is the last layer of the parser — it calls into all four other files:
//   parseType()  (ParserType.cpp)
//   parseDecl()  (ParserDecl.cpp)  — for local declarations inside blocks
//   parseExpr()  (ParserExpr.cpp)
//   parseBlock() (this file)       — recursive
//
// Entry point for all statement parsing is parseStmt(), which is called by
// parseBlock() in a loop. parseBlock() is called by:
//   parseFuncBody()      (ParserDecl.cpp)
//   parseIfStmt/Expr()   (this file / ParserExpr.cpp)
//   parseForStmt()
//   parseWhileStmt()
//   parseDoWhileStmt()
//   parseParallelForStmt()
//   parseParallelBlockStmt()
//   parseArmBody()       (ParserExpr.cpp)
//
// Context depth counters (declared on Parser) are maintained here:
//   loopDepth_     — incremented/decremented around for/while/do bodies
//   parallelDepth_ — incremented/decremented around parallel bodies
//   asyncDepth_    — managed in ParserDecl.cpp::parseFuncBody and
//                    ParserExpr.cpp::parseAnonFuncExpr / parseAwaitExpr
//
// Grammar source: LUC_GRAMMAR.md §Statements
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// parseBlock
//
// Grammar:  block := '{' { stmt } '}'
//
// Foundational building block — every body in the language is a block.
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<BlockStmtAST> Parser::parseBlock()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACE, "expected '{'");

    auto block  = std::make_unique<BlockStmtAST>();
    block->loc  = loc;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Skip optional statement separators.
        if (match(TokenType::SEMICOLON)) continue;

        StmtPtr stmt = parseStmt();
        if (stmt) {
            block->stmts.push_back(std::move(stmt));
        } else {
            // parseStmt already recorded an error.
            // Synchronize to the next statement boundary so we can keep parsing.
            if (!check(TokenType::RBRACE) && !isAtEnd()) {
                synchronize();
            }
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close block");
    return block;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseStmt  — root statement dispatcher
//
// Dispatch priority (in order):
//   1. Declaration keywords (let / const) → local declaration
//   2. pub inside a block                     → error + skip
//   3. Control-flow keywords                  → their specific parsers
//   4. parallel                               → parallel for / parallel block
//   5. Everything else                        → expression statement
//
// 'match' and 'if' in statement position are parsed as expression statements —
// the expression parsers produce MatchExprAST / IfExprAST, and the result is
// wrapped in ExprStmtAST.  The only exception is that 'if' at statement level
// is parsed as IfStmtAST (else optional) rather than IfExprAST (else required).
// This distinction matters for error messages and for the semantic pass.
// ─────────────────────────────────────────────────────────────────────────────

StmtPtr Parser::parseStmt()
{
    // ── Local declarations ────────────────────────────────────────────────────
    if (checkAny({ TokenType::LET, TokenType::CONST })) {
        return parseLocalDecl();
    }

    // ── 'pub' inside a block is always wrong ──────────────────────────────────
    if (check(TokenType::PUB)) {
        errorAt(DiagCode::E2006, "'pub' is not valid inside a block — visibility modifiers are only allowed at top level");
        advance();   // consume 'pub' so we don't spin
        // Try to recover: if the next token is let/const, parse as local decl.
        if (checkAny({ TokenType::LET, TokenType::CONST })) {
            return parseLocalDecl();
        }
        return nullptr;
    }

    // ── Control flow ──────────────────────────────────────────────────────────
    if (check(TokenType::IF))       return parseIfStmt();
    if (check(TokenType::SWITCH))   return parseSwitchStmt();
    if (check(TokenType::FOR))      return parseForStmt();
    if (check(TokenType::WHILE))    return parseWhileStmt();
    if (check(TokenType::DO))       return parseDoWhileStmt();
    if (check(TokenType::RETURN))   return parseReturnStmt();
    if (check(TokenType::BREAK))    return parseBreakStmt();
    if (check(TokenType::CONTINUE)) return parseContinueStmt();

    // ── Parallel ──────────────────────────────────────────────────────────────
    if (check(TokenType::PARALLEL)) {
        // 'parallel for' vs 'parallel { ... }'
        if (peekNext().type == TokenType::FOR) {
            return parseParallelForStmt();
        } else {
            return parseParallelBlockStmt();
        }
    }

    // ── Expression statement ──────────────────────────────────────────────────
    // Anything that is not a keyword starts an expression.
    // We check looksLikeStmtStart() for a readable error on truly garbage input.
    if (!looksLikeStmtStart()) {
        errorAt(DiagCode::E2002, "unexpected token '" + peek().value + "' in statement position");
        return nullptr;
    }

    SourceLocation loc = currentLoc();
    ExprPtr expr = parseExpr();
    if (!expr) return nullptr;

    auto stmt  = std::make_unique<ExprStmtAST>(std::move(expr));
    stmt->loc  = loc;
    return stmt;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseLocalDecl
//
// Parses a let / const declaration inside a block body.
// Produces either VarDeclAST or FuncDeclAST wrapped in DeclStmtAST.
//
// Key differences from top-level declarations:
//   - 'pub' is forbidden (already checked in parseStmt)
//   - 'extern' is forbidden (local extern makes no semantic sense)
//   - struct / enum / trait / impl / type are forbidden (top-level only)
//
// The keyword is consumed here; the name remains at pos_ so that
// looksLikeFuncDecl() and the individual parsers read it correctly.
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<DeclStmtAST> Parser::parseLocalDecl()
{
    SourceLocation loc = currentLoc();

    // Consume the keyword.
    Token kwTok = advance();
    DeclKeyword kw;
    switch (kwTok.type) {
        case TokenType::LET:   kw = DeclKeyword::Let;   break;
        default:               kw = DeclKeyword::Const; break;
    }

    // Name must follow immediately.
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected name after '" + kwTok.value + "'");
        return nullptr;
    }

    // looksLikeFuncDecl() inspects pos_ which is now on the name.
    if (looksLikeFuncDecl()) {
        // Local function declaration.
        auto funcDecl = parseFuncDecl(kw, Visibility::Private);
        if (!funcDecl) return nullptr;

        funcDecl->loc   = loc;
        funcDecl->visibility = Visibility::Private;   // enforce: no pub inside a block

        LocalDecl ld = std::move(funcDecl);
        auto ds  = std::make_unique<DeclStmtAST>(std::move(ld));
        ds->loc  = loc;
        return ds;
    } else {
        // Local variable declaration.
        // parseVarDecl reads tokens_[pos_ - 1] to recover the keyword — that
        // is kwTok, which we just consumed. This contract holds.
        auto varDecl = parseVarDecl(Visibility::Private);
        if (!varDecl) return nullptr;

        varDecl->loc   = loc;
        varDecl->visibility = Visibility::Private;

        LocalDecl ld = std::move(varDecl);
        auto ds  = std::make_unique<DeclStmtAST>(std::move(ld));
        ds->loc  = loc;
        return ds;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// parseIfStmt
//
// Grammar:
//   if_stmt := 'if' expr block [ 'else' ( if_stmt | block ) ]
//
// Statement form — else is optional.  This is distinct from parseIfExpr()
// (in ParserExpr.cpp) where else is required.
//
// Dispatching:
//   In parseStmt() we always route 'if' here.
//   In expression position (e.g. assignment RHS, function return body),
//   parseExpr() → parsePrimaryExpr() → parseIfExpr() is taken instead.
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<IfStmtAST> Parser::parseIfStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::IF, "expected 'if'");

    // Condition — parsed as a full expression, but disallow top-level struct literals
    // to avoid ambiguity with the following block.
    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        return nullptr;
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after if condition");
        return nullptr;
    }
    StmtPtr thenBranch = parseBlock();

    auto node           = std::make_unique<IfStmtAST>();
    node->loc           = loc;
    node->condition     = std::move(condition);
    node->thenBranch    = std::move(thenBranch);

    // Optional else clause.
    if (match(TokenType::ELSE)) {
        if (check(TokenType::IF)) {
            // else if — recurse, produces another IfStmtAST as elseBranch.
            node->elseBranch = parseIfStmt();
        } else if (check(TokenType::LBRACE)) {
            node->elseBranch = parseBlock();
        } else {
            errorAt(DiagCode::E2001, "expected 'if' or '{' after 'else'");
        }
    }
    // elseBranch remains nullptr when no else clause is present.

    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseSwitchStmt
//
// Grammar:
//   switch_stmt := 'switch' expr '{' { case_clause } [ default_clause ] '}'
//   case_clause := 'case' case_value { ',' case_value } ':' block
//   case_value  := expr | range_expr
//   default_clause := 'default' ':' block
//
// switch is statement-oriented (no value produced).
// Multiple values and ranges per case; no fallthrough.
// default is optional (unlike match where it is required).
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<SwitchStmtAST> Parser::parseSwitchStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::SWITCH, "expected 'switch'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'switch'");
        return nullptr;
    }

    consume(TokenType::LBRACE, "expected '{' after switch subject");

    auto node      = std::make_unique<SwitchStmtAST>();
    node->loc      = loc;
    node->subject  = std::move(subject);

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        if (check(TokenType::RBRACE)) break;

        if (check(TokenType::CASE)) {
            SwitchCasePtr sc = parseSwitchCase();
            if (sc) node->cases.push_back(std::move(sc));
            continue;
        }

        if (check(TokenType::DEFAULT)) {
            if (node->defaultBody) {
                errorAt(DiagCode::E2007, "duplicate 'default' clause in switch statement");
            }
            node->defaultLoc = currentLoc();
            advance();  // consume 'default'
            consume(TokenType::COLON, DiagCode::E2001, "expected ':' after 'default'");
            // Default body is a block (the grammar says { stmt } but we always
            // parse it as a full block for uniformity).
            if (!check(TokenType::LBRACE)) {
                errorAt(DiagCode::E2001, "expected '{' to start default body");
            } else {
                node->defaultBody = parseBlock();
            }
            continue;
        }

        errorAt(DiagCode::E2002, "expected 'case' or 'default' inside switch block");
        synchronize();
    }

    consume(TokenType::RBRACE, "expected '}' to close switch statement");
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseSwitchCase
//
// Grammar:
//   case_clause := 'case' case_value { ',' case_value } ':' block
//   case_value  := expr | range_expr
//
// Range detection: after parsing a primary expression, if '..' follows we
// build a RangeExprAST from the already-parsed lo expression and the hi
// expression that follows '..'.
// ─────────────────────────────────────────────────────────────────────────────

SwitchCasePtr Parser::parseSwitchCase()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::CASE, "expected 'case'");

    auto sc   = std::make_unique<SwitchCaseAST>();
    sc->loc   = loc;

    // Parse one or more comma-separated case values.
    do {
        match(TokenType::COMMA);   // optional separator / skip after first
        if (check(TokenType::COLON)) break;   // end of value list

        // Parse a primary-level expression (literal or named constant).
        // We do NOT parse a full expression here to avoid consuming ',' or ':'
        // as binary operators.  The semantic pass verifies values are constants.
        ExprPtr val = parsePrattExpr(0);   // full expression — range check below
        if (!val) {
            errorAt(DiagCode::E2008, "expected case value after 'case'");
            break;
        }

        // If '..' follows, this is a range value.
        if (check(TokenType::RANGE)) {
            sc->values.push_back(parseRangeExpr(std::move(val)));
        } else {
            sc->values.push_back(std::move(val));
        }

    } while (check(TokenType::COMMA));

    consume(TokenType::COLON, DiagCode::E2001, "expected ':' after case values");

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start case body");
    } else {
        sc->body = parseBlock();
    }

    return sc;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseForStmt
//
// Grammar:
//   for_stmt := 'for' IDENTIFIER 'in' expr block
//             | 'for' IDENTIFIER 'in' range_expr block
//
// Both forms map to the same node. Range detection: if the iterable expression
// is followed by '..' we convert it to a RangeExprAST.  The semantic pass
// determines the iteration variable type from the iterable.
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ForStmtAST> Parser::parseForStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::FOR, "expected 'for'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected iteration variable name after 'for'");
        return nullptr;
    }
    std::string varName = advance().value;

    // Optional: Parse explicit type annotation if 'in' does not follow immediately.
    TypePtr varType = nullptr;
    if (!check(TokenType::IN)) {
        varType = parseType();
        if (!varType) {
            errorAt(DiagCode::E2005, "expected 'in' or explicit type after iteration variable name");
            return nullptr;
        }
    }

    consume(TokenType::IN, "expected 'in' after iteration variable");

    // Parse the iterable expression (collection or RangeExprAST).
    ExprPtr iterable = parseExpr(false);
    if (!iterable) {
        errorAt(DiagCode::E2008, "expected iterable expression after 'in'");
        return nullptr;
    }

    // Optional: if '..' follows, build RangeExprAST from boundaries and step.
    ExprPtr step = nullptr;
    if (check(TokenType::RANGE)) {
        iterable = parseRangeExpr(std::move(iterable));
        
        // After start..end, an optional second '..' can signal a step.
        if (match(TokenType::RANGE)) {
            step = parseExpr();
            if (!step) {
                errorAt(DiagCode::E2008, "expected step expression after '..'");
                return nullptr;
            }
        }
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start for loop body");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    auto node       = std::make_unique<ForStmtAST>();
    node->loc       = loc;
    node->varName   = std::move(varName);
    node->varType   = std::move(varType);
    node->iterable  = std::move(iterable);
    node->step      = std::move(step);
    node->body      = std::move(body);
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseWhileStmt
//
// Grammar:  while_stmt := 'while' expr block
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<WhileStmtAST> Parser::parseWhileStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::WHILE, "expected 'while'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while'");
        return nullptr;
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start while loop body");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    auto node           = std::make_unique<WhileStmtAST>();
    node->loc           = loc;
    node->condition     = std::move(condition);
    node->body          = std::move(body);
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseDoWhileStmt
//
// Grammar:  do_while_stmt := 'do' block 'while' expr
//
// The body executes unconditionally before the condition is first evaluated.
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<DoWhileStmtAST> Parser::parseDoWhileStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::DO, "expected 'do'");

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after 'do'");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    consume(TokenType::WHILE, "expected 'while' after do body");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while' in do-while loop");
        return nullptr;
    }

    auto node           = std::make_unique<DoWhileStmtAST>();
    node->loc           = loc;
    node->body          = std::move(body);
    node->condition     = std::move(condition);
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseReturnStmt
//
// Grammar:  return_stmt := 'return' [ expr ]
//
// A bare 'return' (no expression) is valid in void functions.
// Return inside a parallel body is a parse-time error.
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ReturnStmtAST> Parser::parseReturnStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::RETURN, "expected 'return'");

    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'return' is not valid inside a 'parallel' body");
    }

    auto node  = std::make_unique<ReturnStmtAST>();
    node->loc  = loc;

    // A return value is present when the next token can start an expression
    // and is not a statement terminator ('}', ';', or a declaration keyword
    // that would belong to the next statement).
    // We check conservatively: if the next token can start an expression, parse it.
    bool hasValue = !check(TokenType::RBRACE)
                 && !check(TokenType::SEMICOLON)
                 && !isAtEnd()
                 && looksLikeStmtStart();

    // Tighter check: a return value starts an expression, not a new declaration.
    // Exclude the declaration keywords so 'return\nlet x = 5' is two statements.
    if (hasValue && checkAny({ TokenType::LET, TokenType::CONST })) {
        hasValue = false;
    }

    if (hasValue) {
        node->value = parseExpr();
    }

    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseBreakStmt
//
// Grammar:  break_stmt := 'break'
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<BreakStmtAST> Parser::parseBreakStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::BREAK, "expected 'break'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E2006, "'break' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'break' is not valid inside a 'parallel' body");
    }

    auto node  = std::make_unique<BreakStmtAST>();
    node->loc  = loc;
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseContinueStmt
//
// Grammar:  continue_stmt := 'continue'
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ContinueStmtAST> Parser::parseContinueStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::CONTINUE, "expected 'continue'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E2006, "'continue' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'continue' is not valid inside a 'parallel' body");
    }

    auto node  = std::make_unique<ContinueStmtAST>();
    node->loc  = loc;
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseParallelForStmt
//
// Grammar:
//   parallel_for := 'parallel' 'for' IDENTIFIER 'in' expr block
//
// The iteration variable is independently bound per iteration — no shared
// mutable state.  The parallel body disallows await, return, break, continue.
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ParallelForStmtAST> Parser::parseParallelForStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::PARALLEL, "expected 'parallel'");
    consume(TokenType::FOR,      "expected 'for' after 'parallel'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected iteration variable name after 'parallel for'");
        return nullptr;
    }
    std::string varName = advance().value;

    // Optional: Parse explicit type annotation if 'in' does not follow immediately.
    TypePtr varType = nullptr;
    if (!check(TokenType::IN)) {
        varType = parseType();
        if (!varType) {
            errorAt(DiagCode::E2005, "expected 'in' or explicit type after iteration variable name");
            return nullptr;
        }
    }

    consume(TokenType::IN, "expected 'in' after iteration variable");

    // Parse the iterable expression (collection or RangeExprAST).
    ExprPtr iterable = parseExpr(false);
    if (!iterable) {
        errorAt(DiagCode::E2008, "expected iterable expression after 'in'");
        return nullptr;
    }

    // Optional: if '..' follows, build RangeExprAST from boundaries and step.
    ExprPtr step = nullptr;
    if (check(TokenType::RANGE)) {
        iterable = parseRangeExpr(std::move(iterable));
        
        // After start..end, an optional second '..' can signal a step.
        if (match(TokenType::RANGE)) {
            step = parseExpr();
            if (!step) {
                errorAt(DiagCode::E2008, "expected step expression after '..'");
                return nullptr;
            }
        }
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start parallel for body");
        return nullptr;
    }

    // Inside a parallel body: no await, no return, no break/continue.
    ++parallelDepth_;
    StmtPtr body = parseBlock();
    --parallelDepth_;

    auto node       = std::make_unique<ParallelForStmtAST>();
    node->loc       = loc;
    node->varName   = std::move(varName);
    node->varType   = std::move(varType);
    node->iterable  = std::move(iterable);
    node->step      = std::move(step);
    node->body      = std::move(body);
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseParallelBlockStmt
//
// Grammar:
//   parallel_block := 'parallel' '{' { block } '}'
//
// Each inner block is an independent concurrent task.  The outer '{' is the
// parallel container; the inner '{' blocks are the sub-tasks.
//
// Minimum one sub-block — a parallel block with zero tasks is a semantic error
// (recorded as a parse-time error for an earlier diagnostic).
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ParallelBlockStmtAST> Parser::parseParallelBlockStmt()
{
    SourceLocation loc = currentLoc();
    consume(TokenType::PARALLEL, "expected 'parallel'");
    consume(TokenType::LBRACE,   "expected '{' after 'parallel'");

    auto node  = std::make_unique<ParallelBlockStmtAST>();
    node->loc  = loc;

    ++parallelDepth_;

    // Each '{' that appears directly inside the outer '{' is one sub-task block.
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        if (check(TokenType::RBRACE)) break;

        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start a parallel sub-task block");
            synchronize();
            continue;
        }

        node->subBlocks.push_back(parseBlock());
    }

    --parallelDepth_;

    consume(TokenType::RBRACE, "expected '}' to close parallel block");

    if (node->subBlocks.empty()) {
        error(loc, DiagCode::E2007, "parallel block must contain at least one sub-task block");
    }

    return node;
}
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
    LUC_LOG_STMT("parseBlock");
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACE, "expected '{'");

    auto block = std::make_unique<BlockStmtAST>();
    block->loc = loc;
    int stmtCount = 0;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Skip optional statement separators.
        if (match(TokenType::SEMICOLON)) continue;

        StmtPtr stmt = parseStmt();
        if (stmt) {
            block->stmts.push_back(std::move(stmt));
            stmtCount++;
        } else {
            // parseStmt already recorded an error.
            // Synchronize to the next statement boundary so we can keep parsing.
            if (!check(TokenType::RBRACE) && !isAtEnd()) {
                synchronize();
            }
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close block");
    LUC_LOG_STMT("parseBlock: parsed " << stmtCount << " statements");
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
    LUC_LOG_STMT_VERBOSE("parseStmt: token='" << peek().value << "' type=" << static_cast<int>(peek().type));
    
    // ── Local declarations ────────────────────────────────────────────────────
    if (checkAny({ TokenType::LET, TokenType::CONST })) {
        LUC_LOG_STMT_VERBOSE("parseStmt: local declaration");
        return parseLocalDecl();
    }

    // ── 'pub' inside a block is always wrong ──────────────────────────────────
    if (check(TokenType::PUB)) {
        LUC_LOG_STMT("parseStmt: 'pub' inside block - error");
        errorAt(DiagCode::E2006, "'pub' is not valid inside a block — visibility modifiers are only allowed at top level");
        advance();   // consume 'pub' so we don't spin
        // Try to recover: if the next token is let/const, parse as local decl.
        if (checkAny({ TokenType::LET, TokenType::CONST })) {
            return parseLocalDecl();
        }
        return nullptr;
    }

    // ── Control flow ──────────────────────────────────────────────────────────
    if (check(TokenType::IF)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: if statement");
        return parseIfStmt();
    }
    if (check(TokenType::SWITCH)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: switch statement");
        return parseSwitchStmt();
    }
    if (check(TokenType::FOR)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: for loop");
        return parseForStmt();
    }
    if (check(TokenType::WHILE)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: while loop");
        return parseWhileStmt();
    }
    if (check(TokenType::DO)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: do-while loop");
        return parseDoWhileStmt();
    }
    if (check(TokenType::RETURN)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: return statement");
        return parseReturnStmt();
    }
    if (check(TokenType::BREAK)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: break statement");
        return parseBreakStmt();
    }
    if (check(TokenType::CONTINUE)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: continue statement");
        return parseContinueStmt();
    }

    // ── Parallel ──────────────────────────────────────────────────────────────
    if (check(TokenType::PARALLEL)) {
        // 'parallel for' vs 'parallel { ... }'
        if (peekNext().type == TokenType::FOR) {
            LUC_LOG_STMT_VERBOSE("parseStmt: parallel for");
            return parseParallelForStmt();
        } else {
            LUC_LOG_STMT_VERBOSE("parseStmt: parallel block");
            return parseParallelBlockStmt();
        }
    }

    // ── Expression statement ──────────────────────────────────────────────────
    // Anything that is not a keyword starts an expression.
    // We check looksLikeStmtStart() for a readable error on truly garbage input.
    if (!looksLikeStmtStart()) {
        LUC_LOG_STMT("parseStmt: unexpected token - not a statement start");
        errorAt(DiagCode::E2002, "unexpected token '" + peek().value + "' in statement position");
        return nullptr;
    }

    SourceLocation loc = currentLoc();
    ExprPtr expr = parseExpr();
    if (!expr) return nullptr;

    auto stmt = std::make_unique<ExprStmtAST>(std::move(expr));
    stmt->loc = loc;
    LUC_LOG_STMT_EXTREME("parseStmt: expression statement");
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
    LUC_LOG_STMT("parseLocalDecl");
    SourceLocation loc = currentLoc();

    // Consume the keyword.
    Token kwTok = advance();
    DeclKeyword kw;
    switch (kwTok.type) {
        case TokenType::LET:   kw = DeclKeyword::Let;   break;
        default:               kw = DeclKeyword::Const; break;
    }
    LUC_LOG_STMT_VERBOSE("parseLocalDecl: keyword='" << kwTok.value << "'");

    // Name must follow immediately.
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected name after '" + kwTok.value + "'");
        return nullptr;
    }

    // looksLikeFuncDecl() inspects pos_ which is now on the name.
    if (looksLikeFuncDecl()) {
        LUC_LOG_STMT_VERBOSE("parseLocalDecl: parsing as local function");
        // Local function declaration.
        auto funcDecl = parseFuncDecl(kw, Visibility::Private);
        if (!funcDecl) return nullptr;

        funcDecl->loc = loc;
        funcDecl->visibility = Visibility::Private;   // enforce: no pub inside a block

        LocalDecl ld = std::move(funcDecl);
        auto ds = std::make_unique<DeclStmtAST>(std::move(ld));
        ds->loc = loc;
        return ds;
    } else {
        LUC_LOG_STMT_VERBOSE("parseLocalDecl: parsing as local variable");
        // Local variable declaration.
        // parseVarDecl reads tokens_[pos_ - 1] to recover the keyword — that
        // is kwTok, which we just consumed. This contract holds.
        // parseLocalDecl currently calls parseVarDecl(Visibility::Private)
        auto varDecl = parseVarDecl(Visibility::Private, {});  // No attributes in local scope
        if (!varDecl) return nullptr;

        varDecl->loc = loc;
        varDecl->visibility = Visibility::Private;

        LocalDecl ld = std::move(varDecl);
        auto ds = std::make_unique<DeclStmtAST>(std::move(ld));
        ds->loc = loc;
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
// ────────────────────────────────────────────────────────────────────────────
std::unique_ptr<IfStmtAST> Parser::parseIfStmt()
{
    LUC_LOG_STMT("parseIfStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::IF, "expected 'if'");

    // Condition — parsed as a full expression, but disallow top-level struct literals
    // to avoid ambiguity with the following block.
    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        return nullptr;
    }
    LUC_LOG_STMT_VERBOSE("parseIfStmt: condition parsed");

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after if condition");
        return nullptr;
    }
    StmtPtr thenBranch = parseBlock();
    LUC_LOG_STMT_VERBOSE("parseIfStmt: then branch parsed");

    auto node = std::make_unique<IfStmtAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->thenBranch = std::move(thenBranch);

    // Optional else clause.
    if (match(TokenType::ELSE)) {
        LUC_LOG_STMT_VERBOSE("parseIfStmt: has else clause");
        if (check(TokenType::IF)) {
            // else if — recurse, produces another IfStmtAST as elseBranch.
            node->elseBranch = parseIfStmt();
            LUC_LOG_STMT_VERBOSE("parseIfStmt: else-if chain");
        } else if (check(TokenType::LBRACE)) {
            node->elseBranch = parseBlock();
            LUC_LOG_STMT_VERBOSE("parseIfStmt: else block");
        } else {
            errorAt(DiagCode::E2001, "expected 'if' or '{' after 'else'");
        }
    } else {
        LUC_LOG_STMT_EXTREME("parseIfStmt: no else clause");
    }
    // elseBranch remains nullptr when no else clause is present.

    LUC_LOG_STMT_VERBOSE("parseIfStmt: returning IfStmtAST");
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
    LUC_LOG_STMT("parseSwitchStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::SWITCH, "expected 'switch'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'switch'");
        return nullptr;
    }
    LUC_LOG_STMT_VERBOSE("parseSwitchStmt: subject parsed");

    consume(TokenType::LBRACE, "expected '{' after switch subject");

    auto node = std::make_unique<SwitchStmtAST>();
    node->loc = loc;
    node->subject = std::move(subject);
    int caseCount = 0;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        if (check(TokenType::RBRACE)) break;

        if (check(TokenType::CASE)) {
            SwitchCasePtr sc = parseSwitchCase();
            if (sc) {
                node->cases.push_back(std::move(sc));
                caseCount++;
            }
            continue;
        }

        if (check(TokenType::DEFAULT)) {
            if (node->defaultBody) {
                errorAt(DiagCode::E2007, "duplicate 'default' clause in switch statement");
            }
            node->defaultLoc = currentLoc();
            LUC_LOG_STMT_VERBOSE("parseSwitchStmt: parsing default clause");
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
    LUC_LOG_STMT("parseSwitchStmt: " << caseCount << " cases, hasDefault=" << (node->defaultBody != nullptr));
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
    LUC_LOG_STMT_VERBOSE("parseSwitchCase");
    SourceLocation loc = currentLoc();
    consume(TokenType::CASE, "expected 'case'");

    auto sc = std::make_unique<SwitchCaseAST>();
    sc->loc = loc;
    int valueCount = 0;

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
            LUC_LOG_STMT_EXTREME("parseSwitchCase: range value");
        } else {
            sc->values.push_back(std::move(val));
            LUC_LOG_STMT_EXTREME("parseSwitchCase: single value");
        }
        valueCount++;

    } while (check(TokenType::COMMA));

    consume(TokenType::COLON, DiagCode::E2001, "expected ':' after case values");
    LUC_LOG_STMT_VERBOSE("parseSwitchCase: " << valueCount << " values");

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
    LUC_LOG_STMT("parseForStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::FOR, "expected 'for'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected iteration variable name after 'for'");
        return nullptr;
    }
    std::string varName = advance().value;
    LUC_LOG_STMT_VERBOSE("parseForStmt: varName='" << varName << "'");

    // Optional: Parse explicit type annotation if 'in' does not follow immediately.
    TypePtr varType = nullptr;
    if (!check(TokenType::IN)) {
        varType = parseType();
        if (!varType) {
            errorAt(DiagCode::E2005, "expected 'in' or explicit type after iteration variable name");
            return nullptr;
        }
        LUC_LOG_STMT_VERBOSE("parseForStmt: explicit var type");
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
        LUC_LOG_STMT_VERBOSE("parseForStmt: range iteration");
        iterable = parseRangeExpr(std::move(iterable));
        
        // After start..end, an optional second '..' can signal a step.
        if (match(TokenType::RANGE)) {
            step = parseExpr();
            if (!step) {
                errorAt(DiagCode::E2008, "expected step expression after '..'");
                return nullptr;
            }
            LUC_LOG_STMT_VERBOSE("parseForStmt: with step");
        }
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start for loop body");
        return nullptr;
    }

    LUC_LOG_STMT_VERBOSE("parseForStmt: entering loop body (loopDepth=" << loopDepth_ << "->" << loopDepth_ + 1 << ")");
    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;
    LUC_LOG_STMT_VERBOSE("parseForStmt: exited loop body");

    auto node = std::make_unique<ForStmtAST>();
    node->loc = loc;
    node->varName = std::move(varName);
    node->varType = std::move(varType);
    node->iterable = std::move(iterable);
    node->step = std::move(step);
    node->body = std::move(body);
    LUC_LOG_STMT_VERBOSE("parseForStmt: returning ForStmtAST");
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseWhileStmt
//
// Grammar:  while_stmt := 'while' expr block
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<WhileStmtAST> Parser::parseWhileStmt()
{
    LUC_LOG_STMT("parseWhileStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::WHILE, "expected 'while'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while'");
        return nullptr;
    }
    LUC_LOG_STMT_VERBOSE("parseWhileStmt: condition parsed");

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start while loop body");
        return nullptr;
    }

    LUC_LOG_STMT_VERBOSE("parseWhileStmt: entering loop body (loopDepth=" << loopDepth_ << "->" << loopDepth_ + 1 << ")");
    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;
    LUC_LOG_STMT_VERBOSE("parseWhileStmt: exited loop body");

    auto node = std::make_unique<WhileStmtAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->body = std::move(body);
    LUC_LOG_STMT_VERBOSE("parseWhileStmt: returning WhileStmtAST");
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
    LUC_LOG_STMT("parseDoWhileStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::DO, "expected 'do'");

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after 'do'");
        return nullptr;
    }

    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: entering loop body (loopDepth=" << loopDepth_ << "->" << loopDepth_ + 1 << ")");
    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;
    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: exited loop body");

    consume(TokenType::WHILE, "expected 'while' after do body");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while' in do-while loop");
        return nullptr;
    }
    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: condition parsed");

    auto node = std::make_unique<DoWhileStmtAST>();
    node->loc = loc;
    node->body = std::move(body);
    node->condition = std::move(condition);
    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: returning DoWhileStmtAST");
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
    LUC_LOG_STMT("parseReturnStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::RETURN, "expected 'return'");

    if (parallelDepth_ > 0) {
        LUC_LOG_STMT("parseReturnStmt: ERROR - return inside parallel body");
        error(loc, DiagCode::E2006, "'return' is not valid inside a 'parallel' body");
    }

    auto node = std::make_unique<ReturnStmtAST>();
    node->loc = loc;

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
        LUC_LOG_STMT_VERBOSE("parseReturnStmt: has return value");
        node->value = parseExpr();
    } else {
        LUC_LOG_STMT_VERBOSE("parseReturnStmt: void return");
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
    LUC_LOG_STMT("parseBreakStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::BREAK, "expected 'break'");

    if (loopDepth_ == 0) {
        LUC_LOG_STMT("parseBreakStmt: ERROR - break outside loop");
        error(loc, DiagCode::E2006, "'break' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        LUC_LOG_STMT("parseBreakStmt: ERROR - break inside parallel body");
        error(loc, DiagCode::E2006, "'break' is not valid inside a 'parallel' body");
    }

    auto node = std::make_unique<BreakStmtAST>();
    node->loc = loc;
    LUC_LOG_STMT_EXTREME("parseBreakStmt: returning BreakStmtAST");
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseContinueStmt
//
// Grammar:  continue_stmt := 'continue'
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<ContinueStmtAST> Parser::parseContinueStmt()
{
    LUC_LOG_STMT("parseContinueStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::CONTINUE, "expected 'continue'");

    if (loopDepth_ == 0) {
        LUC_LOG_STMT("parseContinueStmt: ERROR - continue outside loop");
        error(loc, DiagCode::E2006, "'continue' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        LUC_LOG_STMT("parseContinueStmt: ERROR - continue inside parallel body");
        error(loc, DiagCode::E2006, "'continue' is not valid inside a 'parallel' body");
    }

    auto node = std::make_unique<ContinueStmtAST>();
    node->loc = loc;
    LUC_LOG_STMT_EXTREME("parseContinueStmt: returning ContinueStmtAST");
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
    LUC_LOG_STMT("parseParallelForStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::PARALLEL, "expected 'parallel'");
    consume(TokenType::FOR, "expected 'for' after 'parallel'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected iteration variable name after 'parallel for'");
        return nullptr;
    }
    std::string varName = advance().value;
    LUC_LOG_STMT_VERBOSE("parseParallelForStmt: varName='" << varName << "'");

    // Optional: Parse explicit type annotation if 'in' does not follow immediately.
    TypePtr varType = nullptr;
    if (!check(TokenType::IN)) {
        varType = parseType();
        if (!varType) {
            errorAt(DiagCode::E2005, "expected 'in' or explicit type after iteration variable name");
            return nullptr;
        }
        LUC_LOG_STMT_VERBOSE("parseParallelForStmt: explicit var type");
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
        LUC_LOG_STMT_VERBOSE("parseParallelForStmt: range iteration");
        iterable = parseRangeExpr(std::move(iterable));
        
        // After start..end, an optional second '..' can signal a step.
        if (match(TokenType::RANGE)) {
            step = parseExpr();
            if (!step) {
                errorAt(DiagCode::E2008, "expected step expression after '..'");
                return nullptr;
            }
            LUC_LOG_STMT_VERBOSE("parseParallelForStmt: with step");
        }
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start parallel for body");
        return nullptr;
    }

    // Inside a parallel body: no await, no return, no break/continue.
    LUC_LOG_STMT_VERBOSE("parseParallelForStmt: entering parallel body (parallelDepth=" << parallelDepth_ << "->" << parallelDepth_ + 1 << ")");
    ++parallelDepth_;
    StmtPtr body = parseBlock();
    --parallelDepth_;
    LUC_LOG_STMT_VERBOSE("parseParallelForStmt: exited parallel body");

    auto node = std::make_unique<ParallelForStmtAST>();
    node->loc = loc;
    node->varName = std::move(varName);
    node->varType = std::move(varType);
    node->iterable = std::move(iterable);
    node->step = std::move(step);
    node->body = std::move(body);
    LUC_LOG_STMT("parseParallelForStmt: returning ParallelForStmtAST");
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
    LUC_LOG_STMT("parseParallelBlockStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::PARALLEL, "expected 'parallel'");
    consume(TokenType::LBRACE, "expected '{' after 'parallel'");

    auto node = std::make_unique<ParallelBlockStmtAST>();
    node->loc = loc;
    int subBlockCount = 0;

    LUC_LOG_STMT_VERBOSE("parseParallelBlockStmt: entering parallel body (parallelDepth=" << parallelDepth_ << "->" << parallelDepth_ + 1 << ")");
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
        subBlockCount++;
        LUC_LOG_STMT_EXTREME("parseParallelBlockStmt: parsed sub-block " << subBlockCount);
    }

    --parallelDepth_;
    LUC_LOG_STMT_VERBOSE("parseParallelBlockStmt: exited parallel body");

    consume(TokenType::RBRACE, "expected '}' to close parallel block");

    if (node->subBlocks.empty()) {
        LUC_LOG_STMT("parseParallelBlockStmt: ERROR - empty parallel block");
        error(loc, DiagCode::E2007, "parallel block must contain at least one sub-task block");
    }

    LUC_LOG_STMT("parseParallelBlockStmt: " << subBlockCount << " sub-blocks");
    return node;
}
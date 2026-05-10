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
ASTPtr<BlockStmtAST> Parser::parseBlock() {
    LUC_LOG_STMT("parseBlock");
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACE, "expected '{'");

    auto block = arena_.make<BlockStmtAST>();
    block->loc = loc;
    int stmtCount = 0;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match(TokenType::SEMICOLON)) continue;

        // Save position for error recovery
        std::size_t savedPos = pos_;
        
        StmtPtr stmt = parseStmt();
        if (stmt) {
            block->stmts.push_back(std::move(stmt));
            stmtCount++;
        } else {
            // If we didn't advance, manually advance to avoid infinite loop
            if (pos_ == savedPos) {
                LUC_LOG_STMT("parseBlock: parser didn't advance, forcing advance");
                advance();
            }
            
            if (!check(TokenType::RBRACE) && !isAtEnd()) {
                synchronize();
            }
        }
    }

    LUC_LOG_STMT_VERBOSE("parseBlock: consuming '}', current token='" << peek().value << "'");
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
    LUC_LOG_STMT_VERBOSE("parseStmt: token='" << peek().value << "'");
    
    // ── Local declarations ────────────────────────────────────────────────────
    if (checkAny({ TokenType::LET, TokenType::CONST })) {
        return parseLocalDecl();
    }

    // ── 'pub' inside a block ──────────────────────────────────────────────────
    if (check(TokenType::PUB)) {
        errorAt(DiagCode::E2006, "'pub' is not valid inside a block");
        advance();
        if (checkAny({ TokenType::LET, TokenType::CONST })) {
            return parseLocalDecl();
        }
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = currentLoc();
        return unknown;
    }

    // ── Control flow ──────────────────────────────────────────────────────────
    if (check(TokenType::IF)) return parseIfStmt();
    if (check(TokenType::SWITCH)) return parseSwitchStmt();
    if (check(TokenType::FOR)) return parseForStmt();
    if (check(TokenType::WHILE)) return parseWhileStmt();
    if (check(TokenType::DO)) return parseDoWhileStmt();
    if (check(TokenType::RETURN)) return parseReturnStmt();
    if (check(TokenType::BREAK)) return parseBreakStmt();
    if (check(TokenType::CONTINUE)) return parseContinueStmt();

    // ── Parallel ──────────────────────────────────────────────────────────────
    if (check(TokenType::PARALLEL)) {
        if (peekNext().type == TokenType::FOR) {
            return parseParallelForStmt();
        } else {
            return parseParallelBlockStmt();
        }
    }

    // ── Expression statement ──────────────────────────────────────────────────
    if (!looksLikeStmtStart()) {
        errorAt(DiagCode::E2002, "unexpected token '" + peek().value + "'");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = currentLoc();
        return unknown;
    }

    SourceLocation loc = currentLoc();
    ExprPtr expr = parseExpr();
    if (!expr) {
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = currentLoc();
        return unknown;
    }

    auto stmt = arena_.make<ExprStmtAST>(std::move(expr));
    stmt->loc = loc;
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
ASTPtr<DeclStmtAST> Parser::parseLocalDecl()
{
    LUC_LOG_STMT("parseLocalDecl");
    SourceLocation loc = currentLoc();
    Token kwTok = advance();
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected name after '" + kwTok.value + "'");
        return nullptr;  // DeclStmtAST can't be unknown - caller handles nullptr
    }

    if (looksLikeFuncDecl()) {
        auto funcDecl = parseFuncDecl(kw, Visibility::Private);
        if (!funcDecl) return nullptr;
        funcDecl->loc = loc;
        auto ds = arena_.make<DeclStmtAST>(std::move(funcDecl));
        ds->loc = loc;
        return ds;
    } else {
        auto varDecl = parseVarDecl(Visibility::Private, {});
        if (!varDecl) return nullptr;
        varDecl->loc = loc;
        auto ds = arena_.make<DeclStmtAST>(std::move(varDecl));
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
ASTPtr<IfStmtAST> Parser::parseIfStmt()
{
    LUC_LOG_STMT("parseIfStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::IF, "expected 'if'");

    // Condition — parsed as a full expression, but disallow top-level struct literals
    // to avoid ambiguity with the following block.
    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        auto unknownExpr = arena_.make<UnknownExprAST>();
        unknownExpr->loc = loc;

        auto unknownStmt = arena_.make<UnknownStmtAST>();
        unknownStmt->loc = loc;  

        auto node = arena_.make<IfStmtAST>();  // ← Create placeholder
        node->loc = loc;
        node->condition = std::move(unknownExpr);
        node->thenBranch = std::move(unknownStmt);
        return node;
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after if condition");

        auto unknownStmt = arena_.make<UnknownStmtAST>();
        unknownStmt->loc = loc;  

        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        node->condition = std::move(condition);
        node->thenBranch = std::move(unknownStmt);
        return node;
    }
    StmtPtr thenBranch = parseBlock();
    LUC_LOG_STMT_VERBOSE("parseIfStmt: then branch parsed");

    auto node = arena_.make<IfStmtAST>();
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
ASTPtr<SwitchStmtAST> Parser::parseSwitchStmt()
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

    auto node = arena_.make<SwitchStmtAST>();
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
SwitchCasePtr Parser::parseSwitchCase() {
    LUC_LOG_STMT_VERBOSE("parseSwitchCase");
    SourceLocation loc = currentLoc();
    consume(TokenType::CASE, "expected 'case'");

    auto sc = arena_.make<SwitchCaseAST>();
    sc->loc = loc;

    // Parse the first value (required unless colon follows immediately)
    if (check(TokenType::COLON)) {
        errorAt(DiagCode::E2001, "expected case value before ':'");
    } else {
        ExprPtr val = parsePrattExpr(0);
        if (val) {
            if (check(TokenType::RANGE)) {
                sc->values.push_back(parseRangeExpr(std::move(val)));
            } else {
                sc->values.push_back(std::move(val));
            }
        }
    }

    // Parse additional comma‑separated values
    while (check(TokenType::COMMA)) {
        advance(); // consume ','
        if (check(TokenType::COLON)) break; // trailing comma allowed

        std::size_t savedPos = pos_;
        ExprPtr val = parsePrattExpr(0);
        if (pos_ == savedPos) {
            // No progress – skip the token and break to avoid infinite loop
            errorAt(DiagCode::E2002, "expected case value after comma");
            advance(); // consume the offending token to recover
            break;
        }
        if (val) {
            if (check(TokenType::RANGE)) {
                sc->values.push_back(parseRangeExpr(std::move(val)));
            } else {
                sc->values.push_back(std::move(val));
            }
        }
    }

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
ASTPtr<ForStmtAST> Parser::parseForStmt()
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

    auto node = arena_.make<ForStmtAST>();
    node->loc = loc;

    // Allocate and initialise iterVar
    node->iterVar = arena_.make<ParamAST>();
    node->iterVar->name = pool_.intern(varName);
    node->iterVar->type = std::move(varType);
    node->iterVar->isVariadic = false;  // loops never use variadic

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
ASTPtr<WhileStmtAST> Parser::parseWhileStmt()
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

    auto node = arena_.make<WhileStmtAST>();
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
ASTPtr<DoWhileStmtAST> Parser::parseDoWhileStmt()
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

    auto node = arena_.make<DoWhileStmtAST>();
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
ASTPtr<ReturnStmtAST> Parser::parseReturnStmt() {
    LUC_LOG_STMT("parseReturnStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::RETURN, "expected 'return'");

    if (parallelDepth_ > 0) {
        LUC_LOG_STMT("parseReturnStmt: ERROR - return inside parallel body");
        error(loc, DiagCode::E2006, "'return' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<ReturnStmtAST>();
    node->loc = loc;

    // CRITICAL: Check if there's a return value
    // Don't consume the '}' that ends the block
    if (!check(TokenType::RBRACE) && !check(TokenType::SEMICOLON) && !isAtEnd()) {
        LUC_LOG_STMT_VERBOSE("parseReturnStmt: parsing return value");
        node->value = parseExpr();
        if (!node->value) {
            errorAt(DiagCode::E2008, "expected expression after 'return'");
        } else {
            LUC_LOG_STMT_VERBOSE("parseReturnStmt: return value parsed, next token='" << peek().value << "'");
        }
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
ASTPtr<BreakStmtAST> Parser::parseBreakStmt()
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

    auto node = arena_.make<BreakStmtAST>();
    node->loc = loc;
    LUC_LOG_STMT_EXTREME("parseBreakStmt: returning BreakStmtAST");
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseContinueStmt
//
// Grammar:  continue_stmt := 'continue'
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<ContinueStmtAST> Parser::parseContinueStmt()
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

    auto node = arena_.make<ContinueStmtAST>();
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
ASTPtr<ParallelForStmtAST> Parser::parseParallelForStmt()
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

    auto node = arena_.make<ParallelForStmtAST>();
    node->loc = loc;

    node->iterVar = arena_.make<ParamAST>();
    node->iterVar->name = pool_.intern(varName);
    node->iterVar->type = std::move(varType);
    node->iterVar->isVariadic = false;

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
ASTPtr<ParallelBlockStmtAST> Parser::parseParallelBlockStmt()
{
    LUC_LOG_STMT("parseParallelBlockStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::PARALLEL, "expected 'parallel'");
    consume(TokenType::LBRACE, "expected '{' after 'parallel'");

    auto node = arena_.make<ParallelBlockStmtAST>();
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
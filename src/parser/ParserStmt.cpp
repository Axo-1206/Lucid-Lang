/**
 * @file ParserStmt.cpp
 * 
 * Parses all statement-oriented grammar rules.
 */

#include "Parser.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <cassert>
#include <algorithm>
#include <iostream>

// -----------------------------------------------------------------------------
// parseStmt - root statement dispatcher
// -----------------------------------------------------------------------------

StmtPtr Parser::parseStmt() {
    // Multi-assignment (reassignment) - safe lookahead
    if (looksLikeMultiAssignStart()) {
        return parseMultiAssignStmt();
    }

    // Multi-variable declaration (let/const with commas)
    if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        size_t savedPos = ts_.getPos();
        ts_.advance(); // consume keyword
        
        bool hasIdentifier = ts_.check(TokenType::IDENTIFIER);
        if (hasIdentifier) ts_.advance();
        
        bool hasType = false;
        if (looksLikeType()) {
            size_t typePos = ts_.getPos();
            // Need a way to check if type is valid without consuming - simplified
            hasType = true;
            if (hasIdentifier && hasType && ts_.check(TokenType::COMMA)) {
                ts_.setPos(savedPos); // Would need setter - simplified approach
                return parseMultiVarDecl();
            }
        }
        // Restore position and fall through to single declaration
        ts_.setPos(savedPos);
    }

    // Local declarations
    if (ts_.checkAny({TokenType::TYPE, TokenType::STRUCT, TokenType::ENUM,
                      TokenType::IMPL, TokenType::TRAIT, TokenType::FROM,
                      TokenType::LET, TokenType::CONST, TokenType::AT_SIGN,
                      TokenType::USE})) {
        DeclPtr decl = parseDeclaration(DeclContext::Local);
        if (!decl) return nullptr;
        
        auto ds = arena_.make<DeclStmtAST>(std::move(decl));
        ds->loc = ts_.currentLoc();
        return ds;
    }

    // 'pub' inside a block - error
    if (ts_.check(TokenType::PUB)) {
        errorAt(DiagCode::E2014, "'pub' is not valid inside a block");
        ts_.advance();
        if (ts_.checkAny({TokenType::LET, TokenType::CONST, TokenType::TYPE,
                          TokenType::STRUCT, TokenType::ENUM, TokenType::IMPL,
                          TokenType::FROM})) {
            DeclPtr decl = parseDeclaration(DeclContext::Local);
            if (decl) {
                auto ds = arena_.make<DeclStmtAST>(std::move(decl));
                ds->loc = ts_.currentLoc();
                return ds;
            }
        }
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        return unknown;
    }

    // Control flow keywords
    if (ts_.check(TokenType::IF))       return parseIfStmt();
    if (ts_.check(TokenType::SWITCH))   return parseSwitchStmt();
    if (ts_.check(TokenType::FOR))      return parseForStmt();
    if (ts_.check(TokenType::WHILE))    return parseWhileStmt();
    if (ts_.check(TokenType::DO))       return parseDoWhileStmt();
    if (ts_.check(TokenType::RETURN))   return parseReturnStmt();
    if (ts_.check(TokenType::BREAK))    return parseBreakStmt();
    if (ts_.check(TokenType::CONTINUE)) return parseContinueStmt();

    // Detect invalid variable declaration missing let/const
    if (ts_.check(TokenType::IDENTIFIER)) {
        size_t savedPos = ts_.getPos();
        ts_.advance();
        if (looksLikeType() && ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2002, "variable declaration requires 'let' or 'const'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::SEMICOLON)) ts_.advance();
            auto unknown = arena_.make<UnknownStmtAST>();
            unknown->loc = ts_.currentLoc();
            return unknown;
        }
        ts_.setPos(savedPos);
    }

    // Expression statement
    if (!looksLikeStmtStart()) {
        errorAt(DiagCode::E2002, "unexpected token '" + ts_.peek().value + "'");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        if (!ts_.isAtEnd()) ts_.advance();
        return unknown;
    }

    SourceLocation loc = ts_.currentLoc();
    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E2008, "expected expression statement");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
            ts_.advance();
        }
        if (ts_.check(TokenType::SEMICOLON)) ts_.advance();
        return unknown;
    }

    auto stmt = arena_.make<ExprStmtAST>(std::move(expr));
    stmt->loc = loc;
    return stmt;
}

// -----------------------------------------------------------------------------
// parseMultiVarDecl
// -----------------------------------------------------------------------------

ASTPtr<MultiVarDeclAST> Parser::parseMultiVarDecl(std::vector<AttributePtr> attrs) {

    if (!attrs.empty()) {
        error(attrs[0]->loc, DiagCode::E2027, "attributes cannot be used on multi-variable declarations");
    }

    SourceLocation loc = ts_.currentLoc();

    if (!ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        errorAt(DiagCode::E2002, "expected 'let' or 'const'");
        return nullptr;
    }
    Token kwTok = ts_.advance();
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    std::vector<std::pair<InternedString, TypePtr>> vars;

    // Parse first variable
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected variable name");
        return nullptr;
    }
    std::string firstName = ts_.advance().value;
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + firstName + "'");
        return nullptr;
    }
    TypePtr firstType = parseType();
    if (!firstType) return nullptr;
    vars.emplace_back(pool_.intern(firstName), std::move(firstType));

    // Parse additional variables
    while (ts_.check(TokenType::COMMA)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected variable name after comma");
            break;
        }
        std::string name = ts_.advance().value;
        if (!looksLikeType()) {
            errorAt(DiagCode::E2005, "expected type annotation for '" + name + "'");
            break;
        }
        TypePtr type = parseType();
        if (!type) break;
        vars.emplace_back(pool_.intern(name), std::move(type));
    }

    ts_.consume(TokenType::ASSIGN, DiagCode::E2001, "expected '=' in multi-assignment");
    ExprPtr rhs = parseExpr();
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after '='");
        return nullptr;
    }

    auto node = arena_.make<MultiVarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    
    // Build vars span
    auto builder = arena_.makeBuilder<std::pair<InternedString, TypePtr>>();
    for (auto& v : vars) builder.push_back(std::move(v));
    node->vars = builder.build();
    
    node->rhs = std::move(rhs);
    
    return node;
}

// -----------------------------------------------------------------------------
// parseMultiAssignStmt
// -----------------------------------------------------------------------------

ASTPtr<MultiAssignStmtAST> Parser::parseMultiAssignStmt() {
    SourceLocation loc = ts_.currentLoc();
    std::vector<ExprPtr> lhs;

    ExprPtr first = parseLvalue();
    if (!first) {
        errorAt(DiagCode::E2008, "expected left-hand side expression");
        return nullptr;
    }
    lhs.push_back(std::move(first));

    while (ts_.check(TokenType::COMMA)) {
        ts_.advance();
        ExprPtr next = parseLvalue();
        if (!next) {
            errorAt(DiagCode::E2008, "expected left-hand side expression after comma");
            break;
        }
        lhs.push_back(std::move(next));
    }

    if (!ts_.check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' in multiple assignment");
        while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
            ts_.advance();
        }
        return nullptr;
    }
    ts_.advance();

    ExprPtr rhs = parseExpr(true);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after '='");
        return nullptr;
    }

    auto node = arena_.make<MultiAssignStmtAST>();
    node->loc = loc;
    
    // Build lhs span
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : lhs) builder.push_back(std::move(e));
    node->lhs = builder.build();
    
    node->rhs = std::move(rhs);
    return node;
}

// -----------------------------------------------------------------------------
// parseBlock
// -----------------------------------------------------------------------------

ASTPtr<BlockStmtAST> Parser::parseBlock() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{'");

    std::vector<StmtPtr> stmts;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        if (ts_.match(TokenType::SEMICOLON)) continue;

        size_t savedPos = ts_.getPos();
        StmtPtr stmt = parseStmt();

        if (ts_.getPos() == savedPos) {
            if (!ts_.isAtEnd()) ts_.advance();
            while (ts_.match(TokenType::SEMICOLON)) {}
            continue;
        }

        if (stmt) stmts.push_back(std::move(stmt));
    }

    ts_.consume(TokenType::RBRACE, "expected '}' to close block");
    
    auto block = arena_.make<BlockStmtAST>();
    block->loc = loc;
    
    auto builder = arena_.makeBuilder<StmtPtr>();
    for (auto& s : stmts) builder.push_back(std::move(s));
    block->stmts = builder.build();
    
    return block;
}

// -----------------------------------------------------------------------------
// parseIfStmt
// -----------------------------------------------------------------------------

ASTPtr<IfStmtAST> Parser::parseIfStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IF, "expected 'if'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        return node;
    }

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after if condition");
        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        node->condition = std::move(condition);
        return node;
    }
    StmtPtr thenBranch = parseBlock();

    auto node = arena_.make<IfStmtAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->thenBranch = std::move(thenBranch);

    if (ts_.match(TokenType::ELSE)) {
        if (ts_.check(TokenType::IF)) {
            node->elseBranch = parseIfStmt();
        } else if (ts_.check(TokenType::LBRACE)) {
            node->elseBranch = parseBlock();
        } else {
            errorAt(DiagCode::E2001, "expected 'if' or '{' after 'else'");
        }
    }

    return node;
}

// -----------------------------------------------------------------------------
// parseSwitchStmt
// -----------------------------------------------------------------------------

ASTPtr<SwitchStmtAST> Parser::parseSwitchStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::SWITCH, "expected 'switch'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'switch'");
        return nullptr;
    }

    ts_.consume(TokenType::LBRACE, "expected '{' after switch subject");

    auto node = arena_.make<SwitchStmtAST>();
    node->loc = loc;
    node->subject = std::move(subject);

    std::vector<SwitchCasePtr> cases;
    bool hasDefault = false;

    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        if (ts_.check(TokenType::CASE)) {
            SwitchCasePtr sc = parseSwitchCase();
            if (sc) cases.push_back(std::move(sc));
            continue;
        }

        if (ts_.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                errorAt(DiagCode::E2007, "duplicate 'default' clause");
            }
            node->defaultLoc = ts_.currentLoc();
            ts_.advance();
            ts_.consume(TokenType::COLON, "expected ':' after 'default'");
            if (!ts_.check(TokenType::LBRACE)) {
                errorAt(DiagCode::E2001, "expected '{' to start default body");
            } else {
                node->defaultBody = parseBlock();
            }
            hasDefault = true;
            continue;
        }

        errorAt(DiagCode::E2002, "expected 'case' or 'default' inside switch");
        ts_.advance();
    }

    auto builder = arena_.makeBuilder<SwitchCasePtr>();
    for (auto& c : cases) builder.push_back(std::move(c));
    node->cases = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close switch");
    return node;
}

// -----------------------------------------------------------------------------
// parseSwitchCase
// -----------------------------------------------------------------------------

SwitchCasePtr Parser::parseSwitchCase() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::CASE, "expected 'case'");

    auto sc = arena_.make<SwitchCaseAST>();
    sc->loc = loc;

    std::vector<ExprPtr> values;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    if (ts_.check(TokenType::COLON)) {
        errorAt(DiagCode::E2001, "expected case value before ':'");
    } else {
        size_t savedPos = ts_.getPos();
        ExprPtr val = parsePrattExpr(0, false);
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2002, "expected case value");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
        } else if (val && !val->isa<UnknownExprAST>()) {
            if (ts_.check(TokenType::RANGE)) {
                values.push_back(parseRangeExpr(std::move(val)));
            } else {
                values.push_back(std::move(val));
            }
            consecutiveErrors = 0;
        }
    }

    while (ts_.check(TokenType::COMMA) && consecutiveErrors < MAX_CONSECUTIVE_ERRORS) {
        ts_.advance();
        if (ts_.check(TokenType::COLON)) break;

        size_t savedPos = ts_.getPos();
        ExprPtr val = parsePrattExpr(0, false);
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2002, "expected case value after comma");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        if (val && !val->isa<UnknownExprAST>()) {
            if (ts_.check(TokenType::RANGE)) {
                values.push_back(parseRangeExpr(std::move(val)));
            } else {
                values.push_back(std::move(val));
            }
        }
    }

    if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
        errorAt(DiagCode::E2002, "too many errors in case values; skipping to ':'");
        while (!ts_.isAtEnd() && !ts_.check(TokenType::COLON)) ts_.advance();
    }

    ts_.consume(TokenType::COLON, "expected ':' after case values");

    // Build values span
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& v : values) builder.push_back(std::move(v));
    sc->values = builder.build();

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start case body");
    } else {
        sc->body = parseBlock();
    }

    return sc;
}

// -----------------------------------------------------------------------------
// parseForStmt
// -----------------------------------------------------------------------------

ASTPtr<ForStmtAST> Parser::parseForStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FOR, "expected 'for'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected iteration variable name");
        return nullptr;
    }
    std::string varName = ts_.advance().value;

    TypePtr varType = nullptr;
    if (!ts_.check(TokenType::IN)) {
        varType = parseType();
        if (!varType) {
            errorAt(DiagCode::E2005, "expected 'in' or explicit type");
            return nullptr;
        }
    }

    ts_.consume(TokenType::IN, "expected 'in'");

    ExprPtr iterable = parseExpr(false);
    if (!iterable) {
        errorAt(DiagCode::E2008, "expected iterable expression after 'in'");
        return nullptr;
    }

    ExprPtr step = nullptr;
    if (ts_.check(TokenType::RANGE)) {
        iterable = parseRangeExpr(std::move(iterable));
        if (ts_.match(TokenType::RANGE)) {
            step = parseExpr();
            if (!step) {
                errorAt(DiagCode::E2008, "expected step expression after '..'");
                return nullptr;
            }
        }
    }

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start for loop body");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    auto node = arena_.make<ForStmtAST>();
    node->loc = loc;
    
    node->iterVar = arena_.make<ParamAST>();
    node->iterVar->name = pool_.intern(varName);
    node->iterVar->type = std::move(varType);
    node->iterVar->isVariadic = false;
    
    node->iterable = std::move(iterable);
    node->step = std::move(step);
    node->body = std::move(body);
    
    return node;
}

// -----------------------------------------------------------------------------
// parseWhileStmt
// -----------------------------------------------------------------------------

ASTPtr<WhileStmtAST> Parser::parseWhileStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::WHILE, "expected 'while'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while'");
        return nullptr;
    }

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start while loop body");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    auto node = arena_.make<WhileStmtAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->body = std::move(body);
    
    return node;
}

// -----------------------------------------------------------------------------
// parseDoWhileStmt
// -----------------------------------------------------------------------------

ASTPtr<DoWhileStmtAST> Parser::parseDoWhileStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::DO, "expected 'do'");

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after 'do'");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    ts_.consume(TokenType::WHILE, "expected 'while' after do body");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while'");
        return nullptr;
    }

    auto node = arena_.make<DoWhileStmtAST>();
    node->loc = loc;
    node->body = std::move(body);
    node->condition = std::move(condition);
    
    return node;
}

// -----------------------------------------------------------------------------
// parseReturnStmt
// -----------------------------------------------------------------------------

ASTPtr<ReturnStmtAST> Parser::parseReturnStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::RETURN, "expected 'return'");

    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E3029, "'return' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<ReturnStmtAST>();
    node->loc = loc;

    if (!ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::SEMICOLON) && !ts_.isAtEnd()) {
        std::vector<ExprPtr> values;
        bool first = true;
        
        while (!ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::SEMICOLON) && !ts_.isAtEnd()) {
            if (!first) {
                if (!ts_.match(TokenType::COMMA)) break;
                if (ts_.check(TokenType::COMMA)) {
                    errorAt(DiagCode::E2008, "empty expression in return list");
                    ts_.advance();
                    continue;
                }
                if (ts_.check(TokenType::RBRACE) || ts_.check(TokenType::SEMICOLON) || ts_.isAtEnd()) {
                    errorAt(DiagCode::E2001, "trailing comma in return list");
                    break;
                }
            }
            first = false;

            size_t savedPos = ts_.getPos();
            ExprPtr expr = parseExpr();
            if (ts_.getPos() == savedPos) {
                errorAt(DiagCode::E2008, "expected expression after 'return'");
                if (!ts_.isAtEnd()) ts_.advance();
                break;
            }
            values.push_back(std::move(expr));
        }
        
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& v : values) builder.push_back(std::move(v));
        node->values = builder.build();
    }

    return node;
}

// -----------------------------------------------------------------------------
// parseBreakStmt
// -----------------------------------------------------------------------------

ASTPtr<BreakStmtAST> Parser::parseBreakStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::BREAK, "expected 'break'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E3031, "'break' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'break' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<BreakStmtAST>();
    node->loc = loc;
    return node;
}

// -----------------------------------------------------------------------------
// parseContinueStmt
// -----------------------------------------------------------------------------

ASTPtr<ContinueStmtAST> Parser::parseContinueStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::CONTINUE, "expected 'continue'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E3031, "'continue' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'continue' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<ContinueStmtAST>();
    node->loc = loc;
    return node;
}
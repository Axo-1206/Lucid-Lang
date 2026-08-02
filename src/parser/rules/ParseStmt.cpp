/**
 * @file ParseStmt.cpp
 * @brief Implementation of statement parsers.
 * 
 * This file implements all statement parsing functions:
 * - Block, If, Switch, For, While, Do-While
 * - Return, Break, Continue
 * - Expression and Declaration statements
 * - Async, Await, Spawn, Join (concurrency)
 * 
 * @design_decision Parser builds AST only, no semantic analysis
 *   Statement AST nodes are pure syntax trees. The semantic phase
 *   handles type checking, control flow analysis, and narrowing.
 */

#include "core/Tokens.hpp"
#include "../Parser.hpp"
#include "../context/ParserContext.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "debug/DebugMacros.hpp"

#include <vector>
#include <optional>

namespace parser {

// ─── Forward declarations for internal helpers ─────────────────────────────

namespace {

/**
 * @brief Check if the current token is a statement terminator.
 */
bool isStatementTerminator(TokenStream& stream) {
    TokenType type = stream.peek().type;
    return type == TokenType::SEMICOLON || 
           type == TokenType::RBRACE ||
           type == TokenType::EOF_TOKEN;
}

} // anonymous namespace

// =============================================================================
// parseStmt – Top-Level Statement Entry Point
// =============================================================================

StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseStmt: parsing statement");
    
    if (stream.isAtEnd() || !ctx.canContinue()) {
        return nullptr;
    }
    
    if (isStatementTerminator(stream)) {
        stream.consume();
        return nullptr;
    }
    
    Token current = stream.peek();
    SourceLocation loc = stream.currentLoc();
    
    switch (current.type) {
        // Control Flow
        case TokenType::IF:      return parseIfStmt(stream, ctx);
        case TokenType::SWITCH:  return parseSwitchStmt(stream, ctx);
        case TokenType::FOR:     return parseForStmt(stream, ctx);
        case TokenType::WHILE:   return parseWhileStmt(stream, ctx);
        case TokenType::DO:      return parseDoWhileStmt(stream, ctx);
            
        // Jumps
        case TokenType::RETURN:  return parseReturnStmt(stream, ctx);
        case TokenType::BREAK:   return parseBreakStmt(stream, ctx);
        case TokenType::CONTINUE: return parseContinueStmt(stream, ctx);
            
        // Declarations
        case TokenType::LET:
        case TokenType::CONST:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
            return parseDeclStmt(stream, ctx);

        case TokenType::IMPORT:
            ctx.diagnostics.errorAt(DiagCode::Syntax_InvalidAttributeTarget, loc,
                                    "import statement is only valid at top level");
            synchronizeToContext(stream, ctx);
            return nullptr;

        // Concurrency
        case TokenType::ASYNC:   return parseAsyncStmt(stream, ctx);
        case TokenType::AWAIT:   return parseAwaitStmt(stream, ctx);
        case TokenType::SPAWN:   return parseSpawnStmt(stream, ctx);
        case TokenType::JOIN:    return parseJoinStmt(stream, ctx);
            
        // Expression Statement (default)
        default:
            return parseExprStmt(stream, ctx);
    }
}

// =============================================================================
// parseBlock – Parses a brace-delimited block
// =============================================================================

BlockStmtAST* parseBlock(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseBlock: parsing block");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, loc,
                                "expected '{', got '", stream.peekValue(), "'");
        return ctx.arena.make<BlockStmtAST>();
    }
    
    BlockStmtAST* block = ctx.arena.make<BlockStmtAST>();
    auto builder = ctx.arena.makeBuilder<StmtPtr>();
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Skip stray semicolons
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected duplicate semicolon in block");
            stream.consume();
        }
        
        if (isStatementTerminator(stream) || !ctx.canContinue()) {
            break;
        }
        
        StmtPtr stmt = parseStmt(stream, ctx);
        if (stmt) {
            builder.push_back(stmt);
        } else {
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                break;
            }
        }
        
        if (!ctx.canContinue()) {
            break;
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '}' to close block");
    } else {
        stream.consume(); // Consume '}'
    }
    
    block->stmts = builder.build();
    block->loc = loc;
    
    LOG_PARSER("parseBlock: parsed block with ", block->stmts.size(), " statements");
    return block;
}

// =============================================================================
// parseIfStmt – Parses if/else statement
// =============================================================================

IfStmtAST* parseIfStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseIfStmt: parsing if statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::IF)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'if', got '", stream.peekValue(), "'");
        return ctx.arena.make<IfStmtAST>();
    }
    
    IfStmtAST* ifStmt = ctx.arena.make<IfStmtAST>();
    ifStmt->loc = loc;
    
    ExprPtr condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected if condition");
        synchronizeToContext(stream, ctx);
        if (!ctx.canContinue()) {
            return ifStmt;
        }
    }
    ifStmt->condition = condition;
    
    StmtPtr thenBranch = parseBlock(stream, ctx);
    if (!thenBranch) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected then branch block");
        synchronizeToContext(stream, ctx);
        return ifStmt;
    }
    ifStmt->thenBranch = thenBranch;
    
    if (stream.match(TokenType::ELSE)) {
        if (stream.check(TokenType::IF)) {
            StmtPtr elseBranch = parseIfStmt(stream, ctx);
            if (elseBranch) {
                ifStmt->elseBranch = elseBranch;
            } else {
                synchronizeToContext(stream, ctx);
            }
        } else {
            StmtPtr elseBranch = parseBlock(stream, ctx);
            if (elseBranch) {
                ifStmt->elseBranch = elseBranch;
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected else branch block");
                synchronizeToContext(stream, ctx);
            }
        }
    }
    
    LOG_PARSER("parseIfStmt: parsed if statement");
    return ifStmt;
}

// =============================================================================
// parseSwitchStmt – Parses switch statement
// =============================================================================

SwitchStmtAST* parseSwitchStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseSwitchStmt: parsing switch statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::SWITCH)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'switch', got '", stream.peekValue(), "'");
        return ctx.arena.make<SwitchStmtAST>();
    }
    
    SwitchStmtAST* switchStmt = ctx.arena.make<SwitchStmtAST>();
    switchStmt->loc = loc;
    
    ExprPtr subject = parseExpr(stream, ctx);
    if (!subject) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected switch subject");
        synchronizeToContext(stream, ctx);
        return switchStmt;
    }
    switchStmt->subject = subject;
    
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return switchStmt;
    }
    
    auto caseBuilder = ctx.arena.makeBuilder<SwitchCasePtr>();
    bool hasDefault = false;
    SourceLocation defaultLoc;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        if (stream.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_MultipleDefaults, stream.currentLoc(),
                                        "multiple default clauses in switch");
                stream.consume();
                continue;
            }
            hasDefault = true;
            defaultLoc = stream.currentLoc();
            stream.consume(); // Consume 'default'
            
            if (!stream.match(TokenType::COLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ':', got '", stream.peekValue(), "'");
                synchronizeTo(stream, ctx, TokenType::RBRACE);
                break;
            }
            
            BlockStmtAST* defaultBody = parseBlock(stream, ctx);
            if (defaultBody) {
                switchStmt->defaultBody = defaultBody;
                switchStmt->defaultLoc = defaultLoc;
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected default body block");
                synchronizeTo(stream, ctx, TokenType::RBRACE);
                break;
            }
        } else if (stream.check(TokenType::CASE)) {
            SwitchCasePtr switchCase = parseSwitchCase(stream, ctx);
            if (switchCase) {
                caseBuilder.push_back(switchCase);
            } else {
                synchronizeTo(stream, ctx, TokenType::RBRACE);
                break;
            }
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected 'case' or 'default', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::RBRACE);
            break;
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '}' to close switch body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    switchStmt->cases = caseBuilder.build();
    
    LOG_PARSER("parseSwitchStmt: parsed switch with ", switchStmt->cases.size(), " cases");
    return switchStmt;
}

// =============================================================================
// parseSwitchCase – Parses a single case clause inside a switch
// =============================================================================

SwitchCaseAST* parseSwitchCase(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseSwitchCase: parsing case");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::CASE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'case', got '", stream.peekValue(), "'");
        return ctx.arena.make<SwitchCaseAST>();
    }
    
    SwitchCaseAST* switchCase = ctx.arena.make<SwitchCaseAST>();
    switchCase->loc = loc;
    auto valueBuilder = ctx.arena.makeBuilder<ExprPtr>();
    
    do {
        ExprPtr value = parseExpr(stream, ctx);
        if (value) {
            valueBuilder.push_back(value);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected case value");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::COLON);
            break;
        }
    } while (stream.match(TokenType::COMMA));
    
    if (!stream.match(TokenType::COLON)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ':', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
        return switchCase;
    }
    
    BlockStmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected case body block");
        synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
        return switchCase;
    }
    switchCase->body = body;
    switchCase->values = valueBuilder.build();
    
    LOG_PARSER_DETAIL("parseSwitchCase: parsed case with ", switchCase->values.size(), " values");
    return switchCase;
}

// =============================================================================
// parseForStmt – Parses for loop (range or collection iteration)
// =============================================================================

ForStmtAST* parseForStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseForStmt: parsing for loop");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::FOR)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'for', got '", stream.peekValue(), "'");
        return ctx.arena.make<ForStmtAST>();
    }
    
    ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
    forStmt->loc = loc;
    
    // Parse index binding
    ParamAST* indexParam = nullptr;
    
    if (stream.check(TokenType::UNDERSCORE)) {
        stream.consume(); // Consume '_'
        indexParam = nullptr;
    } else if (stream.check(TokenType::IDENTIFIER)) {
        indexParam = ctx.arena.make<ParamAST>();
        Token nameTok = stream.consume();
        indexParam->name = ctx.pool.intern(nameTok.value);
        indexParam->loc = stream.currentLoc();
        
        TypePtr type = parseType(stream, ctx);
        if (type) {
            indexParam->type = type;
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected index variable type");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::IN);
        }
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected index variable name or '_', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::IN);
    }
    forStmt->indexVar = indexParam;
    
    bool hasValueBinding = stream.match(TokenType::COMMA);
    
    if (hasValueBinding) {
        // Parse value binding (collection iteration)
        ParamAST* valueParam = nullptr;
        
        if (stream.check(TokenType::UNDERSCORE)) {
            stream.consume(); // Consume '_'
            valueParam = nullptr;
        } else if (stream.check(TokenType::IDENTIFIER)) {
            valueParam = ctx.arena.make<ParamAST>();
            Token nameTok = stream.consume();
            valueParam->name = ctx.pool.intern(nameTok.value);
            valueParam->loc = stream.currentLoc();
            
            TypePtr type = parseType(stream, ctx);
            if (type) {
                valueParam->type = type;
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                        "expected value variable type");
                synchronizeTo(stream, ctx, TokenType::IN);
            }
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected value variable name or '_', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::IN);
        }
        forStmt->valueVar = valueParam;
    } else {
        forStmt->valueVar = nullptr;
    }
    
    if (!stream.match(TokenType::IN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'in', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return forStmt;
    }
    
    ExprPtr iterable = parseExpr(stream, ctx);
    if (!iterable) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected iterable expression");
        synchronizeToContext(stream, ctx);
        return forStmt;
    }
    forStmt->iterable = iterable;
    
    // Parse optional step (ONLY for range loops)
    if (stream.match(TokenType::RANGE)) {
        ExprPtr step = parseExpr(stream, ctx);
        if (step) {
            forStmt->step = step;
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected step expression");
            synchronizeToContext(stream, ctx);
        }
    } else {
        forStmt->step = nullptr;
    }
    
    StmtPtr body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected loop body block");
        synchronizeToContext(stream, ctx);
        return forStmt;
    }
    forStmt->body = body;
    
    LOG_PARSER("parseForStmt: parsed for loop");
    return forStmt;
}

// =============================================================================
// parseWhileStmt – Parses while loop
// =============================================================================

WhileStmtAST* parseWhileStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseWhileStmt: parsing while loop");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::WHILE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'while', got '", stream.peekValue(), "'");
        return ctx.arena.make<WhileStmtAST>();
    }
    
    WhileStmtAST* whileStmt = ctx.arena.make<WhileStmtAST>();
    whileStmt->loc = loc;
    
    ExprPtr condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected while condition");
        synchronizeToContext(stream, ctx);
        return whileStmt;
    }
    whileStmt->condition = condition;
    
    StmtPtr body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected loop body block");
        synchronizeToContext(stream, ctx);
        return whileStmt;
    }
    whileStmt->body = body;
    
    LOG_PARSER("parseWhileStmt: parsed while loop");
    return whileStmt;
}

// =============================================================================
// parseDoWhileStmt – Parses do-while loop
// =============================================================================

DoWhileStmtAST* parseDoWhileStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseDoWhileStmt: parsing do-while loop");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::DO)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'do', got '", stream.peekValue(), "'");
        return ctx.arena.make<DoWhileStmtAST>();
    }
    
    DoWhileStmtAST* doWhileStmt = ctx.arena.make<DoWhileStmtAST>();
    doWhileStmt->loc = loc;
    
    StmtPtr body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected loop body block");
        synchronizeToContext(stream, ctx);
        return doWhileStmt;
    }
    doWhileStmt->body = body;
    
    if (!stream.match(TokenType::WHILE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'while', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return doWhileStmt;
    }
    
    ExprPtr condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected do-while condition");
        synchronizeToContext(stream, ctx);
        return doWhileStmt;
    }
    doWhileStmt->condition = condition;
    
    LOG_PARSER("parseDoWhileStmt: parsed do-while loop");
    return doWhileStmt;
}

// =============================================================================
// parseReturnStmt – Parses return statement
// =============================================================================

ReturnStmtAST* parseReturnStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseReturnStmt: parsing return statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::RETURN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'return', got '", stream.peekValue(), "'");
        return ctx.arena.make<ReturnStmtAST>();
    }
    
    ReturnStmtAST* returnStmt = ctx.arena.make<ReturnStmtAST>();
    returnStmt->loc = loc;
    
    ExprPtr value = parseExpr(stream, ctx);
    if (!value) {
        // Bare return (no value) - valid for void functions
        returnStmt->value = nullptr;
    } else {
        returnStmt->value = value;
    }
    
    LOG_PARSER("parseReturnStmt: parsed return statement");
    return returnStmt;
}

// =============================================================================
// parseBreakStmt – Parses break statement
// =============================================================================

BreakStmtAST* parseBreakStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseBreakStmt: parsing break statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::BREAK)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'break', got '", stream.peekValue(), "'");
        return ctx.arena.make<BreakStmtAST>();
    }
    
    BreakStmtAST* breakStmt = ctx.arena.make<BreakStmtAST>();
    breakStmt->loc = loc;
    
    LOG_PARSER("parseBreakStmt: parsed break statement");
    return breakStmt;
}

// =============================================================================
// parseContinueStmt – Parses continue statement
// =============================================================================

ContinueStmtAST* parseContinueStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseContinueStmt: parsing continue statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::CONTINUE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'continue', got '", stream.peekValue(), "'");
        return ctx.arena.make<ContinueStmtAST>();
    }
    
    ContinueStmtAST* continueStmt = ctx.arena.make<ContinueStmtAST>();
    continueStmt->loc = loc;
    
    LOG_PARSER("parseContinueStmt: parsed continue statement");
    return continueStmt;
}

// =============================================================================
// parseExprStmt – Parses an expression used as a statement
// =============================================================================

ExprStmtAST* parseExprStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseExprStmt: parsing expression statement");
    
    SourceLocation loc = stream.currentLoc();
    
    ExprPtr expr = parseExpr(stream, ctx);
    if (!expr) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected expression statement");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<ExprStmtAST>(nullptr);
    }
    
    ExprStmtAST* exprStmt = ctx.arena.make<ExprStmtAST>(expr);
    exprStmt->loc = loc;
    
    LOG_PARSER_DETAIL("parseExprStmt: parsed expression statement");
    return exprStmt;
}

// =============================================================================
// parseDeclStmt – Parses a declaration statement
// =============================================================================

DeclStmtAST* parseDeclStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseDeclStmt: parsing declaration statement");
    
    SourceLocation loc = stream.currentLoc();

    DeclPtr decl = parseDecl(stream, ctx);
    if (!decl) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected declaration");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<DeclStmtAST>(nullptr);
    }
    
    DeclStmtAST* declStmt = ctx.arena.make<DeclStmtAST>(decl);
    declStmt->loc = loc;
    
    LOG_PARSER_DETAIL("parseDeclStmt: parsed declaration statement");
    return declStmt;
}

// =============================================================================
// Concurrency Statements
// =============================================================================

AsyncStmtAST* parseAsyncStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseAsyncStmt: parsing async statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::ASYNC)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'async', got '", stream.peekValue(), "'");
        return ctx.arena.make<AsyncStmtAST>();
    }
    
    AsyncStmtAST* asyncStmt = ctx.arena.make<AsyncStmtAST>();
    asyncStmt->loc = loc;
    
    // Parse target variable
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return asyncStmt;
    }
    Token targetTok = stream.consume();
    auto* idExpr = ctx.arena.make<IdentifierExprAST>();
    idExpr->name = ctx.pool.intern(targetTok.value);
    asyncStmt->target = idExpr;
    
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return asyncStmt;
    }
    
    ExprPtr call = parseExpr(stream, ctx);
    if (!call) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected async call expression");
        synchronizeToContext(stream, ctx);
        return asyncStmt;
    }
    asyncStmt->call = call;
    
    LOG_PARSER("parseAsyncStmt: parsed async statement");
    return asyncStmt;
}

AwaitStmtAST* parseAwaitStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseAwaitStmt: parsing await statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::AWAIT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'await', got '", stream.peekValue(), "'");
        return ctx.arena.make<AwaitStmtAST>();
    }
    
    AwaitStmtAST* awaitStmt = ctx.arena.make<AwaitStmtAST>();
    awaitStmt->loc = loc;
    
    auto targetBuilder = ctx.arena.makeBuilder<ExprPtr>();
    
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected variable name, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA);
            break;
        }
        Token targetTok = stream.consume();
        auto* idExpr = ctx.arena.make<IdentifierExprAST>();
        idExpr->name = ctx.pool.intern(targetTok.value);
        targetBuilder.push_back(idExpr);
    } while (stream.match(TokenType::COMMA));
    
    awaitStmt->targets = targetBuilder.build();
    
    LOG_PARSER("parseAwaitStmt: parsed await statement");
    return awaitStmt;
}

SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseSpawnStmt: parsing spawn statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::SPAWN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'spawn', got '", stream.peekValue(), "'");
        return ctx.arena.make<SpawnStmtAST>();
    }
    
    SpawnStmtAST* spawnStmt = ctx.arena.make<SpawnStmtAST>();
    spawnStmt->loc = loc;
    
    // Parse target variable (or '_' for discard)
    if (stream.check(TokenType::UNDERSCORE)) {
        stream.consume(); // Consume '_'
        spawnStmt->target = nullptr; // nullptr means discard
    } else if (stream.check(TokenType::IDENTIFIER)) {
        Token targetTok = stream.consume();
        auto* idExpr = ctx.arena.make<IdentifierExprAST>();
        idExpr->name = ctx.pool.intern(targetTok.value);
        spawnStmt->target = idExpr;
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name or '_', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return spawnStmt;
    }
    
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return spawnStmt;
    }
    
    ExprPtr call = parseExpr(stream, ctx);
    if (!call) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected spawn call expression");
        synchronizeToContext(stream, ctx);
        return spawnStmt;
    }
    spawnStmt->call = call;
    
    LOG_PARSER("parseSpawnStmt: parsed spawn statement");
    return spawnStmt;
}

JoinStmtAST* parseJoinStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseJoinStmt: parsing join statement");
    
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::JOIN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'join', got '", stream.peekValue(), "'");
        return ctx.arena.make<JoinStmtAST>();
    }
    
    JoinStmtAST* joinStmt = ctx.arena.make<JoinStmtAST>();
    joinStmt->loc = loc;
    
    auto targetBuilder = ctx.arena.makeBuilder<ExprPtr>();
    
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected variable name, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA);
            break;
        }
        Token targetTok = stream.consume();
        auto* idExpr = ctx.arena.make<IdentifierExprAST>();
        idExpr->name = ctx.pool.intern(targetTok.value);
        targetBuilder.push_back(idExpr);
    } while (stream.match(TokenType::COMMA));
    
    joinStmt->targets = targetBuilder.build();
    
    LOG_PARSER("parseJoinStmt: parsed join statement");
    return joinStmt;
}

} // namespace parser
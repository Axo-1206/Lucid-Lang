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
#include "parser/Parser.hpp"

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
    StmtAST* result = nullptr;
    
    switch (current.type) {
        // Control Flow - these DO NOT take semicolon
        case TokenType::IF:     
            result = parseIfStmt(stream, ctx);
            // No semicolon consumed - if statement ends with block
            break;
        case TokenType::SWITCH: 
            result = parseSwitchStmt(stream, ctx);
            // No semicolon consumed - switch statement ends with block
            break;
        case TokenType::FOR:    
            result = parseForStmt(stream, ctx);
            // No semicolon consumed - for statement ends with block
            break;
        case TokenType::WHILE:  
            result = parseWhileStmt(stream, ctx);
            // No semicolon consumed - while statement ends with block
            break;
        case TokenType::DO:     
            result = parseDoWhileStmt(stream, ctx);
            // do-while requires semicolon AFTER the while condition
            if (result) {
                if (!stream.match(TokenType::SEMICOLON)) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                            "expected ';' after do-while statement");
                }
            }
            break;
            
        // Jumps - these REQUIRE semicolon
        case TokenType::RETURN:  
            result = parseReturnStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after return statement");
            }
            break;
        case TokenType::BREAK:   
            result = parseBreakStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after break statement");
            }
            break;
        case TokenType::CONTINUE: 
            result = parseContinueStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after continue statement");
            }
            break;
            
        // Declarations
        case TokenType::LET:
        case TokenType::CONST:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
            result = parseDeclStmt(stream, ctx);
            // parseDeclStmt will handle semicolon based on declaration type
            break;

        case TokenType::IMPORT:
            ctx.diagnostics.errorAt(DiagCode::Syntax_InvalidAttributeTarget, loc,
                                    "import statement is only valid at top level");
            synchronizeToContext(stream, ctx);
            return nullptr;

        // Concurrency - async/await/spawn/join are statements with their own rules
        case TokenType::ASYNC:   
            result = parseAsyncStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after async statement");
            }
            break;
        case TokenType::AWAIT:   
            result = parseAwaitStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after await statement");
            }
            break;
        case TokenType::SPAWN:   
            result = parseSpawnStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after spawn statement");
            }
            break;
        case TokenType::JOIN:    
            result = parseJoinStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after join statement");
            }
            break;
            
        // Expression Statement (default) - REQUIRES semicolon
        default:
            result = parseExprStmt(stream, ctx);
            if (result && !stream.match(TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ';' after expression statement");
            }
            break;
    }
    
    return result;
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
    auto builder = ctx.arena.makeBuilder<StmtAST*>();
    
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
        
        StmtAST* stmt = parseStmt(stream, ctx);
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
    
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected if condition");
        synchronizeToContext(stream, ctx);
        if (!ctx.canContinue()) {
            return ifStmt;
        }
    }
    ifStmt->condition = condition;
    
    StmtAST* thenBranch = parseBlock(stream, ctx);
    if (!thenBranch) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected then branch block");
        synchronizeToContext(stream, ctx);
        return ifStmt;
    }
    ifStmt->thenBranch = thenBranch;
    
    if (stream.match(TokenType::ELSE)) {
        if (stream.check(TokenType::IF)) {
            StmtAST* elseBranch = parseIfStmt(stream, ctx);
            if (elseBranch) {
                ifStmt->elseBranch = elseBranch;
            } else {
                synchronizeToContext(stream, ctx);
            }
        } else {
            StmtAST* elseBranch = parseBlock(stream, ctx);
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
    
    ExprAST* subject = parseExpr(stream, ctx);
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
    
    auto caseBuilder = ctx.arena.makeBuilder<SwitchCaseAST*>();
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
            SwitchCaseAST* switchCase = parseSwitchCase(stream, ctx);
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
    auto valueBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    do {
        ExprAST* value = parseExpr(stream, ctx);
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
    
    // ─── 1. Parse index binding ────────────────────────────────────────────
    ParamAST* indexParam = nullptr;
    bool isRangeLoop = true;  // Assume range loop unless we see a comma
    
    if (stream.check(TokenType::UNDERSCORE)) {
        stream.consume(); // Consume '_'
        indexParam = nullptr;
    } else if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        InternedString name = ctx.pool.intern(nameTok.value);
        
        TypeAST* type = parseType(stream, ctx);
        if (!type) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected index variable type");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::IN);
        }
        
        // Create ParamAST using constructor (keyword is always Let for loop variables)
        indexParam = ctx.arena.make<ParamAST>(name, type, false, false);
        indexParam->loc = stream.currentLoc();
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected index variable name or '_', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::IN);
    }
    
    // =======================================================================
    /// COLLECTION: PATH:
    // ─── 2. Check for collection iteration (has comma) ─────────────────────
    // =======================================================================

    bool hasValueBinding = stream.match(TokenType::COMMA);
    
    if (hasValueBinding) {
        isRangeLoop = false;  // Collection iteration
        // ─── Parse value binding (collection iteration) ──────────────────
        ParamAST* valueParam = nullptr;
        
        if (stream.check(TokenType::UNDERSCORE)) {
            stream.consume(); // Consume '_'
            valueParam = nullptr;
        } else if (stream.check(TokenType::IDENTIFIER)) {
            Token nameTok = stream.consume();
            InternedString name = ctx.pool.intern(nameTok.value);
            
            TypeAST* type = parseType(stream, ctx);
            if (!type) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                        "expected value variable type");
                synchronizeTo(stream, ctx, TokenType::IN);
            }
            
            // Create ParamAST using constructor
            valueParam = ctx.arena.make<ParamAST>(name, type, false, false);
            valueParam->loc = stream.currentLoc();
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected value variable name or '_', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::IN);
        }
        
        // ─── 3. Create ForStmtAST with collection iteration ──────────────
        ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
        forStmt->loc = loc;
        forStmt->indexVar = indexParam;
        forStmt->valueVar = valueParam;
        
        // ─── 4. Parse 'in' ─────────────────────────────────────────────────
        if (!stream.match(TokenType::IN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected 'in', got '", stream.peekValue(), "'");
            synchronizeToContext(stream, ctx);
            return forStmt;
        }
        
        // ─── 5. Parse iterable expression ──────────────────────────────────
        ExprAST* iterable = parseExpr(stream, ctx);
        if (!iterable) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected iterable expression");
            synchronizeToContext(stream, ctx);
            return forStmt;
        }
        forStmt->iterable = iterable;
        
        // ─── 6. Parse loop body ──────────────────────────────────────────────
        StmtAST* body = parseBlock(stream, ctx);
        if (!body) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                    "expected loop body block");
            synchronizeToContext(stream, ctx);
            return forStmt;
        }
        forStmt->body = body;
        forStmt->step = nullptr;  // Step not allowed in collection loops
        
        LOG_PARSER("parseForStmt: parsed collection loop");
        return forStmt;
    }
    
    // =======================================================================
    /// RANGE: LOOP: PATH:
    // =======================================================================
    
    // ─── 3. Parse 'in' ─────────────────────────────────────────────────────
    if (!stream.match(TokenType::IN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'in', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        // Return a partial AST
        ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
        forStmt->loc = loc;
        forStmt->indexVar = indexParam;
        return forStmt;
    }
    
    // ─── 4. Parse range expression ──────────────────────────────────────────
    ExprAST* iterable = parseExpr(stream, ctx);
    if (!iterable || !iterable->isa<RangeExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected range expression (start..end)");
        synchronizeToContext(stream, ctx);
        ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
        forStmt->loc = loc;
        forStmt->indexVar = indexParam;
        return forStmt;
    }
    
    // ─── 5. Parse optional step ─────────────────────────────────────────────
    ExprAST* step = nullptr;
    if (stream.match(TokenType::RANGE)) {
        step = parseExpr(stream, ctx);
        if (!step) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected step expression after '..'");
            synchronizeToContext(stream, ctx);
        }
    }
    
    // ─── 6. Parse loop body ────────────────────────────────────────────────
    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected loop body block");
        synchronizeToContext(stream, ctx);
        ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
        forStmt->loc = loc;
        forStmt->indexVar = indexParam;
        forStmt->iterable = iterable;
        forStmt->step = step;
        return forStmt;
    }
    
    // ─── 7. Create ForStmtAST with all fields ──────────────────────────────
    ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
    forStmt->loc = loc;
    forStmt->indexVar = indexParam;
    forStmt->valueVar = nullptr;  // No value variable in range loops
    forStmt->iterable = iterable;
    forStmt->step = step;
    forStmt->body = body;
    
    LOG_PARSER("parseForStmt: parsed range loop");
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
    
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected while condition");
        synchronizeToContext(stream, ctx);
        return whileStmt;
    }
    whileStmt->condition = condition;
    
    StmtAST* body = parseBlock(stream, ctx);
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
    
    StmtAST* body = parseBlock(stream, ctx);
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
    
    ExprAST* condition = parseExpr(stream, ctx);
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
    
    ExprAST* value = parseExpr(stream, ctx);
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
    
    ExprAST* expr = parseExpr(stream, ctx);
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

    DeclAST* decl = parseDecl(stream, ctx);
    if (!decl) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected declaration");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<DeclStmtAST>(nullptr);
    }
    
    // parseDecl already handles semicolon consumption based on declaration type
    // So we don't need to consume another semicolon here
    
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
    
    // 1. Parse 'async' keyword
    if (!stream.match(TokenType::ASYNC)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'async', got '", stream.peekValue(), "'");
        return ctx.arena.make<AsyncStmtAST>();
    }
    
    // 2. Parse declaration keyword (let/const)
    //    async const result int = fetchData(url)
    //    async let result int = fetchData(url)
    //    ───┬─── ──┬─── ─┬─ ─┬─
    //    async keyword name type
    
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const' after 'async', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<AsyncStmtAST>();
    }
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
    // 3. Parse the variable name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name for async binding, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<AsyncStmtAST>();
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // 4. Parse the type annotation (required)
    TypeAST* innerType = parseType(stream, ctx);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected type for async binding, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<AsyncStmtAST>();
    }
    
    // 5. Wrap the type in FutureTypeAST
    TypeAST* wrappedType = ctx.arena.make<FutureTypeAST>(innerType);
    
    // 6. Create the VarDeclAST for the binding using constructor
    VarDeclAST* binding = ctx.arena.make<VarDeclAST>(name, keyword, wrappedType, nullptr);
    binding->loc = loc;
    
    // 7. Parse '=' (required)
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<AsyncStmtAST>();
    }
    
    // 8. Parse the async call expression
    ExprAST* call = parseExpr(stream, ctx);
    if (!call || !call->isa<CallExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected async call expression");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<AsyncStmtAST>();
    }
    
    // 9. Create AsyncStmtAST with all fields
    AsyncStmtAST* asyncStmt = ctx.arena.make<AsyncStmtAST>();
    asyncStmt->loc = loc;
    asyncStmt->binding = binding;
    asyncStmt->call = call;
    
    LOG_PARSER("parseAsyncStmt: parsed async statement with binding '", 
               ctx.pool.lookup(name), "' (", isConst ? "const" : "let", ")");
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
    
    auto targetBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected variable name, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA);
            break;
        }
        Token targetTok = stream.consume();
        auto* idExpr = ctx.arena.make<IdentifierExprAST>(ctx.pool.intern(targetTok.value));
        targetBuilder.push_back(idExpr);
    } while (stream.match(TokenType::COMMA));
    
    awaitStmt->targets = targetBuilder.build();
    
    LOG_PARSER("parseAwaitStmt: parsed await statement");
    return awaitStmt;
}

SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseSpawnStmt: parsing spawn statement");

    SourceLocation loc = stream.currentLoc();
    
    // 1. Parse 'spawn' keyword
    if (!stream.match(TokenType::SPAWN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'spawn', got '", stream.peekValue(), "'");
        return ctx.arena.make<SpawnStmtAST>();
    }
    
    VarDeclAST* binding = nullptr;
    
    // 2. Check for discard pattern ('_') first
    //    spawn _ = logToFile("started")
    //    ───┬─── ─┬─
    //    spawn  _
    if (stream.check(TokenType::UNDERSCORE)) {
        // Discard pattern - no binding, no keyword, no type required
        stream.consume(); // Consume '_'
        binding = nullptr;
        
        // Parse '=' (required for discard pattern too)
        if (!stream.match(TokenType::ASSIGN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '=', got '", stream.peekValue(), "'");
            synchronizeToContext(stream, ctx);
            return ctx.arena.make<SpawnStmtAST>();
        }
        
        // Parse the spawn call expression
        ExprAST* call = parseExpr(stream, ctx);
        if (!call || !call->isa<CallExprAST>()) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected spawn call expression");
            synchronizeToContext(stream, ctx);
            return ctx.arena.make<SpawnStmtAST>();
        }
        
        // Create SpawnStmtAST with discard pattern
        SpawnStmtAST* spawnStmt = ctx.arena.make<SpawnStmtAST>();
        spawnStmt->loc = loc;
        spawnStmt->binding = nullptr;
        spawnStmt->call = call;
        
        LOG_PARSER("parseSpawnStmt: parsed spawn discard pattern");
        return spawnStmt;
    }
    
    // 3. Parse declaration keyword (let/const) for named bindings
    //    spawn const result int = computeHeavyData()
    //    spawn let result int = computeHeavyData()
    //    ───┬─── ──┬─── ─┬─ ─┬─
    //    spawn keyword name type
    
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const' after 'spawn' (or '_' for discard), got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<SpawnStmtAST>();
    }
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
    // 4. Parse the variable name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name for spawn binding, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<SpawnStmtAST>();
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // 5. Parse the type annotation (required for named bindings)
    TypeAST* innerType = parseType(stream, ctx);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected type for spawn binding, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<SpawnStmtAST>();
    }
    
    // 6. Wrap the type in ThreadTypeAST
    TypeAST* wrappedType = ctx.arena.make<ThreadTypeAST>(innerType);
    
    // 7. Create the VarDeclAST for the binding using constructor
    binding = ctx.arena.make<VarDeclAST>(name, keyword, wrappedType, nullptr);
    binding->loc = loc;
    
    LOG_PARSER_DETAIL("parseSpawnStmt: binding '", ctx.pool.lookup(name), 
                      "' (", isConst ? "const" : "let", ")");
    
    // 8. Parse '=' (required)
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<SpawnStmtAST>();
    }
    
    // 9. Parse the spawn call expression
    ExprAST* call = parseExpr(stream, ctx);
    if (!call || !call->isa<CallExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected spawn call expression");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<SpawnStmtAST>();
    }
    
    // 10. Create SpawnStmtAST with all fields
    SpawnStmtAST* spawnStmt = ctx.arena.make<SpawnStmtAST>();
    spawnStmt->loc = loc;
    spawnStmt->binding = binding;
    spawnStmt->call = call;
    
    LOG_PARSER("parseSpawnStmt: parsed spawn statement with binding '", 
               ctx.pool.lookup(name), "'");
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
    
    auto targetBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected variable name, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA);
            break;
        }
        Token targetTok = stream.consume();
        auto* idExpr = ctx.arena.make<IdentifierExprAST>(ctx.pool.intern(targetTok.value));
        targetBuilder.push_back(idExpr);
    } while (stream.match(TokenType::COMMA));
    
    joinStmt->targets = targetBuilder.build();
    
    LOG_PARSER("parseJoinStmt: parsed join statement");
    return joinStmt;
}

} // namespace parser
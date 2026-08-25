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
// parseStmt – Statement Entry Point
// =============================================================================

StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx) {
    // Check at the entry point - if we can't continue, bail out
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
            break;
        case TokenType::SWITCH: 
            result = parseSwitchStmt(stream, ctx);
            break;
        case TokenType::FOR:    
            result = parseForStmt(stream, ctx);
            break;
        case TokenType::WHILE:  
            result = parseWhileStmt(stream, ctx);
            break;
        case TokenType::DO:     
            result = parseDoWhileStmt(stream, ctx);
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
            break;

        case TokenType::IMPORT:
            ctx.diagnostics.errorAt(DiagCode::Syntax_InvalidAttributeTarget, loc,
                                    "import statement is only valid at top level");
            // Remove synchronizeToContext - let parseBlock's loop handle recovery
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
    
    if (result) result->loc = loc;
    return result;
}

// =============================================================================
// parseBlock – Parses a brace-delimited block
// =============================================================================

BlockStmtAST* parseBlock(TokenStream& stream, ParserContext& ctx) {
    BlockStmtAST* block = ctx.arena.make<BlockStmtAST>();

    // ─── Parse opening brace ──────────────────────────────────────────────
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' to open block body");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }

    block->loc = stream.previousLoc();

    auto builder = ctx.arena.makeBuilder<StmtAST*>();
    
    // ─── Parse statements until '}' ──────────────────────────────────────
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) && ctx.canContinue()) {
        // ─── Filter invalid tokens in this context ──────────────────────
        // A statement starts with a statement keyword, declaration keyword,
        // or an identifier/expression. We also skip stray semicolons.
        if (!stream.check(TokenType::SEMICOLON) &&
            !is_statement_keyword(stream.peekType()) &&
            !is_declaration_keyword(stream.peekType()) &&
            stream.peekType() != TokenType::IDENTIFIER) {
            
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected token '", stream.peekValue(), "' inside block body");
            
            // Synchronize to nearest valid statement to recover
            synchronizeTo(stream, ctx, 
                TokenType::SEMICOLON,      // Skip stray semicolons
                TokenType::RBRACE,         // Block closing
                TokenType::IDENTIFIER,     // Expression statement start
                TokenType::IF,             // Control flow
                TokenType::SWITCH,
                TokenType::FOR,
                TokenType::WHILE,
                TokenType::DO,
                TokenType::RETURN,
                TokenType::BREAK,
                TokenType::CONTINUE,
                TokenType::ASYNC,          // Concurrency
                TokenType::AWAIT,
                TokenType::SPAWN,
                TokenType::JOIN,
                TokenType::LET,            // Declarations
                TokenType::CONST,
                TokenType::STRUCT,
                TokenType::ENUM,
                TokenType::TRAIT
            );
            
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }

        // ─── Skip stray semicolons ──────────────────────────────────────
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        // ─── Progress guard ──────────────────────────────────────────────
        // Save position before parsing to detect zero-progress
        size_t savedPos = stream.getPos();

        // ─── Parse statement ──────────────────────────────────────────────
        StmtAST* stmt = parseStmt(stream, ctx);
        if (stmt) {
            builder.push_back(stmt);
        } else {
            // parseStmt reported the error. If no progress was made, consume
            // one token to avoid infinite loop.
            if (stream.getPos() == savedPos && !stream.isAtEnd()) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                        "skipping unexpected token '", stream.peekValue(), "'");
                stream.consume();
            }
        }
    }

    // ─── Parse closing brace ──────────────────────────────────────────────
    if (!stream.check(TokenType::RBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '}' to close block body");
        synchronizeTo(stream, ctx, TokenType::RBRACE);
        if (stream.check(TokenType::RBRACE)) {
            stream.consume();
        }
    } else {
        stream.consume(); // Consume '}'
    }
    
    block->stmts = builder.build();
    
    return block;
}

// =============================================================================
// parseIfStmt – Parses if/else statement
// =============================================================================

IfStmtAST* parseIfStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::IF)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'if', got '", stream.peekValue(), "'");
        return ctx.arena.make<IfStmtAST>();
    }
    
    IfStmtAST* ifStmt = ctx.arena.make<IfStmtAST>();
    
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected if condition");
        return nullptr;
    }
    ifStmt->condition = condition;
    
    StmtAST* thenBranch = parseBlock(stream, ctx);
    if (!thenBranch) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected then branch block");
        return nullptr;
    }
    ifStmt->thenBranch = thenBranch;
    
    if (stream.match(TokenType::ELSE)) {
        if (stream.check(TokenType::IF)) {
            StmtAST* elseBranch = parseIfStmt(stream, ctx);
            if (elseBranch) {
                ifStmt->elseBranch = elseBranch;
            } else {
                return nullptr;
            }
        } else {
            StmtAST* elseBranch = parseBlock(stream, ctx);
            if (elseBranch) {
                ifStmt->elseBranch = elseBranch;
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected else branch block");
                return nullptr;
            }
        }
    }
    
    return ifStmt;
}

// =============================================================================
// parseSwitchStmt – Parses switch statement
// =============================================================================

SwitchStmtAST* parseSwitchStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::SWITCH)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'switch', got '", stream.peekValue(), "'");
        return ctx.arena.make<SwitchStmtAST>();
    }
    
    SwitchStmtAST* switchStmt = ctx.arena.make<SwitchStmtAST>();

    // ─── Parse subject ──────────────────────────────────────────────────────
    ExprAST* subject = parseExpr(stream, ctx);
    if (!subject) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected switch subject");
        // If we can't parse the subject, try to recover to the '{'
        synchronizeTo(stream, ctx, TokenType::LBRACE, TokenType::SEMICOLON, TokenType::RBRACE);
        
        if (stream.check(TokenType::LBRACE)) {
            // We found the body - create a dummy subject
            subject = ctx.arena.make<IdentifierExprAST>(ctx.pool.intern("_"));
        } else {
            // Can't recover - return what we have
            return switchStmt;
        }
    }
    switchStmt->subject = subject;
    
    // ─── Parse body ────────────────────────────────────────────────────────
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        return switchStmt;
    }
    
    // ─── Push SwitchBody context ───────────────────────────────────────────
    ScopedContext switchContext(ctx, SyntacticContext::SwitchBody, stream.currentLoc());
    
    auto bodyBuilder = ctx.arena.makeBuilder<SwitchCaseAST*>();
    bool hasDefault = false;
    SourceLocation defaultLoc;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // ─── Filter invalid tokens in this context ──────────────────────
        // A switch body contains 'case' or 'default' clauses. We also skip
        // stray semicolons and commas.
        if (!stream.checkAny(TokenType::SEMICOLON, TokenType::COMMA) &&
            stream.peekType() != TokenType::CASE &&
            stream.peekType() != TokenType::DEFAULT) {
            
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected token '", stream.peekValue(), "' inside switch body");
            
            // Synchronize to nearest valid clause to recover
            synchronizeTo(stream, ctx, 
                TokenType::SEMICOLON,      // Skip stray semicolons
                TokenType::COMMA,          // Skip stray commas
                TokenType::CASE,           // Case clause
                TokenType::DEFAULT,        // Default clause
                TokenType::RBRACE          // End of switch body
            );
            
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }

        // ─── Skip stray semicolons and commas ──────────────────────────
        if (stream.checkAny(TokenType::SEMICOLON, TokenType::COMMA)) {
            stream.consume();
            continue;
        }
        
        // ─── Parse default clause ───────────────────────────────────────
        if (stream.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_MultipleDefaults, stream.currentLoc(),
                                        "multiple default clauses in switch");
                stream.consume(); // Consume 'default'
                
                // Skip the rest of the invalid default clause
                if (!stream.match(TokenType::COLON)) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                            "expected ':', got '", stream.peekValue(), "'");
                    synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
                    continue;
                }
                
                // Parse and discard the body since it's a duplicate
                if (stream.check(TokenType::LBRACE)) {
                    parseBlock(stream, ctx);
                } else {
                    synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
                }
                continue;
            }
            
            hasDefault = true;
            defaultLoc = stream.currentLoc();
            stream.consume(); // Consume 'default'
            
            if (!stream.match(TokenType::COLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ':', got '", stream.peekValue(), "'");
                if (stream.check(TokenType::LBRACE)) {
                    BlockStmtAST* defaultBody = parseBlock(stream, ctx);
                    if (defaultBody) {
                        switchStmt->defaultBody = defaultBody;
                        switchStmt->defaultLoc = defaultLoc;
                    }
                } else {
                    synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
                }
                continue;
            }
            
            BlockStmtAST* defaultBody = parseBlock(stream, ctx);
            if (defaultBody) {
                switchStmt->defaultBody = defaultBody;
                switchStmt->defaultLoc = defaultLoc;
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected default body block");
                // Try to recover to the next case or default
                synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
                if (stream.checkAny(TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE)) {
                    continue;
                }
                break;
            }
            
        } else if (stream.check(TokenType::CASE)) {
            // ─── Parse case clause ──────────────────────────────────────────
            SwitchCaseAST* switchCase = parseSwitchCase(stream, ctx);
            if (switchCase) {
                if (switchCase->values.empty()) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedCaseValue, switchCase->loc,
                                            "case clause has no values");
                }
                bodyBuilder.push_back(switchCase);
            } else {
                // parseSwitchCase failed - try to recover to the next case or default
                synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
                if (stream.checkAny(TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE)) {
                    continue;
                }
                break;
            }
            
        } else {
            // This should not happen due to filtering, but just in case
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected 'case' or 'default', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
            if (stream.checkAny(TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE)) {
                continue;
            }
            break;
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '}' to close switch body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    switchStmt->cases = bodyBuilder.build();
    
    return switchStmt;
}

SwitchCaseAST* parseSwitchCase(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::CASE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'case', got '", stream.peekValue(), "'");
        return ctx.arena.make<SwitchCaseAST>();
    }
    
    SwitchCaseAST* switchCase = ctx.arena.make<SwitchCaseAST>();
    switchCase->loc = loc;
    auto valueBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    // ─── Parse case values ──────────────────────────────────────────────────
    bool hasValue = false;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::COLON)) {
        // Check if we've hit a case/default keyword (shouldn't happen before colon)
        if (stream.check(TokenType::CASE) || stream.check(TokenType::DEFAULT)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ':' before next case, got '", stream.peekValue(), "'");
            // Stop here - let parseSwitchStmt handle the next case
            switchCase->values = valueBuilder.build();
            return switchCase;
        }
        
        // Check if we've hit a brace (shouldn't happen before colon)
        if (stream.check(TokenType::LBRACE)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ':' before case body, got '{'");
            break;
        }
        
        ExprAST* value = parseExpr(stream, ctx);
        if (value) {
            valueBuilder.push_back(value);
            hasValue = true;
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected case value");
            
            // Try to recover to the next comma, colon, or stop at case/default
            // We must NOT skip past another case or default
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::COLON, 
                          TokenType::CASE, TokenType::DEFAULT);
            
            if (stream.check(TokenType::CASE) || stream.check(TokenType::DEFAULT)) {
                // We found another case - stop and return partial AST
                switchCase->values = valueBuilder.build();
                return switchCase;
            } else if (stream.check(TokenType::COMMA)) {
                stream.consume();
                continue;
            } else if (stream.check(TokenType::COLON)) {
                break;
            } else {
                // Can't recover - return partial AST
                switchCase->values = valueBuilder.build();
                return switchCase;
            }
        }
        
        // Handle comma between values
        if (stream.match(TokenType::COMMA)) {
            // Check for trailing comma (next token is colon or case/default)
            if (stream.check(TokenType::COLON) || stream.check(TokenType::CASE) || stream.check(TokenType::DEFAULT)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                        "unexpected trailing comma in case values");
                // Don't consume the colon/case/default - let the caller handle it
                break;
            }
            // Continue to parse the next value
            continue;
        }
        
        // If we're not at a comma or colon, something is wrong
        if (!stream.check(TokenType::COLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ',' or ':', got '", stream.peekValue(), "'");
            
            // Try to recover, but don't skip case/default
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::COLON,
                          TokenType::CASE, TokenType::DEFAULT);
            
            if (stream.check(TokenType::CASE) || stream.check(TokenType::DEFAULT)) {
                // We found another case - stop and return partial AST
                switchCase->values = valueBuilder.build();
                return switchCase;
            } else if (stream.check(TokenType::COMMA)) {
                stream.consume();
                continue;
            } else if (stream.check(TokenType::COLON)) {
                break;
            } else {
                switchCase->values = valueBuilder.build();
                return switchCase;
            }
        }
    }
    
    // ─── Check for colon ────────────────────────────────────────────────────
    if (!stream.match(TokenType::COLON)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ':', got '", stream.peekValue(), "'");
        switchCase->values = valueBuilder.build();
        return switchCase;
    }
    
    // ─── Validate we have at least one value ──────────────────────────────
    if (!hasValue) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedCaseValue, loc,
                                "case clause must have at least one value");
        switchCase->values = valueBuilder.build();
        return switchCase;
    }
    
    // ─── Parse body ─────────────────────────────────────────────────────────
    // Check if we're at a block body
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for case body, got '", stream.peekValue(), "'");
        // Try to recover - if we see another case/default, stop
        synchronizeTo(stream, ctx, TokenType::LBRACE, TokenType::CASE, TokenType::DEFAULT);
        if (stream.check(TokenType::CASE) || stream.check(TokenType::DEFAULT)) {
            // Found another case - return partial AST
            switchCase->values = valueBuilder.build();
            return switchCase;
        } else if (stream.check(TokenType::LBRACE)) {
            // Found the body - continue parsing
        } else {
            switchCase->values = valueBuilder.build();
            return switchCase;
        }
    }
    
    BlockStmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected case body block");
        switchCase->values = valueBuilder.build();
        return switchCase;
    }
    
    switchCase->body = body;
    switchCase->values = valueBuilder.build();

    return switchCase;
}

// =============================================================================
// parseForStmt – Parses for loop (range or collection iteration)
// =============================================================================

ForStmtAST* parseForStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::FOR)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
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
            return nullptr;
        }
        
        // Create ParamAST using constructor (keyword is always Let for loop variables)
        indexParam = ctx.arena.make<ParamAST>(name, type, false, false);
        indexParam->loc = stream.currentLoc();
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected index variable name or '_', got '", stream.peekValue(), "'");
        return nullptr;
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
                return nullptr;
            }
            
            // Create ParamAST using constructor
            valueParam = ctx.arena.make<ParamAST>(name, type, false, false);
            valueParam->loc = stream.currentLoc();
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected value variable name or '_', got '", stream.peekValue(), "'");
            return nullptr;
        }
        
        // ─── 3. Create ForStmtAST with collection iteration ──────────────
        ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
        forStmt->indexVar = indexParam;
        forStmt->valueVar = valueParam;
        
        // ─── 4. Parse 'in' ─────────────────────────────────────────────────
        if (!stream.match(TokenType::IN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected 'in', got '", stream.peekValue(), "'");
            return nullptr;
        }
        
        // ─── 5. Parse iterable expression ──────────────────────────────────
        ExprAST* iterable = parseExpr(stream, ctx);
        if (!iterable) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected iterable expression");
                
            // Attempt to recover if the next token is '{', if it is not '{' then stop here
            if (!stream.check(TokenType::LBRACE)) {
                return nullptr;
            }
        }
        forStmt->iterable = iterable;
        
        // ─── 6. Parse loop body ──────────────────────────────────────────────
        StmtAST* body = parseBlock(stream, ctx);
        if (!body) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                    "expected loop body block");
            return nullptr;
        }
        forStmt->body = body;
        forStmt->step = nullptr;  // Step not allowed in collection loops
        
        return forStmt;
    }
    
    // =======================================================================
    /// RANGE: LOOP: PATH:
    // =======================================================================
    
    // ─── 3. Parse 'in' ─────────────────────────────────────────────────────
    if (!stream.match(TokenType::IN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'in', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── 4. Parse range expression ──────────────────────────────────────────
    ExprAST* iterable = parseExpr(stream, ctx);
    if (!iterable || !iterable->isa<RangeExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected range expression (start..end)");
        return nullptr;
    }
    
    // ─── 5. Parse optional step ─────────────────────────────────────────────
    ExprAST* step = nullptr;
    if (stream.match(TokenType::RANGE)) {
        step = parseExpr(stream, ctx);
        if (!step) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected step expression after '..'");
            return nullptr;
        }
    }
    
    // ─── 6. Parse loop body ────────────────────────────────────────────────
    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected loop body block");
        return nullptr;
    }
    
    // ─── 7. Create ForStmtAST with all fields ──────────────────────────────
    ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
    forStmt->indexVar = indexParam;
    forStmt->valueVar = nullptr;  // No value variable in range loops
    forStmt->iterable = iterable;
    forStmt->step = step;
    forStmt->body = body;
    
    return forStmt;
}

// =============================================================================
// parseWhileStmt – Parses while loop
// =============================================================================

WhileStmtAST* parseWhileStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::WHILE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'while', got '", stream.peekValue(), "'");
        return ctx.arena.make<WhileStmtAST>();
    }
    
    WhileStmtAST* whileStmt = ctx.arena.make<WhileStmtAST>();

    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected while condition");
        // Stop if we can't continue parseBlock
        if (!stream.check(TokenType::LBRACE)) {
            return nullptr;
        }
    }
    whileStmt->condition = condition;
    
    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected loop body block");
        return nullptr;
    }
    whileStmt->body = body;
    
    return whileStmt;
}

// =============================================================================
// parseDoWhileStmt – Parses do-while loop
// =============================================================================

DoWhileStmtAST* parseDoWhileStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::DO)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'do', got '", stream.peekValue(), "'");
        return ctx.arena.make<DoWhileStmtAST>();
    }
    
    DoWhileStmtAST* doWhileStmt = ctx.arena.make<DoWhileStmtAST>();
    
    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected loop body block");
        return nullptr;
    }
    doWhileStmt->body = body;
    
    if (!stream.match(TokenType::WHILE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'while', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected do-while condition");
        return nullptr;
    }
    doWhileStmt->condition = condition;
    
    return doWhileStmt;
}

// =============================================================================
// parseReturnStmt – Parses return statement
// =============================================================================

ReturnStmtAST* parseReturnStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::RETURN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'return', got '", stream.peekValue(), "'");
        return ctx.arena.make<ReturnStmtAST>();
    }
    
    ReturnStmtAST* returnStmt = ctx.arena.make<ReturnStmtAST>();
    
    ExprAST* value = parseExpr(stream, ctx);
    if (!value) {
        // Bare return (no value) - valid for void functions
        returnStmt->value = nullptr;
    } else {
        returnStmt->value = value;
    }
    
    return returnStmt;
}

// =============================================================================
// parseBreakStmt – Parses break statement
// =============================================================================

BreakStmtAST* parseBreakStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::BREAK)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'break', got '", stream.peekValue(), "'");
        return ctx.arena.make<BreakStmtAST>();
    }
    
    BreakStmtAST* breakStmt = ctx.arena.make<BreakStmtAST>();

    return breakStmt;
}

// =============================================================================
// parseContinueStmt – Parses continue statement
// =============================================================================

ContinueStmtAST* parseContinueStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::CONTINUE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'continue', got '", stream.peekValue(), "'");
        return ctx.arena.make<ContinueStmtAST>();
    }
    
    ContinueStmtAST* continueStmt = ctx.arena.make<ContinueStmtAST>();
    
    return continueStmt;
}

// =============================================================================
// parseExprStmt – Parses an expression used as a statement
// =============================================================================

ExprStmtAST* parseExprStmt(TokenStream& stream, ParserContext& ctx) {
    ExprAST* expr = parseExpr(stream, ctx);
    if (!expr) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected expression statement");
        return nullptr;
    }
    
    ExprStmtAST* exprStmt = ctx.arena.make<ExprStmtAST>(expr);

    return exprStmt;
}

// =============================================================================
// parseDeclStmt – Parses a declaration statement
// =============================================================================

/// NOTE: we are unwanted double assign the SourceLocation to the declaration here
///       the parseDecl will set the SourceLocation for the node but the parseStmt
///       also do it, it's double assignment, but this is acceptable
DeclStmtAST* parseDeclStmt(TokenStream& stream, ParserContext& ctx) {
    DeclAST* decl = parseDecl(stream, ctx);
    if (!decl) {
        /// Error already reported by parseDecl
        return nullptr;
    }
    
    DeclStmtAST* declStmt = ctx.arena.make<DeclStmtAST>(decl);

    return declStmt;
}

// =============================================================================
// Concurrency Statements
// =============================================================================

AsyncStmtAST* parseAsyncStmt(TokenStream& stream, ParserContext& ctx) {
    SourceLocation bindingLoc = stream.currentLoc();

    // 1. Parse 'async' keyword
    if (!stream.match(TokenType::ASYNC)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, bindingLoc,
                                "expected 'async', got '", stream.peekValue(), "'");
        return ctx.arena.make<AsyncStmtAST>();
    }
    
    // 2. Parse declaration keyword (let/const)
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const' after 'async', got '", stream.peekValue(), "'");
        return nullptr;
    }
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
    // 3. Parse the variable name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name for async binding, got '", stream.peekValue(), "'");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // 4. Parse the type annotation (required)
    TypeAST* innerType = parseType(stream, ctx);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected type for async binding, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // 5. Wrap the type in FutureTypeAST
    TypeAST* wrappedType = ctx.arena.make<FutureTypeAST>(innerType);
    
    // 6. Create the VarDeclAST for the binding
    VarDeclAST* binding = ctx.arena.make<VarDeclAST>(name, keyword, wrappedType, nullptr);
    binding->loc = bindingLoc;
    
    // 7. Parse '=' (required)
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // 8. Parse the async call expression
    ExprAST* call = parseExpr(stream, ctx);
    if (!call || !call->isa<CallExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected async call expression");
        return nullptr;
    }
    
    // 9. Create AsyncStmtAST
    AsyncStmtAST* asyncStmt = ctx.arena.make<AsyncStmtAST>();
    asyncStmt->binding = binding;
    asyncStmt->call = call;
    
    return asyncStmt;
}

AwaitStmtAST* parseAwaitStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::AWAIT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'await', got '", stream.peekValue(), "'");
        return ctx.arena.make<AwaitStmtAST>();
    }
    
    AwaitStmtAST* awaitStmt = ctx.arena.make<AwaitStmtAST>();
    
    auto targetBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected variable name, got '", stream.peekValue(), "'");
            break;
        }
        Token targetTok = stream.consume();
        auto* idExpr = ctx.arena.make<IdentifierExprAST>(ctx.pool.intern(targetTok.value));
        targetBuilder.push_back(idExpr);
    } while (stream.match(TokenType::COMMA));
    
    awaitStmt->targets = targetBuilder.build();
    
    return awaitStmt;
}

SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx) {
    SourceLocation bindingLoc = stream.currentLoc();
    
    // 1. Parse 'spawn' keyword
    if (!stream.match(TokenType::SPAWN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, bindingLoc,
                                "expected 'spawn', got '", stream.peekValue(), "'");
        return ctx.arena.make<SpawnStmtAST>();
    }
    
    VarDeclAST* binding = nullptr;
    
    // 2. Check for discard pattern ('_') first
    if (stream.check(TokenType::UNDERSCORE)) {
        stream.consume(); // Consume '_'
        binding = nullptr;
        
        if (!stream.match(TokenType::ASSIGN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '=', got '", stream.peekValue(), "'");
            return nullptr;
        }
        
        ExprAST* call = parseExpr(stream, ctx);
        if (!call || !call->isa<CallExprAST>()) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected spawn call expression");
            return nullptr;
        }
        
        SpawnStmtAST* spawnStmt = ctx.arena.make<SpawnStmtAST>();
        spawnStmt->binding = nullptr;
        spawnStmt->call = call;
        
        return spawnStmt;
    }
    
    // 3. Parse declaration keyword (let/const) for named bindings
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const' after 'spawn' (or '_' for discard), got '", stream.peekValue(), "'");
        return nullptr;
    }
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
    // 4. Parse the variable name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name for spawn binding, got '", stream.peekValue(), "'");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // 5. Parse the type annotation (required for named bindings)
    TypeAST* innerType = parseType(stream, ctx);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected type for spawn binding, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // 6. Wrap the type in ThreadTypeAST
    TypeAST* wrappedType = ctx.arena.make<ThreadTypeAST>(innerType);
    
    // 7. Create the VarDeclAST for the binding
    binding = ctx.arena.make<VarDeclAST>(name, keyword, wrappedType, nullptr);
    binding->loc = bindingLoc;
    
    // 8. Parse '=' (required)
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // 9. Parse the spawn call expression
    ExprAST* call = parseExpr(stream, ctx);
    if (!call || !call->isa<CallExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected spawn call expression");
        return nullptr;
    }
    
    // 10. Create SpawnStmtAST
    SpawnStmtAST* spawnStmt = ctx.arena.make<SpawnStmtAST>();
    spawnStmt->binding = binding;
    spawnStmt->call = call;
    
    return spawnStmt;
}

JoinStmtAST* parseJoinStmt(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::JOIN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'join', got '", stream.peekValue(), "'");
        return ctx.arena.make<JoinStmtAST>();
    }
    
    JoinStmtAST* joinStmt = ctx.arena.make<JoinStmtAST>();
    
    auto targetBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected variable name, got '", stream.peekValue(), "'");
            break;
        }
        Token targetTok = stream.consume();
        auto* idExpr = ctx.arena.make<IdentifierExprAST>(ctx.pool.intern(targetTok.value));
        targetBuilder.push_back(idExpr);
    } while (stream.match(TokenType::COMMA));
    
    joinStmt->targets = targetBuilder.build();
    
    return joinStmt;
}

} // namespace parser
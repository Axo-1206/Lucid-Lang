/**
 * @file ParseStmt.cpp
 * 
 * @responsibility Implements statement parsing for the Lucid language.
 * 
 * @related_files
 *   - src/parser/Parser.hpp – function declarations
 *   - src/parser/Parser.cpp – core parsing infrastructure
 *   - src/parser/ParserExpr.cpp – expression parsing (used by statement parsers)
 *   - src/parser/ParserDecl.cpp – declaration parsing (used by DeclStmtAST)
 *   - src/parser/Helpers.cpp – helper functions (parseArgList, etc.)
 */

#include "core/Tokens.hpp"
#include "parser/Parser.hpp"
#include "parser/support/TokenStream.hpp"
#include "parser/support/ParserContext.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
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

/**
 * @note parseStmt does not call parseBlock
*/
StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseStmt: parsing statement");
    
    // Guard: Check for EOF or error state
    if (stream.isAtEnd() || !ctx.canContinue()) {
        return nullptr;
    }
    
    // Check if we're at a statement terminator - skip it
    if (isStatementTerminator(stream)) {
        stream.advance(); // Consume the terminator to avoid infinite loop
        return nullptr;
    }
    
    Token current = stream.peek();
    SourceLocation loc = stream.currentLoc();
    
    // Dispatch based on the first token
    switch (current.type) {
        // ─── Control Flow ────────────────────────────────────────────────
        case TokenType::IF:
            return parseIfStmt(stream, ctx);
        case TokenType::SWITCH:
            return parseSwitchStmt(stream, ctx);
        case TokenType::FOR:
            return parseForStmt(stream, ctx);
        case TokenType::WHILE:
            return parseWhileStmt(stream, ctx);
        case TokenType::DO:
            return parseDoWhileStmt(stream, ctx);
            
        // ─── Jumps ──────────────────────────────────────────────────────
        case TokenType::RETURN:
            return parseReturnStmt(stream, ctx);
        case TokenType::BREAK:
            return parseBreakStmt(stream, ctx);
        case TokenType::CONTINUE:
            return parseContinueStmt(stream, ctx);
            
        // ─── Declaration Statements ──────────────────────────────────────
        case TokenType::LET:
        case TokenType::CONST:
            // Check if this is a multi-var declaration: `let a int, b int = expr`
            // We need to look ahead to see if there's a comma after a type
            if (looksLikeMultiAssignStart(stream, ctx)) {
                return parseMultiVarDecl(stream, ctx);
            }
            return parseDeclStmt(stream, ctx);
            
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
            return parseDeclStmt(stream, ctx);

        case TokenType::IMPORT:
            ctx.error(stream, DiagCode::E1010, "importing", "the keyword 'import' is only valid at top level");
            return nullptr;

        case TokenType::ASYNC:
            return parseAsyncStmt(stream, ctx);
        case TokenType::AWAIT:
            return parseAwaitStmt(stream, ctx);

        case TokenType::SPAWN:
            return parseSpawnStmt(stream, ctx);
        case TokenType::JOIN:
            return parseJoinStmt(stream, ctx);

            
        // ─── Expression Statement (default) ─────────────────────────────
        default:
            // Check if this is a multi-assign to existing variables:
            // `value, ok = parseInt("42");`. Ordinary single-target
            // assignment (`x = 5;`) is intentionally NOT matched here —
            // it is already handled inside parseExpr()/parsePrattExpr()
            // via the right-associative ASSIGN infix operator
            // (parseInfixAssign), so it keeps going through
            // parseExprStmt() below exactly as before.
            if (looksLikeMultiAssignTargets(stream, ctx)) {
                return parseMultiAssignStmt(stream, ctx);
            }
            // Try to parse as an expression statement
            return parseExprStmt(stream, ctx);
    }
}

// =============================================================================
// parseBlock – Parses a brace-delimited block
// =============================================================================

BlockStmtAST* parseBlock(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseBlock: parsing block");
    
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Expect LBRACE ──────────────────────────────────────────────────
    if (!stream.match(TokenType::LBRACE)) {
        ctx.error(stream, DiagCode::E1004, "{", "block", stream.peekValue());
        return ctx.arena.make<BlockStmtAST>();
    }
    
    // ─── 2. Push block context for error recovery ────────────────────────
    // Blocks are entered for function bodies, if/else branches, loop bodies, etc.
    // We don't need a special SyntacticContext for blocks since they're handled
    // by the enclosing context (FuncBody, etc.)
    
    // ─── 3. Parse statements until we hit RBRACE or EOF ──────────────────
    BlockStmtAST* block = ctx.arena.make<BlockStmtAST>();
    auto builder = ctx.arena.makeBuilder<StmtPtr>();
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Skip any stray semicolons (common in error recovery)
        while (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            // We can completely ignore the trailing `;` or stray `;`
            stream.advance();
        }
        
        // If we're at a statement terminator or can't continue, break
        if (isStatementTerminator(stream) || !ctx.canContinue()) {
            break;
        }
        
        // Parse a statement
        StmtPtr stmt = parseStmt(stream, ctx);
        if (stmt) {
            builder.push_back(stmt);
        } else {
            // Error already reported by parseStmt, try to recover
            // Use synchronizeToContext to skip to the next statement boundary
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                break;
            }
        }
        
        // Check for error threshold
        if (!ctx.canContinue()) {
            break;
        }
    }
    
    // ─── 4. Expect RBRACE ──────────────────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, "}", "block", "<EOF>");
    } else {
        // Consume RBRACE (we know it's there from loop condition)
        stream.advance();
    }
    
    // ─── 5. Store statements ──────────────────────────────────────────────
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
    
    // ─── 1. Expect IF keyword ─────────────────────────────────────────────────
    if (!stream.match(TokenType::IF)) {
        ctx.error(stream, DiagCode::E1001, "if", stream.peekValue());
        return ctx.arena.make<IfStmtAST>();
    }
    
    IfStmtAST* ifStmt = ctx.arena.make<IfStmtAST>();
    ifStmt->loc = loc;
    
    // ─── 2. Parse condition ────────────────────────────────────────────────
    ExprPtr condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.error(stream, DiagCode::E1006, "if condition", stream.peekValue());
        // Try to recover by skipping to the then branch
        synchronizeToContext(stream, ctx);
        // If we can't recover, return what we have
        if (!ctx.canContinue()) {
            return ifStmt;
        }
    }
    ifStmt->condition = condition;
    
    // ─── 3. Parse then branch (must be a block) ──────────────────────────
    StmtPtr thenBranch = parseBlock(stream, ctx);
    if (!thenBranch) {
        ctx.error(stream, DiagCode::E1011, "if branch", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return ifStmt;
    }
    ifStmt->thenBranch = thenBranch;
    
    // ─── 4. Parse else branch (optional) ──────────────────────────────────
    if (stream.match(TokenType::ELSE)) {
        // Check if it's an else-if chain
        if (stream.check(TokenType::IF)) {
            // Parse as nested if statement
            StmtPtr elseBranch = parseIfStmt(stream, ctx);
            if (elseBranch) {
                ifStmt->elseBranch = elseBranch;
            } else {
                // Erorrs are reported by parseIfStmt
                synchronizeToContext(stream, ctx);
            }
        } else {
            // Regular else block
            StmtPtr elseBranch = parseBlock(stream, ctx);
            if (elseBranch) {
                ifStmt->elseBranch = elseBranch;
            } else {
                ctx.error(stream, DiagCode::E1011, "else branch", stream.peekValue());
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
    
    // ─── 1. Expect SWITCH keyword ────────────────────────────────────────────
    if (!stream.match(TokenType::SWITCH)) {
        ctx.error(stream, DiagCode::E1001, "switch", stream.peekValue());
        return ctx.arena.make<SwitchStmtAST>();
    }
    
    SwitchStmtAST* switchStmt = ctx.arena.make<SwitchStmtAST>();
    switchStmt->loc = loc;
    
    // ─── 2. Parse subject expression ──────────────────────────────────────
    ExprPtr subject = parseExpr(stream, ctx);
    if (!subject) {
        ctx.error(stream, DiagCode::E1105, stream.peekValue());
        synchronizeToContext(stream, ctx);
        return switchStmt;
    }
    switchStmt->subject = subject;
    
    // ─── 3. Parse body (must be a block containing cases) ─────────────────
    if (!stream.match(TokenType::LBRACE)) {
        ctx.error(stream, DiagCode::E1004, "{", "switch body", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return switchStmt;
    }
    
    // Parse cases until we hit RBRACE
    auto caseBuilder = ctx.arena.makeBuilder<SwitchCasePtr>();
    bool hasDefault = false;
    SourceLocation defaultLoc;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Check for default
        if (stream.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                ctx.error(stream, DiagCode::E1108);
                continue;
            }
            hasDefault = true;
            defaultLoc = stream.currentLoc();
            stream.advance(); // Consume 'default'
            
            if (!stream.match(TokenType::COLON)) {
                ctx.error(stream, DiagCode::E1004, ":", "default clause", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::RBRACE);
                break;
            }
            
            // Parse default body
            BlockStmtAST* defaultBody = parseBlock(stream, ctx);
            if (defaultBody) {
                switchStmt->defaultBody = defaultBody;
                switchStmt->defaultLoc = defaultLoc;
            } else {
                ctx.error(stream, DiagCode::E1011, "default clause", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::RBRACE);
                break;
            }
        } else if (stream.check(TokenType::CASE)) {
            SwitchCasePtr switchCase = parseSwitchCase(stream, ctx);
            if (switchCase) {
                caseBuilder.push_back(switchCase);
            } else {
                // Error already reported by parseSwitchCase
                synchronizeTo(stream, ctx, TokenType::RBRACE);
                break;
            }
        } else {
            ctx.error(stream, DiagCode::E1008, stream.peekValue(), "case or default clause in switch");
            synchronizeTo(stream, ctx, TokenType::RBRACE);
            break;
        }
    }
    
    // Consume RBRACE
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, "}", "switch body", "<EOF>");
    } else {
        stream.advance();
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
    
    // ─── 1. Expect CASE keyword ──────────────────────────────────────────────
    if (!stream.match(TokenType::CASE)) {
        ctx.error(stream, DiagCode::E1001, "case", stream.peekValue());
        return ctx.arena.make<SwitchCaseAST>();
    }
    
    SwitchCaseAST* switchCase = ctx.arena.make<SwitchCaseAST>();
    switchCase->loc = loc;
    auto valueBuilder = ctx.arena.makeBuilder<ExprPtr>();
    
    // ─── 2. Parse case values (comma-separated) ──────────────────────────
    do {
        // Parse a case value (literal, enum variant, or range)
        ExprPtr value = parseExpr(stream, ctx);
        if (value) {
            valueBuilder.push_back(value);
        } else {
            ctx.error(stream, DiagCode::E1006, "case value", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::COLON);
            break;
        }
    } while (stream.match(TokenType::COMMA));
    
    // ─── 3. Expect COLON ──────────────────────────────────────────────────
    if (!stream.match(TokenType::COLON)) {
        ctx.error(stream, DiagCode::E1004, ":", "case clause", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
        return switchCase;
    }
    
    // ─── 4. Parse case body (must be a block) ─────────────────────────────
    BlockStmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.error(stream, DiagCode::E1011, "switch-case", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::CASE, TokenType::DEFAULT, TokenType::RBRACE);
        return switchCase;
    }
    switchCase->body = body;
    
    // ─── 5. Store values ──────────────────────────────────────────────────
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
    
    // ─── 1. Expect FOR keyword ──────────────────────────────────────────────
    if (!stream.match(TokenType::FOR)) {
        ctx.error(stream, DiagCode::E1001, "for", stream.peekValue());
        return ctx.arena.make<ForStmtAST>();
    }
    
    ForStmtAST* forStmt = ctx.arena.make<ForStmtAST>();
    forStmt->loc = loc;
    
    // ─── 2. Parse index binding ───────────────────────────────────────────
    // Grammar: IDENTIFIER type | '_'
    ParamAST* indexParam = nullptr;
    
    if (stream.check(TokenType::UNDERSCORE)) {
        // Index is ignored
        stream.advance(); // Consume '_'
        indexParam = nullptr;
    } else if (stream.check(TokenType::IDENTIFIER)) {
        // Parse as a parameter (name + type)
        indexParam = ctx.arena.make<ParamAST>();
        Token nameTok = stream.advance();
        indexParam->name = ctx.pool.intern(nameTok.value);
        indexParam->loc = stream.currentLoc();
        
        // Parse type annotation (required)
        TypePtr type = parseType(stream, ctx);
        if (type) {
            indexParam->type = type;
        } else {
            ctx.error(stream, DiagCode::E1003, "index variable type", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::IN);
        }
    } else {
        ctx.error(stream, DiagCode::E1002, "index variable name or '_'", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::IN);
    }
    forStmt->indexVar = indexParam;
    
    // ─── 3. Check if we have a value binding (comma indicates collection) ──
    bool hasValueBinding = stream.match(TokenType::COMMA);
    
    if (hasValueBinding) {
        // ─── 4. Parse value binding (collection iteration) ──────────────
        // Grammar: IDENTIFIER type | '_'
        ParamAST* valueParam = nullptr;
        
        if (stream.check(TokenType::UNDERSCORE)) {
            stream.advance(); // Consume '_'
            valueParam = nullptr;
        } else if (stream.check(TokenType::IDENTIFIER)) {
            valueParam = ctx.arena.make<ParamAST>();
            Token nameTok = stream.advance();
            valueParam->name = ctx.pool.intern(nameTok.value);
            valueParam->loc = stream.currentLoc();
            
            TypePtr type = parseType(stream, ctx);
            if (type) {
                valueParam->type = type;
            } else {
                ctx.error(stream, DiagCode::E1003, "value variable type", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::IN);
            }
        } else {
            ctx.error(stream, DiagCode::E1002, "value variable name or '_'", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::IN);
        }
        forStmt->valueVar = valueParam;
    } else {
        // No comma means range iteration - valueVar remains nullptr
        forStmt->valueVar = nullptr;
    }
    
    // ─── 5. Expect IN ─────────────────────────────────────────────────────
    if (!stream.match(TokenType::IN)) {
        ctx.error(stream, DiagCode::E1001, "in", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return forStmt;
    }
    
    // ─── 6. Parse iterable ────────────────────────────────────────────────
    // For range loops: this should be a RangeExprAST
    // For collection loops: this is a collection expression
    ExprPtr iterable = parseExpr(stream, ctx);
    if (!iterable) {
        ctx.error(stream, DiagCode::E1006, "iterable expression", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return forStmt;
    }
    forStmt->iterable = iterable;
    
    // ─── 7. Parse optional step (ONLY for range loops) ──────────────────
    // Step is only valid for range loops (no value binding)
    // Grammar: `for i T in range_expr .. step`
    if (stream.match(TokenType::RANGE)) {
        ExprPtr step = parseExpr(stream, ctx);
        if (step) {
            forStmt->step = step;
        } else {
            ctx.error(stream, DiagCode::E1006, "for-loop step expression", stream.peekValue());
            synchronizeToContext(stream, ctx);
        }
    } else {
        forStmt->step = nullptr;
    }
    
    // ─── 8. Parse body (must be a block) ──────────────────────────────────
    StmtPtr body = parseBlock(stream, ctx);
    if (!body) {
        ctx.error(stream, DiagCode::E1011, "for-loop", stream.peekValue());
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
    
    // ─── 1. Expect WHILE keyword ─────────────────────────────────────────────
    if (!stream.match(TokenType::WHILE)) {
        ctx.error(stream, DiagCode::E1001, "while", stream.peekValue());
        return ctx.arena.make<WhileStmtAST>();
    }
    
    WhileStmtAST* whileStmt = ctx.arena.make<WhileStmtAST>();
    whileStmt->loc = loc;
    
    // ─── 2. Parse condition ────────────────────────────────────────────────
    ExprPtr condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.error(stream, DiagCode::E1006, "while condition", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return whileStmt;
    }
    whileStmt->condition = condition;
    
    // ─── 3. Parse body (must be a block) ──────────────────────────────────
    StmtPtr body = parseBlock(stream, ctx);
    if (!body) {
        ctx.error(stream, DiagCode::E1011, "while loop", stream.peekValue());
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
    
    // ─── 1. Expect DO keyword ─────────────────────────────────────────────────
    if (!stream.match(TokenType::DO)) {
        ctx.error(stream, DiagCode::E1001, "do", stream.peekValue());
        return ctx.arena.make<DoWhileStmtAST>();
    }
    
    DoWhileStmtAST* doWhileStmt = ctx.arena.make<DoWhileStmtAST>();
    doWhileStmt->loc = loc;
    
    // ─── 2. Parse body (must be a block) ──────────────────────────────────
    StmtPtr body = parseBlock(stream, ctx);
    if (!body) {
        ctx.error(stream, DiagCode::E1011, "do-while loop", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return doWhileStmt;
    }
    doWhileStmt->body = body;
    
    // ─── 3. Expect WHILE ───────────────────────────────────────────────────
    if (!stream.match(TokenType::WHILE)) {
        ctx.error(stream, DiagCode::E1004, "while", "do-while loop", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return doWhileStmt;
    }
    
    // ─── 4. Parse condition ────────────────────────────────────────────────
    ExprPtr condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.error(stream, DiagCode::E1006, "do-while condition", stream.peekValue());
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
    
    // ─── 1. Expect RETURN keyword ────────────────────────────────────────────
    if (!stream.match(TokenType::RETURN)) {
        ctx.error(stream, DiagCode::E1001, "return", stream.peekValue());
        return ctx.arena.make<ReturnStmtAST>();
    }
    
    ReturnStmtAST* returnStmt = ctx.arena.make<ReturnStmtAST>();
    returnStmt->loc = loc;
    auto valueBuilder = ctx.arena.makeBuilder<ExprPtr>();
    
    // ─── 2. Check if this is a bare return (no values) ───────────────────
    // If the next token is a statement terminator or isAtEnd, it's a bare return
    if (!isStatementTerminator(stream) && !stream.isAtEnd()) {
        // Parse one or more return values
        do {
            ExprPtr value = parseExpr(stream, ctx);
            if (value) {
                valueBuilder.push_back(value);
            } else {
                ctx.error(stream, DiagCode::E1006, "return value", stream.peekValue());
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::SEMICOLON);
                break;
            }
        } while (stream.match(TokenType::COMMA));
    }
    
    // ─── 3. Store values ──────────────────────────────────────────────────
    returnStmt->values = valueBuilder.build();
    
    LOG_PARSER("parseReturnStmt: parsed return with ", returnStmt->values.size(), " values");
    return returnStmt;
}

// =============================================================================
// parseBreakStmt – Parses break statement
// =============================================================================

BreakStmtAST* parseBreakStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseBreakStmt: parsing break statement");
    
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Expect BREAK keyword ─────────────────────────────────────────────
    if (!stream.match(TokenType::BREAK)) {
        ctx.error(stream, DiagCode::E1001, "break", stream.peekValue());
        return ctx.arena.make<BreakStmtAST>();
    }
    
    // ─── 2. Create the break node ─────────────────────────────────────────
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
    
    // ─── 1. Expect CONTINUE keyword ──────────────────────────────────────────
    if (!stream.match(TokenType::CONTINUE)) {
        ctx.error(stream, DiagCode::E1001, "continue", stream.peekValue());
        return ctx.arena.make<ContinueStmtAST>();
    }
    
    // ─── 2. Create the continue node ──────────────────────────────────────
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
    
    // ─── 1. Parse an expression ────────────────────────────────────────────
    ExprPtr expr = parseExpr(stream, ctx);
    if (!expr) {
        ctx.error(stream, DiagCode::E1006, "expression statement", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<ExprStmtAST>(nullptr);
    }
    
    // ─── 2. Create the expression statement ───────────────────────────────
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

    // ─── 1. Parse a declaration ────────────────────────────────────────────
    DeclPtr decl = parseDecl(stream, ctx);
    if (!decl) {
        ctx.error(stream, DiagCode::E1003, "declaration");
        synchronizeToContext(stream, ctx);
        return ctx.arena.make<DeclStmtAST>(nullptr);
    }
    
    // ─── 2. Create the declaration statement ──────────────────────────────
    DeclStmtAST* declStmt = ctx.arena.make<DeclStmtAST>(decl);
    declStmt->loc = loc;
    
    LOG_PARSER_DETAIL("parseDeclStmt: parsed declaration statement");
    return declStmt;
}

// =============================================================================
// parseMultiVarDecl – Parses multiple variable declaration
// =============================================================================

MultiVarDeclAST* parseMultiVarDecl(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseMultiVarDecl: parsing multi-variable declaration");
    
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Expect LET or CONST keyword ──────────────────────────────────────
    Token keywordTok = stream.peek();
    if (keywordTok.type != TokenType::LET && keywordTok.type != TokenType::CONST) {
        ctx.error(stream, DiagCode::E1001, "let or const", stream.peekValue());
        return ctx.arena.make<MultiVarDeclAST>();
    }
    stream.advance(); // Consume the keyword
    
    MultiVarDeclAST* multiDecl = ctx.arena.make<MultiVarDeclAST>();
    multiDecl->loc = loc;
    auto varBuilder = ctx.arena.makeBuilder<std::pair<InternedString, TypePtr>>();
    
    // Set the keyword
    multiDecl->keyword = (keywordTok.type == TokenType::LET) 
        ? DeclKeyword::Let
        : DeclKeyword::Const;
    
    // ─── 2. Parse variable bindings: IDENTIFIER type { ',' IDENTIFIER type } ─
    do {
        // Parse variable name
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "variable name", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::ASSIGN);
            break;
        }
        Token nameTok = stream.advance();
        InternedString name = ctx.pool.intern(nameTok.value);
        
        // Parse type annotation
        TypePtr type = parseType(stream, ctx);
        if (!type) {
            ctx.error(stream, DiagCode::E1003, "variable type");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::ASSIGN);
            break;
        }
        
        varBuilder.push_back({name, type});
    } while (stream.match(TokenType::COMMA));
    
    // ─── 3. Expect ASSIGN ──────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.error(stream, DiagCode::E1004, "=", "multi-variable declaration", stream.peekValue());
        synchronizeToContext(stream, ctx);
        // Store what we have and return
        multiDecl->vars = varBuilder.build();
        return multiDecl;
    }
    
    // ─── 4. Parse RHS expression ──────────────────────────────────────────
    ExprPtr rhs = parseExpr(stream, ctx);
    if (!rhs) {
        ctx.error(stream, DiagCode::E1006, "right-hand side of multi-variable declaration", stream.peekValue());
        synchronizeToContext(stream, ctx);
        multiDecl->vars = varBuilder.build();
        return multiDecl;
    }
    multiDecl->rhs = rhs;
    
    // ─── 5. Store variables ───────────────────────────────────────────────
    multiDecl->vars = varBuilder.build();
    
    LOG_PARSER("parseMultiVarDecl: parsed ", multiDecl->vars.size(), " variables");
    return multiDecl;
}

// =============================================================================
// parseMultiAssignStmt – Parses multiple assignment statement
// =============================================================================

MultiAssignStmtAST* parseMultiAssignStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseMultiAssignStmt: parsing multi-assignment statement");
    
    SourceLocation loc = stream.currentLoc();
    
    MultiAssignStmtAST* multiAssign = ctx.arena.make<MultiAssignStmtAST>();
    multiAssign->loc = loc;
    auto lhsBuilder = ctx.arena.makeBuilder<ExprPtr>();
    
    // ─── 1. Parse LHS targets: expr_lhs { ',' expr_lhs } ─────────────────
    do {
        ExprPtr lhs = parseLvalue(stream, ctx);
        if (lhs) {
            lhsBuilder.push_back(lhs);
        } else {
            ctx.error(stream, DiagCode::E1006, "left-hand side of assignment", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::ASSIGN);
            break;
        }
    } while (stream.match(TokenType::COMMA));
    
    // ─── 2. Expect ASSIGN ──────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.error(stream, DiagCode::E1007, "=", stream.peekValue());
        synchronizeToContext(stream, ctx);
        multiAssign->lhs = lhsBuilder.build();
        return multiAssign;
    }
    
    // ─── 3. Parse RHS expression ──────────────────────────────────────────
    ExprPtr rhs = parseExpr(stream, ctx);
    if (!rhs) {
        ctx.error(stream, DiagCode::E1006, "right-hand side of multi-assignment", stream.peekValue());
        synchronizeToContext(stream, ctx);
        multiAssign->lhs = lhsBuilder.build();
        return multiAssign;
    }
    multiAssign->rhs = rhs;
    
    // ─── 4. Store LHS targets ─────────────────────────────────────────────
    multiAssign->lhs = lhsBuilder.build();
    
    LOG_PARSER("parseMultiAssignStmt: parsed ", multiAssign->lhs.size(), " assignments");
    return multiAssign;
}

} // namespace parser
/**
 * @file ParserStmt.cpp
 *
 * @responsibility Parses all statement-oriented grammar rules.
 *
 * This file implements the parsers for:
 *   - Statement dispatch (parseStmt) – the root of statement parsing
 *   - Multi‑variable declaration (parseMultiVarDecl) – let x int, y int = f()
 *   - Multi‑assignment (parseMultiAssignStmt) – a, b = f()
 *   - Block statements (parseBlock) – { stmt* }
 *   - Control flow: if, switch, for, while, do-while
 *   - Jump statements: return, break, continue
 *
 * All statement parsers consume tokens from the parser's stream and build
 * corresponding StmtAST nodes. They include error recovery and infinite‑loop
 * prevention mechanisms.
 *
 * @related_files
 *   - Parser.hpp – class declaration and shared utilities
 *   - Parser.cpp – core token stream primitives
 *   - ParserDecl.cpp – declaration parsing (called for local declarations inside blocks)
 *   - ParserExpr.cpp – expression parsing (used in conditions, iterables, etc.)
 *   - ParserType.cpp – type parsing (used in for loop type annotations)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Root Statement Dispatcher
 *   parseStmt()                      – dispatch to appropriate statement parser
 *
 * ██ Multi‑Assignment & Declaration
 *   parseMultiVarDecl()              – let x int, y int = f()
 *   parseMultiAssignStmt()           – a, b = f()
 *
 * ██ Block
 *   parseBlock()                     – { stmt* }
 *
 * ██ Branching Statements
 *   parseIfStmt()                    – if expr block [ else (if_stmt | block) ]
 *   parseSwitchStmt()                – switch expr { case ... default ... }
 *   parseSwitchCase()                – case value { ',' value } ':' block
 *
 * ██ Loop Statements
 *   parseForStmt()                   – for ident [type] in expr block
 *   parseWhileStmt()                 – while expr block
 *   parseDoWhileStmt()               – do block while expr
 *
 * ██ Jump Statements
 *   parseReturnStmt()                – return [ expr { ',' expr } ]
 *   parseBreakStmt()                 – break
 *   parseContinueStmt()              – continue
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * CONTEXT FLAGS (parser state)
 *
 *   loopDepth_      – incremented when entering a loop (for/while/do-while),
 *                     decremented on exit. Used by parseBreakStmt() and
 *                     parseContinueStmt() to report errors when outside a loop.
 *
 *   parallelDepth_  – incremented when entering a parallel body, decremented
 *                     on exit. Used by parseReturnStmt(), parseBreakStmt(),
 *                     parseContinueStmt(), and parseAwaitExpr() (in ExprParser)
 *                     to report invalid usage inside parallel contexts.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DISPATCH PRIORITY IN parseStmt()
 *
 *   1. Multi‑assignment (reassignment)     – looksLikeMultiAssignStart()
 *   2. Multi‑variable declaration           – let/const with commas
 *   3. Local declarations                   – type, struct, enum, impl, trait, from, let/const, use
 *   4. 'pub' inside a block                 – error + skip
 *   5. Control flow keywords                – if, switch, for, while, do, return, break, continue
 *   6. Expression statement                 – fallback
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * LOOP SAFETY & PROGRESS GUARDS
 *
 *   - parseBlock() uses a progress guard: if parseStmt() makes no progress,
 *     consumes one token and continues (prevents infinite loop).
 *   - parseSwitchCase() uses a consecutive error counter (max 5) to prevent
 *     infinite loops on malformed case values.
 *   - All loops that parse statements/declarations have progress checks.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "Parser.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <cassert>
#include <algorithm>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Root Statement Dispatcher
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseStmt
//
// Root dispatcher for statement parsing. Examines the current token and routes
// to the appropriate statement parser.
//
// Grammar (statement):
//   stmt := multi_assign_stmt | multi_var_decl | local_decl | if_stmt
//         | switch_stmt | for_stmt | while_stmt | do_while_stmt
//         | return_stmt | break_stmt | continue_stmt | expr_stmt
//
// ─── Dispatch Priority (in order) ───────────────────────────────────────────
//   1. Multi‑assignment (reassignment)     – looksLikeMultiAssignStart()
//   2. Multi‑variable declaration           – let/const with commas (detected by lookahead)
//   3. Local declarations                   – type, struct, enum, impl, trait, from, let/const, use
//   4. 'pub' inside a block                 – error + skip (pub/export not allowed locally)
//   5. Control flow keywords                – if, switch, for, while, do, return, break, continue
//   6. Expression statement                 – fallback
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the entire statement (including optional trailing semicolon).
// - Returns a StmtPtr (never nullptr; on error returns UnknownStmtAST).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Invalid variable declaration without let/const (e.g., "x int = 5") is detected
//   and reported; tokens are skipped until semicolon or closing brace.
// - If no valid statement start is recognised, reports error, consumes one token,
//   and returns UnknownStmtAST.
// - Expression statement fallback: if parseExpr() fails, skips tokens until a
//   statement boundary to avoid infinite loop.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - The multi‑var detection lookahead is pure (restores pos_ on failure).
// - Each branch consumes at least one token or returns an error node.
// - Expression statement fallback includes a loop that skips tokens until a
//   safe boundary, guaranteeing progress.
// ─────────────────────────────────────────────────────────────────────────────
StmtPtr Parser::parseStmt() {
    LUC_LOG_STMT_VERBOSE("parseStmt: token='" << peek().value << "'");

    // ── 1. Multi‑assignment (reassignment) – safe lookahead ──────────────
    if (looksLikeMultiAssignStart()) {
        LUC_LOG_STMT_VERBOSE("Parse multiple assignment - no let/const");
        return parseMultiAssignStmt();
    }

    // ── 2. Multi‑variable declaration (let/const with commas) ────────────
    //    Detect a pattern like: let x int, y int = f()
    if (checkAny({TokenType::LET, TokenType::CONST})) {
        std::size_t savedPos = pos_;
        // Tentatively parse the first variable spec to check for a comma
        advance(); // consume keyword
        skipComments(pos_);
        bool hasIdentifier = check(TokenType::IDENTIFIER);
        if (hasIdentifier) advance(); // consume identifier
        
        bool hasType = false;
        if (looksLikeType()) {
            std::size_t typePos = pos_;
            if (skipType(typePos)) {
                hasType = true;
                // Move pos_ to after the type for the next check
                std::size_t nextPos = typePos;
                skipComments(nextPos);
                bool nextIsComma = (nextPos < tokens_.size() && tokens_[nextPos].type == TokenType::COMMA);
                
                if (hasIdentifier && hasType && nextIsComma) {
                    pos_ = savedPos; // restore for multi-var parser
                    return parseMultiVarDecl();
                }
            }
        }
        pos_ = savedPos; // restore position
        // Otherwise fall through to single declaration (handled below)
    }

    // ── 3. All other local declarations (type, struct, enum, impl, trait, from, let/const) ──
    if (checkAny({TokenType::TYPE, TokenType::STRUCT, TokenType::ENUM,
                  TokenType::IMPL, TokenType::TRAIT, TokenType::FROM,
                  TokenType::LET, TokenType::CONST, TokenType::AT_SIGN,
                  TokenType::USE})) {
        // Unified local declaration parser – consumes the keyword,
        // handles attributes, rejects pub/export, returns DeclPtr.
        DeclPtr decl = parseDeclaration(DeclContext::Local);
        if (!decl) {
            // parseDeclaration already reported an error
            return nullptr;
        }
        // Wrap the declaration in a DeclStmtAST
        auto ds = arena_.make<DeclStmtAST>(std::move(decl));
        ds->loc = currentLoc();
        return ds;
    }

    // ── 4. 'pub' inside a block ──────────────────────────────────────────
    if (check(TokenType::PUB)) {
        errorAt(DiagCode::E2006, "'pub' is not valid inside a block");
        advance();
        // After skipping pub, try to parse a declaration if one follows
        if (checkAny({TokenType::LET, TokenType::CONST,
                      TokenType::TYPE, TokenType::STRUCT, TokenType::ENUM,
                      TokenType::IMPL, TokenType::FROM})) {
            DeclPtr decl = parseDeclaration(DeclContext::Local);
            if (decl) {
                auto ds = arena_.make<DeclStmtAST>(std::move(decl));
                ds->loc = currentLoc();
                return ds;
            }
        }
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = currentLoc();
        return unknown;
    }

    // ── 5. Control flow keywords ─────────────────────────────────────────
    if (check(TokenType::IF))       return parseIfStmt();
    if (check(TokenType::SWITCH))   return parseSwitchStmt();
    if (check(TokenType::FOR))      return parseForStmt();
    if (check(TokenType::WHILE))    return parseWhileStmt();
    if (check(TokenType::DO))       return parseDoWhileStmt();
    if (check(TokenType::RETURN))   return parseReturnStmt();
    if (check(TokenType::BREAK))    return parseBreakStmt();
    if (check(TokenType::CONTINUE)) return parseContinueStmt();

    // ── Detect invalid variable declaration missing let/const: IDENTIFIER type '='
    if (check(TokenType::IDENTIFIER)) {
        std::size_t savedPos = pos_;
        advance(); // consume the identifier
        if (looksLikeType() && check(TokenType::ASSIGN)) {
            // Invalid: variable declaration without let/const
            errorAt(DiagCode::E2002,
                    "variable declaration requires 'let' or 'const' (found: '" +
                    tokens_[savedPos].value + " " + peek().value + " = ...')");
            // Recover: skip until the end of the statement (semicolon or closing brace)
            while (!isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::RBRACE)) {
                advance();
            }
            if (check(TokenType::SEMICOLON)) advance();
            auto unknown = arena_.make<UnknownStmtAST>();
            unknown->loc = currentLoc();
            return unknown;
        }
        pos_ = savedPos; // restore position for normal parsing
    }

    // ── 6. Expression statement ──────────────────────────────────────────
    if (!looksLikeStmtStart()) {
        errorAt(DiagCode::E2002, "unexpected token '" + peek().value + "'");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = currentLoc();
        if (!isAtEnd()) advance();
        return unknown;
    }

    SourceLocation loc = currentLoc();
    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E2008, "expected expression statement");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = currentLoc();
        // Skip tokens until a statement boundary to avoid infinite loop.
        while (!isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::RBRACE)) {
            advance();
        }
        if (check(TokenType::SEMICOLON)) advance();
        return unknown;
    }

    auto stmt = arena_.make<ExprStmtAST>(std::move(expr));
    stmt->loc = loc;
    return stmt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi‑Assignment & Declaration
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseMultiVarDecl
//
// Parses a multi‑variable declaration statement (with let/const).
//
// Grammar:
//   multi_var_decl := decl_keyword var_spec { ',' var_spec } '=' expr
//   var_spec       := IDENTIFIER type_ann
//   decl_keyword   := 'let' | 'const'
//
// Examples:
//   let q int, r int = divmod(10, 3)
//   const w int, h int = getScreenSize()
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called when the current token is 'let' or 'const' and lookahead indicates
//   a multi‑variable declaration pattern (identifier, type, comma).
// - The keyword token is consumed by this function.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'let' or 'const' keyword.
// - For each variable:
//     * Consumes an IDENTIFIER (variable name)
//     * Consumes a type annotation via parseType()
// - Consumes commas between variable specifications.
// - Consumes '=' (required).
// - Consumes the RHS expression via parseExpr().
// - Does NOT consume trailing semicolon (caller handles optional semicolons).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing variable name: reports error, returns nullptr.
// - Missing type annotation: reports error, returns nullptr.
// - Missing '=': reports error, returns nullptr.
// - Missing RHS expression: reports error, returns nullptr.
// - If a variable fails to parse, the loop may break or continue depending on
//   error severity.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   MultiVarDeclAST {
//       keyword: DeclKeyword (Let or Const)
//       vars:    vector<pair<InternedString, TypePtr>> (name + type pairs)
//       rhs:     ExprPtr (the initialiser expression)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<MultiVarDeclAST> Parser::parseMultiVarDecl(std::vector<AttributePtr> attrs) {
    SourceLocation loc = currentLoc();

    // Single keyword at the start (already consumed by caller)
    if (!checkAny({TokenType::LET, TokenType::CONST})) {
        errorAt(DiagCode::E2002, "expected 'let' or 'const'");
        return nullptr;
    }
    Token kwTok = advance();
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    std::vector<std::pair<InternedString, TypePtr>> vars;

    // Parse first variable
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected variable name");
        return nullptr;
    }
    std::string firstName = advance().value;
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + firstName + "'");
        return nullptr;
    }
    TypePtr firstType = parseType();
    if (!firstType) return nullptr;
    vars.emplace_back(pool_.intern(firstName), std::move(firstType));
    // Parse additional variables separated by commas
    while (check(TokenType::COMMA)) {
        advance(); // consume ','
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected variable name after comma");
            break;
        }
        std::string name = advance().value;
        if (!looksLikeType()) {
            errorAt(DiagCode::E2005, "expected type annotation for '" + name + "'");
            break;
        }
        TypePtr type = parseType();
        if (!type) break;
        vars.emplace_back(pool_.intern(name), std::move(type));
    }

    consume(TokenType::ASSIGN, DiagCode::E2001, "expected '=' in multi-assignment");
    ExprPtr rhs = parseExpr();
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after '='");
        return nullptr;
    }

    auto node = arena_.make<MultiVarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->vars = std::move(vars);
    node->rhs = std::move(rhs);
    node->attributes = std::move(attrs);   // NEW
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseMultiAssignStmt
//
// Parses a multi‑assignment statement (reassignment to existing variables).
//
// Grammar:
//   multi_assign_stmt := expr_lhs { ',' expr_lhs } '=' expr
//   expr_lhs          := IDENTIFIER
//                      | expr '.' IDENTIFIER
//                      | expr '[' expr ']'
//
// Examples:
//   a, b = f()
//   p.x, arr[i] = g()
//   x, y = getCoordinates()
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - Called when looksLikeMultiAssignStart() returns true (identifier followed
//   by comma and eventually '=').
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Parses the first lvalue via parseLvalue().
// - While the next token is ',':
//     * Consumes ','
//     * Parses the next lvalue via parseLvalue()
// - Consumes '=' (required).
// - Consumes the RHS expression via parseExpr().
// - Does NOT consume trailing semicolon (caller handles optional semicolons).
//
// ─── Lvalue Parsing ─────────────────────────────────────────────────────────
// - Uses parseLvalue() which handles identifiers, field accesses, and indices.
// - Function calls and behavior accesses are NOT valid lvalues.
// - The RHS must be a single expression that returns as many values as there
//   are lvalues (semantic pass enforces this).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing first lvalue: reports error, returns nullptr.
// - Missing lvalue after comma: reports error, breaks.
// - Missing '=': reports error, skips tokens until semicolon or brace, returns nullptr.
// - Missing RHS expression: reports error, returns nullptr.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   MultiAssignStmtAST {
//       lhs: vector<ExprPtr> (lvalue expressions in order)
//       rhs: ExprPtr (the right‑hand side expression)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<MultiAssignStmtAST> Parser::parseMultiAssignStmt() {
    SourceLocation loc = currentLoc();
    std::vector<ExprPtr> lhs;

    // Parse the first lvalue using the full lvalue parser
    ExprPtr first = parseLvalue();
    if (!first) {
        errorAt(DiagCode::E2008, "expected left‑hand side expression");
        return nullptr;
    }
    lhs.push_back(std::move(first));

    // Parse additional lvalues after commas
    while (check(TokenType::COMMA)) {
        advance(); // consume ','
        ExprPtr next = parseLvalue();
        if (!next) {
            errorAt(DiagCode::E2008, "expected left‑hand side expression after comma");
            break;
        }
        lhs.push_back(std::move(next));
    }

    // Must have '='
    if (!check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' in multiple assignment, found: " + peek().value);
        while (!isAtEnd() && !check(TokenType::SEMICOLON) && !check(TokenType::RBRACE))
            advance();
        return nullptr;
    }
    advance(); // consume '='

    // Parse RHS expression (allow struct literals)
    ExprPtr rhs = parseExpr(true);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after '='");
        return nullptr;
    }

    auto node = arena_.make<MultiAssignStmtAST>();
    node->loc = loc;
    node->lhs = std::move(lhs);
    node->rhs = std::move(rhs);
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// Block
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseBlock
//
// Parses a block statement: a brace‑delimited sequence of statements.
//
// Grammar:
//   block := '{' { stmt } '}'
//
// Examples:
//   { let x int = 5; io.printl(x) }
//   { return 42 }
//   {}   // empty block
//
// ─── Foundational Role ──────────────────────────────────────────────────────
// - Every function body, if branch, loop body, and parallel sub‑block is a BlockStmtAST.
// - The semantic pass opens a new scope when entering a block and closes it on exit.
// - Names declared inside are not visible outside.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '{'.
// - Repeatedly calls parseStmt() to parse statements until '}' or EOF.
// - Consumes the closing '}'.
// - Optional semicolons between statements are consumed by parseStmt() or matched.
//
// ─── Loop Safety & Progress Guarantee ───────────────────────────────────────
// - Uses a progress guard: saves pos_ before each parseStmt() call.
// - If parseStmt() makes no progress (pos_ == savedPos):
//     * Forcibly consumes one token (advance())
//     * Skips any following semicolons that might cause another stall
//     * Continues to next iteration (does NOT push a statement)
// - This guarantees forward progress even on malformed input.
// - The loop terminates when '}' is found or EOF is reached.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing opening '{': consume() reports error.
// - If parseStmt() returns nullptr (error), the statement is skipped and the
//   loop continues.
// - Missing closing '}': consume() reports error and recovers.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   BlockStmtAST {
//       stmts: vector<StmtPtr> (statements in source order)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<BlockStmtAST> Parser::parseBlock() {
    LUC_LOG_STMT("parseBlock");
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACE, "expected '{', found: " + peek().value);

    auto block = arena_.make<BlockStmtAST>();
    block->loc = loc;
    int stmtCount = 0;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match(TokenType::SEMICOLON)) continue;

        std::size_t savedPos = pos_;
        StmtPtr stmt = parseStmt();

        // If parseStmt made no progress, force consumption of one token
        if (pos_ == savedPos) {
            LUC_LOG_STMT("parseBlock: no progress, forcing advance");
            if (!isAtEnd()) {
                advance(); // consume the offending token
            }
            // Skip any subsequent semicolons that might cause another stall
            while (match(TokenType::SEMICOLON)) {}
            continue; // do not push a statement
        }

        // Progress was made; if the statement is valid, add it
        if (stmt) {
            block->stmts.push_back(std::move(stmt));
            stmtCount++;
        }
        // If stmt is nullptr, an error was already reported; skip it
    }

    consume(TokenType::RBRACE, "expected '}' to close block");
    LUC_LOG_STMT("parseBlock: parsed " << stmtCount << " statements");
    return block;
}

// ─────────────────────────────────────────────────────────────────────────────
// Branching Statements
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseIfStmt
//
// Parses an 'if' statement (statement form, not expression form).
//
// Grammar:
//   if_stmt := 'if' expr block [ 'else' ( if_stmt | block ) ]
//
// Examples:
//   if score >= 90 { io.printl("A") }
//   if score >= 90 { io.printl("A") } else { io.printl("F") }
//   if x < 0 { return } else if x == 0 { ... } else { ... }
//
// ─── Comparison with parseIfExpr() ──────────────────────────────────────────
//   IfStmtAST (this function)       | IfExprAST (in ParserExpr.cpp)
//   --------------------------------|------------------------------------------
//   Statement context               | Expression context
//   'else' optional                 | 'else' required
//   No value produced               | Produces a value
//   No '??' separator               | Uses '??' as separator
//
// ─── Dispatching ────────────────────────────────────────────────────────────
// - In parseStmt(), 'if' always routes here.
// - In expression position (assignment RHS, function return), parseExpr() →
//   parsePrimaryExpr() → parseIfExpr() is used instead.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'if' keyword.
// - Parses the condition expression (struct literals disabled to avoid ambiguity
//   with the following block).
// - Consumes the then‑branch block via parseBlock().
// - Optional 'else' clause:
//     * If 'else if' → recursively calls parseIfStmt() (produces nested IfStmtAST)
//     * If 'else {' → parses a block via parseBlock()
// - Does NOT consume any tokens beyond the else branch.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing condition after 'if': reports error, returns placeholder node.
// - Missing '{' after condition: reports error, returns placeholder node.
// - Missing block after 'else' (if present): reports error.
// - Type narrowing applies inside then‑branch when condition is an 'is' expression
//   (enforced by semantic pass, not parser).
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   IfStmtAST {
//       condition:  ExprPtr
//       thenBranch: StmtPtr (always a BlockStmtAST)
//       elseBranch: StmtPtr (nullptr, BlockStmtAST, or IfStmtAST)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<IfStmtAST> Parser::parseIfStmt() {
    LUC_LOG_STMT("parseIfStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::IF, "expected 'if', found: " + peek().value);

    // Condition — parsed as a full expression, but disallow top-level struct literals
    // to avoid ambiguity with the following block.
    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if', found: " + peek().value);
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
        errorAt(DiagCode::E2001, "expected '{' after if condition, found: " + peek().value);

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
            errorAt(DiagCode::E2001, "expected 'if' or '{' after 'else', found: " + peek().value);
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
// Parses a 'switch' statement – statement‑oriented value dispatch.
//
// Grammar:
//   switch_stmt := 'switch' expr '{' { case_clause } [ default_clause ] '}'
//   case_clause := 'case' case_value { ',' case_value } ':' block
//   case_value  := expr | range_expr
//   default_clause := 'default' ':' block
//
// Examples:
//   switch code {
//       case 200, 201: { io.printl("ok") }
//       case 1..10:    { io.printl("light") }
//       default:       { io.printl("unknown") }
//   }
//
// ─── Comparison with MatchExprAST ───────────────────────────────────────────
//   SwitchStmtAST (this)            | MatchExprAST (in ParserExpr.cpp)
//   --------------------------------|------------------------------------------
//   Statement (no value produced)   | Expression (produces a value)
//   'default' optional              | 'default' required
//   Body is a block (statements)    | Body is an expression
//   No pattern matching             | Full pattern matching
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'switch' keyword.
// - Parses the subject expression (struct literals disabled).
// - Consumes the opening '{'.
// - Repeatedly parses case clauses via parseSwitchCase() until '}' or 'default'.
// - Optionally parses a default clause (at most one).
// - Consumes the closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing subject after 'switch': reports error, returns nullptr.
// - Missing '{' after subject: reports error, returns nullptr.
// - Duplicate 'default' clause: reports error, second default is skipped.
// - Unrecognised token inside switch block: reports error, calls synchronize().
// - Missing closing '}': consume() reports error and recovers.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - The case loop calls synchronize() on unrecognised tokens, which consumes
//   tokens until a safe boundary (case, default, or '}').
// - Each iteration either parses a valid case/default or advances past an error.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   SwitchStmtAST {
//       subject:     ExprPtr
//       cases:       vector<SwitchCasePtr>
//       defaultBody: BlockStmtPtr (nullptr if no default)
//       defaultLoc:  optional<SourceLocation>
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<SwitchStmtAST> Parser::parseSwitchStmt() {
    LUC_LOG_STMT("parseSwitchStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::SWITCH, "expected 'switch', found: " + peek().value);

    ExprPtr subject = parseExpr(false); // disallow struct literal
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'switch', found: " + peek().value);
        return nullptr;
    }
    LUC_LOG_STMT_VERBOSE("parseSwitchStmt: subject parsed");

    consume(TokenType::LBRACE, "expected '{' after switch subject, found: " + peek().value);

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
            consume(TokenType::COLON, DiagCode::E2001, "expected ':' after 'default', found: " + peek().value);
            // Default body is a block (the grammar says { stmt } but we always
            // parse it as a full block for uniformity).
            if (!check(TokenType::LBRACE)) {
                errorAt(DiagCode::E2001, "expected '{' to start default body, found: " + peek().value);
            } else {
                node->defaultBody = parseBlock();
            }
            continue;
        }

        errorAt(DiagCode::E2002, "expected 'case' or 'default' inside switch block, found: " + peek().value);
        synchronize();
    }

    consume(TokenType::RBRACE, "expected '}' to close switch statement, found: " + peek().value);
    LUC_LOG_STMT("parseSwitchStmt: " << caseCount << " cases, hasDefault=" << (node->defaultBody != nullptr));
    return node;
}


// ─────────────────────────────────────────────────────────────────────────────
// parseSwitchCase
//
// Parses a single case clause inside a switch statement.
//
// Grammar:
//   case_clause := 'case' case_value { ',' case_value } ':' block
//   case_value  := expr | range_expr
//
// Examples:
//   case 200, 201: { io.printl("ok") }
//   case 1..10:    { io.printl("light") }
//   case 0x41, 0x30..0x39: { handleInput() }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'case' keyword.
// - Parses the first case value (expression or range) via parsePrattExpr(0).
// - While the next token is ',':
//     * Consumes ','
//     * Parses the next case value
// - Consumes ':' after all values.
// - Consumes the case body block via parseBlock().
// - Does NOT consume any tokens beyond the closing '}' of the block.
//
// ─── Range Detection ────────────────────────────────────────────────────────
// - After parsing a primary expression, if the next token is '..' (RANGE),
//   calls parseRangeExpr() to convert the expression into a RangeExprAST.
// - Supports both inclusive '..' and exclusive '..<' ranges.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Uses a consecutive error counter (MAX_CONSECUTIVE_ERRORS = 5) to prevent
//   infinite loops on malformed case values.
// - If the first value cannot be parsed:
//     * If no progress (pos_ == savedPos): consumes one token, increments counter.
//     * If progress but UnknownExprAST: increments counter, skips adding value.
// - If consecutive errors reach the limit: skips tokens until ':'.
// - Missing ':' after values: consume() reports error.
// - Missing '{' for body: reports error.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - The value loop increments consecutiveErrors on failures.
// - When a value parses successfully, consecutiveErrors is reset to 0.
// - If errors exceed the limit, the loop exits and skips to ':'.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   SwitchCaseAST {
//       values: vector<ExprPtr> (case values, may include RangeExprAST)
//       body:   BlockStmtPtr
//   }
// ─────────────────────────────────────────────────────────────────────────────
SwitchCasePtr Parser::parseSwitchCase() {
    LUC_LOG_STMT_VERBOSE("parseSwitchCase");
    SourceLocation loc = currentLoc();
    consume(TokenType::CASE, "expected 'case'");

    auto sc = arena_.make<SwitchCaseAST>();
    sc->loc = loc;

    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    // Parse the first value
    if (check(TokenType::COLON)) {
        errorAt(DiagCode::E2001, "expected case value before ':'");
    } else {
        std::size_t savedPos = pos_;
        ExprPtr val = parsePrattExpr(0);
        if (pos_ == savedPos) {
            errorAt(DiagCode::E2002, "expected case value");
            if (!isAtEnd()) advance();
            consecutiveErrors++;
        } else if (val && !val->isa<UnknownExprAST>()) {
            if (check(TokenType::RANGE)) {
                sc->values.push_back(parseRangeExpr(std::move(val)));
            } else {
                sc->values.push_back(std::move(val));
            }
            consecutiveErrors = 0;
        } else {
            // Invalid value but progress was made; skip it without adding
            consecutiveErrors++;
        }
    }

    // Parse additional comma‑separated values
    while (check(TokenType::COMMA) && consecutiveErrors < MAX_CONSECUTIVE_ERRORS) {
        advance(); // consume ','
        if (check(TokenType::COLON)) break; // trailing comma allowed

        std::size_t savedPos = pos_;
        ExprPtr val = parsePrattExpr(0);
        if (pos_ == savedPos) {
            errorAt(DiagCode::E2002, "expected case value after comma");
            if (!isAtEnd()) advance();
            consecutiveErrors++;
            break; // exit loop to avoid infinite repetition
        }
        if (val && !val->isa<UnknownExprAST>()) {
            if (check(TokenType::RANGE)) {
                sc->values.push_back(parseRangeExpr(std::move(val)));
            } else {
                sc->values.push_back(std::move(val));
            }
            consecutiveErrors = 0;
        } else {
            consecutiveErrors++;
        }
    }

    // If we exited due to too many errors, skip to the colon
    if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
        errorAt(DiagCode::E2002, "too many errors in case values; skipping to ':'");
        while (!isAtEnd() && !check(TokenType::COLON)) {
            advance();
        }
    }

    consume(TokenType::COLON, DiagCode::E2001, "expected ':' after case values, found: " + peek().value);

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start case body, found: " + peek().value);
    } else {
        sc->body = parseBlock();
    }

    return sc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop Statements
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseForStmt
//
// Parses a 'for' loop statement for iterating over collections or ranges.
//
// Grammar:
//   for_stmt := 'for' IDENTIFIER [ type_ann ] 'in' ( range_expr | expr ) block
//
// Examples:
//   for item in items { io.printl(item) }
//   for i in 0..10 { io.printl(string(i)) }
//   for i in 0..10..2 { io.printl(string(i)) }  (step)
//   for i int in 0..10 { io.printl(string(i)) }  (explicit type)
//
// ─── Two Iteration Forms ────────────────────────────────────────────────────
//   1. Range iteration: iterable is a RangeExprAST (lo..hi or lo..<hi)
//      - Optional step after second '..'
//      - Iteration variable type defaults to 'int' if not specified
//   2. Collection iteration: iterable is any expression that produces a
//      collection type (array, slice, dynamic array)
//      - Iteration variable type is inferred from element type
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'for' keyword.
// - Consumes the iteration variable name (IDENTIFIER).
// - Optional explicit type annotation (if 'in' does not follow immediately).
// - Consumes the 'in' keyword.
// - Parses the iterable expression (struct literals disabled).
// - If the iterable is followed by '..', converts it to a RangeExprAST.
// - Optionally, if another '..' follows, parses a step expression.
// - Consumes the loop body block via parseBlock().
//
// ─── Loop Depth Tracking ─────────────────────────────────────────────────────
// - Increments loopDepth_ before parsing the body, decrements after.
// - Used by parseBreakStmt() and parseContinueStmt() to validate loop context.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing iteration variable: reports error, returns nullptr.
// - Missing 'in' after variable/type: reports error, returns nullptr.
// - Missing iterable expression: reports error, returns nullptr.
// - Missing step expression after second '..': reports error, returns nullptr.
// - Missing '{' for body: reports error, returns nullptr.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   ForStmtAST {
//       iterVar:  ParamPtr (name + optional type annotation)
//       iterable: ExprPtr (collection or RangeExprAST)
//       step:     ExprPtr (nullptr if no step)
//       body:     StmtPtr (always a BlockStmtAST)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<ForStmtAST> Parser::parseForStmt() {
    LUC_LOG_STMT("parseForStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::FOR, "expected 'for', found: " + peek().value);

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected iteration variable name after 'for', found: " + peek().value);
        return nullptr;
    }
    std::string varName = advance().value;
    LUC_LOG_STMT_VERBOSE("parseForStmt: varName='" << varName << "'");

    // Optional: Parse explicit type annotation if 'in' does not follow immediately.
    TypePtr varType = nullptr;
    if (!check(TokenType::IN)) {
        varType = parseType();
        if (!varType) {
            errorAt(DiagCode::E2005, "expected 'in' or explicit type after iteration variable name, found: " + peek().value);
            return nullptr;
        }
        LUC_LOG_STMT_VERBOSE("parseForStmt: explicit var type");
    }

    consume(TokenType::IN, "expected 'in' after iteration variable, found: " + peek().value);

    // Parse the iterable expression (collection or RangeExprAST).
    ExprPtr iterable = parseExpr(false);
    if (!iterable) {
        errorAt(DiagCode::E2008, "expected iterable expression after 'in', found: " + peek().value);
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
                errorAt(DiagCode::E2008, "expected step expression after '..', found: " + peek().value);
                return nullptr;
            }
            LUC_LOG_STMT_VERBOSE("parseForStmt: with step");
        }
    }

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start for loop body, found: " + peek().value);
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
// Parses a 'while' loop statement – condition tested before each iteration.
//
// Grammar:
//   while_stmt := 'while' expr block
//
// Example:
//   while n < 5 { n += 1 }
//   while !queue.isEmpty() { process(queue.pop() ?? defaultItem) }
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - The condition is evaluated before each iteration.
// - If the condition evaluates to true, the loop body executes.
// - The loop exits when the condition evaluates to false, or when a 'break'
//   statement is reached.
// - The loop body never executes if the condition is initially false.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'while' keyword.
// - Parses the condition expression (struct literals disabled).
// - Consumes the loop body block via parseBlock().
//
// ─── Loop Depth Tracking ─────────────────────────────────────────────────────
// - Increments loopDepth_ before parsing the body, decrements after.
// - Used by parseBreakStmt() and parseContinueStmt() to validate loop context.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing condition after 'while': reports error, returns nullptr.
// - Missing '{' after condition: reports error, returns nullptr.
// - Missing body block: reports error, returns nullptr.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   WhileStmtAST {
//       condition: ExprPtr (must resolve to bool)
//       body:      StmtPtr (always a BlockStmtAST)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<WhileStmtAST> Parser::parseWhileStmt() {
    LUC_LOG_STMT("parseWhileStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::WHILE, "expected 'while', found: " + peek().value);

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while', found: " + peek().value);
        return nullptr;
    }
    LUC_LOG_STMT_VERBOSE("parseWhileStmt: condition parsed");

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start while loop body, found: " + peek().value);
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
// Parses a 'do-while' loop statement – body executed before condition test.
//
// Grammar:
//   do_while_stmt := 'do' block 'while' expr
//
// Example:
//   do { retries += 1 } while retries < 3
//   do { c = readChar() } while c != '\n'
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - The loop body executes unconditionally at least once.
// - After the body executes, the condition is evaluated.
// - If the condition evaluates to true, the loop repeats.
// - The loop exits when the condition evaluates to false, or when a 'break'
//   statement is reached.
// - Useful when the exit condition depends on a side effect of the body.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'do' keyword.
// - Consumes the loop body block via parseBlock().
// - Consumes the 'while' keyword.
// - Parses the condition expression (struct literals disabled).
//
// ─── Loop Depth Tracking ─────────────────────────────────────────────────────
// - Increments loopDepth_ before parsing the body, decrements after.
// - Used by parseBreakStmt() and parseContinueStmt() to validate loop context.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '{' after 'do': reports error, returns nullptr.
// - Missing 'while' after body: reports error, returns nullptr.
// - Missing condition after 'while': reports error, returns nullptr.
// - Missing body block: reports error, returns nullptr.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   DoWhileStmtAST {
//       body:      StmtPtr (always a BlockStmtAST)
//       condition: ExprPtr (must resolve to bool)
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<DoWhileStmtAST> Parser::parseDoWhileStmt() {
    LUC_LOG_STMT("parseDoWhileStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::DO, "expected 'do', found: " + peek().value);

    if (!check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after 'do', found: " + peek().value);
        return nullptr;
    }

    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: entering loop body (loopDepth=" << loopDepth_ << "->" << loopDepth_ + 1 << ")");
    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;
    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: exited loop body");

    consume(TokenType::WHILE, "expected 'while' after do body, found: " + peek().value);

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while' in do-while loop, found: " + peek().value);
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
// Jump Statements
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseReturnStmt
//
// Parses a 'return' statement that exits the enclosing function.
//
// Grammar:
//   return_stmt := 'return' [ expr { ',' expr } ]
//
// Examples:
//   return           – void return (no value)
//   return 42        – single return value
//   return a + b     – expression return
//   return a, b, c   – multiple return values (comma‑separated)
//
// ─── Multiple Return Values ─────────────────────────────────────────────────
// - Luc supports multiple return values from functions.
// - The return statement can list multiple comma‑separated expressions.
// - The number and types of return values must match the function's signature
//   (enforced by the semantic pass).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'return' keyword.
// - If the next token is not '}', ';', or EOF, parses one or more comma‑separated
//   expressions.
// - Supports consecutive commas detection (empty expression) – reports error and skips.
// - Trailing comma detection – reports error.
// - Does NOT consume any tokens beyond the last expression (caller handles
//   semicolons or closing braces).
//
// ─── Semantic Restrictions (Parser Checks) ──────────────────────────────────
// - If parallelDepth_ > 0 (inside a parallel body), reports an error.
// - All other restrictions (async context, type matching) are enforced by the
//   semantic pass.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If an expression fails to parse (no progress), reports error, consumes one token,
//   and breaks out of the expression loop.
// - Consecutive commas: reports error, skips the extra comma, continues.
// - Trailing comma: reports error, breaks out of loop.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - The expression loop uses a progress guard: if parseExpr() makes no progress,
//   consumes one token and breaks (prevents infinite loop).
// - Each iteration either parses an expression or consumes a comma, guaranteeing
//   forward progress.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   ReturnStmtAST {
//       values: vector<ExprPtr> (empty for bare 'return')
//   }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<ReturnStmtAST> Parser::parseReturnStmt() {
    LUC_LOG_STMT("parseReturnStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::RETURN, "expected 'return', found: " + peek().value);

    if (parallelDepth_ > 0) {
        LUC_LOG_STMT("parseReturnStmt: ERROR - return inside parallel body");
        error(loc, DiagCode::E2006, "'return' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<ReturnStmtAST>();
    node->loc = loc;

    // Check if there is any expression after 'return'
    if (!check(TokenType::RBRACE) && !check(TokenType::SEMICOLON) && !isAtEnd()) {
        // Parse one or more comma‑separated expressions
        bool first = true;
        while (!check(TokenType::RBRACE) && !check(TokenType::SEMICOLON) && !isAtEnd()) {
            if (!first) {
                // Expect a comma before the next expression
                if (!match(TokenType::COMMA)) {
                    // No comma – we are done (e.g., no more expressions)
                    break;
                }
                // If we just consumed a comma and the next token is another comma, that's an empty expression
                if (check(TokenType::COMMA)) {
                    errorAt(DiagCode::E2008, "empty expression in return list (consecutive commas)");
                    // Skip the second comma to avoid infinite loop
                    advance();
                    continue;
                }
                // If after comma we reach a closing brace, semicolon, or EOF, that's a trailing comma error
                if (check(TokenType::RBRACE) || check(TokenType::SEMICOLON) || isAtEnd()) {
                    errorAt(DiagCode::E2001, "trailing comma in return list");
                    break;
                }
            }
            first = false;

            std::size_t savedPos = pos_;
            ExprPtr expr = parseExpr();
            if (pos_ == savedPos) {
                errorAt(DiagCode::E2008, "expected expression after 'return', found: " + peek().value);
                // Consume the offending token to avoid infinite loop
                if (!isAtEnd()) advance();
                // Do not break – maybe there are more expressions after an error? Better to break.
                break;
            }
            node->values.push_back(std::move(expr));
        }
    }

    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseBreakStmt
//
// Parses a 'break' statement that exits the nearest enclosing loop.
//
// Grammar:
//   break_stmt := 'break'
//
// Example:
//   break
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - Exits the nearest enclosing loop (for, while, or do‑while).
// - Control transfers to the first statement after the loop.
// - Cannot be used outside of a loop body.
// - Cannot be used inside a parallel body (parallel for, parallel block).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'break' keyword.
// - Does NOT consume any tokens beyond the keyword.
//
// ─── Parser‑Time Checks ─────────────────────────────────────────────────────
// - If loopDepth_ == 0 (not inside any loop), reports an error.
// - If parallelDepth_ > 0 (inside a parallel body), reports an error.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - Errors are reported but parsing continues (the node is still created).
// - The semantic pass may also enforce restrictions on break targets.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   BreakStmtAST (no fields)
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<BreakStmtAST> Parser::parseBreakStmt() {
    LUC_LOG_STMT("parseBreakStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::BREAK, "expected 'break', found: " + peek().value);

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
// Parses a 'continue' statement that skips to the next iteration of the
// nearest enclosing loop.
//
// Grammar:
//   continue_stmt := 'continue'
//
// Example:
//   continue
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - Skips the rest of the current loop iteration and jumps to the next iteration
//   of the nearest enclosing loop (for, while, or do‑while).
// - Cannot be used outside of a loop body.
// - Cannot be used inside a parallel body (parallel for, parallel block).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'continue' keyword.
// - Does NOT consume any tokens beyond the keyword.
//
// ─── Parser‑Time Checks ─────────────────────────────────────────────────────
// - If loopDepth_ == 0 (not inside any loop), reports an error.
// - If parallelDepth_ > 0 (inside a parallel body), reports an error.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - Errors are reported but parsing continues (the node is still created).
// - The semantic pass may also enforce restrictions on continue targets.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   ContinueStmtAST (no fields)
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<ContinueStmtAST> Parser::parseContinueStmt() {
    LUC_LOG_STMT("parseContinueStmt");
    SourceLocation loc = currentLoc();
    consume(TokenType::CONTINUE, "expected 'continue', found: " + peek().value);

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

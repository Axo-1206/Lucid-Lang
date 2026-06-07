/**
 * @file Parser.hpp
 * @brief Luc language parser – converts token streams into AST.
 * 
 * ============================================================================
 * PARSER OVERVIEW
 * ============================================================================
 * 
 * The Luc parser is a recursive‑descent parser with a Pratt parser for
 * expressions. It consumes a token stream from the lexer and produces an
 * Abstract Syntax Tree (AST) using arena allocation.
 * 
 * ## Architecture
 * 
 *   Lexer → TokenStream → Parser → ProgramAST (Arena‑allocated)
 * 
 * The parser is split across 7 implementation files:
 *   - Parser.cpp       : Core infrastructure (TokenStream, error recovery)
 *   - ParserDecl.cpp   : Declarations (struct, enum, trait, impl, from)
 *   - ParserExpr.cpp   : Expressions (Pratt parser, operators, literals)
 *   - ParserStmt.cpp   : Statements (if, switch, loops, return)
 *   - ParserType.cpp   : Type annotations
 *   - ParserLookahead.cpp : Non‑consuming lookahead helpers
 *   - ParserPattern.cpp   : Pattern matching for `match` expressions
 * 
 * ## Key Design Decisions
 * 
 * 1. **Arena Allocation**: All AST nodes are allocated in a bump‑pointer arena.
 *    This provides O(1) allocation, excellent cache locality, and bulk
 *    deallocation. Nodes use raw pointers (arena owns all memory).
 * 
 * 2. **TokenStream Abstraction**: Wraps the token vector with safe accessors,
 *    automatic comment skipping, and position manipulation for lookahead.
 * 
 * 3. **Loop Safety**: Every loop that parses sequences uses a saved position
 *    pattern to prevent infinite loops on malformed input.
 * 
 * 4. **Error Recovery**: Panic‑mode `synchronize()` skips to the next safe
 *    token (statement/declaration boundary). Individual parsers report errors
 *    and attempt to continue.
 * 
 * 5. **Qualifier Resolution**: `~async`, `~nullable`, `~parallel` are resolved
 *    in the parser using `QualifierRegistry`. This is necessary because
 *    `~parallel` affects `parallelDepth_` and `~async` affects whether `await`
 *    is allowed in the function body.
 * 
 * 6. **SpanBuilder Pattern**: Temporary `std::vector<T>` collections are
 *    converted to immutable `ArenaSpan<T>` at the point of AST assignment.
 * 
 * ## Visualized Parsing Flow
 * 
 * ```
 *                         ┌─────────────────┐
 *                         │   parse()       │
 *                         └────────┬────────┘
 *                                  │
 *                    ┌─────────────▼─────────────┐
 *                    │  parsePackageDecl()       │
 *                    └─────────────┬─────────────┘
 *                                  │
 *                    ┌─────────────▼─────────────┐
 *                    │  while (!ts_.isAtEnd())   │
 *                    │    parseTopLevelDecl()    │
 *                    └─────────────┬─────────────┘
 *                                  │
 *          ┌───────────────────────┼───────────────────────┐
 *          │                       │                       │
 *          ▼                       ▼                       ▼
 *   ┌─────────────┐         ┌─────────────┐         ┌─────────────┐
 *   │ parseUse    │         │ parseStruct │         │ parseFunc   │
 *   │ Decl()      │         │ Decl()      │         │ Decl()      │
 *   └─────────────┘         └─────────────┘         └─────────────┘
 *          │                       │                       │
 *          └───────────────────────┼───────────────────────┘
 *                                  │
 *                    ┌─────────────▼─────────────┐
 *                    │   ProgramAST built        │
 *                    │   with ArenaSpan<DeclPtr> │
 *                    └───────────────────────────┘
 * ```
 * 
 * ## Token Consumption Guarantee
 * 
 * Every parser function guarantees forward progress:
 *   - On success: consumes at least one token
 *   - On error: either consumes tokens (recovery) or returns nullptr
 *   - No infinite loops (enforced by saved position checks)
 * 
 * @see LUC_GRAMMAR.md for language grammar
 * @see ASTArena.hpp for arena allocation details
 */

#pragma once

#include "Tokens.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/support/ASTArena.hpp"
#include "registry/QualifierRegistry.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/Diagnostic.hpp"

#include <optional>
#include <utility>
#include <vector>
#include <string>

namespace {
    // Precedence levels
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
}

// ============================================================================
// TokenStream – Safe token stream abstraction
// ============================================================================

/**
 * @brief Wraps the token vector with safe accessors and comment skipping.
 * 
 * TokenStream is the primary interface for consuming tokens during parsing.
 * It automatically skips LINE_COMMENT and DOC_COMMENT tokens, making them
 * invisible to the grammar (they are harvested separately by harvestDocComment).
 * 
 * ## Usage Example
 * 
 *   if (ts_.check(TokenType::IDENTIFIER)) {
 *       InternedString name = pool_.intern(ts_.advance().value);
 *   }
 * 
 *   ts_.consume(TokenType::LBRACE, "expected '{'");
 * 
 * ## Position Management
 * 
 *   - `getPos()` / `setPos()` – for lookahead and error recovery
 *   - `peek()`, `peekNext()`, `peekAt()` – inspect without consuming
 *   - `advance()` – consume current token, skip following comments
 * 
 * ## Comment Skipping
 * 
 * All `peek*()` and `advance()` methods transparently skip LINE_COMMENT and
 * DOC_COMMENT tokens. The only way to access comments is via `getTokens()`
 * and scanning backward (used by `harvestDocComment`).
 * 
 * ## Thread Safety
 * 
 * Not thread‑safe – designed for single‑threaded parsing.
 * 
 * @see Parser::harvestDocComment() for doc comment extraction
 */
class TokenStream {
public:
    TokenStream(std::vector<Token> tokens, InternedString filePath);

    // ------------------------------------------------------------------------
    // Grammar interface (skips comments automatically)
    // ------------------------------------------------------------------------
    const Token& peek() const;
    Token advance();
    bool check(TokenType type) const;
    bool checkAny(std::initializer_list<TokenType> types) const;
    bool match(TokenType type);
    bool matchAny(std::initializer_list<TokenType> types);
    std::optional<Token> consumeIf(TokenType type);
    Token consume(TokenType type, DiagCode code, const std::string& msg);
    Token consume(TokenType type, const std::string& msg);
    bool isAtEnd() const;
    SourceLocation currentLoc() const;

    // ------------------------------------------------------------------------
    // Lookahead helpers (non‑consuming, skip comments)
    // ------------------------------------------------------------------------
    TokenType peekType() const { return peek().type; }
    TokenType peekNextType() const;
    const Token& peekNext() const;
    const Token& peekAt(size_t offset) const;

    // ------------------------------------------------------------------------
    // Raw inspection (keeps comments, for doc harvesting & lookahead)
    // ------------------------------------------------------------------------
    const std::vector<Token>& getTokens() const { return tokens_; }
    size_t getTokenCount() const { return tokens_.size(); }
    const Token& getTokenAt(size_t idx) const { return tokens_[idx]; }
    size_t getPos() const { return pos_; }
    void setPos(size_t pos) { pos_ = pos; }

    // Helper for raw lookahead (exposed)
    size_t skipCommentsFrom(size_t start) const;

    // Convert a token to SourceLocation
    SourceLocation locOf(const Token& tok) const;

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    InternedString filePath_;   // used for error reporting

    static const Token eofToken;
};

// ============================================================================
// QualifierSet – Parsed qualifier container
// ============================================================================

/**
 * @brief Result of parsing `~async`, `~nullable`, `~parallel` qualifiers.
 * 
 * Why `QualifierSet` instead of using `QualifierRegistry` directly?
 * 
 *   - **Separation of concerns**: Registry provides metadata and validation,
 *     Set holds the parsed result (raw names + computed bitmask).
 *   - **Performance**: Bitmask is pre‑computed during parsing for O(1) checks
 *     during semantic analysis.
 *   - **Error reporting**: Raw names are preserved for accurate error messages
 *     (e.g., "unknown qualifier '~custom'").
 *   - **AST storage**: Bitmask fits in 32 bits; raw names are interned.
 * 
 * The registry is used **during parsing** to validate names and compute bits.
 * The resulting `QualifierSet` is stored in the AST for later phases.
 * 
 * @note `~parallel` does NOT affect type equality (different from `~async`/
 *       `~nullable`). This is enforced by the registry's `affectsTypeEquality`
 *       flag.
 * 
 * @see QualifierRegistry for qualifier metadata and validation
 */
struct QualifierSet {
    std::vector<InternedString> raw;   ///< Original qualifier names (for errors)
    uint32_t bitmask;                  ///< OR of QualifierBits flags
    
    bool isAsync()   const { return (bitmask & QualifierBits::Async) != 0; }
    bool isNullable() const { return (bitmask & QualifierBits::Nullable) != 0; }
    bool isParallel() const { return (bitmask & QualifierBits::Parallel) != 0; }
};

// ============================================================================
// Parser – Main parsing class
// ============================================================================

/**
 * @brief Recursive‑descent parser for the Luc language.
 * 
 * ## Parsing Flow
 * 
 * ```
 *                           ┌─────────────────┐
 *     Tokens ──────────────►│   Parser()      │
 *                           └────────┬────────┘
 *                                    │
 *                           ┌────────▼────────┐
 *                           │   parse()       │
 *                           └────────┬────────┘
 *                                    │
 *              ┌─────────────────────┼─────────────────────┐
 *              │                     │                     │
 *        ┌─────▼─────┐         ┌─────▼─────┐         ┌─────▼─────┐
 *        │Package    │         │TopLevel  │         │Attributes │
 *        │Decl       │         │Decls     │         │& Metadata │
 *        └───────────┘         └───────────┘         └───────────┘
 * ```
 * 
 * ## Expression Parsing (Pratt Parser)
 * 
 * ```
 *   parseExpr() ──► parsePrattExpr(minPrec)
 *                         │
 *         ┌───────────────┼───────────────┐
 *         │               │               │
 *    ┌────▼────┐     ┌─────▼─────┐   ┌─────▼─────┐
 *    │Prefix   │     │Infix Loop │   │Postfix    │
 *    │Expr     │     │(prec >    │   │Expr       │
 *    │         │     │ minPrec)  │   │           │
 *    └─────────┘     └───────────┘   └───────────┘
 * ```
 * 
 * ## Loop Safety Pattern
 * 
 * Every loop that parses a sequence uses the saved position pattern:
 * 
 * ```cpp
 * size_t savedPos = ts_.getPos();
 * while (condition) {
 *     parseItem();
 *     if (ts_.getPos() == savedPos) {
 *         // No progress – consume token to break stalemate
 *         if (!ts_.isAtEnd()) ts_.advance();
 *         break;
 *     }
 *     savedPos = ts_.getPos();
 * }
 * ```
 * 
 * This prevents infinite loops on malformed input.
 * 
 * ## Error Recovery
 * 
 *   - **Panic mode**: `synchronize()` skips tokens until a statement or
 *     declaration boundary (IF, FOR, WHILE, LET, CONST, RBRACE, etc.).
 *   - **Per‑function recovery**: On parse failure, functions may call
 *     `synchronize()` or `synchronizeTo()` to skip to a safe point.
 *   - **Consecutive error counter**: Used in lists (arg lists, case values)
 *     to prevent infinite loops after multiple errors.
 * 
 * ## Memory Management
 * 
 *   - All AST nodes allocated via `arena_.make<T>()` returning raw pointers
 *   - The arena owns all memory; nodes are never freed individually
 *   - Temporary lists use `std::vector<T>`, converted to `ArenaSpan<T>` via
 *     `SpanBuilder` at the point of AST assignment
 * 
 * @see TokenStream for token consumption
 * @see ASTArena for arena allocation
 * @see QualifierRegistry for qualifier resolution
 * @see ParserDecl.cpp, ParserExpr.cpp, ParserStmt.cpp, ParserType.cpp
 *      for implementation details
 */
class Parser {
public:
    Parser(std::vector<Token> tokens,
           InternedString filePath, StringPool& pool, ASTArena& arena);

    ProgramASTPtr parse();  // Returns raw pointer (arena owns)
    bool hasErrors() const { return diagnostic::hasErrors(); }

private:
    // ========================================================================
    // State
    // ========================================================================
    TokenStream ts_;
    InternedString filePath_;
    StringPool& pool_;
    ASTArena& arena_;

    // ========================================================================
    // Error handling
    // ========================================================================
    void error(const SourceLocation& loc, DiagCode code, std::initializer_list<std::string> args = {});
    void errorAt(DiagCode code, std::initializer_list<std::string> args = {});
    
    template<typename... Args>
    void error(const SourceLocation& loc, DiagCode code, Args&&... args) {
        error(loc, code, {std::forward<Args>(args)...});
    }
    
    template<typename... Args>
    void errorAt(DiagCode code, Args&&... args) {
        error(ts_.currentLoc(), code, {std::forward<Args>(args)...});
    }

    void synchronize();
    void synchronizeTo(std::initializer_list<TokenType> stopTokens);

    // ========================================================================
    // Helpers for list parsing
    // ========================================================================

    std::vector<ParamPtr> parseParamList();           // returns std::vector<ParamAST*>
    std::vector<InternedString> parseUsePath();
    std::vector<AttributePtr> parseAttributes();

    ArenaSpan<ExprAST*> parseArgList();
    ArenaSpan<TypeAST*> parseReturnList();
   
    GenericParamPtr parseGenericParam();              // returns GenericParamAST*
    ArenaSpan<GenericParamPtr> parseGenericParams();
    TypePtr parseGenericArg();                        // returns TypeAST*
    ArenaSpan<TypePtr> parseGenericArgs();

    // ========================================================================
    // Declaration detection & dispatch
    // ========================================================================
    enum class DeclContext { TopLevel, Local };
    bool isStartOfDeclaration() const;
    bool isStartOfStatement() const;
    bool isStartOfType() const;
    DeclPtr parseTopLevelDecl();                      // returns DeclAST*
    DeclPtr parseDeclaration(DeclContext ctx);

    // ========================================================================
    // Metadata (doc comments + attributes)
    // ========================================================================
    std::optional<DocComment> harvestDocComment();
    AttributePtr parseAttribute();                    // returns AttributeAST*
    AttributeArgPtr parseAttributeArgLiteral();       // returns AttributeArgAST*

    template<typename DeclNode>
    void attachMetadata(DeclNode& node,
                        std::optional<DocComment> doc,
                        std::vector<AttributePtr> attrs) {
        if (doc) node.doc = doc;
        if (!attrs.empty()) {
            auto builder = arena_.template makeBuilder<AttributePtr>();
            for (auto& a : attrs) builder.push_back(a);
            node.attributes = builder.build();
        }
    }

    // ========================================================================
    // Visibility & qualifiers
    // ========================================================================
    Visibility parseVisibility();
    QualifierSet parseQualifiers();

    // ========================================================================
    // Type parsing
    // ========================================================================
    TypePtr parseTypeWithNullable();                  // returns TypeAST*
    TypePtr parseType();                              // returns TypeAST*
    TypePtr parseBaseType();                          // returns TypeAST*
    TypePtr parsePrimitiveType();                     // returns TypeAST*
    TypePtr parseNamedType();                         // returns TypeAST*
    TypePtr parseGenericParamType();                  // returns TypeAST*
    TypePtr parseArrayType();                         // returns TypeAST*
    TypePtr parseGenericArray();                      // returns TypeAST*
    TypePtr parseRefType();                           // returns TypeAST*
    TypePtr parsePtrType();                           // returns TypeAST*
    TypePtr parseFuncType();                          // returns TypeAST*

    // ========================================================================
    // Declaration parsers
    // ========================================================================
    PackageDeclPtr parsePackageDecl();
    UseDeclPtr parseUseDecl(Visibility vis);
    VarDeclPtr parseVarDecl(Visibility vis);
    FuncDeclPtr parseFuncDecl(DeclKeyword kw, Visibility vis);
    StructDeclPtr parseStructDecl(Visibility vis);
    EnumDeclPtr parseEnumDecl(Visibility vis);
    TraitDeclPtr parseTraitDecl(Visibility vis);
    ImplDeclPtr parseImplDecl(Visibility vis);
    FromDeclPtr parseFromDecl(Visibility vis);
    TypeAliasDeclPtr parseTypeAliasDecl(Visibility vis);

    // ---- Sub‑components ----
    FieldDeclPtr parseFieldDecl();                    // returns FieldDeclAST*
    EnumVariantPtr parseEnumVariant();                // returns EnumVariantAST*
    TraitMethodPtr parseTraitMethod();                // returns TraitMethodAST*
    TraitRefPtr parseTraitRef();                      // returns TraitRefAST*
    MethodDeclPtr parseMethodDecl();                  // returns MethodDeclAST*
    ExprPtr parseFuncRef();                           // returns ExprAST* (for impl method assignment)
    FromEntryPtr parseFromEntry();                    // returns FromEntryAST*

    // ========================================================================
    // Expression parsing (Pratt parser)
    // ========================================================================
    ExprPtr parseExpr(bool allowStructLiteral = true);     // returns ExprAST*
    ExprPtr parsePrattExpr(int minPrec, bool allowStructLiteral);
    ExprPtr parsePrefixExpr(bool allowStructLiteral);
    ExprPtr parsePrimaryExpr(bool allowStructLiteral);
    ExprPtr parsePostfixExpr(ExprPtr lhs);

    // ---- Operator handlers ----
    ExprPtr parsePipelineExpr(ExprPtr seed);
    ExprPtr parseComposeExpr(ExprPtr lhs);
    PipelineStepPtr parsePipelineStep();              // returns PipelineStepAST*
    ComposeOperandPtr parseComposeOperand();          // returns ComposeOperandAST*

    // ---- Primary expression factories ----
    ExprPtr parseLiteralExpr();                       // returns ExprAST*
    ExprPtr parseArrayLiteralExpr();                  // returns ExprAST*
    ExprPtr parseStructLiteralExpr(InternedString typeName, ArenaSpan<TypeAST*> genericArgs);
    ExprPtr parseAnonFuncExpr();                      // returns ExprAST*
    ExprPtr parseAwaitExpr(bool allowStructLiteral = true);
    ExprPtr parseIntrinsicCallExpr();                 // returns ExprAST*
    ExprPtr parseMatchExpr();                         // returns ExprAST*
    ExprPtr parseIfExpr(bool allowStructLiteral = true);
    ExprPtr parseRangeExpr(ExprPtr lo, bool allowStructLiteral = true);

    // ---- Call & index ----
    ExprPtr parseCallExpr(ExprPtr callee, ArenaSpan<TypeAST*> genericArgs);
    ExprPtr parseIndexExpr(ExprPtr target);           // returns IndexExprAST*
    ExprPtr parseSliceExpr(ExprPtr target, ExprPtr start, ExprPtr end, bool isExclusive);  // returns SliceExprAST*

    // ---- Precedence helpers ----
    int infixPrec(TokenType type) const;
    BinaryOp tokenToBinaryOp(TokenType type) const;
    AssignOp tokenToAssignOp(TokenType type) const;
    bool isAssignOp(TokenType type) const;

    // ---- Infix dispatch ----
    ExprPtr parseInfixAssign(ExprPtr lhs, bool allowStructLiteral);
    ExprPtr parseInfixIs(ExprPtr lhs);
    ExprPtr parseInfixNullCoalesce(ExprPtr lhs, bool allowStructLiteral);
    ExprPtr parseInfixBinary(ExprPtr lhs, TokenType opTok, int prec, bool allowStructLiteral);

    // ---- Pipeline step cases ----
    PipelineStepPtr parseAnonFuncPipelineStep();
    PipelineStepPtr parseBehaviorPipelineStep(InternedString typeName, ArenaSpan<TypeAST*> genericArgs);
    PipelineStepPtr parseFieldPipelineStep(InternedString ident, ArenaSpan<TypeAST*> genericArgs);
    PipelineStepPtr parseIndexPipelineStep(InternedString ident, ArenaSpan<TypeAST*> genericArgs);
    PipelineStepPtr parseArgPackPipelineStep(InternedString ident, ArenaSpan<TypeAST*> genericArgs);

    // ---- Error handler ----
    OkArmPtr parseOkArm();                            // returns OkArmAST*
    ErrArmPtr parseErrArm();                          // returns ErrArmAST*
    ExprPtr parseResolveExpr();                       // returns ExprAST*

    // ========================================================================
    // Pattern parsing (for match expressions)
    // ========================================================================
    MatchArmPtr parseMatchArm();                      // returns MatchArmAST*
    DefaultArmPtr parseDefaultArm();                  // returns DefaultArmAST*
    PatternPtr parsePattern();                        // returns PatternAST*
    PatternPtr parseLiteralOrRangePattern();
    BindPatternPtr parseBindPattern(InternedString name);
    TypePatternPtr parseTypePattern(InternedString bindName);
    WildcardPatternPtr parseWildcardPattern();
    StructPatternPtr parseStructPattern(InternedString typeName);
    FieldPatternPtr parseFieldPattern();              // returns FieldPatternAST*

    // ========================================================================
    // Lvalue parsing (for assignments)
    // ========================================================================
    ExprPtr parseLvalue();                            // returns ExprAST*

    // ========================================================================
    // Statement parsing
    // ========================================================================
    StmtPtr parseStmt();                              // returns StmtAST*
    BlockStmtPtr parseBlock();
    IfStmtPtr parseIfStmt();
    SwitchStmtPtr parseSwitchStmt();
    SwitchCasePtr parseSwitchCase();                  // returns SwitchCaseAST*
    ForStmtPtr parseForStmt();
    WhileStmtPtr parseWhileStmt();
    DoWhileStmtPtr parseDoWhileStmt();
    ReturnStmtPtr parseReturnStmt();
    BreakStmtPtr parseBreakStmt();
    ContinueStmtPtr parseContinueStmt();
    MultiVarDeclPtr parseMultiVarDecl(std::vector<AttributePtr> attrs = {});
    MultiAssignStmtPtr parseMultiAssignStmt();

    // ========================================================================
    // Lookahead helpers (non‑consuming)
    // ========================================================================
    bool looksLikeType() const;
    bool isFunctionTypeAfterParen(size_t startPos) const;
    bool looksLikeFuncDecl() const;
    bool looksLikeAnonFunc() const;
    bool looksLikeStructLiteral() const;
    bool looksLikeStmtStart() const;
    bool looksLikeDeclStart() const;
    bool looksLikeMultiAssignStart() const;
    bool looksLikeBehaviorAccess() const;
    bool looksLikeGenericArray() const;
    bool looksLikeGenericTypeInstantiation() const;
    static bool isPrimitiveTypeToken(TokenType type);
};
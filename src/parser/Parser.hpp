/**
 * @file Parser.hpp
 * @brief Complete refactor of the Luc parser interface.
 */

#pragma once

#include "Tokens.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/support/ASTArena.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "registry/QualifierRegistry.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <optional>
#include <vector>
#include <string>

// -----------------------------------------------------------------------------
// TokenStream – lightweight wrapper over the token vector.
// -----------------------------------------------------------------------------
class TokenStream {
public:
    TokenStream(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    size_t getPos() const { return pos_; }
    void setPos(size_t pos) { pos_ = pos; }
    const std::vector<Token>& getTokens() const { return tokens_; }
    size_t getTokenCount() const { return tokens_.size(); }
    const Token& getTokenAt(size_t idx) const { return tokens_[idx]; }

    bool isAtEnd() const { return pos_ >= tokens_.size(); }
    const Token& peek() const { return tokens_[pos_]; }
    const Token& peekNext() const { return tokens_[pos_ + 1]; }
    const Token& peekAt(size_t offset) const { return tokens_[pos_ + offset]; }
    Token advance() { return tokens_[pos_++]; }

    TokenType peekType() const { return peek().type; }
    TokenType peekNextType() const { return peekNext().type; }

    bool check(TokenType type) const { return !isAtEnd() && peekType() == type; }
    bool checkAny(std::initializer_list<TokenType> types) const;
    bool match(TokenType type);
    bool matchAny(std::initializer_list<TokenType> types);
    std::optional<Token> consumeIf(TokenType type);
    Token consume(TokenType type, DiagCode code, const std::string& msg);
    Token consume(TokenType type, const std::string& msg);

    SourceLocation currentLoc() const;
    SourceLocation locOf(const Token& tok) const;

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
};

// -----------------------------------------------------------------------------
// QualifierSet – result of parsing ~async, ~nullable, ~parallel.
// -----------------------------------------------------------------------------
struct QualifierSet {
    std::vector<InternedString> raw;   // original names for error messages
    uint32_t bitmask;                  // OR of QualifierBits
    bool isAsync()   const { return (bitmask & QualifierBits::Async) != 0; }
    bool isNullable() const { return (bitmask & QualifierBits::Nullable) != 0; }
    bool isParallel() const { return (bitmask & QualifierBits::Parallel) != 0; }
};

// -----------------------------------------------------------------------------
// Parser – main class
// -----------------------------------------------------------------------------
class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticEngine& dc,
           InternedString filePath, StringPool& pool, ASTArena& arena);

    ASTPtr<ProgramAST> parse();
    bool hasErrors() const { return dc_.hasErrors(); }

private:
    // State
    TokenStream ts_;
    InternedString filePath_;
    StringPool& pool_;
    ASTArena& arena_;
    DiagnosticEngine& dc_;

    // Context flags
    int loopDepth_ = 0;
    int parallelDepth_ = 0;

    // Error handling
    void error(const SourceLocation& loc, DiagCode code, const std::string& msg);
    void errorAt(DiagCode code, const std::string& msg);
    void synchronize();
    void synchronizeTo(std::initializer_list<TokenType> stopTokens);

    // Helpers for list parsing
    void validateAnonFuncBodySig(FuncSignature& declaredSig, const std::string& declName);

    // ---- Temporary list builders (return std::vector) ----
    std::vector<ExprPtr> parseExprList(TokenType endType);
    std::vector<TypePtr> parseTypeList(TokenType endType);
    std::vector<StmtPtr> parseStmtList(TokenType endType);
    std::vector<ParamPtr> parseParamList();           // ends at RPAREN
    std::vector<InternedString> parseModulePath();    // dotted identifiers
    std::vector<AttributePtr> parseAttributes();      // collects '@' attributes

    // ---- Direct storage builders (return ArenaSpan) ----
    ParamGroup parseParamGroup();                                // '(' param-list ')'
    ArenaSpan<TypePtr> parseReturnList();                        // after '->'
    ArenaSpan<GenericParamPtr> parseGenericParams();             // '<' generic-params '>'
    ArenaSpan<TypePtr> parseGenericArgs();                       // '<' type-list '>'
    ArenaSpan<ExprPtr> parseArgList();                           // until ')'

    // Delimited list helper (returns vector, as it's a building block)
    template<typename T>
    std::vector<T> parseDelimitedList(TokenType start, TokenType end,
                                      T (Parser::*parseItem)());

    // Declaration detection & dispatch
    enum class DeclContext { TopLevel, Local };
    bool isStartOfDeclaration() const;
    bool isStartOfStatement() const;
    bool isStartOfType() const;
    DeclPtr parseTopLevelDecl();
    DeclPtr parseDeclaration(DeclContext ctx);

    // Metadata (doc comments + attributes)
    std::optional<DocComment> harvestDocComment();
    AttributePtr parseAttribute();
    AttributeArgPtr parseAttributeArgLiteral();

    // Attach doc and attributes to a declaration node (attrs as vector)
    template<typename DeclNode>
    void attachMetadata(DeclNode& node,
                        std::optional<DocComment> doc,
                        std::vector<AttributePtr> attrs) {
        if (doc) node.doc = std::move(doc);
        if (!attrs.empty()) {
            auto builder = arena_.template makeBuilder<AttributePtr>();
            for (auto& a : attrs) builder.push_back(std::move(a));
            node.attributes = builder.build();
        }
    }

    // Visibility & qualifiers
    Visibility parseVisibility();
    QualifierSet parseQualifiers();

    // Type parsing
    TypePtr parseTypeWithNullable();
    TypePtr parseType();
    TypePtr parseBaseType();
    TypePtr parsePrimitiveType();
    TypePtr parseNamedType();
    TypePtr parseArrayType();
    TypePtr parseRefType();
    TypePtr parsePtrType();
    TypePtr parseFuncType();

    // Declaration parsers (return raw ASTPtr)
    ASTPtr<PackageDeclAST> parsePackageDecl();
    ASTPtr<UseDeclAST> parseUseDecl(Visibility vis);
    ASTPtr<VarDeclAST> parseVarDecl(Visibility vis);
    ASTPtr<FuncDeclAST> parseFuncDecl(DeclKeyword kw, Visibility vis);
    ASTPtr<StructDeclAST> parseStructDecl(Visibility vis);
    ASTPtr<EnumDeclAST> parseEnumDecl(Visibility vis);
    ASTPtr<TraitDeclAST> parseTraitDecl(Visibility vis);
    ASTPtr<ImplDeclAST> parseImplDecl(Visibility vis);
    ASTPtr<FromDeclAST> parseFromDecl(Visibility vis);
    ASTPtr<TypeAliasDeclAST> parseTypeAliasDecl(Visibility vis);

    // Sub‑components
    GenericParamPtr parseGenericParam();
    FieldDeclPtr parseFieldDecl();
    EnumVariantPtr parseEnumVariant();
    TraitMethodPtr parseTraitMethod();
    TraitRefPtr parseTraitRef();
    MethodDeclPtr parseMethodDecl();

    // Expression parsing (Pratt parser)
    ExprPtr parseExpr(bool allowStructLiteral = true);
    ExprPtr parsePrattExpr(int minPrec, bool allowStructLiteral);
    ExprPtr parsePrefixExpr(bool allowStructLiteral);
    ExprPtr parsePrimaryExpr(bool allowStructLiteral);
    ExprPtr parsePostfixExpr(ExprPtr lhs);

    // Operator handlers
    ExprPtr parsePipelineExpr(ExprPtr seed);
    ExprPtr parseComposeExpr(ExprPtr lhs);
    PipelineStepPtr parsePipelineStep();
    ComposeOperandPtr parseComposeOperand();

    // Primary expression factories
    ExprPtr parseLiteralExpr();
    ExprPtr parseArrayLiteralExpr();
    ExprPtr parseStructLiteralExpr(std::string typeName, ArenaSpan<TypePtr> genericArgs);
    ExprPtr parseAnonFuncExpr();
    ExprPtr parseAwaitExpr(bool allowStructLiteral = true);
    ExprPtr parseIntrinsicCallExpr();
    ExprPtr parseMatchExpr();
    ExprPtr parseIfExpr(bool allowStructLiteral = true);
    ExprPtr parseTypeConvExpr(bool isUnsafe, TypePtr targetType);
    ExprPtr parseRangeExpr(ExprPtr lo, bool allowStructLiteral = true);

    // Call & index
    ExprPtr parseCallExpr(ExprPtr callee, ArenaSpan<TypePtr> genericArgs);
    ExprPtr parseIndexExpr(ExprPtr target);

    // Precedence helpers
    int infixPrec(TokenType type) const;
    BinaryOp tokenToBinaryOp(TokenType type) const;
    AssignOp tokenToAssignOp(TokenType type) const;
    bool isAssignOp(TokenType type) const;

    // Infix dispatch
    ExprPtr parseInfixAssign(ExprPtr lhs, bool allowStructLiteral);
    ExprPtr parseInfixIs(ExprPtr lhs);
    ExprPtr parseInfixNullCoalesce(ExprPtr lhs, bool allowStructLiteral);
    ExprPtr parseInfixBinary(ExprPtr lhs, TokenType opTok, int prec, bool allowStructLiteral);

    // Pipeline step cases
    PipelineStepPtr parseAnonFuncPipelineStep();
    PipelineStepPtr parseBehaviorPipelineStep(const std::string& typeName, ArenaSpan<TypePtr> genericArgs);
    PipelineStepPtr parseFieldPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs);
    PipelineStepPtr parseIndexPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs);
    PipelineStepPtr parseArgPackPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs);

    // Pattern parsing (for match)
    MatchArmPtr parseMatchArm();
    ASTPtr<DefaultArmAST> parseDefaultArm();
    ASTPtr<PatternAST> parsePattern();
    ASTPtr<PatternAST> parseLiteralOrRangePattern();
    ASTPtr<BindPatternAST> parseBindPattern(InternedString name);
    ASTPtr<TypePatternAST> parseTypePattern(InternedString bindName);
    ASTPtr<WildcardPatternAST> parseWildcardPattern();
    ASTPtr<StructPatternAST> parseStructPattern(InternedString typeName);
    FieldPatternPtr parseFieldPattern();

    // Lvalue parsing (for assignments)
    ExprPtr parseLvalue();

    // Statement parsing
    StmtPtr parseStmt();
    ASTPtr<BlockStmtAST> parseBlock();
    ASTPtr<IfStmtAST> parseIfStmt();
    ASTPtr<SwitchStmtAST> parseSwitchStmt();
    SwitchCasePtr parseSwitchCase();
    ASTPtr<ForStmtAST> parseForStmt();
    ASTPtr<WhileStmtAST> parseWhileStmt();
    ASTPtr<DoWhileStmtAST> parseDoWhileStmt();
    ASTPtr<ReturnStmtAST> parseReturnStmt();
    ASTPtr<BreakStmtAST> parseBreakStmt();
    ASTPtr<ContinueStmtAST> parseContinueStmt();
    ASTPtr<MultiVarDeclAST> parseMultiVarDecl(std::vector<AttributePtr> attrs = {});
    ASTPtr<MultiAssignStmtAST> parseMultiAssignStmt();

    // Lookahead helpers
    bool looksLikeType() const;
    bool looksLikeFuncDecl() const;
    bool looksLikeAnonFunc() const;
    bool looksLikeStructLiteral() const;
    bool looksLikeStmtStart() const;
    bool looksLikeDeclStart() const;
    bool looksLikeMultiAssignStart() const;
    bool looksLikeBehaviorAccess() const;
    static bool isPrimitiveTypeToken(TokenType type);
};

// -----------------------------------------------------------------------------
// Template implementations
// -----------------------------------------------------------------------------
template<typename T>
std::vector<T> Parser::parseDelimitedList(TokenType start, TokenType end,
                                          T (Parser::*parseItem)()) {
    if (!ts_.match(start)) return {};
    if (ts_.check(end)) {
        ts_.advance();
        return {};
    }
    std::vector<T> result;
    while (!ts_.check(end) && !ts_.isAtEnd()) {
        T item = (this->*parseItem)();
        if (item) result.push_back(std::move(item));
        if (!ts_.match(TokenType::COMMA)) break;
    }
    ts_.consume(end, DiagCode::E2001, 
                "Expected '" + LucDebug::tokenTypeToString(end) + "'");
    return result;
}
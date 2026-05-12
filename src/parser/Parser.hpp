/**
 * @file Parser.hpp
 * @responsibility The Syntax Engine. Transforms a flat Token stream into a structured ProgramAST.
 *
 * @architectural_note
 *   The Parser class is a single large entity split across 5 implementation
 * units (ParserDecl, ParserExpr, etc.) to keep file sizes manageable. It uses a
 * Pratt Parser for expressions and a Recursive Descent approach for declarations.
 *
 *   The parser is registry-free by design. It performs no attribute validation,
 *   qualifier resolution, or intrinsic lookup — those are semantic concerns.
 *   Specifically:
 *     - Qualifier names (~async, ~nullable, ~parallel) are stored as raw
 *       InternedStrings in FuncSignature::rawQualifiers. The semantic phase
 *       resolves them to the qualifiers bitmask and reports unknown names.
 *     - @extern and other attribute semantics (body presence, etc.) are not
 *       enforced here. The parser makes a '=' body optional for all top-level
 *       function declarations; the semantic phase enforces the rules.
 *     - Intrinsic type-vs-value argument disambiguation uses a syntactic
 *       looksLikeType() check; the semantic phase validates correctness.
 *
 * @related_files
 *   - src/lexer/Lexer.hpp, Tokens.hpp (Input provider)
 *   - src/diagnostics/DiagnosticEngine.hpp, DiagnosticCodes.hpp (Error reporting)
 *   - src/ast/BaseAST.hpp (Output definition)
 *   - src/parser/ParserExpr.cpp, ParserDecl.cpp, ParserStmt.cpp, ParserType.cpp (Implementation split)
 */

#pragma once

#include "Tokens.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/support/ASTArena.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "diagnostics/DiagnosticEngine.hpp"
#include "debug/DebugMacros.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Parser
//
// Consumes a flat token stream produced by the Lexer and builds a ProgramAST.
// The implementation is split across five translation units:
//
//   Parser.cpp        — parse(), program root, package decl, top-level
//   dispatch,
//                       doc comment attachment, synchronize()
//   ParserType.cpp    — parseType() and every type sub-rule
//   ParserDecl.cpp    — every top-level declaration sub-rule
//   ParserExpr.cpp    — Pratt expression parser, patterns, pipelines, match
//   ParserStmt.cpp    — statements, blocks, control flow, parallel
//
// All five files include this header and compile against the same class.
// ─────────────────────────────────────────────────────────────────────────────

class Parser {
public:
    // ── Construction ──────────────────────────────────────────────────────────

    // Takes ownership of the token stream produced by Lexer::tokenize().
    // filePath is stored on every ProgramAST node and propagated to
    // SourceLocation::file — it should be the path relative to the package root
    // (e.g. "math/vec2.luc").
    // Parser.hpp
    explicit Parser(std::vector<Token> tokens, DiagnosticEngine &dc,
                    InternedString filePath, StringPool& pool, ASTArena& arena);

    // ── Entry point ───────────────────────────────────────────────────────────

    // Parses the entire token stream and returns the root AST node.
    // Never throws — all errors are collected in errors_.
    // Caller must check hasErrors() before using the returned tree.
    ASTPtr<ProgramAST> parse();

    // ── Error reporting ───────────────────────────────────────────────────────

    bool hasErrors() const {
        return dc_.hasErrors();
    }

private:
    // ── Token stream state ────────────────────────────────────────────────────

    std::vector<Token> tokens_; // full token stream, EOF_TOKEN guaranteed last
    std::size_t pos_ = 0;       // index of the current (not yet consumed) token
    InternedString filePath_;      // stored on ProgramAST and every SourceLocation
    StringPool& pool_;
    ASTArena&   arena_;

    DiagnosticEngine &dc_;      // shared diagnostic engine

    // ─────────────────────────────────────────────────────────────────────────
    // Token stream primitives
    // ─────────────────────────────────────────────────────────────────────────

    // Current token (not yet consumed). Safe at EOF — returns EOF_TOKEN.
    const Token &peek() const;

    // One token ahead of peek(). Safe at EOF.
    const Token &peekNext() const;

    // Two tokens ahead of peek(). Safe at EOF.
    const Token &peekAt(std::size_t offset) const;

    // Consume and return the current token, advancing pos_.
    Token advance();

    // Returns true if the current token matches type — does NOT consume.
    bool check(TokenType type) const;

    // Returns true if the current token matches any of the given types.
    bool checkAny(std::initializer_list<TokenType> types) const;

    // If current token matches type, consume and return true. Otherwise false.
    bool match(TokenType type);

    // If current token matches any of the given types, consume and return true.
    bool matchAny(std::initializer_list<TokenType> types);

    // Consume the current token if it matches type; otherwise record an error
    // using msg and return a dummy token without consuming. Never throws.
    Token consume(TokenType type, DiagCode code, const std::string &msg);

    // Convenience shorthand for the most common consume case: "Expected T"
    // (E2001)
    Token consume(TokenType type, const std::string &msg);

    // True when pos_ is at EOF_TOKEN.
    bool isAtEnd() const;

    // Build a SourceLocation from the current token's line/column + filePath_.
    SourceLocation currentLoc() const;

    // Build a SourceLocation from an already-consumed token.
    SourceLocation locOf(const Token &tok) const;

    // ─────────────────────────────────────────────────────────────────────────
    // Error handling & recovery
    // ─────────────────────────────────────────────────────────────────────────

    // Record an error at loc with message. Does not alter pos_.
    void error(const SourceLocation &loc, DiagCode code, const std::string &msg);

    // Record an error at the current token's position.
    void errorAt(DiagCode code, const std::string &msg);

    // Panic-mode recovery: skip tokens until a statement-boundary or
    // declaration-boundary token is found. Used after emitting an error to
    // resume parsing at a known-good point and surface further errors.
    //
    // Synchronisation stops at: IF ELSE FOR WHILE DO RETURN BREAK CONTINUE
    //                            LET CONST STRUCT ENUM TRAIT IMPL TYPE
    //                            PACKAGE USE AT_SIGN RBRACE EOF_TOKEN
    void synchronize();

    // ─────────────────────────────────────────────────────────────────────────
    // Doc comment harvesting
    //
    // Called in Parser.cpp before each top-level parse call. Inspects the
    // tokens immediately before the current position and, if they are
    // DOC_COMMENT or consecutive line-comment tokens on adjacent lines,
    // returns the assembled DocComment to be attached to the next AST node.
    //
    // Returns nullopt when no doc comment precedes the current token.
    //
    // Attachment priority (highest to lowest):
    //   1. Block doc /-- ... --/     (DOC_COMMENT token)
    //   2. Stacked -- lines above    (requires consecutive IDENTIFIER-free lines)
    //   3. Trailing -- on same line  (attached by the individual parse functions
    //                                 that read the declaration's final token)
    // ─────────────────────────────────────────────────────────────────────────

    std::optional<DocComment> harvestDocComment();

    // Attach a pre-harvested doc comment (if present) to any AST node.
    template <typename T>
    void attachDoc(T &node, std::optional<DocComment> doc) {
        if (doc)
            node.doc = std::move(doc);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Visibility & modifier helpers
    // ─────────────────────────────────────────────────────────────────────────

    // Parses any leading visibility modifier (pub, export) and returns the
    // corresponding Visibility tier. Consumes the token if present.
    // Default is Visibility::Private.
    Visibility parseVisibility();

    // ─────────────────────────────────────────────────────────────────────────
    // ParserType.cpp — type annotation parsing
    // ─────────────────────────────────────────────────────────────────────────

    // Root type parser.
    // Called wherever a type annotation is expected.
    TypePtr parseType();

    // Parse a single type (primitive, named, array, ref, ptr, func).
    TypePtr parseBaseType();

    // Primitive keyword → PrimitiveTypeAST. Returns nullptr if current token
    // is not a type keyword (caller decides whether that is an error).
    TypePtr parsePrimitiveType();

    // Named / generic type: IDENTIFIER [ '<' type { ',' type } '>' ]
    TypePtr parseNamedType();

    // Array types — disambiguates [N]T, []T, [*]T from the '[' token.
    TypePtr parseArrayType();

    // &T — safe managed reference.
    TypePtr parseRefType();

    // *T — raw pointer, only valid on @extern-decorated declarations.
    // The semantic pass enforces the restriction; the parser
    // produces PtrTypeAST regardless of context.
    TypePtr parsePtrType();

    // func_type := [ qualifier_list ] param_group { param_group } [ '->' return_list ]
    TypePtr parseFuncType();

    // Wrap a TypePtr in NullableTypeAST if the next token is '?'.
    TypePtr wrapNullable(TypePtr inner);

    // '<' type { ',' type } '>' — generic argument list on the use side.
    // Used by parseNamedType() and parseCallExpr() for explicit instantiation.
    std::vector<TypePtr> parseGenericArgs();

    // ─────────────────────────────────────────────────────────────────────────
    // ParserDecl.cpp — top-level declaration parsing
    // ─────────────────────────────────────────────────────────────────────────

    // Top-level dispatch: looks at the current token (after optional pub/extern)
    // and calls the appropriate sub-parser. Returns nullptr on unrecognised input
    // (caller records an error and calls synchronize()).
    DeclPtr parseTopLevelDecl();

    // package IDENTIFIER
    ASTPtr<PackageDeclAST> parsePackageDecl();

    // [vis] use module_path [ as IDENTIFIER ]
    ASTPtr<UseDeclAST> parseUseDecl(Visibility vis);

    // [vis] let / const IDENTIFIER type [ '=' expr ]
    ASTPtr<VarDeclAST> parseVarDecl(Visibility vis, std::vector<AttributePtr> attrs = {});

    // [vis] let / const IDENTIFIER [<generics>] param_group+ [return_type]
    // '=' body Distinguishes a function from a variable by lookahead after the
    // name:
    //   function → next meaningful token after name (and optional generics) is '('
    //   variable → type keyword, named type, '[', '&', '*', or '?'
    ASTPtr<FuncDeclAST> parseFuncDecl(DeclKeyword kw, Visibility vis, std::vector<AttributePtr> attrs = {});

    // Parse one parameter group '(' [ param_list ] ')'.
    // Used by parseFuncDecl and parseFuncType.
    ParamGroup parseParamGroup();

    // return_list := return_type { ',' return_type }
    std::vector<TypePtr> parseReturnList();

    // Parse generic parameter list: '<' generic_param { ',' generic_param } '>'
    std::vector<GenericParamPtr> parseGenericParams();

    // Parse one generic parameter: IDENTIFIER [ ':' IDENTIFIER { '+' IDENTIFIER }
    // ]
    GenericParamPtr parseGenericParam();

    // [vis] struct IDENTIFIER [<generics>] '{' { field_decl } '}'
    ASTPtr<StructDeclAST> parseStructDecl(Visibility vis);

    // IDENTIFIER type [ '=' expr ]  — inside a struct body
    FieldDeclPtr parseFieldDecl();

    // [vis] enum IDENTIFIER '{' enum_variant { ',' enum_variant } '}'
    ASTPtr<EnumDeclAST> parseEnumDecl(Visibility vis);

    // IDENTIFIER [ '=' INT_LITERAL ]
    EnumVariantPtr parseEnumVariant();

    // [vis] trait IDENTIFIER [<generics>] '{' { trait_method } '}'
    ASTPtr<TraitDeclAST> parseTraitDecl(Visibility vis);

    // IDENTIFIER '(' [ param_list ] ')' [ return_type ]  — signature only, no
    // body
    TraitMethodPtr parseTraitMethod();

    // [vis] impl [<generics>] IDENTIFIER [generic_args] [':' trait_ref] '{'
    // members '}'
    ASTPtr<ImplDeclAST> parseImplDecl(Visibility vis);

    // ':' IDENTIFIER [ '<' type_args '>' ] — the trait conformance annotation
    TraitRefPtr parseTraitRef();

    // IDENTIFIER '(' params ')' [ return_type ] '=' body — inside impl
    MethodDeclPtr parseMethodDecl();

    // [vis] from '(' IDENTIFIER type ')' IDENTIFIER '=' body
    ASTPtr<FromDeclAST> parseFromDecl(Visibility vis);

    // [vis] type IDENTIFIER [<generics>] '=' type
    ASTPtr<TypeAliasDeclAST> parseTypeAliasDecl(Visibility vis);

    // ── Anon-func body signature helper ──────────────────────────────────

    // Called when a func/method body uses the verbose anon-func form:
    //   = (params...) RetType { ... }
    //   = (a int) (b int) RetType { ... }   ← curried form
    //
    // Responsibilities:
    //   1. Consume ALL repeated parameter groups (while LPAREN).
    //   2. Optionally parse a repeated return type annotation.
    //   3. Compare against declaredSig.returnType:
    //        • Kinds match         → no diagnostic.
    //        • Kinds differ        → W3001 warning; header type is canonical.
    //        • Header has no type  → adopt body's return type (permissive).
    //
    // After returning, the parser sits immediately before '{' (or at a bad
    // token if malformed — the caller owns the final LBRACE check).
    void validateAnonFuncBodySig(FuncSignature& declaredSig,
                                 const std::string& declName);

    // ── Compiler Directive parsers ────────────────────────────────────────

    // Parse one '@' directive: '@' IDENTIFIER [ '(' attr_args ')' ]
    // Called before any declaration that may carry attributes.
    // Returns nullptr if current token is not AT_SIGN.
    AttributePtr parseAttribute();

    // Parse zero or more '@' directives that precede a declaration.
    // Stops when the current token is not AT_SIGN.
    std::vector<AttributePtr> parseAttributes();

    // ─────────────────────────────────────────────────────────────────────────
    // ParserExpr.cpp — Pratt expression parser
    // ─────────────────────────────────────────────────────────────────────────

    // Root expression entry point. Handles assignment operators (lowest
    // precedence).
    //
    // allowStructLiteral: When false, prevents an IDENTIFIER followed by '{'
    // from being parsed as a StructLiteralExprAST. This is used in control-flow
    // headers (if, for, while) to avoid greedily consuming the following block.
    ExprPtr parseExpr(bool allowStructLiteral = true);

    // Pratt parser core. minPrec controls which operators are consumed at this
    // level — call with 0 to parse a full expression.
    ExprPtr parsePrattExpr(int minPrec, bool allowStructLiteral = true);

    // Parse a prefix (unary) or primary expression.
    ExprPtr parsePrefixExpr(bool allowStructLiteral = true);

    // Parse a primary expression: literal, identifier, struct literal, array
    // literal, parenthesised expr, anon func, match expr, if expr.
    ExprPtr parsePrimaryExpr(bool allowStructLiteral = true);

    // Parse all postfix operations on an already-parsed lhs:
    // '.' field, ':' method, '?.' chain, '??' fallback,
    // '[' index / slice, '(' call, '!' arg-pack suffix.
    // Returns the fully-decorated expression.
    ExprPtr parsePostfixExpr(ExprPtr lhs);

    // ── Operator-specific parsers (called from parsePrattExpr) ────────────────

    // Binary infix: BinaryExprAST for arithmetic, comparison, logical, bitwise.
    ExprPtr parseBinaryExpr(ExprPtr lhs, BinaryOp op, int nextPrec);

    // Assignment: '=' and compound operators.
    ExprPtr parseAssignExpr(ExprPtr lhs, AssignOp op);

    // is-expression: expr 'is' type — IsExprAST.
    ExprPtr parseIsExpr(ExprPtr lhs);

    // Pipeline: lhs '->' step { '->' step }
    // Consumes the entire chain starting from the first '->' after lhs.
    ExprPtr parsePipelineExpr(ExprPtr seed);

    // Parse a single pipeline step (after '->' has been consumed).
    PipelineStepPtr parsePipelineStep();

    // Compose: lhs '+>' operand { '+>' operand }
    ExprPtr parseComposeExpr(ExprPtr lhs);

    // Parse a single compose operand (after '+>' has been consumed).
    ComposeOperandPtr parseComposeOperand();

    // ── Primary sub-parsers ───────────────────────────────────────────────────

    // INT, FLOAT, STRING, RAW_STRING, CHAR, HEX, BINARY, true, false, nil
    ExprPtr parseLiteralExpr();

    // Array literal: '[' [ expr { ',' expr } ] ']'
    ExprPtr parseArrayLiteralExpr();

    // Struct literal: IDENTIFIER [generic_args] '{' { IDENTIFIER '=' expr } '}'
    // Called from parsePrimaryExpr when IDENTIFIER is followed by '{' (and the
    // previous token context rules out a block statement).
    ExprPtr parseStructLiteralExpr(std::string typeName, std::vector<TypePtr> genericArgs);

    // Anonymous function: [ async ] '(' params ')' [ return_type ] block
    ExprPtr parseAnonFuncExpr();

    // await expr
    ExprPtr parseAwaitExpr();

    // #intrinsicName ( [ type | expr, ... ] )
    ExprPtr parseIntrinsicCallExpr();

    // match expr '{' arm* default_arm '}'
    ExprPtr parseMatchExpr();

    // if expr block else block  — expression form (else required)
    ExprPtr parseIfExpr();

    // Explicit type cast: IDENTIFIER '(' expr ')'  where IDENTIFIER is a type name
    // or '*' IDENTIFIER '(' expr ')' for unsafe bit reinterpret.
    ExprPtr parseTypeConvExpr(bool isUnsafe, TypePtr targetType);

    // Range: expr '..' expr  — used in for loops, match patterns, slice indexing.
    ExprPtr parseRangeExpr(ExprPtr lo);

    // ── Call & index ─────────────────────────────────────────────────────────

    // '(' [ arg_list ] ')' postfix — builds CallExprAST on the given callee.
    ExprPtr parseCallExpr(ExprPtr callee, std::vector<TypePtr> genericArgs);

    // '[' expr ']' or '[' expr '..' expr ']' postfix — IndexExprAST.
    ExprPtr parseIndexExpr(ExprPtr target);

    // ── Argument list ─────────────────────────────────────────────────────────

    // Parse comma-separated expressions until ')' — used by parseCallExpr.
    // Does not consume the closing ')'.
    std::vector<ExprPtr> parseArgList();

    // ── Precedence table ──────────────────────────────────────────────────────

    // Return the infix precedence of the current token, or -1 if it is not an
    // infix operator. Mirrors the operator precedence table in LUC_GRAMMAR.md.
    int infixPrec(TokenType type) const;

    // Return the BinaryOp for an infix operator token.
    // Precondition: infixPrec(type) >= 0 and type is a binary (not assign) op.
    BinaryOp tokenToBinaryOp(TokenType type) const;

    // Return the AssignOp for an assignment token.
    AssignOp tokenToAssignOp(TokenType type) const;

    // True if the current token is an infix assignment operator
    // ('=' '+=', '-=', '*=', '/=', '^=', '%=').
    bool isAssignOp(TokenType type) const;

    // ── Pattern parsing (called from parseMatchExpr) ──────────────────────────

    // Parse one match arm: pattern_list [guard] '->' body
    MatchArmPtr parseMatchArm();

    // Parse the default arm: 'default' '->' body
    ASTPtr<DefaultArmAST> parseDefaultArm();

    // Dispatch to the correct pattern sub-parser based on current token.
    ASTPtr<PatternAST> parsePattern();

    // Literal or range pattern: literal [ '..' [ '<' ] literal ]
    ASTPtr<PatternAST> parseLiteralOrRangePattern();

    // Bind pattern: IDENTIFIER  (not followed by 'is' or '{')
    ASTPtr<BindPatternAST> parseBindPattern(InternedString name);

    // Type pattern: IDENTIFIER 'is' type
    ASTPtr<TypePatternAST> parseTypePattern(InternedString bindName);

    // Wildcard pattern: '_'
    ASTPtr<WildcardPatternAST> parseWildcardPattern();

    // Struct pattern: IDENTIFIER '{' { field_pattern } '}'
    ASTPtr<StructPatternAST> parseStructPattern(InternedString typeName);

    // One field entry inside a struct pattern: IDENTIFIER [ ':' pattern ]
    FieldPatternPtr parseFieldPattern();

    // ─────────────────────────────────────────────────────────────────────────
    // ParserStmt.cpp — statement parsing
    // ─────────────────────────────────────────────────────────────────────────

    // Root statement dispatcher. Called in a loop by parseBlock().
    StmtPtr parseStmt();

    // '{' { stmt } '}'
    ASTPtr<BlockStmtAST> parseBlock();

    // if expr block [ else ( if_stmt | block ) ]  — statement form
    ASTPtr<IfStmtAST> parseIfStmt();

    // switch expr '{' { case_clause } [ default_clause ] '}'
    ASTPtr<SwitchStmtAST> parseSwitchStmt();

    // One case clause: 'case' case_value { ',' case_value } ':' { stmt }
    SwitchCasePtr parseSwitchCase();

    // for IDENTIFIER in expr block
    ASTPtr<ForStmtAST> parseForStmt();

    // while expr block
    ASTPtr<WhileStmtAST> parseWhileStmt();

    // do block while expr
    ASTPtr<DoWhileStmtAST> parseDoWhileStmt();

    // return [ expr ]
    ASTPtr<ReturnStmtAST> parseReturnStmt();

    // break
    ASTPtr<BreakStmtAST> parseBreakStmt();

    // continue
    ASTPtr<ContinueStmtAST> parseContinueStmt();

    // Local declaration inside a block: let / const → VarDeclAST or
    // FuncDeclAST. Wrapped in DeclStmtAST. pub is forbidden inside a block —
    // recorded as an error if encountered and then ignored so parsing continues.
    ASTPtr<DeclStmtAST> parseLocalDecl();

    // ─────────────────────────────────────────────────────────────────────────
    // Context flags
    //
    // These are set / cleared by statement and expression parsers to enforce
    // semantic restrictions that are most naturally caught at parse time.
    // The semantic pass re-checks them against the full AST, but early
    // detection here gives better error messages.
    // ─────────────────────────────────────────────────────────────────────────

    // Incremented when entering a loop body (for/while/do); decremented on exit.
    // parseBreakStmt() / parseContinueStmt() record an error if this is 0.
    int loopDepth_ = 0;

    // Incremented when entering a parallel body; decremented on exit.
    // parseAwaitExpr(), parseReturnStmt(), parseBreakStmt(), parseContinueStmt()
    // all record an error if this is > 0.
    int parallelDepth_ = 0;

    // ─────────────────────────────────────────────────────────────────────────
    // Disambiguation helpers
    //
    // Several grammar constructs share a leading token and require lookahead
    // to resolve. These predicates encapsulate the lookahead logic so the
    // individual parse functions stay readable.
    // ─────────────────────────────────────────────────────────────────────────

    // True when the current position looks like the start of a type annotation:
    // a primitive keyword, IDENTIFIER, '[', '&', '*', or '(' followed by a
    // parameter-list-then-')' shape (function type).
    bool looksLikeType() const;

    /**
    * @brief Determines whether a token type is a primitive type keyword.
    *
    * Primitive types include bool, byte, short, int, long, ubyte, ushort, uint,
    * ulong, int8, int16, int32, int64, uint8, uint16, uint32, uint64, float,
    * double, decimal, string, char, any.
    *
    * This helper centralises the 22‑case switch that previously appeared in
    * five different locations (looksLikeStmtStart, parsePipelineStep,
    * parseComposeOperand, parseFuncType, and the isTypeStart lambda in
    * parsePrimaryExpr). It is static because it only inspects the TokenType
    * enum and does not depend on any parser instance state.
    */
    static bool isPrimitiveTypeToken(TokenType type);

    // True when current token is IDENTIFIER and the token after any optional
    // generic args '<...>' is '(' — indicating a function declaration rather
    // than a variable declaration.
    bool looksLikeFuncDecl() const;

    /**
    * @brief Performs a non‑consuming lookahead to detect an anonymous function.
    *
    * The grammar for an anonymous function is:
    *   [ '~' IDENTIFIER ]* '(' [ param_list ] ')' [ return_type ] block
    * where param_list is ( IDENTIFIER type [ ',' ... ] ) and block is '{' ... '}'.
    * Currying (multiple parameter groups) is supported, e.g. (a int) (b int) int { ... }.
    *
    * This function returns true when the current token stream matches a valid
    * anonymous function header. It never modifies the parser position.
    *
    * Used in parsePipelineStep to avoid misclassifying a parenthesised expression
    * (e.g., (x + y)) as an anonymous function, which would produce confusing
    * error messages.
    */
    bool looksLikeAnonFunc() const;

    // True when current token is IDENTIFIER followed immediately by '{' in an
    // expression context — indicating a struct literal rather than a block.
    // Uses the preceding token to distinguish: a struct literal cannot follow
    // '=', 'return', or another operator without being a valid struct literal,
    // but a bare IDENTIFIER '{' after a statement boundary is a block.
    bool looksLikeStructLiteral() const;

    // True when the current token starts a statement: a keyword from the
    // control-flow / declaration set, or any expression-starter.
    bool looksLikeStmtStart() const;

    // True if the current token is the start of a top-level declaration
    // (used by synchronize() to know when to stop skipping).
    bool looksLikeDeclStart() const;
};
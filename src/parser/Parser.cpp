/**
 * @file Parser.cpp
 * 
 * Core parsing infrastructure and top-level dispatch.
 * Implements TokenStream, error recovery, list helpers,
 * attribute parsing, and top-level declaration dispatch.
 */

#include "Parser.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

// -----------------------------------------------------------------------------
// TokenStream implementation
// -----------------------------------------------------------------------------

bool TokenStream::checkAny(std::initializer_list<TokenType> types) const {
    for (TokenType t : types)
        if (check(t)) return true;
    return false;
}

bool TokenStream::match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

bool TokenStream::matchAny(std::initializer_list<TokenType> types) {
    for (TokenType t : types)
        if (match(t)) return true;
    return false;
}

std::optional<Token> TokenStream::consumeIf(TokenType type) {
    if (check(type)) return advance();
    return std::nullopt;
}

Token TokenStream::consume(TokenType type, DiagCode code, const std::string& msg) {
    if (check(type)) return advance();
    return {type, "", 0, 0};
}

Token TokenStream::consume(TokenType type, const std::string& msg) {
    return consume(type, DiagCode::E2001, msg);
}

SourceLocation TokenStream::currentLoc() const {
    return locOf(peek());
}

SourceLocation TokenStream::locOf(const Token& tok) const {
    return SourceLocation(static_cast<uint32_t>(tok.line),
                          static_cast<uint32_t>(tok.column));
}

// -----------------------------------------------------------------------------
// Parser construction
// -----------------------------------------------------------------------------

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& dc,
               InternedString filePath, StringPool& pool, ASTArena& arena)
    : ts_(std::move(tokens)), filePath_(std::move(filePath)),
      pool_(pool), arena_(arena), dc_(dc) {}

// -----------------------------------------------------------------------------
// Error handling
// -----------------------------------------------------------------------------

void Parser::error(const SourceLocation& loc, DiagCode code, const std::string& msg) {
    dc_.error(DiagnosticCategory::Syntax, filePath_, loc, code, {msg});
}

void Parser::errorAt(DiagCode code, const std::string& msg) {
    error(ts_.currentLoc(), code, msg);
}

void Parser::synchronize() {
    synchronizeTo({
        TokenType::AT_SIGN, TokenType::PACKAGE, TokenType::USE,
        TokenType::PUB, TokenType::EXPORT, TokenType::STRUCT,
        TokenType::ENUM, TokenType::TRAIT, TokenType::IMPL,
        TokenType::TYPE, TokenType::FROM, TokenType::LET,
        TokenType::CONST, TokenType::IF, TokenType::FOR,
        TokenType::WHILE, TokenType::DO, TokenType::RETURN,
        TokenType::BREAK, TokenType::CONTINUE, TokenType::MATCH,
        TokenType::SWITCH, TokenType::RBRACE
    });
}

void Parser::synchronizeTo(std::initializer_list<TokenType> stopTokens) {
    while (!ts_.isAtEnd()) {
        if (ts_.checkAny(stopTokens))
            return;
        ts_.advance();
    }
}

// -----------------------------------------------------------------------------
// Visibility
// -----------------------------------------------------------------------------

Visibility Parser::parseVisibility() {
    if (ts_.match(TokenType::PUB)) return Visibility::Package;
    if (ts_.match(TokenType::EXPORT)) return Visibility::Export;
    return Visibility::Private;
}

// -----------------------------------------------------------------------------
// Qualifiers
// -----------------------------------------------------------------------------

QualifierSet Parser::parseQualifiers() {
    QualifierSet qs;
    auto& registry = QualifierRegistry::instance();
    
    while (ts_.check(TokenType::TILDE)) {
        SourceLocation loc = ts_.currentLoc();
        ts_.advance();
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            error(loc, DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        InternedString name = pool_.intern(ts_.advance().value);
        
        const QualifierInfo* info = registry.lookup(name);
        if (!info) {
            error(loc, DiagCode::E2010, 
                  "unknown qualifier '~" + std::string(pool_.lookup(name)) + 
                  "'; known qualifiers: " + registry.allNames());
            continue;
        }
        
        qs.raw.push_back(name);
        qs.bitmask |= info->bit;
    }
    return qs;
}

// -----------------------------------------------------------------------------
// Module path parsing
// -----------------------------------------------------------------------------

std::vector<InternedString> Parser::parseModulePath() {
    std::vector<InternedString> path;
    if (!ts_.check(TokenType::IDENTIFIER)) return path;
    path.push_back(pool_.intern(ts_.advance().value));
    while (ts_.match(TokenType::DOT)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after '.'");
            break;
        }
        path.push_back(pool_.intern(ts_.advance().value));
    }
    return path;
}

// -----------------------------------------------------------------------------
// List helpers (temporary vector builders)
// -----------------------------------------------------------------------------

std::vector<ExprPtr> Parser::parseExprList(TokenType endType) {
    std::vector<ExprPtr> list;
    while (!ts_.check(endType) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        ExprPtr expr = parseExpr();
        if (expr) list.push_back(std::move(expr));
    }
    ts_.consume(endType, "expected '" + LucDebug::tokenTypeToString(endType) + "'");
    return list;
}

std::vector<TypePtr> Parser::parseTypeList(TokenType endType) {
    std::vector<TypePtr> list;
    while (!ts_.check(endType) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        TypePtr ty = parseType();
        if (ty) list.push_back(std::move(ty));
    }
    ts_.consume(endType, "expected '" + LucDebug::tokenTypeToString(endType) + "'");
    return list;
}

std::vector<StmtPtr> Parser::parseStmtList(TokenType endType) {
    std::vector<StmtPtr> list;
    while (!ts_.check(endType) && !ts_.isAtEnd()) {
        StmtPtr stmt = parseStmt();
        if (stmt) list.push_back(std::move(stmt));
    }
    ts_.consume(endType, "expected '" + LucDebug::tokenTypeToString(endType) + "'");
    return list;
}

std::vector<ParamPtr> Parser::parseParamList() {
    std::vector<ParamPtr> list;
    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected parameter name");
            break;
        }
        auto param = arena_.make<ParamAST>();
        param->name = pool_.intern(ts_.advance().value);
        param->isVariadic = ts_.match(TokenType::VARIADIC);
        param->type = parseType();
        if (param->type) list.push_back(std::move(param));
    }
    return list;
}

// -----------------------------------------------------------------------------
// Delimited list helper template instantiations
// -----------------------------------------------------------------------------

template std::vector<ExprPtr> Parser::parseDelimitedList<ExprPtr>(
    TokenType start, TokenType end, ExprPtr (Parser::*parseItem)());
template std::vector<TypePtr> Parser::parseDelimitedList<TypePtr>(
    TokenType start, TokenType end, TypePtr (Parser::*parseItem)());
template std::vector<ParamPtr> Parser::parseDelimitedList<ParamPtr>(
    TokenType start, TokenType end, ParamPtr (Parser::*parseItem)());

// -----------------------------------------------------------------------------
// Doc comment harvesting
// -----------------------------------------------------------------------------

std::optional<DocComment> Parser::harvestDocComment() {
    const auto& tokens = ts_.getTokens();
    size_t pos = ts_.getPos();
    
    if (pos == 0) return std::nullopt;
    
    int declLine = ts_.peek().line;
    std::optional<std::string> trailingText;
    std::vector<std::string> stackedLines;
    int stackedTopLine = -1;
    std::optional<std::string> blockText;
    
    for (size_t i = pos; i > 0; ) {
        --i;
        const Token& t = tokens[i];
        
        if (t.type == TokenType::LINE_COMMENT) {
            if (t.line <= 0) continue;
            if (t.line == declLine) {
                if (!trailingText.has_value()) {
                    trailingText = t.value;
                }
                continue;
            }
            if (stackedLines.empty()) {
                if (declLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            } else {
                if (stackedTopLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            }
        }
        
        if (t.type == TokenType::DOC_COMMENT) {
            if (t.line <= 0) continue;
            if (declLine - t.line <= 1) {
                blockText = t.value;
            }
            break;
        }
        
        break;
    }
    
    if (blockText.has_value()) {
        return DocComment{pool_.intern(*blockText), DocCommentForm::Block};
    }
    if (!stackedLines.empty()) {
        std::string combined;
        for (int i = static_cast<int>(stackedLines.size()) - 1; i >= 0; --i) {
            if (!combined.empty()) combined += '\n';
            combined += stackedLines[i];
        }
        return DocComment{pool_.intern(combined), DocCommentForm::Stacked};
    }
    if (trailingText.has_value()) {
        return DocComment{pool_.intern(*trailingText), DocCommentForm::Trailing};
    }
    
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// Attribute parsing
// -----------------------------------------------------------------------------

std::vector<AttributePtr> Parser::parseAttributes() {
    std::vector<AttributePtr> attrs;
    while (ts_.check(TokenType::AT_SIGN)) {
        AttributePtr attr = parseAttribute();
        if (attr) attrs.push_back(std::move(attr));
    }
    return attrs;
}

AttributePtr Parser::parseAttribute() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AT_SIGN, "expected '@'");
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected attribute name after '@'");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    auto attr = arena_.make<AttributeAST>();
    attr->name = name;
    attr->loc = loc;

    if (ts_.match(TokenType::LPAREN)) {
        std::vector<AttributeArgPtr> args;
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            if (!args.empty() && !ts_.match(TokenType::COMMA))
                break;
            AttributeArgPtr arg = parseAttributeArgLiteral();
            if (arg) args.push_back(std::move(arg));
        }
        ts_.consume(TokenType::RPAREN, "expected ')' after attribute arguments");
        
        auto builder = arena_.makeBuilder<AttributeArgPtr>();
        for (auto& a : args) builder.push_back(std::move(a));
        attr->args = builder.build();
    }
    return attr;
}

AttributeArgPtr Parser::parseAttributeArgLiteral() {
    if (ts_.check(TokenType::STRING_LITERAL)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::StringLit,
                                            pool_.intern(ts_.advance().value));
    }
    if (ts_.check(TokenType::INT_LITERAL) || ts_.check(TokenType::HEX_LITERAL) ||
        ts_.check(TokenType::BINARY_LITERAL)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::IntLit,
                                            pool_.intern(ts_.advance().value));
    }
    if (ts_.check(TokenType::TRUE) || ts_.check(TokenType::FALSE)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::BoolLit,
                                            pool_.intern(ts_.advance().value));
    }
    if (ts_.check(TokenType::IDENTIFIER)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::TypeIdent,
                                            pool_.intern(ts_.advance().value));
    }
    errorAt(DiagCode::E2002, "expected string, integer, boolean, or type identifier in attribute argument");
    return nullptr;
}

// -----------------------------------------------------------------------------
// Top-level parsing
// -----------------------------------------------------------------------------

ASTPtr<ProgramAST> Parser::parse() {
    auto program = arena_.make<ProgramAST>();
    program->filePath = filePath_;
    program->loc = ts_.currentLoc();

    std::vector<DeclPtr> decls;

    // Package declaration
    if (!ts_.check(TokenType::PACKAGE)) {
        errorAt(DiagCode::E2001, "expected 'package' declaration at start of file");
        synchronize();
        auto dummy = arena_.make<PackageDeclAST>(pool_.intern("<unknown>"));
        dummy->loc = ts_.currentLoc();
        program->packageName = pool_.intern("<error>");
        decls.push_back(std::move(dummy));
    } else {
        auto pkg = parsePackageDecl();
        if (pkg) {
            program->packageName = pkg->name;
            decls.push_back(std::move(pkg));
        } else {
            auto dummy = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
            dummy->loc = ts_.currentLoc();
            program->packageName = pool_.intern("<error>");
            decls.push_back(std::move(dummy));
        }
    }

    // Top-level declarations
    while (!ts_.isAtEnd()) {
        auto doc = harvestDocComment();
        DeclPtr decl = parseTopLevelDecl();
        if (decl) {
            if (doc) decl->doc = std::move(doc);
            decls.push_back(std::move(decl));
        } else {
            synchronize();
        }
    }

    // Build the ArenaSpan for program->decls
    auto builder = arena_.makeBuilder<DeclPtr>();
    for (auto& d : decls) builder.push_back(std::move(d));
    program->decls = builder.build();

    return program;
}

DeclPtr Parser::parseTopLevelDecl() {
    return parseDeclaration(DeclContext::TopLevel);
}

DeclPtr Parser::parseDeclaration(DeclContext ctx) {
    // Parse attributes (temporary vector)
    std::vector<AttributePtr> attrs = parseAttributes();

    // Parse visibility (top-level only)
    Visibility vis = Visibility::Private;
    if (ctx == DeclContext::TopLevel) {
        vis = parseVisibility();
    } else {
        if (ts_.checkAny({TokenType::PUB, TokenType::EXPORT})) {
            errorAt(DiagCode::E2014, "visibility modifier not allowed in local declaration");
            ts_.advance();
        }
    }

    // Dispatch to specific declaration parser
    DeclPtr decl;
    
    if (ts_.check(TokenType::USE)) {
        decl = parseUseDecl(vis);
    } else if (ts_.check(TokenType::STRUCT)) {
        decl = parseStructDecl(vis);
    } else if (ts_.check(TokenType::ENUM)) {
        decl = parseEnumDecl(vis);
    } else if (ts_.check(TokenType::TRAIT)) {
        decl = parseTraitDecl(vis);
    } else if (ts_.check(TokenType::IMPL)) {
        decl = parseImplDecl(vis);
    } else if (ts_.check(TokenType::FROM)) {
        decl = parseFromDecl(vis);
    } else if (ts_.check(TokenType::TYPE)) {
        decl = parseTypeAliasDecl(vis);
    } else if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        Token kwTok = ts_.advance();
        DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;
        if (looksLikeFuncDecl()) {
            decl = parseFuncDecl(kw, vis);
        } else {
            decl = parseVarDecl(vis);
        }
    } else {
        errorAt(DiagCode::E2002, "expected declaration");
        return nullptr;
    }

    if (decl) {
        // Attach attributes (convert vector to span)
        if (!attrs.empty()) {
            auto builder = arena_.makeBuilder<AttributePtr>();
            for (auto& a : attrs) builder.push_back(std::move(a));
            decl->attributes = builder.build();
        }
        decl->loc = ts_.currentLoc();
    }
    
    return decl;
}
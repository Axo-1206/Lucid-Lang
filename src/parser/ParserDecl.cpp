/**
 * @file ParserDecl.cpp
 * 
 * Parses all declaration-oriented grammar rules.
 */

#include "Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

// -----------------------------------------------------------------------------
// validateAnonFuncBodySig
// -----------------------------------------------------------------------------

void Parser::validateAnonFuncBodySig(FuncSignature& declaredSig, const std::string& declName) {
    if (ts_.check(TokenType::TILDE)) {
        errorAt(DiagCode::E2015, "anonymous function body cannot have qualifiers");
        while (ts_.check(TokenType::TILDE)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            ts_.advance();
        }
    }

    // Parse and discard repeated parameter groups
    while (ts_.check(TokenType::LPAREN)) {
        parseParamGroup();
    }

    bool hasArrow = ts_.match(TokenType::ARROW);
    ArenaSpan<TypePtr> bodyReturnTypes;

    if (hasArrow) {
        bodyReturnTypes = parseReturnList();
        if (bodyReturnTypes.empty()) {
            errorAt(DiagCode::E2005, "expected at least one return type after '->'");
        }
    }

    if (declaredSig.returnTypes.empty()) {
        if (hasArrow && !bodyReturnTypes.empty()) {
            declaredSig.returnTypes = bodyReturnTypes;
        } else if (hasArrow) {
            errorAt(DiagCode::E2005, "expected return type after '->' for function '" + declName + "'");
        }
    } else if (!hasArrow) {
        errorAt(DiagCode::E2001, "expected '->' return list in body for function '" + declName + "'");
    } else {
        if (declaredSig.returnTypes.size() != bodyReturnTypes.size()) {
            errorAt(DiagCode::E2005, "return type count mismatch for function '" + declName + "'");
        } else {
            for (size_t i = 0; i < declaredSig.returnTypes.size(); ++i) {
                if (declaredSig.returnTypes[i]->kind != bodyReturnTypes[i]->kind) {
                    errorAt(DiagCode::W3001, "return type #" + std::to_string(i) + " mismatch");
                    break;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// parsePackageDecl
// -----------------------------------------------------------------------------

ASTPtr<PackageDeclAST> Parser::parsePackageDecl() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::PACKAGE, "expected 'package'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected package name");
        auto node = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
        node->loc = loc;
        return node;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    auto node = arena_.make<PackageDeclAST>(name);
    node->loc = loc;
    return node;
}

// -----------------------------------------------------------------------------
// parseUseDecl
// -----------------------------------------------------------------------------

ASTPtr<UseDeclAST> Parser::parseUseDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::USE, "expected 'use'");

    auto node = arena_.make<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected module path after 'use'");
        return node;
    }

    std::vector<InternedString> path;
    path.push_back(pool_.intern(ts_.advance().value));

    while (ts_.check(TokenType::DOT) && ts_.peekNextType() == TokenType::IDENTIFIER) {
        ts_.advance();
        path.push_back(pool_.intern(ts_.advance().value));
    }

    auto builder = arena_.makeBuilder<InternedString>();
    for (auto& p : path) builder.push_back(std::move(p));
    node->path = builder.build();

    if (ts_.match(TokenType::AS)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected alias name after 'as'");
        } else {
            node->alias = pool_.intern(ts_.advance().value);
        }
    }

    return node;
}

// -----------------------------------------------------------------------------
// parseVarDecl
// -----------------------------------------------------------------------------

ASTPtr<VarDeclAST> Parser::parseVarDecl(Visibility vis) {
    const Token& kwTok = ts_.peekAt(0);
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected variable name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for variable '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    ExprPtr init;
    if (ts_.match(TokenType::ASSIGN)) {
        init = parseExpr();
        if (!init) {
            errorAt(DiagCode::E2008, "expected expression after '=' in variable declaration");
        }
    }

    auto node = arena_.make<VarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->type = std::move(type);
    node->init = std::move(init);
    node->visibility = vis;

    return node;
}

// -----------------------------------------------------------------------------
// parseFuncDecl
// -----------------------------------------------------------------------------

ASTPtr<FuncDeclAST> Parser::parseFuncDecl(DeclKeyword kw, Visibility vis) {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected function name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    auto node = arena_.make<FuncDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    // Raw qualifiers
    std::vector<InternedString> rawQuals;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        rawQuals.push_back(pool_.intern(ts_.advance().value));
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    node->sig.rawQualifiers = qBuilder.build();

    // Parameter groups: accumulate flat
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for function '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }
    while (ts_.check(TokenType::LPAREN)) {
        std::vector<ParamPtr> group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    node->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    node->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        node->sig.returnTypes = parseReturnList();
    }

    // Body
    if (!ts_.check(TokenType::ASSIGN)) {
        ts_.match(TokenType::SEMICOLON);
        return node;
    }

    ts_.advance();

    if (ts_.check(TokenType::LBRACE)) {
        node->body = parseBlock();
    } else if (ts_.check(TokenType::LPAREN)) {
        validateAnonFuncBodySig(node->sig, std::string(pool_.lookup(name)));
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start function body");
            return nullptr;
        }
        node->body = parseBlock();
    } else {
        SourceLocation bodyLoc = ts_.currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E2008, "expected expression after '='");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        std::vector<ExprPtr> vals;
        vals.push_back(std::move(expr));
        auto valsBuilder = arena_.makeBuilder<ExprPtr>();
        for (auto& v : vals) valsBuilder.push_back(std::move(v));
        ret->values = valsBuilder.build();

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        std::vector<StmtPtr> stmts;
        stmts.push_back(std::move(ret));
        auto stmtsBuilder = arena_.makeBuilder<StmtPtr>();
        for (auto& s : stmts) stmtsBuilder.push_back(std::move(s));
        block->stmts = stmtsBuilder.build();

        node->body = std::move(block);
    }
    
    ts_.match(TokenType::SEMICOLON);
    return node;
}

// -----------------------------------------------------------------------------
// parseStructDecl
// -----------------------------------------------------------------------------

ASTPtr<StructDeclAST> Parser::parseStructDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::STRUCT, "expected 'struct'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<StructDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open struct body");

    std::vector<FieldDeclPtr> fields;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        FieldDeclPtr field = parseFieldDecl();
        if (field) {
            fields.push_back(std::move(field));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<FieldDeclPtr>();
    for (auto& f : fields) builder.push_back(std::move(f));
    node->fields = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct body");
    return node;
}

// -----------------------------------------------------------------------------
// parseFieldDecl
// -----------------------------------------------------------------------------

FieldDeclPtr Parser::parseFieldDecl() {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for field '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    ExprPtr defaultVal;
    if (ts_.match(TokenType::ASSIGN)) {
        defaultVal = parseExpr();
        if (!defaultVal) {
            errorAt(DiagCode::E2008, "expected expression after '=' in field default value");
        }
    }

    auto field = arena_.make<FieldDeclAST>();
    field->loc = loc;
    field->name = name;
    field->type = std::move(type);
    field->defaultVal = std::move(defaultVal);
    return field;
}

// -----------------------------------------------------------------------------
// parseEnumDecl
// -----------------------------------------------------------------------------

ASTPtr<EnumDeclAST> Parser::parseEnumDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::ENUM, "expected 'enum'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<EnumDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    ts_.consume(TokenType::LBRACE, "expected '{' to open enum body");

    std::vector<EnumVariantPtr> variants;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        EnumVariantPtr variant = parseEnumVariant();
        if (variant) {
            variants.push_back(std::move(variant));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<EnumVariantPtr>();
    for (auto& v : variants) builder.push_back(std::move(v));
    node->variants = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close enum body");
    return node;
}

// -----------------------------------------------------------------------------
// parseEnumVariant
// -----------------------------------------------------------------------------

EnumVariantPtr Parser::parseEnumVariant() {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum variant name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto variant = arena_.make<EnumVariantAST>(name);
    variant->loc = loc;

    if (ts_.match(TokenType::ASSIGN)) {
        if (ts_.check(TokenType::INT_LITERAL) || ts_.check(TokenType::HEX_LITERAL) ||
            ts_.check(TokenType::BINARY_LITERAL)) {
            Token valTok = ts_.advance();
            std::string raw = valTok.value;
            raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

            int base = (valTok.type == TokenType::HEX_LITERAL) ? 16 : 10;
            char* endPtr = nullptr;
            errno = 0;
            long long val = std::strtoll(raw.c_str(), &endPtr, base);

            if (endPtr != raw.c_str() && *endPtr == '\0' && errno != ERANGE) {
                variant->explicitValue = val;
            } else {
                error(ts_.locOf(valTok), DiagCode::E2009,
                      "enum variant value '" + valTok.value + "' is not a valid integer");
            }
        } else {
            errorAt(DiagCode::E2009, "expected integer literal after '=' in enum variant");
        }
    }

    return variant;
}

// -----------------------------------------------------------------------------
// parseTraitDecl
// -----------------------------------------------------------------------------

ASTPtr<TraitDeclAST> Parser::parseTraitDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TRAIT, "expected 'trait'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<TraitDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open trait body");

    std::vector<TraitMethodPtr> methods;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        TraitMethodPtr method = parseTraitMethod();
        if (method) {
            methods.push_back(std::move(method));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<TraitMethodPtr>();
    for (auto& m : methods) builder.push_back(std::move(m));
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close trait body");
    return node;
}

// -----------------------------------------------------------------------------
// parseTraitMethod
// -----------------------------------------------------------------------------

TraitMethodPtr Parser::parseTraitMethod() {
    SourceLocation loc = ts_.currentLoc();
    
    auto method = arena_.make<TraitMethodAST>();
    method->loc = loc;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait method name");
        return nullptr;
    }
    method->name = pool_.intern(ts_.advance().value);
    
    // Raw qualifiers
    std::vector<InternedString> rawQuals;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        rawQuals.push_back(pool_.intern(ts_.advance().value));
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    method->sig.rawQualifiers = qBuilder.build();

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' for trait method parameters");
        return nullptr;
    }
    while (ts_.check(TokenType::LPAREN)) {
        std::vector<ParamPtr> group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    method->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    method->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
    }
    
    return method;
}

// -----------------------------------------------------------------------------
// parseTraitRef
// -----------------------------------------------------------------------------

TraitRefPtr Parser::parseTraitRef() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::COLON, "expected ':' before trait name");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name after ':'");
        return nullptr;
    }

    auto ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(ts_.advance().value);

    if (ts_.check(TokenType::LESS)) {
        ref->genericArgs = parseGenericArgs();
    }

    return ref;
}

// -----------------------------------------------------------------------------
// parseGenericParams
// -----------------------------------------------------------------------------

ArenaSpan<GenericParamPtr> Parser::parseGenericParams() {
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<GenericParamPtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<GenericParamPtr>();
    }
    
    std::vector<GenericParamPtr> params;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        GenericParamPtr gp = parseGenericParam();
        if (gp) params.push_back(std::move(gp));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' after generic parameters");
    
    auto builder = arena_.makeBuilder<GenericParamPtr>();
    for (auto& p : params) builder.push_back(std::move(p));
    return builder.build();
}

// -----------------------------------------------------------------------------
// parseGenericParam
// -----------------------------------------------------------------------------

GenericParamPtr Parser::parseGenericParam() {
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected generic parameter name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    auto gp = arena_.make<GenericParamAST>(name);
    
    if (ts_.match(TokenType::COLON)) {
        std::vector<InternedString> constraints;
        while (true) {
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected trait name in constraint");
                break;
            }
            constraints.push_back(pool_.intern(ts_.advance().value));
            if (!ts_.match(TokenType::PLUS)) break;
        }
        auto builder = arena_.makeBuilder<InternedString>();
        for (auto& c : constraints) builder.push_back(std::move(c));
        gp->constraints = builder.build();
    }
    
    return gp;
}

// -----------------------------------------------------------------------------
// parseImplDecl
// -----------------------------------------------------------------------------

ASTPtr<ImplDeclAST> Parser::parseImplDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IMPL, "expected 'impl'");

    auto node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected target type after 'impl'");
        return nullptr;
    }

    if (ts_.match(TokenType::AS)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after 'as' for receiver alias");
        } else {
            node->receiverAlias = pool_.intern(ts_.advance().value);
        }
    }

    TypePtr targetType = parseNamedType();
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid target type in impl block");
        return nullptr;
    }
    node->targetType = std::move(targetType);

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    if (ts_.check(TokenType::COLON)) {
        node->traitRef = parseTraitRef();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open impl body");

    std::vector<MethodDeclPtr> methods;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();

        if (ts_.check(TokenType::IDENTIFIER)) {
            MethodDeclPtr md = parseMethodDecl();
            if (md) {
                methods.push_back(std::move(md));
            } else {
                if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
                while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                       !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
            }
            continue;
        }

        errorAt(DiagCode::E2002, "expected method declaration inside impl block");
        if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
        while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
               !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
    }

    auto builder = arena_.makeBuilder<MethodDeclPtr>();
    for (auto& m : methods) builder.push_back(std::move(m));
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close impl body");
    return node;
}

// -----------------------------------------------------------------------------
// parseMethodDecl
// -----------------------------------------------------------------------------

MethodDeclPtr Parser::parseMethodDecl() {
    SourceLocation loc = ts_.currentLoc();

    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name");
        return nullptr;
    }
    method->name = pool_.intern(ts_.advance().value);

    // Raw qualifiers
    std::vector<InternedString> rawQuals;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        rawQuals.push_back(pool_.intern(ts_.advance().value));
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    method->sig.rawQualifiers = qBuilder.build();

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for method");
        return nullptr;
    }
    while (ts_.check(TokenType::LPAREN)) {
        std::vector<ParamPtr> group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    method->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    method->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
    }

    // Body
    if (!ts_.check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' before method body");
        return nullptr;
    }
    ts_.advance();

    if (ts_.check(TokenType::LBRACE)) {
        method->body = parseBlock();
    } else if (ts_.check(TokenType::LPAREN)) {
        validateAnonFuncBodySig(method->sig, std::string(pool_.lookup(method->name)));
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start method body");
            return nullptr;
        }
        method->body = parseBlock();
    } else {
        SourceLocation bodyLoc = ts_.currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E2008, "expected expression after '=' for method");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        std::vector<ExprPtr> vals;
        vals.push_back(std::move(expr));
        auto valsBuilder = arena_.makeBuilder<ExprPtr>();
        for (auto& v : vals) valsBuilder.push_back(std::move(v));
        ret->values = valsBuilder.build();

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        std::vector<StmtPtr> stmts;
        stmts.push_back(std::move(ret));
        auto stmtsBuilder = arena_.makeBuilder<StmtPtr>();
        for (auto& s : stmts) stmtsBuilder.push_back(std::move(s));
        block->stmts = stmtsBuilder.build();

        method->body = std::move(block);
    }

    ts_.match(TokenType::SEMICOLON);
    return method;
}

// -----------------------------------------------------------------------------
// parseFromDecl
// -----------------------------------------------------------------------------

ASTPtr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected target type name after 'from'");
        return nullptr;
    }

    TypePtr targetType = parseNamedType();
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid target type in from block");
        return nullptr;
    }
    node->targetType = std::move(targetType);

    // Generic parameters from target type (simplified)
    if (node->targetType->isa<NamedTypeAST>()) {
        auto* named = node->targetType->as<NamedTypeAST>();
        std::vector<GenericParamPtr> genericParams;
        for (auto& arg : named->genericArgs) {
            if (arg && arg->isa<NamedTypeAST>()) {
                auto* argNamed = arg->as<NamedTypeAST>();
                if (argNamed->isGenericParam || argNamed->genericArgs.empty()) {
                    auto gp = arena_.make<GenericParamAST>(argNamed->name);
                    gp->loc = argNamed->loc;
                    genericParams.push_back(std::move(gp));
                }
            }
        }
        auto builder = arena_.makeBuilder<GenericParamPtr>();
        for (auto& gp : genericParams) builder.push_back(std::move(gp));
        node->genericParams = builder.build();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open from block");

    std::vector<FromEntryPtr> entries;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t entrySavedPos = ts_.getPos();

        if (!ts_.check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' to start parameter list for conversion entry");
            if (ts_.getPos() == entrySavedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::LPAREN)) ts_.advance();
            continue;
        }

        auto entry = arena_.make<FromEntryAST>();
        entry->loc = ts_.currentLoc();

        // Parameter groups: flat accumulation
        std::vector<ParamPtr> allParams;
        std::vector<size_t> groupSizes;
        while (ts_.check(TokenType::LPAREN)) {
            std::vector<ParamPtr> group = parseParamGroup();
            groupSizes.push_back(group.size());
            for (auto& p : group) {
                allParams.push_back(std::move(p));
            }
        }
        auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
        for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
        entry->sig.allParams = paramsBuilder.build();

        auto gsBuilder = arena_.makeBuilder<size_t>();
        for (auto& sz : groupSizes) gsBuilder.push_back(sz);
        entry->sig.groupSizes = gsBuilder.build();

        if (!ts_.check(TokenType::ARROW)) {
            errorAt(DiagCode::E2001, "expected '->' before return type for conversion entry");
            continue;
        }
        ts_.advance();

        TypePtr returnType = parseType();
        if (!returnType) {
            errorAt(DiagCode::E2005, "expected return type after '->'");
            continue;
        }
        entry->returnType = std::move(returnType);

        if (!ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2001, "expected '=' before body for conversion entry");
            continue;
        }
        ts_.advance();

        if (ts_.check(TokenType::LBRACE)) {
            entry->body = parseBlock();
        } else {
            SourceLocation bodyLoc = ts_.currentLoc();
            ExprPtr expr = parseExpr();
            if (expr) {
                auto ret = arena_.make<ReturnStmtAST>();
                ret->loc = bodyLoc;
                std::vector<ExprPtr> vals;
                vals.push_back(std::move(expr));
                auto valsBuilder = arena_.makeBuilder<ExprPtr>();
                for (auto& v : vals) valsBuilder.push_back(std::move(v));
                ret->values = valsBuilder.build();

                auto block = arena_.make<BlockStmtAST>();
                block->loc = bodyLoc;
                std::vector<StmtPtr> stmts;
                stmts.push_back(std::move(ret));
                auto stmtsBuilder = arena_.makeBuilder<StmtPtr>();
                for (auto& s : stmts) stmtsBuilder.push_back(std::move(s));
                block->stmts = stmtsBuilder.build();

                entry->body = std::move(block);
            } else {
                errorAt(DiagCode::E2008, "expected expression after '=' in conversion entry");
            }
        }

        entries.push_back(std::move(entry));
    }

    auto builder = arena_.makeBuilder<FromEntryPtr>();
    for (auto& e : entries) builder.push_back(std::move(e));
    node->entries = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close from block");
    return node;
}

// -----------------------------------------------------------------------------
// parseTypeAliasDecl
// -----------------------------------------------------------------------------

ASTPtr<TypeAliasDeclAST> Parser::parseTypeAliasDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TYPE, "expected 'type' before type alias");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type alias name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<TypeAliasDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::ASSIGN, "expected '=' in type alias");

    node->aliasedType = parseType();
    if (!node->aliasedType) {
        errorAt(DiagCode::E2005, "expected type on the right-hand side of type alias");
    }

    return node;
}
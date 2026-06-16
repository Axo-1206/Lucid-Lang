/**
 * @file ImplParser.cpp
 * @brief Parses `impl` declarations and method implementations.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of `impl` blocks, which bind method
 * implementations to types (primitives, structs, enums, arrays, or type aliases).
 * 
 * ## Impl Declaration Grammar (from LUC_GRAMMAR.md)
 * 
 *   impl_decl := [ visibility_mod ] 'impl' impl_target [ impl_generic_params ]
 *                [ 'as' IDENTIFIER ] [ ':' trait_ref ] '{' method_decl* '}'
 * 
 *   impl_target := IDENTIFIER                    -- named type
 *                | primitive_type                -- primitive
 *                | array_type                    -- concrete array
 *                | generic_array_type            -- generic array (with <T>)
 * 
 *   generic_array_type := '[' '_' ',' '<' IDENTIFIER '>' ']'
 *                       | '[' '*' ',' '<' IDENTIFIER '>' ']'
 *                       | '[' INT_LITERAL ',' '<' IDENTIFIER '>' ']'
 * 
 * @see ParserDecl.cpp for declaration dispatch
 * @see MethodDeclAST for method representation
 */

#include "ast/BaseAST.hpp"
#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// 1. IMPL DECLARATION
// ============================================================================

/**
 * @brief Parses an `impl` block that binds methods to a type.
 *
 * Grammar (from LUC_GRAMMAR.md):
 *   impl_decl := [ visibility_mod ] 'impl' impl_target [ impl_generic_params ]
 *                [ 'as' IDENTIFIER ] [ ':' trait_ref ] '{' method_decl* '}'
 *
 * @par Examples
 *   impl Vec2 {
 *       length () -> float = { return #sqrt(self.x*self.x + self.y*self.y) }
 *   }
 *
 *   impl Box<T> as b {
 *       get () -> T = { return b.value }
 *   }
 *
 *   impl Circle as c : Drawable {
 *       draw () { c:render() }
 *   }
 *
 *   impl int as i {
 *       isEven () -> bool = { return i % 2 == 0 }
 *   }
 *
 *   impl [_, int] as list {  -- concrete array target
 *       sum () -> int = { ... }
 *   }
 *
 *   impl [*, <T>] as a {  -- generic array target
 *       first () -> T = { return a[0] }
 *   }
 *
 * ─── Parsing Order ─────────────────────────────────────────────────────────
 *   1. 'impl' keyword
 *   2. Target type (primitive, named, or array)
 *   3. Optional impl-level generic parameters (only for generic structs/aliases)
 *   4. Optional 'as' alias (replaces 'self' as receiver name)
 *   5. Optional ':' trait conformance
 *   6. '{' method_decl* '}'
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'impl' keyword
 * On exit:  positioned after the closing '}' of the impl body
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: reports error, returns nullptr
 * - Invalid target type: reports error, returns nullptr
 * - Missing '{' after header: reports error, returns nullptr
 * - Invalid method: skips method, continues parsing remaining methods
 * - Missing '}': consume() reports error
 *
 * @param vis Visibility modifier (Private, Package, or Export)
 * @return ImplDeclPtr – impl node on success, nullptr on error
 */
ImplDeclPtr Parser::parseImplDecl(Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseImplDecl: entering at line " << ts_.currentLoc().line() 
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IMPL, "expected 'impl'");

    auto node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Determine the target type
    TypePtr targetType = nullptr;

    // Case 1: Array target (concrete or generic)
    if (ts_.check(TokenType::LBRACKET)) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: array target");
        // parseArrayType() handles both concrete and generic arrays
        // It will create GenericArrayTypeAST when it sees [_, <T>]
        targetType = parseArrayType();
        
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseImplDecl: ERROR - invalid array target");
            errorAt(DiagCode::E1005, "invalid array target in impl block");
            return nullptr;
        }
        node->targetType = targetType;
        
        // Set TargetKind based on result
        if (targetType->isa<GenericArrayTypeAST>()) {
            node->targetKind = TargetKind::GenericArray;
            auto* genArray = targetType->as<GenericArrayTypeAST>();
            node->arrayTypeParamName = genArray->typeParamName;
            LUC_LOG_DECL_EXTREME("parseImplDecl: generic array target with param <" 
                                 << pool_.lookup(node->arrayTypeParamName) << ">");
        } else {
            node->targetKind = TargetKind::Concrete;
        }
    }
    // Case 2: Primitive type
    else if (isPrimitiveTypeToken(ts_.peekType())) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: primitive type target");
        targetType = parsePrimitiveType();
        node->targetType = targetType;
        node->targetKind = TargetKind::Concrete;
    }
    // Case 3: Named type (struct, enum, type alias) - may have generic parameters
    else if (ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: named type target");
        
        // Parse the type name
        SourceLocation nameLoc = ts_.currentLoc();
        Token nameToken = ts_.advance();
        InternedString typeName = pool_.intern(nameToken.value);
        LUC_LOG_DECL("Named type: '" << pool_.lookup(typeName) << "'");
        
        // Check for generic PARAMETERS (declaration), not generic arguments
        // For impl Box<T>, the <T> declares a generic parameter for the impl block
        if (ts_.check(TokenType::LESS)) {
            LUC_LOG_DECL_EXTREME("parseImplDecl: parsing generic parameters for impl target");
            node->genericParams = parseGenericParams();
            node->targetKind = TargetKind::GenericNamed;
            LUC_LOG_DECL_EXTREME("parseImplDecl: " << node->genericParams.size() 
                                 << " generic parameter(s)");
        } else {
            node->targetKind = TargetKind::Concrete;
        }
        
        // Create a named type WITHOUT generic arguments
        // The generic parameters are stored on the impl node, not on the type reference
        auto namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = nameLoc;
        node->targetType = namedType;
    }
    // Case 4: Error
    else {
        LUC_LOG_DECL("parseImplDecl: ERROR - expected target type after 'impl'");
        errorAt(DiagCode::E1003, "expected target type after 'impl' (primitive, identifier, or '[')");
        return nullptr;
    }

    if (!node->targetType || node->targetType->isa<UnknownTypeAST>()) {
        LUC_LOG_DECL("parseImplDecl: ERROR - invalid target type");
        errorAt(DiagCode::E1005, "invalid target type in impl block");
        return nullptr;
    }

    // Parse 'as' alias (optional)
    if (ts_.match(TokenType::AS)) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: parsing 'as' alias");
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseImplDecl: ERROR - expected identifier after 'as'");
            errorAt(DiagCode::E1003, "expected identifier after 'as' for receiver alias");
        } else {
            node->receiverAlias = pool_.intern(ts_.advance().value);
            LUC_LOG_DECL_EXTREME("parseImplDecl: receiver alias = " << pool_.lookup(node->receiverAlias));
        }
    }

    // Parse trait conformance (optional)
    if (ts_.check(TokenType::COLON)) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: parsing trait conformance");
        node->traitRef = parseTraitRef();
        if (node->traitRef) {
            LUC_LOG_DECL_EXTREME("parseImplDecl: trait = " << pool_.lookup(node->traitRef->name));
        }
    }

    // Parse impl body
    ts_.consume(TokenType::LBRACE, "expected '{' to open impl body");

    std::vector<MethodDeclPtr> methods;
    int methodCount = 0;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();

        if (ts_.check(TokenType::IDENTIFIER)) {
            MethodDeclPtr md = parseMethodDecl();
            if (md) {
                methodCount++;
                LUC_LOG_DECL_EXTREME("parseImplDecl: parsed method #" << methodCount);
                methods.push_back(md);
            } else {
                LUC_LOG_DECL("parseImplDecl: ERROR - failed to parse method");
                if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
                while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                       !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
            }
            continue;
        }

        LUC_LOG_DECL("parseImplDecl: ERROR - expected method declaration inside impl block");
        errorAt(DiagCode::E1002, "expected method declaration inside impl block");
        if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
        while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
               !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
    }

    auto builder = arena_.makeBuilder<MethodDeclPtr>();
    for (auto& m : methods) builder.push_back(m);
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close impl body");
    
    LUC_LOG_DECL_VERBOSE("parseImplDecl: parsed " << methodCount << " method(s)");
    return node;
}

// ============================================================================
// 2. METHOD DECLARATION
// ============================================================================

/**
 * @brief Parses a method declaration inside an `impl` block.
 *
 * Grammar (from LUC_GRAMMAR.md, Impl Declaration section):
 *
 *   method_decl := IDENTIFIER [ qualifier_list ] param_group { param_group }
 *                  [ '->' return_list ] '=' block                     -- inline body
 *
 *                | IDENTIFIER '=' func_ref                            -- plain assignment
 *
 *                | IDENTIFIER '=' func_ref '(' receiver_arg ')' '!'   -- injection form
 *
 * @param return MethodDeclPtr – parsed method node, or nullptr on error.
 */
MethodDeclPtr Parser::parseMethodDecl() {
    LUC_LOG_DECL_EXTREME("parseMethodDecl: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();

    // ---------- 1. Parse method name ----------
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseMethodDecl: ERROR - expected method name");
        errorAt(DiagCode::E1003, "expected method name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseMethodDecl: method name = " << pool_.lookup(name));
    
    // ---------- 2. NO generic parameters on methods! ----------
    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_DECL("parseMethodDecl: ERROR - generic parameters not allowed on methods");
        errorAt(DiagCode::E1001, 
                "generic parameters are not allowed on method declarations; "
                "use generic parameters on the impl target instead (e.g., 'impl Box<T>')");
        // Skip the generic parameters to recover
        int depth = 1;
        while (!ts_.isAtEnd() && depth > 0) {
            if (ts_.check(TokenType::LESS)) depth++;
            else if (ts_.check(TokenType::GREATER)) depth--;
            ts_.advance();
        }
    }
    
    // ---------- 3. Determine if this is an assignment form ----------
    size_t peekPos = ts_.skipCommentsFrom(ts_.getPos());
    bool hasQualifiers = false;
    bool hasParameterGroups = false;
    
    if (peekPos < ts_.getTokenCount()) {
        TokenType nextType = ts_.getTokenAt(peekPos).type;
        if (nextType == TokenType::TILDE) {
            hasQualifiers = true;
        } else if (nextType == TokenType::LPAREN) {
            hasParameterGroups = true;
        }
    }
    
    if (hasQualifiers || hasParameterGroups) {
        // ========== INLINE BODY FORM ==========
        LUC_LOG_DECL_EXTREME("parseMethodDecl: inline body form");
        
        TypePtr funcType = parseFuncType();
        if (!funcType || funcType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseMethodDecl: ERROR - invalid method signature");
            errorAt(DiagCode::E1005, "invalid method signature");
            return nullptr;
        }
        
        // Expect '='
        if (!ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E1001, "expected '=' before method body");
            return nullptr;
        }
        ts_.advance(); // consume '='
        
        // Parse body - ONLY block bodies allowed for methods
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E1001, "expected '{' for method body; method bodies must be blocks");
            return nullptr;
        }
        
        auto method = arena_.make<MethodDeclAST>();
        method->loc = loc;
        method->name = name;
        method->funcType = funcType->as<FuncTypeAST>();
        method->body = parseBlock();
        
        ts_.match(TokenType::SEMICOLON);
        LUC_LOG_DECL_EXTREME("parseMethodDecl: inline body success");
        return method;
    }
    
    // ========== ASSIGNMENT FORM (plain or injection) ==========
    LUC_LOG_DECL_EXTREME("parseMethodDecl: assignment form");
    
    if (!ts_.check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E1001, "expected '=' after method name for assignment form");
        return nullptr;
    }
    ts_.advance(); // consume '='
    
    // Parse func_ref (may include generic instantiation)
    ExprPtr funcRef = parseFuncRef();
    if (!funcRef || funcRef->isa<UnknownExprAST>()) {
        errorAt(DiagCode::E1008, "expected function reference after '='");
        return nullptr;
    }
    
    // Check for injection form: '(' receiver_arg ')' '!'
    bool isInjection = false;
    InternedString receiverArg;
    
    size_t afterFuncRef = ts_.skipCommentsFrom(ts_.getPos());
    if (afterFuncRef < ts_.getTokenCount() && 
        ts_.getTokenAt(afterFuncRef).type == TokenType::LPAREN) {
        
        LUC_LOG_DECL_EXTREME("parseMethodDecl: checking for injection form");
        ts_.setPos(afterFuncRef);
        ts_.advance(); // consume '('
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E1003, "expected receiver name in injection form");
        } else {
            receiverArg = pool_.intern(ts_.advance().value);
            LUC_LOG_DECL_EXTREME("parseMethodDecl: injection receiver = " 
                                 << pool_.lookup(receiverArg));
        }
        
        ts_.consume(TokenType::RPAREN, "expected ')' after receiver name");
        
        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E1001, "expected '!' for injection form");
        } else {
            isInjection = true;
            LUC_LOG_DECL_EXTREME("parseMethodDecl: injection form detected");
        }
    }
    
    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;
    method->name = name;
    method->assignmentRef = funcRef;
    method->receiverArg = receiverArg;
    method->isInjection = isInjection;
    
    ts_.match(TokenType::SEMICOLON);
    LUC_LOG_DECL_EXTREME("parseMethodDecl: assignment form success");
    return method;
}

/**
 * @brief Parses a trait reference in impl declarations.
 * 
 * Grammar: ':' IDENTIFIER [ '<' type_args '>' ]
 * 
 * Example: `: Drawable`, `: Comparable<int>`
 * 
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller MUST have already verified that the current token is COLON.
 * This function assumes it is positioned at the ':' token.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at ':' token
 * On exit:  positioned after the trait name (and optional generic arguments)
 * 
 * @return TraitRefPtr – parsed trait reference node, or nullptr on error
 */
TraitRefPtr Parser::parseTraitRef() {
    LOG_DECL_EXTREME("parseTraitRef: entering at line " << ts_.currentLoc().line()
                     << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();
    
    // Check for ':' token (should be present if called correctly)
    if (!ts_.check(TokenType::COLON)) {
        LOG_DECL("parseTraitRef: ERROR - expected ':' before trait name");
        errorAt(DiagCode::E1001, ":", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume ':'

    // Parse trait name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseTraitRef: ERROR - expected trait name after ':'");
        errorAt(DiagCode::E1002, "trait name", ts_.peek().value);
        return nullptr;
    }

    auto* ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseTraitRef: trait name = " << pool_.lookup(ref->name));

    // Parse generic arguments if present
    if (ts_.check(TokenType::LESS)) {
        LOG_DECL_EXTREME("parseTraitRef: parsing generic arguments");
        ts_.advance(); // Consume '<'
        ref->genericArgs = parseGenericArgs();
        LOG_DECL_EXTREME("parseTraitRef: parsed " << ref->genericArgs.size() << " generic argument(s)");
    }

    return ref;
}
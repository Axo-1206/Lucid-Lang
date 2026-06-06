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
 *   method_decl := IDENTIFIER [ generic_params ] [ qualifier_list ]
 *                  param_group { param_group } [ '->' return_list ]
 *                  '=' func_body                                 -- inline body
 *                | IDENTIFIER '=' func_ref                       -- plain assignment
 *                | IDENTIFIER '=' func_ref '(' receiver_arg ')' '!'
 *                                                               -- injection form
 * 
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | func_ref generic_args
 * 
 * IMPORTANT: Method references with colon (`obj:method`) are NOT part of
 * `func_ref` grammar. They belong to pipeline steps and compose operands.
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
 *   impl [_, int] as list {                     -- concrete array target
 *       sum () -> int = { ... }
 *   }
 *
 *   impl [*, <T>] as a {                        -- generic array target
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
 * @return ASTPtr<ImplDeclAST> – impl node on success, nullptr on error
 */
ASTPtr<ImplDeclAST> Parser::parseImplDecl(Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseImplDecl: entering at line " << ts_.currentLoc().line() 
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IMPL, "expected 'impl'");

    auto node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Log current token before parsing target type
    LUC_LOG_DECL("Current token before parsing target: " 
                 << LucDebug::tokenToString(ts_.peek()) 
                 << " at line " << ts_.peek().line 
                 << ", col " << ts_.peek().column);

    // Determine the target type
    TypePtr targetType;

    // Case 1: Array target (concrete or generic)
    if (ts_.check(TokenType::LBRACKET)) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: array target at " 
                             << ts_.peek().line << ":" << ts_.peek().column);
        if (looksLikeGenericArray()) {
            LUC_LOG_DECL_EXTREME("parseImplDecl: generic array target");
            targetType = parseGenericArray();
        } else {
            targetType = parseArrayType();
        }
        
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseImplDecl: ERROR - invalid array target at line " 
                         << loc.line());
            errorAt(DiagCode::E1005, "invalid array target in impl block");
            return nullptr;
        }
        node->targetType = std::move(targetType);
    }
    // Case 2: Primitive type
    else if (isPrimitiveTypeToken(ts_.peekType())) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: primitive type target at " 
                             << ts_.peek().line << ":" << ts_.peek().column);
        LUC_LOG_DECL("Primitive type token: " 
                     << LucDebug::tokenToString(ts_.peek())
                     << " at line " << ts_.peek().line << ", col " << ts_.peek().column);
        targetType = parsePrimitiveType();
        node->targetType = std::move(targetType);
        LUC_LOG_DECL("After parsePrimitiveType, at token: " 
                     << LucDebug::tokenToString(ts_.peek())
                     << " line " << ts_.peek().line << ", col " << ts_.peek().column);
    }
    // Case 3: Named type (struct, enum, type alias) - may have generic parameters
    else if (ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: named type target at " 
                             << ts_.peek().line << ":" << ts_.peek().column);
        
        // Parse the type name
        SourceLocation loc = ts_.currentLoc();
        Token nameToken = ts_.advance();
        InternedString typeName = pool_.intern(nameToken.value);
        LUC_LOG_DECL("Named type identifier: '" << pool_.lookup(typeName) 
                     << "' at line " << nameToken.line << ", col " << nameToken.column);
        
        // Check for generic PARAMETERS (declaration), not generic arguments
        // For impl Box<T>, the <T> declares a generic parameter for the impl block
        ArenaSpan<GenericParamPtr> implGenericParams;
        
        if (ts_.check(TokenType::LESS)) {
            LUC_LOG_DECL_EXTREME("parseImplDecl: parsing generic PARAMETERS for impl target");
            // Parse generic parameters (these are declarations for the impl block)
            implGenericParams = parseGenericParams();
            LUC_LOG_DECL_EXTREME("parseImplDecl: parsed " << implGenericParams.size() 
                                 << " generic parameter(s)");
        }
        
        // Store generic parameters on the impl node
        node->genericParams = implGenericParams;
        
        // Create a named type WITHOUT generic arguments
        // The generic parameters are stored on the impl node, not on the type reference
        auto namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = loc;
        targetType = std::move(namedType);
        node->targetType = std::move(targetType);
        
        LUC_LOG_DECL("After parsing impl target (with possible generics), at token: " 
                     << LucDebug::tokenToString(ts_.peek())
                     << " line " << ts_.peek().line << ", col " << ts_.peek().column);
    }
    // Case 4: Error
    else {
        LUC_LOG_DECL("parseImplDecl: ERROR - expected target type after 'impl' at line " 
                     << ts_.peek().line << ", col " << ts_.peek().column);
        LUC_LOG_DECL("Current token: " << LucDebug::tokenToString(ts_.peek())
                     << " at line " << ts_.peek().line << ", col " << ts_.peek().column);
        errorAt(DiagCode::E1003, 
                "expected target type after 'impl' (primitive, identifier, or '[')");
        return nullptr;
    }

    if (!node->targetType || node->targetType->isa<UnknownTypeAST>()) {
        LUC_LOG_DECL("parseImplDecl: ERROR - invalid target type");
        errorAt(DiagCode::E1005, "invalid target type in impl block");
        return nullptr;
    }

    // For generic array targets, the type variable is already stored in the node.
    // The impl should NOT have additional generic parameters (already handled above).
    bool isGenericArray = node->targetType->isa<GenericArrayTypeAST>();

    // REMOVE the old generic parameter parsing here since it's now handled above
    // Parse impl-level generic parameters (only for generic structs/aliases)
    // if (!isGenericArray && ts_.check(TokenType::LESS)) {
    //     LUC_LOG_DECL_EXTREME("parseImplDecl: parsing generic parameters at line " 
    //                          << ts_.peek().line << ", col " << ts_.peek().column);
    //     node->genericParams = parseGenericParams();
    //     LUC_LOG_DECL_EXTREME("parseImplDecl: " << node->genericParams.size() << " generic parameter(s)");
    // }

    // Parse 'as' alias (optional)
    if (ts_.match(TokenType::AS)) {
        LUC_LOG_DECL_EXTREME("parseImplDecl: parsing 'as' alias at " 
                             << ts_.peek().line << ":" << ts_.peek().column);
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
        LUC_LOG_DECL_EXTREME("parseImplDecl: parsing trait conformance at line " 
                             << ts_.peek().line << ", col " << ts_.peek().column);
        LUC_LOG_DECL("Found ':' at line " << ts_.peek().line 
                     << ", col " << ts_.peek().column << ", parsing trait reference");
        node->traitRef = parseTraitRef();
        if (node->traitRef) {
            LUC_LOG_DECL_EXTREME("parseImplDecl: trait = " << pool_.lookup(node->traitRef->name));
            LUC_LOG_DECL("After parsing trait ref, at token: " 
                         << LucDebug::tokenToString(ts_.peek())
                         << " line " << ts_.peek().line << ", col " << ts_.peek().column);
        }
    }

    // Parse impl body
    LUC_LOG_DECL("Expecting '{' at line " << ts_.peek().line << ", col " << ts_.peek().column);
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
                methods.push_back(std::move(md));
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
    for (auto& m : methods) builder.push_back(std::move(m));
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
 *                | IDENTIFIER '=' func_ref '(' receiver_arg ')' '!'
 *                                                                     -- injection form
 *
 *   block       := '{' { stmt } '}'
 *   func_ref    := IDENTIFIER
 *                | IDENTIFIER '.' IDENTIFIER
 *                | func_ref generic_args
 *
 * ─── Important ────────────────────────────────────────────────────────────
 *   - Method declarations CANNOT have their own generic parameters (`<...>`).
 *     Generic parameters belong to the impl target (e.g., `impl Box<T>`).
 *
 *   - Inline method bodies MUST be blocks (`{ ... }`). Expression bodies are
 *     NOT allowed for methods (unlike functions).
 *
 * ─── Three Forms ─────────────────────────────────────────────────────────
 *
 * 1. **Inline Body Form** (has `=` followed by `{`)
 *    - Includes optional qualifiers, parameter groups, return types, and a block body.
 *    - Expression bodies are NOT allowed – method bodies must be blocks.
 *    - `funcType` is populated with the signature.
 *    - `body` is a `BlockStmtAST`.
 *    - `assignmentRef` and `receiverArg` remain null/empty.
 *    - `isInjection` is false.
 *
 * 2. **Plain Assignment Form** (has `=` followed immediately by a `func_ref`)
 *    - No qualifiers, parameter groups, or return types.
 *    - The full type (including qualifiers) is read from `func_ref`.
 *    - `assignmentRef` points to the `func_ref` expression.
 *    - `funcType` remains null (type is resolved from `func_ref` by semantic pass).
 *    - `isInjection` is false.
 *
 * 3. **Injection Form** (has `=` followed by `func_ref (receiver_arg) !`)
 *    - Same as plain assignment, but with receiver injection.
 *    - The first parameter of the referenced function is removed.
 *    - `receiverArg` holds the receiver name (must be `self` or impl alias).
 *    - `isInjection` is true.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the method name (IDENTIFIER).
 * On exit:  positioned after the semicolon (or after the closing '}' of body).
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing method name: reports error, returns nullptr.
 * - Generic parameters on method: reports error, skips to matching '>', continues.
 * - Missing '(' for inline form: reports error, returns nullptr.
 * - Missing '=' before body: reports error, returns nullptr.
 * - Missing '{' after '=' for inline form: reports error, returns nullptr.
 * - Invalid func_ref in assignment form: reports error, returns nullptr.
 * - Missing '!' after (receiver_arg) in injection form: reports error,
 *   treats as plain assignment (continues).
 *
 * @return MethodDeclPtr – parsed method node, or nullptr on error.
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
    // If we see '<', it's a syntax error - report and skip to recover
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
        // Continue parsing - the error will be reported once
    }
    
    // ---------- 3. Determine if this is an assignment form ----------
    // Peek ahead to see if we have signature components that indicate inline body form
    size_t peekPos = ts_.skipCommentsFrom(ts_.getPos());
    bool hasQualifiers = false;
    bool hasParameterGroups = false;
    
    if (peekPos < ts_.getTokenCount()) {
        TokenType nextType = ts_.getTokenAt(peekPos).type;
        if (nextType == TokenType::TILDE) {
            hasQualifiers = true;
            LUC_LOG_DECL_EXTREME("parseMethodDecl: detected qualifiers (inline body)");
        } else if (nextType == TokenType::LPAREN) {
            hasParameterGroups = true;
            LUC_LOG_DECL_EXTREME("parseMethodDecl: detected parameter groups (inline body)");
        }
    }
    
    if (hasQualifiers || hasParameterGroups) {
        // ========== INLINE BODY FORM ==========
        LUC_LOG_DECL_EXTREME("parseMethodDecl: inline body form");
        
        // Parse qualifiers
        std::vector<InternedString> rawQuals;
        uint32_t qualMask = 0;
        while (ts_.check(TokenType::TILDE)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E1003, "expected qualifier name after '~'");
                break;
            }
            InternedString q = pool_.intern(ts_.advance().value);
            rawQuals.push_back(q);
            std::string_view qstr = pool_.lookup(q);
            if (qstr == "async") qualMask |= QualifierBits::Async;
            else if (qstr == "nullable") qualMask |= QualifierBits::Nullable;
            else if (qstr == "parallel") qualMask |= QualifierBits::Parallel;
        }
        
        // Parse parameter groups
        std::vector<ParamPtr> allParams;
        std::vector<size_t> groupSizes;
        int groupCount = 0;
        
        if (!ts_.check(TokenType::LPAREN)) {
            LUC_LOG_DECL("parseMethodDecl: ERROR - expected '(' to start parameter list");
            errorAt(DiagCode::E1001, "expected '(' to start parameter list for method");
            return nullptr;
        }
        
        while (ts_.check(TokenType::LPAREN)) {
            groupCount++;
            ParamGroup group = parseParamGroup();
            groupSizes.push_back(group.size());
            LUC_LOG_DECL_EXTREME("parseMethodDecl: parameter group #" << groupCount 
                                 << " has " << group.size() << " parameter(s)");
            for (auto& p : group) {
                allParams.push_back(std::move(p));
            }
        }
        
        // Parse return types
        std::vector<TypePtr> returnTypes;
        if (ts_.match(TokenType::ARROW)) {
            ArenaSpan<TypePtr> returnSpan = parseReturnList();
            for (size_t i = 0; i < returnSpan.size(); ++i) {
                returnTypes.push_back(std::move(const_cast<TypePtr&>(returnSpan[i])));
            }
            LUC_LOG_DECL_EXTREME("parseMethodDecl: parsed " << returnTypes.size() << " return type(s)");
        }
        
        // Expect '='
        if (!ts_.check(TokenType::ASSIGN)) {
            LUC_LOG_DECL("parseMethodDecl: ERROR - expected '=' before method body");
            errorAt(DiagCode::E1001, "expected '=' before method body");
            return nullptr;
        }
        ts_.advance(); // consume '='
        
        // Parse body - ONLY block bodies allowed for methods
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E1001, "expected '{' for method body; method bodies must be blocks");
            return nullptr;
        }
        
        LUC_LOG_DECL_EXTREME("parseMethodDecl: block body");
        
        auto method = arena_.make<MethodDeclAST>();
        method->loc = loc;
        method->name = name;
        // methodGenericParams remains empty (not allowed on methods)
        method->body = parseBlock();
        
        // Build FuncTypeAST
        auto funcType = arena_.make<FuncTypeAST>();
        funcType->loc = loc;
        
        auto qBuilder = arena_.makeBuilder<InternedString>();
        for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
        funcType->rawQualifiers = qBuilder.build();
        funcType->qualifiers = qualMask;
        
        auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
        for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
        funcType->sig.allParams = paramsBuilder.build();
        
        auto gsBuilder = arena_.makeBuilder<size_t>();
        for (auto& sz : groupSizes) gsBuilder.push_back(sz);
        funcType->sig.groupSizes = gsBuilder.build();
        
        auto retBuilder = arena_.makeBuilder<TypePtr>();
        for (auto& t : returnTypes) retBuilder.push_back(std::move(t));
        funcType->sig.returnTypes = retBuilder.build();
        
        method->funcType = std::move(funcType);
        
        ts_.match(TokenType::SEMICOLON);
        LUC_LOG_DECL_EXTREME("parseMethodDecl: inline body success");
        return method;
    }
    
    // ========== ASSIGNMENT FORM (plain or injection) ==========
    // At this point, we should have just IDENTIFIER followed by '='
    LUC_LOG_DECL_EXTREME("parseMethodDecl: assignment form (plain or injection)");
    
    // Check for '='
    if (!ts_.check(TokenType::ASSIGN)) {
        LUC_LOG_DECL("parseMethodDecl: ERROR - expected '=' for assignment form");
        errorAt(DiagCode::E1001, "expected '=' after method name for assignment form");
        return nullptr;
    }
    ts_.advance(); // consume '='
    
    // Parse func_ref (may include generic instantiation)
    ExprPtr funcRef = parseFuncRef();
    if (!funcRef || funcRef->isa<UnknownExprAST>()) {
        LUC_LOG_DECL("parseMethodDecl: ERROR - expected function reference after '='");
        errorAt(DiagCode::E1008, "expected function reference after '='");
        return nullptr;
    }
    LUC_LOG_DECL_EXTREME("parseMethodDecl: parsed function reference");
    
    // Check for injection form: '(' receiver_arg ')' '!'
    bool isInjection = false;
    InternedString receiverArg;
    
    // Skip any whitespace/comments before checking for '('
    size_t afterFuncRef = ts_.skipCommentsFrom(ts_.getPos());
    if (afterFuncRef < ts_.getTokenCount() && 
        ts_.getTokenAt(afterFuncRef).type == TokenType::LPAREN) {
        
        LUC_LOG_DECL_EXTREME("parseMethodDecl: checking for injection form");
        ts_.setPos(afterFuncRef);
        ts_.advance(); // consume '('
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseMethodDecl: ERROR - expected receiver name in injection form");
            errorAt(DiagCode::E1003, "expected receiver name in injection form");
        } else {
            receiverArg = pool_.intern(ts_.advance().value);
            LUC_LOG_DECL_EXTREME("parseMethodDecl: injection receiver = " 
                                 << pool_.lookup(receiverArg));
        }
        
        ts_.consume(TokenType::RPAREN, "expected ')' after receiver name");
        
        if (!ts_.match(TokenType::BANG)) {
            LUC_LOG_DECL("parseMethodDecl: ERROR - expected '!' for injection form");
            errorAt(DiagCode::E1001, "expected '!' for injection form");
            // Continue without injection flag (treat as plain assignment)
        } else {
            isInjection = true;
            LUC_LOG_DECL_EXTREME("parseMethodDecl: injection form detected");
        }
    }
    
    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;
    method->name = name;
    method->assignmentRef = std::move(funcRef);
    method->receiverArg = receiverArg;
    method->isInjection = isInjection;
    // methodGenericParams remains empty (not allowed on methods)
    
    ts_.match(TokenType::SEMICOLON);
    LUC_LOG_DECL_EXTREME("parseMethodDecl: assignment form success");
    return method;
}
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

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IMPL, "expected 'impl'");

    auto node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Determine the target type
    TypePtr targetType;

    // Case 1: Array target (concrete or generic)
    if (ts_.check(TokenType::LBRACKET)) {
        // Check if this is a generic array target (contains '<' after kind)
        size_t savedPos = ts_.getPos();
        
        // Peek to see if this is a generic array
        bool isGenericArray = false;
        if (looksLikeGenericArray()) {
            isGenericArray = true;
        }
        
        if (isGenericArray) {
            targetType = parseGenericArray();
        } else {
            targetType = parseArrayType();
        }
        
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid array target in impl block");
            return nullptr;
        }
    }
    // Case 2: Primitive type
    else if (isPrimitiveTypeToken(ts_.peekType())) {
        targetType = parsePrimitiveType();
    }
    // Case 3: Named type (struct, enum, type alias)
    else if (ts_.check(TokenType::IDENTIFIER)) {
        targetType = parseNamedType();
    }
    // Case 4: Error
    else {
        errorAt(DiagCode::E2003, 
                "expected target type after 'impl' (primitive, identifier, or '[')");
        return nullptr;
    }

    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid target type in impl block");
        return nullptr;
    }
    node->targetType = std::move(targetType);

    // For generic array targets, the type variable is already stored in the node.
    // The impl should NOT have additional generic parameters.
    bool isGenericArray = node->targetType->isa<GenericArrayTypeAST>();

    // Parse impl-level generic parameters (only for generic structs/aliases)
    if (!isGenericArray && ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    // Parse 'as' alias (optional)
    if (ts_.match(TokenType::AS)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after 'as' for receiver alias");
        } else {
            node->receiverAlias = pool_.intern(ts_.advance().value);
        }
    }

    // Parse trait conformance (optional)
    if (ts_.check(TokenType::COLON)) {
        node->traitRef = parseTraitRef();
    }

    // Parse impl body
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

// ============================================================================
// 2. METHOD DECLARATION
// ============================================================================

/**
 * @brief Parses a method implementation inside an `impl` block.
 *
 * Grammar (from LUC_GRAMMAR.md):
 *   method_decl := IDENTIFIER [ '<' generic_params '>' ] [ qualifier_list ] 
 *                  param_group+ [ '->' return_list ] '=' func_body           -- inline body
 *                | IDENTIFIER '=' func_ref                                   -- plain assignment
 *                | IDENTIFIER '=' func_ref '(' receiver_arg ')' '!'          -- injection assignment
 *
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | func_ref generic_args
 *
 * @par Examples
 *   // Inline body with generic parameters
 *   map<U> (f (T) -> U) -> Box<U> = { ... }
 *
 *   // Plain assignment with generic instantiation
 *   toStr = utils.toStr<int>
 *
 *   // Injection assignment with generic instantiation
 *   map = transform<T, U>(self)!
 *
 *   // Inline body without generics
 *   length () -> float = { return #sqrt(self.x*self.x + self.y*self.y) }
 *
 * ─── The Three Forms ──────────────────────────────────────────────────────
 *
 * 1. **Inline Body** – Full signature with optional generic parameters,
 *    qualifiers, parameter groups, return types, and a block/expression body.
 *
 * 2. **Plain Assignment** – Method name, `=`, and a function reference.
 *    No qualifiers, no parameter groups, no return type. The function reference
 *    may include generic instantiation (e.g., `utils.toStr<int>`).
 *
 * 3. **Injection Assignment** – Same as plain assignment, but with `(receiver_arg)!`
 *    appended. The function reference may include generic instantiation.
 *
 * ─── Detection ─────────────────────────────────────────────────────────────
 *   The parser peeks ahead after the method name to see if the next non‑comment
 *   token is `=`. If yes, it treats the declaration as an assignment form;
 *   otherwise, it falls back to the inline body form.
 *
 * @return MethodDeclPtr – parsed method node, or nullptr on error
 */
MethodDeclPtr Parser::parseMethodDecl() {
    SourceLocation loc = ts_.currentLoc();

    // ---------- 1. Parse method name ----------
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    // ---------- 2. Check for generic parameters on the method ----------
    // Save position before parsing generic params (we may need to restore for assignment form)
    size_t beforeGenericParams = ts_.getPos();
    ArenaSpan<GenericParamPtr> methodGenericParams;
    bool hasGenericParams = false;
    
    if (ts_.check(TokenType::LESS)) {
        // Try to parse generic parameters
        // For assignment forms, there should NOT be generic parameters after the name
        size_t savedPos = ts_.getPos();
        ArenaSpan<GenericParamPtr> tempParams = parseGenericParams();
        
        // After parsing, check if the next token is '(' or '~' (inline body) or '=' (assignment)
        size_t afterParams = ts_.getPos();
        if (ts_.check(TokenType::ASSIGN)) {
            // This is an assignment form with bogus generic parameters - error
            errorAt(DiagCode::E2001, 
                    "assignment form method cannot have generic parameters; remove '<...>'");
            // Restore and continue as assignment form
            ts_.setPos(savedPos);
        } else {
            // Valid inline body with generic parameters
            methodGenericParams = tempParams;
            hasGenericParams = true;
        }
    }
    
    // ---------- 3. Peek to see if we have an assignment form ----------
    size_t savedPos = ts_.getPos();
    bool isAssignment = false;
    
    // Skip comments and look for '=' without consuming any tokens yet
    size_t peekPos = ts_.skipCommentsFrom(ts_.getPos());
    if (peekPos < ts_.getTokenCount() && 
        ts_.getTokenAt(peekPos).type == TokenType::ASSIGN) {
        isAssignment = true;
    }
    
    // ---------- 4. Assignment form (plain or injection) ----------
    if (isAssignment) {
        // If we parsed generic parameters but it's actually an assignment form,
        // restore to before they were parsed
        if (hasGenericParams) {
            ts_.setPos(beforeGenericParams);
        }
        
        // Consume the '=' token
        ts_.advance();
        
        // Parse the function reference (may include generic instantiation)
        ExprPtr funcRef = parseFuncRef();
        if (!funcRef || funcRef->isa<UnknownExprAST>()) {
            errorAt(DiagCode::E2008, "expected function reference after '='");
            return nullptr;
        }
        
        // Check for injection form: '(' receiver_arg ')' '!'
        bool isInjection = false;
        InternedString receiverArg;
        
        if (ts_.check(TokenType::LPAREN)) {
            ts_.advance(); // consume '('
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected receiver name in injection form");
            } else {
                receiverArg = pool_.intern(ts_.advance().value);
            }
            ts_.consume(TokenType::RPAREN, "expected ')' after receiver name");
            if (!ts_.match(TokenType::BANG)) {
                errorAt(DiagCode::E2001, "expected '!' for injection form");
            } else {
                isInjection = true;
            }
        }
        
        auto method = arena_.make<MethodDeclAST>();
        method->loc = loc;
        method->name = name;
        method->assignmentRef = std::move(funcRef);
        method->receiverArg = receiverArg;
        method->isInjection = isInjection;
        // methodGenericParams remains empty for assignment form
        
        ts_.match(TokenType::SEMICOLON);
        return method;
    }
    
    // ---------- 5. Inline body form ----------
    // Restore position to after the method name (or after generic params if we parsed them)
    if (hasGenericParams) {
        // Position is already after generic params from earlier parsing
        // No need to restore
    } else {
        ts_.setPos(savedPos);
    }
    
    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;
    method->name = name;
    method->methodGenericParams = methodGenericParams;
    
    // Create a FuncTypeAST to hold signature and qualifiers
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    std::vector<InternedString> rawQuals;
    uint32_t qualMask = 0;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        InternedString q = pool_.intern(ts_.advance().value);
        rawQuals.push_back(q);
        std::string_view qstr = pool_.lookup(q);
        if (qstr == "async") qualMask |= QualifierBits::Async;
        else if (qstr == "nullable") qualMask |= QualifierBits::Nullable;
        else if (qstr == "parallel") qualMask |= QualifierBits::Parallel;
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for method");
        return nullptr;
    }
    while (ts_.check(TokenType::LPAREN)) {
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    funcType->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    funcType->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        funcType->sig.returnTypes = parseReturnList();
    }

    method->funcType = std::move(funcType);

    // Body
    if (!ts_.check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' before method body");
        return nullptr;
    }
    ts_.advance();

    if (ts_.check(TokenType::LBRACE)) {
        method->body = parseBlock();
    } else {
        // Expression body
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

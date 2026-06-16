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
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller MUST have already verified that the current token is IMPL.
 * This function assumes it is positioned at the 'impl' keyword.
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
 * ─── Note on Metadata ─────────────────────────────────────────────────────
 * Doc comments and attributes are handled by the dispatcher (parseDeclaration)
 * for the impl declaration itself. Methods inside the impl block have their
 * own metadata harvested by parseMethodDecl().
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing methods. If parseMethodDecl() makes
 * no progress, consumes one token and continues (prevents infinite loop).
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: reports error, returns nullptr
 * - Invalid target type: reports error, returns nullptr
 * - Missing '{' after header: reports error, returns nullptr
 * - Invalid method: skips method, continues parsing remaining methods
 * - Missing '}': reports error (returns partially built node)
 *
 * @param vis Visibility modifier (Private, Package, or Export)
 * @return ImplDeclPtr – impl node on success, nullptr on error
 */
ImplDeclPtr Parser::parseImplDecl(Visibility vis) {
    LOG_DECL_VERBOSE("parseImplDecl: entering at line " << ts_.currentLoc().line() 
                     << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();
    
    // Check for 'impl' keyword (should be present if called correctly)
    if (!ts_.check(TokenType::IMPL)) {
        LOG_DECL("parseImplDecl: ERROR - expected 'impl' keyword");
        errorAt(DiagCode::E1001, "impl", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume 'impl' keyword

    auto* node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Determine the target type
    TypePtr targetType = nullptr;

    // Case 1: Array target (concrete or generic)
    if (ts_.check(TokenType::LBRACKET)) {
        LOG_DECL_EXTREME("parseImplDecl: array target");
        // parseArrayType() handles both concrete and generic arrays
        targetType = parseArrayType();
        
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            LOG_DECL("parseImplDecl: ERROR - invalid array target");
            errorAt(DiagCode::E1008, "array type", ts_.peek().value);
            return nullptr;
        }
        node->targetType = targetType;
    }
    // Case 2: Primitive type
    else if (ts_.isPrimitiveTypeToken(ts_.peekType())) {
        LOG_DECL_EXTREME("parseImplDecl: primitive type target");
        targetType = parsePrimitiveType();
        node->targetType = targetType;
    }
    // Case 3: Named type (struct, enum, type alias) - may have generic parameters
    else if (ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL_EXTREME("parseImplDecl: named type target");
        
        // Parse the type name
        SourceLocation nameLoc = ts_.currentLoc();
        Token nameToken = ts_.advance();
        InternedString typeName = pool_.intern(nameToken.value);
        LOG_DECL_EXTREME("parseImplDecl: named type = " << pool_.lookup(typeName));
        
        // Check for generic PARAMETERS (declaration), not generic arguments
        // For impl Box<T>, the <T> declares a generic parameter for the impl block
        if (ts_.check(TokenType::LESS)) {
            LOG_DECL_EXTREME("parseImplDecl: parsing generic parameters for impl target");
            node->genericParams = parseGenericParamDecls();
            LOG_DECL_EXTREME("parseImplDecl: " << node->genericParams.size() 
                             << " generic parameter(s)");
        }
        
        // Create a named type WITHOUT generic arguments
        // The generic parameters are stored on the impl node, not on the type reference
        auto* namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = nameLoc;
        node->targetType = namedType;
    }
    // Case 4: Error
    else {
        LOG_DECL("parseImplDecl: ERROR - expected target type after 'impl'");
        errorAt(DiagCode::E1008, "target type", ts_.peek().value);
        return nullptr;
    }

    if (!node->targetType || node->targetType->isa<UnknownTypeAST>()) {
        LOG_DECL("parseImplDecl: ERROR - invalid target type");
        errorAt(DiagCode::E1008, "target type", ts_.peek().value);
        return nullptr;
    }

    // Parse 'as' alias (optional)
    if (ts_.match(TokenType::AS)) {
        LOG_DECL_EXTREME("parseImplDecl: parsing 'as' alias");
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LOG_DECL("parseImplDecl: ERROR - expected identifier after 'as'");
            errorAt(DiagCode::E1002, "alias name", ts_.peek().value);
        } else {
            node->receiverAlias = pool_.intern(ts_.advance().value);
            LOG_DECL_EXTREME("parseImplDecl: receiver alias = " << pool_.lookup(node->receiverAlias));
        }
    }

    // Parse trait conformance (optional)
    if (ts_.check(TokenType::COLON)) {
        LOG_DECL_EXTREME("parseImplDecl: parsing trait conformance");
        node->traitRef = parseTraitRef();
        if (node->traitRef) {
            LOG_DECL_EXTREME("parseImplDecl: trait = " << pool_.lookup(node->traitRef->name));
        }
    }

    // Expect opening brace
    if (!ts_.check(TokenType::LBRACE)) {
        LOG_DECL("parseImplDecl: ERROR - expected '{' to open impl body");
        errorAt(DiagCode::E1004, "{", "impl body", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume '{'

    // Parse methods
    std::vector<MethodDeclPtr> methods;
    int methodCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 5;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        // Skip optional separators
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);

        size_t savedPos = ts_.getPos();

        if (ts_.check(TokenType::IDENTIFIER)) {
            // parseMethodDecl() now handles its own doc comments and attributes
            MethodDeclPtr md = parseMethodDecl();
            if (md) {
                methodCount++;
                LOG_DECL_EXTREME("parseImplDecl: parsed method #" << methodCount);
                methods.push_back(md);
                consecutiveFailures = 0;
            } else {
                consecutiveFailures++;
                LOG_DECL("parseImplDecl: ERROR - failed to parse method (attempt " 
                         << consecutiveFailures << ")");
                
                // Check for progress
                if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                    LOG_DECL("parseImplDecl: no progress, forcing token consumption");
                    ts_.advance();
                }
                
                // Skip to next potential method start
                while (!ts_.isAtEnd() && 
                       !ts_.check(TokenType::RBRACE) && 
                       !ts_.check(TokenType::IDENTIFIER)) {
                    ts_.advance();
                }
            }
        } else {
            consecutiveFailures++;
            LOG_DECL("parseImplDecl: ERROR - expected method declaration inside impl block (attempt " 
                     << consecutiveFailures << ")");
            errorAt(DiagCode::E1002, "method declaration", ts_.peek().value);
            
            // Check for progress
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                LOG_DECL("parseImplDecl: no progress, forcing token consumption");
                ts_.advance();
            }
            
            // Skip to next potential method start
            while (!ts_.isAtEnd() && 
                   !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) {
                ts_.advance();
            }
        }
        
        // Safety: prevent infinite loops by jumping to the nearest '}' or EOF
        if (consecutiveFailures > MAX_CONSECUTIVE_FAILURES) {
            LOG_DECL("parseImplDecl: too many consecutive failures, forcing skip to RBRACE");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE)) {
                ts_.advance();
            }
            // The loop will exit naturally because we're at RBRACE or EOF
        }
    }

    // Consume the closing brace
    if (ts_.check(TokenType::RBRACE)) {
        ts_.advance(); // Consume '}'
    } else {
        // We're not at RBRACE - this means we hit EOF or max consecutive failures
        LOG_DECL("parseImplDecl: ERROR - expected '}' to close impl body");
        errorAt(DiagCode::E1005, "}", "impl body", ts_.peek().value);
    }

    // Build the methods span
    auto builder = arena_.makeBuilder<MethodDeclPtr>();
    for (auto& m : methods) builder.push_back(m);
    node->methods = builder.build();
    
    LOG_DECL_VERBOSE("parseImplDecl: parsed " << methodCount << " method(s)");
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
 * ─── Metadata Support ─────────────────────────────────────────────────────
 * Methods support doc comments and attributes:
 *   -- Documentation for the method
 *   @inline
 *   length () -> float = { ... }
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at method name or before doc comment/attributes
 * On exit:  positioned after the method body or assignment
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing method name: returns nullptr
 * - Invalid signature: reports error, returns nullptr
 * - Missing '=': reports error, returns nullptr
 * - Missing body block: reports error, returns nullptr
 *
 * @return MethodDeclPtr – parsed method node, or nullptr on error
 */
MethodDeclPtr Parser::parseMethodDecl() {
    LOG_DECL_EXTREME("parseMethodDecl: entering at line " << ts_.currentLoc().line()
                     << ", col " << ts_.currentLoc().column());
    
    // Harvest doc comments attached to this method
    auto doc = harvestDocComment();
    
    // Parse attributes for this method
    auto attrs = parseAttributes();
    
    SourceLocation loc = ts_.currentLoc();

    // ---------- 1. Parse method name ----------
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseMethodDecl: ERROR - expected method name");
        errorAt(DiagCode::E1002, "method name", ts_.peek().value);
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseMethodDecl: method name = " << pool_.lookup(name));
    
    // ---------- 2. NO generic parameters on methods! ----------
    if (ts_.check(TokenType::LESS)) {
        LOG_DECL("parseMethodDecl: ERROR - generic parameters not allowed on methods");
        errorAt(DiagCode::E1009, "generic parameters",
                "Generic parameters are not allowed on methods, "
                "use generic parameters on the impl target instead (e.g., 'impl Box<T>')", 
                ts_.peek().value);
        // Skip the generic parameters to recover
        int depth = 1;
        while (!ts_.isAtEnd() && depth > 0) {
            if (ts_.check(TokenType::LESS)) depth++;
            else if (ts_.check(TokenType::GREATER)) depth--;
            ts_.advance();
        }
    }
    
    // ---------- 3. Determine if this is an assignment form ----------
    bool hasQualifiers = false;
    bool hasParameterGroups = false;
    
    // Use check on the next token (skips comments automatically)
    if (ts_.check(TokenType::TILDE)) {
        hasQualifiers = true;
    } else if (ts_.check(TokenType::LPAREN)) {
        hasParameterGroups = true;
    }
    
    MethodDeclPtr method = nullptr;
    
    if (hasQualifiers || hasParameterGroups) {
        // ========== INLINE BODY FORM ==========
        LOG_DECL_EXTREME("parseMethodDecl: inline body form");
        
        TypePtr funcType = parseFuncType();
        if (!funcType || funcType->isa<UnknownTypeAST>()) {
            LOG_DECL("parseMethodDecl: ERROR - invalid method signature");
            errorAt(DiagCode::E1008, "function", ts_.peek().value);
            return nullptr;
        }
        
        // Expect '='
        if (!ts_.check(TokenType::ASSIGN)) {
            LOG_DECL("parseMethodDecl: ERROR - expected '=' before method body");
            errorAt(DiagCode::E1007, "=", "before method body", ts_.peek().value);
            return nullptr;
        }
        ts_.advance(); // consume '='
        
        // Parse body - ONLY block bodies allowed for methods
        if (!ts_.check(TokenType::LBRACE)) {
            LOG_DECL("parseMethodDecl: ERROR - expected '{' for method body");
            errorAt(DiagCode::E1004, "{", "method body", ts_.peek().value);
            return nullptr;
        }
        
        method = arena_.make<MethodDeclAST>();
        method->loc = loc;
        method->name = name;
        method->funcType = funcType->as<FuncTypeAST>();
        method->body = parseBlock();
        
        ts_.match(TokenType::SEMICOLON);
        LOG_DECL_EXTREME("parseMethodDecl: inline body success");
    } else {
        // ========== ASSIGNMENT FORM (plain or injection) ==========
        LOG_DECL_EXTREME("parseMethodDecl: assignment form");
        
        if (!ts_.check(TokenType::ASSIGN)) {
            LOG_DECL("parseMethodDecl: ERROR - expected '=' after method name for assignment form");
            errorAt(DiagCode::E1007, "=", "after method name", ts_.peek().value);
            return nullptr;
        }
        ts_.advance(); // consume '='
        
        // Parse func_ref (may include generic instantiation)
        ExprPtr funcRef = parseFuncRef();
        if (!funcRef || funcRef->isa<UnknownExprAST>()) {
            LOG_DECL("parseMethodDecl: ERROR - expected function reference after '='");
            errorAt(DiagCode::E1008, "function", ts_.peek().value);
            return nullptr;
        }
        
        // Check for injection form: '(' receiver_arg ')' '!'
        bool isInjection = false;
        InternedString receiverArg;
        
        if (ts_.check(TokenType::LPAREN)) {
            LOG_DECL_EXTREME("parseMethodDecl: checking for injection form");
            ts_.advance(); // consume '('
            
            if (!ts_.check(TokenType::IDENTIFIER)) {
                LOG_DECL("parseMethodDecl: ERROR - expected receiver name in injection form");
                errorAt(DiagCode::E1002, "receiver name", ts_.peek().value);
            } else {
                receiverArg = pool_.intern(ts_.advance().value);
                LOG_DECL_EXTREME("parseMethodDecl: injection receiver = " 
                                 << pool_.lookup(receiverArg));
            }
            
            // Expect closing ')'
            if (!ts_.check(TokenType::RPAREN)) {
                LOG_DECL("parseMethodDecl: ERROR - expected ')' after receiver name");
                errorAt(DiagCode::E1005, ")", "receiver name", ts_.peek().value);
            } else {
                ts_.advance(); // Consume ')'
            }
            
            if (!ts_.match(TokenType::BANG)) {
                LOG_DECL("parseMethodDecl: ERROR - expected '!' for injection form");
                errorAt(DiagCode::E1007, "!", "for injection form", ts_.peek().value);
            } else {
                isInjection = true;
                LOG_DECL_EXTREME("parseMethodDecl: injection form detected");
            }
        }
        
        method = arena_.make<MethodDeclAST>();
        method->loc = loc;
        method->name = name;
        method->assignmentRef = funcRef;
        method->receiverArg = receiverArg;
        method->isInjection = isInjection;
        
        ts_.match(TokenType::SEMICOLON);
        LOG_DECL_EXTREME("parseMethodDecl: assignment form success");
    }
    
    // Attach metadata if we have a valid method
    if (method) {
        attachMetadata(*method, std::move(doc), std::move(attrs));
    }
    
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
        errorAt(DiagCode::E1007, ":", "before trait name", ts_.peek().value);
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
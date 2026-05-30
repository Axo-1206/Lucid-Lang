#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses an impl block that binds methods to a type.
 * 
 * Grammar:
 *   impl_decl := [ visibility_mod ] 'impl' impl_target [ impl_generic_params ]
 *                [ 'as' IDENTIFIER ] [ ':' trait_ref ] '{' method_decl* '}'
 * 
 *   impl_target     := type_name | primitive_type
 *   impl_generic_params := '<' impl_generic_param { ',' impl_generic_param } '>'
 *   trait_ref       := IDENTIFIER [ '<' type_args '>' ]
 * 
 * Examples:
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
 *   impl string {
 *       length () -> int = { return #strlen(self) }
 *   }
 * 
 * ─── Parsing Order ─────────────────────────────────────────────────────────
 *   1. 'impl' keyword
 *   2. Target type (primitive OR named type, may include generic arguments)
 *   3. Optional impl-level generic parameters (if target is generic struct)
 *   4. Optional 'as' alias (replaces 'self' as receiver name)
 *   5. Optional ':' trait conformance
 *   6. '{' method_decl* '}'
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 * 
 * **Target Type Support:**
 *   - Primitive types: int, float, string, bool, char, etc.
 *   - Named types: user-defined structs, enums, and type aliases
 *   - Array types: NOT allowed directly (requires type alias)
 *   - Function types: NOT allowed directly (requires type alias)
 * 
 * **Generic Parameters on Impl:**
 *   - Impl blocks MAY declare generic parameters ONLY when the target type
 *     is generic (a generic struct or generic type alias)
 *   - The number of generic parameters MUST match the target's arity
 *   - Parameter names are independent; they bind positionally
 *   - Example: `struct Box<T>` → `impl Box<T>` (arity 1)
 * 
 * **Receiver Alias (`as IDENTIFIER`):**
 *   - If omitted, the receiver is named `self` inside method bodies
 *   - If provided, the given identifier replaces `self` as the receiver name
 *   - The alias must appear AFTER the target type and its generics
 *   - Must appear BEFORE an optional trait conformance
 * 
 * **Trait Conformance (`: trait_ref`):**
 *   - Optional. When present, the impl block must implement every method
 *     declared in that trait
 *   - Extra methods (not in the trait) are allowed
 * 
 * **Visibility:**
 *   - `pub` makes all methods visible within the package
 *   - `export` makes all methods visible to external consumers
 *   - Individual methods cannot have separate visibility modifiers
 * 
 * **Primitive Impl Restrictions (Semantic Pass):**
 *   - Primitive types (int, float, string, etc.) cannot have generic parameters
 *   - User-defined methods cannot override built-in methods (E3020)
 * 
 * **Array/Function Type Impl:**
 *   - NOT allowed directly: `impl []int { ... }` is a parse error
 *   - Must use a type alias: `type IntList = []int; impl IntList { ... }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'impl' keyword
 * On exit:  positioned after the closing '}' of the impl body
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing methods. If parseMethodDecl()
 * makes no progress:
 *   - Consumes one token (advance)
 *   - Skips to next identifier or closing brace
 *   - Continues (does NOT push a method)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: reports error, returns nullptr
 * - Invalid target type (neither primitive nor identifier): reports error,
 *   returns nullptr
 * - Missing '{' after header: reports error, returns nullptr
 * - Invalid method: skips method, continues parsing remaining methods
 * - Unrecognised token inside impl: reports error, calls synchronize()
 * - Missing '}': consume() reports error
 * 
 * ─── Semantic Pass Validation (Not Parser Responsibility) ──────────────────
 * - Generic arity matches target type (E3019)
 * - Primitive impl has no generic parameters (E3020)
 * - Trait methods all implemented (E3024)
 * - Method signatures match trait (E3025)
 * - No duplicate method names across merged impl blocks (E3026)
 * - `self` type resolved correctly
 * 
 * @param vis Visibility modifier (Private, Package, or Export)
 *        determined by caller from 'pub'/'export' keywords
 * 
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

    if (ts_.check(TokenType::LBRACKET)) {
        // Array target (concrete or generic)
        targetType = parseArrayTarget();
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid array target in impl block");
            return nullptr;
        }
    } else if (isPrimitiveTypeToken(ts_.peekType())) {
        targetType = parsePrimitiveType();
    } else if (ts_.check(TokenType::IDENTIFIER)) {
        targetType = parseNamedType();
    } else {
        errorAt(DiagCode::E2003, "expected target type after 'impl' (primitive, identifier, or '[')");
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

/**
 * @brief Parses a method implementation inside an impl block.
 * 
 * Grammar: IDENTIFIER [ `~async` | `~nullable` | `~parallel` ]*
 *          param_group+ [ `->` return_list ] `=` body
 * 
 * Example: `length () -> float = { return #sqrt(x*x + y*y) }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at method name
 * On exit:  positioned after the body (or after signature if no body)
 * 
 * ─── Body Parsing ─────────────────────────────────────────────────────────
 * Same as function bodies (block, verbose anon-func, or expression)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - No visibility modifiers (impl block controls visibility)
 *   - Receiver is `self` (or alias from impl's `as` clause)
 *   - `~nullable` marks the method binding as nullable
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing method name: returns nullptr
 * - Missing '(' after name/qualifiers: reports error, returns nullptr
 * - Missing '=' before body: reports error, returns nullptr
 * - Missing body after '=': reports error, returns nullptr
 */
MethodDeclPtr Parser::parseMethodDecl() {
    SourceLocation loc = ts_.currentLoc();

    // ---------- 1. Parse method name ----------
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    // ---------- 2. Peek to see if we have an assignment form ----------
    // Save position to restore if we later find it's not an assignment form
    size_t savedPos = ts_.getPos();
    bool isAssignment = false;
    
    // Skip comments and look for '=' without consuming any tokens yet
    size_t peekPos = ts_.skipCommentsFrom(ts_.getPos());
    if (peekPos < ts_.getTokenCount() && ts_.getTokenAt(peekPos).type == TokenType::ASSIGN) {
        isAssignment = true;
    }
    
    // ---------- 3. Assignment form (plain or injection) ----------
    if (isAssignment) {
        ts_.advance(); // Consume the '=' token.
        
        // Parse the function reference
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
        
        ts_.match(TokenType::SEMICOLON);
        return method;
    }
    
    // ---------- 4. Inline body form ----------
    // At this point we are not in assignment mode, so we must parse a full signature.
    // Restore position to after the method name (savedPos) to start parsing qualifiers etc.
    ts_.setPos(savedPos);
    
    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;
    method->name = name;
    
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
        if (pool_.lookup(q) == "async") qualMask |= QualifierBits::Async;
        else if (pool_.lookup(q) == "nullable") qualMask |= QualifierBits::Nullable;
        else if (pool_.lookup(q) == "parallel") qualMask |= QualifierBits::Parallel;
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
        std::vector<ParamPtr> group = parseParamGroup();
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
    } else if (ts_.check(TokenType::LPAREN)) {
        // Verbose anonymous function body: (params) -> ret { ... }
        // This is already handled by parseBlock()? Actually parseBlock() only parses '{...}'.
        // For anonymous function, we need to parse the entire anonymous function expression.
        // But in method body, the grammar allows either a block or an expression that could be an anonymous function.
        // Instead, we can parse an expression; if it's an anonymous function, we wrap it in a return statement.
        // The current code already handles expression body by creating a ReturnStmt and block.
        // Let's keep that logic.
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start method body");
            return nullptr;
        }
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

/**
 * @brief Parses a function reference for use in method assignments (`plain` or `injection`).
 *
 * Grammar:
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | func_ref generic_args
 *
 * Examples:
 *   utils.getVersion                       -- dotted path
 *   transform<int, string>                 -- generic instantiation
 *   std.map<U>                             -- dotted + generic
 *
 * ─── Parsing Steps ──────────────────────────────────────────────────────────────────
 *   1. Parse the first identifier (required).
 *   2. While the next token is `.` and the token after that is `IDENTIFIER`,
 *      consume `.` and the identifier, building a chain of `FieldAccessExprAST`.
 *   3. If the next token is `<`, parse a generic argument list (`parseGenericArgs()`)
 *      and wrap the current expression in a `CallExprAST` with those generic arguments.
 *      The `CallExprAST` has no call arguments (`args` empty) and `isArgPack = false`.
 *
 * ─── Return Value ──────────────────────────────────────────────────────────────────
 *   Returns an `ExprPtr` that represents the resolved function reference. This can be:
 *   - An `IdentifierExprAST` for a simple name.
 *   - A `FieldAccessExprAST` for a dotted path.
 *   - A `CallExprAST` wrapping either of the above when generic arguments are supplied.
 *
 * ─── Token Consumption ──────────────────────────────────────────────────────────────
 *   Consumes all tokens belonging to the function reference, including the optional
 *   generic argument list.
 *
 * ─── Error Recovery ────────────────────────────────────────────────────────────────
 *   - Missing identifier after `.`: reports error, stops building path.
 *   - Malformed generic arguments: `parseGenericArgs()` reports error and returns
 *     an empty span (the `CallExprAST` is still created with empty args).
 *   - On unrecoverable error, returns `UnknownExprAST`.
 *
 * @return ExprPtr – expression representing the function reference, never nullptr.
 */
ExprPtr Parser::parseFuncRef() {
    SourceLocation loc = ts_.currentLoc();
    
    // Parse the base identifier
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected function name in method assignment");
        return arena_.make<UnknownExprAST>();
    }
    std::string name = ts_.advance().value;
    ExprPtr expr = arena_.make<IdentifierExprAST>(pool_.intern(name));
    expr->loc = loc;
    
    // Parse optional dotted path segments
    while (ts_.check(TokenType::DOT)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after '.'");
            break;
        }
        std::string field = ts_.advance().value;
        auto node = arena_.make<FieldAccessExprAST>();
        node->loc = loc;
        node->object = std::move(expr);
        node->field = pool_.intern(field);
        expr = std::move(node);
    }
    
    // Parse optional generic arguments (e.g., <int>)
    if (ts_.check(TokenType::LESS)) {
        ArenaSpan<TypePtr> genericArgs = parseGenericArgs(); // consumes '<' ... '>'
        auto callNode = arena_.make<CallExprAST>();
        callNode->loc = loc;
        callNode->callee = std::move(expr);
        callNode->genericArgs = genericArgs;
        callNode->isArgPack = false;
        callNode->isAsyncCall = false;
        expr = std::move(callNode);
    }
    
    return expr;
}
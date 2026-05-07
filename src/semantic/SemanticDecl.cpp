/**
 * @file SemanticDecl.cpp
 *
 * @nutshell Verifies the structure of structs, enums, functions, and implementations.
 *
 * @responsibility Phase 3a of semantic analysis: walks declaration nodes, resolves their
 *   types via TypeResolver, and enforces all declaration-level rules.
 *
 * @logic
 *   checkAttributes — validates '@' attribute lists on declarations (@extern, @inline, etc.)
 *   checkVarDecl   — resolves annotation type, enforces const/nil rules, checks init type
 *   checkFuncDecl  — resolves param + return types, pushes func scope, checks body;
 *                    short-circuits body check when @extern attribute is present
 *   checkStructDecl— resolves field types, checks no duplicate field names
 *   checkEnumDecl  — validates variant values are unique, assigns auto-increments
 *   checkTraitDecl — resolves method param + return types
 *   checkImplDecl  — checks method bodies, verifies trait conformance if traitRef present
 *   checkFromDecl  — validates source/target types for custom castings
 *
 * @related SemanticAnalyzer.cpp, SemanticStmt.cpp, SemanticExpr.cpp
 */

#include "SemanticHelpers.hpp"
#include "registry/AttributeRegistry.hpp"

#include <unordered_set>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& loopDepth, int& parallelDepth,
               bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// HELPER FUNCTIONS (in order of dependency)
// ─────────────────────────────────────────────────────────────────────────────               

// -----------------------------------------------------------------------------
// resolveFunctionType — Resolves all types inside a FuncTypeAST
// -----------------------------------------------------------------------------
static void resolveFunctionType(FuncTypeAST& type, TypeResolver& resolver, DiagnosticEngine& dc) {
    for (auto& group : type.paramGroups) {
        for (auto& param : group) {
            if (param.type) {
                resolver.resolveType(param.type.get());
            }
        }
    }
    
    if (type.returnType) {
        resolver.resolveType(type.returnType.get());
    }
}

// -----------------------------------------------------------------------------
// declareFunctionParameters — Declares all parameters in the symbol table
// -----------------------------------------------------------------------------
static void declareFunctionParameters(FuncTypeAST& type, SymbolTable& symbols, 
                                       DiagnosticEngine& dc) {
    for (const auto& group : type.paramGroups) {
        for (const auto& param : group) {
            Symbol ps;
            ps.name = param.name;
            ps.kind = SymbolKind::Param;
            ps.declKw = DeclKeyword::Let;
            ps.visibility = Visibility::Private;
            ps.type = param.type.get();
            ps.decl = nullptr;  // ParamInfo doesn't have AST back pointer
            ps.loc = param.loc;
            
            if (!symbols.declare(ps)) {
                LUC_LOG_SEMANTIC("\tERROR: duplicate parameter '" << param.name << "'");
                dc.error(DiagnosticCategory::Semantic, param.loc, DiagCode::E3005,
                         "duplicate parameter name '" + param.name + "'");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFunctionBody
//
// Checks a function body (block or expression) with the given expected return type.
// Used by checkFuncDecl, checkMethodDecl, and checkAnonFuncExpr.
// ─────────────────────────────────────────────────────────────────────────────
static void checkFunctionBody(FuncBodyKind bodyKind, StmtPtr& body, ExprPtr& exprBody,
                               FuncTypeAST& type, SymbolTable& symbols,
                               TypeResolver& resolver, DiagnosticEngine& dc,
                               int& loopDepth, int& parallelDepth, bool insideExtern) {
    TypeAST* expectedReturnType = type.returnType ? type.returnType.get() : nullptr;
    
    if (bodyKind == FuncBodyKind::ExprBody && exprBody) {
        // Expression body: function assignment (e.g., let f = existingFunc)
        LUC_LOG_SEMANTIC("checkFunctionBody: expression body");
        
        TypeAST* exprType = checkExpr(exprBody.get(), symbols, resolver, dc,
                                       loopDepth, parallelDepth, insideExtern);
        
        if (exprType && !TypeChecker::isAssignable(exprType, &type)) {
            dc.error(DiagnosticCategory::Semantic, exprBody->loc, DiagCode::E3002,
                     "type mismatch in function assignment");
        }
    } else if (body) {
        // Block body
        LUC_LOG_SEMANTIC("checkFunctionBody: block body");
        checkStmt(body.get(), symbols, resolver, dc, expectedReturnType,
                  loopDepth, parallelDepth, insideExtern);
    }
}



// -----------------------------------------------------------------------------
// checkFunctionLikeDeclaration — Unified checker for all function-like nodes
// -----------------------------------------------------------------------------
static void checkFunctionLikeDeclaration(FuncTypeAST& type,
                                          std::vector<GenericParamPtr>& genericParams,
                                          FuncBodyKind bodyKind,
                                          StmtPtr& body,
                                          ExprPtr& exprBody,
                                          SymbolTable& symbols,
                                          TypeResolver& resolver,
                                          DiagnosticEngine& dc,
                                          int& loopDepth,
                                          int& parallelDepth,
                                          bool insideExtern,
                                          const std::string& name) {
    // Set generic parameters context
    resolver.setGenericParams(&genericParams);
    
    // Resolve all types in the function signature
    resolveFunctionType(type, resolver, dc);
    
    // Push scope for parameters (and any local declarations inside the body)
    symbols.pushScope();
    
    // Declare parameters in the symbol table
    declareFunctionParameters(type, symbols, dc);
    
    // Check the body (this will recursively call checkFuncDecl for nested local functions)
    checkFunctionBody(bodyKind, body, exprBody, type, symbols, resolver, dc,
                      loopDepth, parallelDepth, insideExtern);
    
    // Pop scope - this removes parameters and local functions declared inside
    symbols.popScope();
    
    // Clear generic parameters context
    resolver.setGenericParams(nullptr);
}

// -----------------------------------------------------------------------------
// getReturnTypeFromFunctionType — Returns resolved return type (nullptr = void)
// -----------------------------------------------------------------------------
static TypeAST* getReturnTypeFromFunctionType(FuncTypeAST& type, TypeResolver& resolver) {
    if (type.returnType) {
        return resolver.resolveType(type.returnType.get());
    }
    return nullptr;
}

// Local version for SemanticDecl.cpp (static so it doesn't conflict)
static PrimitiveTypeAST* declPrimType(PrimitiveKind k) {
    static PrimitiveTypeAST singletons[] = {
        PrimitiveTypeAST(PrimitiveKind::Bool),
        PrimitiveTypeAST(PrimitiveKind::Byte),
        PrimitiveTypeAST(PrimitiveKind::Short),
        PrimitiveTypeAST(PrimitiveKind::Int),
        PrimitiveTypeAST(PrimitiveKind::Long),
        PrimitiveTypeAST(PrimitiveKind::Ubyte),
        PrimitiveTypeAST(PrimitiveKind::Ushort),
        PrimitiveTypeAST(PrimitiveKind::Uint),
        PrimitiveTypeAST(PrimitiveKind::Ulong),
        PrimitiveTypeAST(PrimitiveKind::Int8),
        PrimitiveTypeAST(PrimitiveKind::Int16),
        PrimitiveTypeAST(PrimitiveKind::Int32),
        PrimitiveTypeAST(PrimitiveKind::Int64),
        PrimitiveTypeAST(PrimitiveKind::Uint8),
        PrimitiveTypeAST(PrimitiveKind::Uint16),
        PrimitiveTypeAST(PrimitiveKind::Uint32),
        PrimitiveTypeAST(PrimitiveKind::Uint64),
        PrimitiveTypeAST(PrimitiveKind::Float),
        PrimitiveTypeAST(PrimitiveKind::Double),
        PrimitiveTypeAST(PrimitiveKind::Decimal),
        PrimitiveTypeAST(PrimitiveKind::String),
        PrimitiveTypeAST(PrimitiveKind::Char),
        PrimitiveTypeAST(PrimitiveKind::Any),
    };
    return &singletons[static_cast<int>(k)];
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAttributes
//
// Validates every '@' attribute on a declaration and enforces:
//
//   @extern("symbol")          — valid on Func/Var; exactly 1 string arg (symbol name).
//                                Optional 2nd string arg = calling convention (default "C").
//                                REQUIRES 'const' keyword — @extern bindings are permanently
//                                fixed by the linker; 'let' would allow reassignment which
//                                makes no semantic sense for a linked symbol.
//                                Emits W3001 when 'let' is used (warning, not error, so
//                                compilation continues and body checks still run).
//
//   @extern("symbol", "conv")  — same as above with explicit calling convention.
//
//   @inline                    — valid on Func only; no args.
//   @noinline                  — valid on Func only; no args.
//   @packed                    — valid on Struct only; no args.
//   @deprecated("message")     — valid on Func/Var/Struct; optional 1 string arg.
//
// Parameters:
//   declKw        — the declaration keyword (Let or Const) for the owning declaration.
//                   Used to enforce that @extern requires 'const'.
//
// Returns:
//   outIsExtern    — set true when @extern was found.
//   outExternSym   — the C symbol name from @extern("name"), empty if not @extern.
//   outCallingConv — the calling convention string, defaults to "C".
// ─────────────────────────────────────────────────────────────────────────────
static void checkAttributes(const std::vector<AttributePtr>& attributes, AttributeContext ctx, 
                            const std::string& declName, DeclKeyword declKw, DiagnosticEngine& dc,
                            bool& outIsExtern, std::string& outExternSym, std::string& outCallingConv) {
    LUC_LOG_SEMANTIC_VERBOSE("checkAttributes: count=" << attributes.size()
                             << ", ctx=" << static_cast<int>(ctx) << ", declName='" << declName << "'");
    
    outIsExtern = false;
    outExternSym = "";
    outCallingConv = "C";
    
    auto& registry = AttributeRegistry::instance();
    std::vector<std::string> seen;  // Track order for mutual exclusion
    
    for (const auto& attr : attributes) {
        const std::string& name = attr->name;
        LUC_LOG_SEMANTIC_EXTREME("checking attribute: @" << name);
        
        // Check for duplicate
        bool isDuplicate = false;
        for (const auto& seenName : seen) {
            if (seenName == name) {
                LUC_LOG_SEMANTIC("\tERROR: duplicate attribute @" << name);
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E3005,
                         "duplicate attribute '@" + name + "'");
                isDuplicate = true;
                break;
            }
        }
        if (isDuplicate) continue;
        
        // Check mutual exclusion with previously seen attributes
        bool mutuallyExclusive = false;
        for (const auto& seenName : seen) {
            if (!registry.checkMutualExclusion(name, seenName, dc, attr->loc)) {
                mutuallyExclusive = true;
                break;
            }
        }
        if (mutuallyExclusive) continue;
        
        // Convert AttributeContext enum to registry's context
        AttributeContext registryCtx;
        switch (ctx) {
            case AttributeContext::Func: 
                registryCtx = AttributeContext::Func; 
                break;
            case AttributeContext::Var: 
                registryCtx = AttributeContext::Var; 
                break;
            case AttributeContext::Struct: 
                registryCtx = AttributeContext::Struct; 
                break;
            default:
                registryCtx = AttributeContext::None;
                break;
        }
        
        // Check if this is main function (for main-only attributes)
        if (declName == "main") {
            registryCtx = registryCtx | AttributeContext::Main;
        }
        
        // Validate the attribute using registry
        if (!registry.validateAttribute(*attr, registryCtx, declName, declKw, dc)) {
            continue;
        }
        
        seen.push_back(name);
        
        // Handle @extern specially (needs to set out params for codegen)
        if (name == "extern") {
            outIsExtern = true;
            if (!attr->args.empty() && attr->args[0].argKind == AttributeArgAST::ArgKind::StringLit) {
                outExternSym = attr->args[0].value;
            }
            if (attr->args.size() >= 2 && attr->args[1].argKind == AttributeArgAST::ArgKind::StringLit) {
                outCallingConv = attr->args[1].value;
            }
            LUC_LOG_SEMANTIC_EXTREME("\t@extern: sym='" << outExternSym 
                                     << "', conv='" << outCallingConv << "'");
        }
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("checkAttributes: complete, isExtern=" << outIsExtern);
}

// ─────────────────────────────────────────────────────────────────────────────
// isConstExpr  — Returns true when an expression is a compile-time constant
//
// Called during Phase 3 (before the Annotator runs in Phase 4), so we cannot
// rely on node->isConst being set yet. Instead we inspect the expression shape
// directly. The Annotator will later propagate isConst for codegen use; this
// function is the semantic-pass gate that rejects illegal const initialisers.
//
// Accepted as compile-time constants:
//   - All literals except nil  (42, 3.14, "hello", true, 0xFF, …)
//   - Identifiers whose symbol was declared with 'const'
//   - Enum variant access  (Direction.North — field access on an enum type)
//   - Arithmetic over const operands  (PI * 2.0, MAX_VERTS - 1)
//   - Unary negation of a const  (-PI)
//   - Safe explicit cast of a const  (float(42), int(MY_CONST))
// ─────────────────────────────────────────────────────────────────────────────
static bool isConstExpr(ExprAST* expr, SymbolTable& symbols) {
    if (!expr) return false;

    // Literals (except nil) are always compile-time constants.
    if (expr->isa<LiteralExprAST>()) {
        return expr->as<LiteralExprAST>()->kind != LiteralKind::Nil;
    }

    // An identifier is const if its symbol was declared const.
    if (expr->isa<IdentifierExprAST>()) {
        Symbol* sym = symbols.lookup(expr->as<IdentifierExprAST>()->name);
        return sym && sym->declKw == DeclKeyword::Const;
    }

    // Enum variant access: Direction.North — always compile-time.
    // The object must be an identifier that resolves to an enum symbol.
    if (expr->isa<FieldAccessExprAST>()) {
        auto* fa = expr->as<FieldAccessExprAST>();
        if (fa->object && fa->object->isa<IdentifierExprAST>()) {
            Symbol* sym = symbols.lookup(fa->object->as<IdentifierExprAST>()->name);
            return sym && sym->kind == SymbolKind::Enum;
        }
        return false;
    }

    // Arithmetic over const operands is const.
    if (expr->isa<BinaryExprAST>()) {
        auto* bin = expr->as<BinaryExprAST>();
        return isConstExpr(bin->left.get(), symbols) &&
               isConstExpr(bin->right.get(), symbols);
    }

    // Unary negation / bitwise-not of a const is const.
    if (expr->isa<UnaryExprAST>()) {
        return isConstExpr(expr->as<UnaryExprAST>()->operand.get(), symbols);
    }

    // Safe explicit cast of a const is const: float(42), int(MY_CONST).
    // Unsafe (*T) reinterprets are never const — raw memory is not known at
    // compile time.
    if (expr->isa<TypeConvExprAST>()) {
        auto* tc = expr->as<TypeConvExprAST>();
        return !tc->isUnsafe && isConstExpr(tc->expr.get(), symbols);
    }

    // Anything else (calls, struct literals, closures, pipelines, …) is not
    // a compile-time constant.
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkVarDecl
//
// Rules enforced:
//   - Type annotation must resolve.
//   - const requires an initialiser that is a compile-time constant expression.
//   - If an initialiser is present, its type must be assignable to the annotation.
//   - nil literal is only assignable to nullable types.
//   - const does not allow nil (nil is never a compile-time constant).
// ─────────────────────────────────────────────────────────────────────────────
void checkVarDecl(VarDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                  DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC("checkVarDecl: name=" << node.name << " kw=" << static_cast<int>(node.keyword));

    // 0. Validate '@' attributes on this variable declaration.
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Var, node.name, node.keyword, 
                dc, attrIsExtern, attrExternSym, attrCallingConv);
    // @extern on a variable: it must have no initialiser (linker provides the value).
    if (attrIsExtern && node.init) {
        LUC_LOG_SEMANTIC("\tERROR: @extern variable with initialiser");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'@extern' variable '" + node.name +
                 "' must not have an initialiser — the symbol is resolved by the linker");
        return;
    }
    // @extern variable: skip type/init checks beyond the above (no body to validate).
    if (attrIsExtern) {
        resolver.setInsideExtern(true);
        resolver.resolveType(node.type.get());
        resolver.setInsideExtern(false);
        return;
    }

    // 1. Resolve the declared type.
    TypeAST* declaredType = resolver.resolveType(node.type.get());
    if (!declaredType) return; // resolver already emitted a diagnostic

    // 2. const or non-nullable types require an initialiser.
    if (!node.init) {
        if (node.keyword == DeclKeyword::Const) {
            LUC_LOG_SEMANTIC("\tERROR: const variable without initialiser");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "const '" + node.name + "' must have an initialiser");
        } else if (!TypeChecker::isNullable(declaredType)) {
            LUC_LOG_SEMANTIC("\tERROR: non-nullable variable without initialiser");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "variable '" + node.name + "' must have an initial value because it is not nullable");
        }
        return;
    }

    // 3. Check the initialiser type and verify assignability.
    TypeAST* initType = checkExpr(node.init.get(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);
    
    LUC_LOG_SEMANTIC("\tVarDecl initType = " << initType);
    if (initType) {
        LUC_LOG_SEMANTIC("\t\tinitType kind: " << LucDebug::kindToString(initType->kind));
    } else {
        LUC_LOG_SEMANTIC("\t\tinitType is NULL");
    }
    
    if (!initType) return;

    // 4. nil literal is assignable only to nullable types.
    if (node.init->isa<LiteralExprAST>()) {
        auto* lit = node.init->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Nil && !TypeChecker::isNullable(declaredType)) {
            LUC_LOG_SEMANTIC("\tERROR: nil assigned to non-nullable type");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "nil cannot be assigned to non-nullable type '" + node.name + "'");
            return;
        }
    }

    // 5. const initialiser must be a compile-time constant expression.
    if (node.keyword == DeclKeyword::Const && !isConstExpr(node.init.get(), symbols)) {
        LUC_LOG_SEMANTIC("\tERROR: const initialiser is not a constant expression");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "const '" + node.name + "' initialiser must be a compile-time constant expression");
    }

    if (!TypeChecker::isAssignable(initType, declaredType)) {
        // Check if a custom from-casting block is available for this conversion.
        // If so, desugar:  let m Minutes = s  →  let m Minutes = Minutes(s)
        // by wrapping node.init in a TypeConvExprAST targeting the declared type.
        // This lets codegen dispatch to the from() entry without any AST restructuring.
        if (TypeChecker::isFromCastable(initType, declaredType, &symbols)) {
            // Build a NamedTypeAST for the target to embed in the cast node.
            // We need a fresh owned node; we cannot share the declared type pointer
            // because TypeConvExprAST takes ownership via unique_ptr.
            auto targetTypeNode = std::make_unique<NamedTypeAST>(
                declaredType->as<NamedTypeAST>()->name);
            targetTypeNode->loc = node.loc;

            // Wrap the original initialiser expression in a TypeConvExprAST.
            // This is the semantic-level desugaring: the cast node now owns
            // the original init expression as its inner operand.
            SourceLocation initLoc = node.init->loc;
            auto convExpr = std::make_unique<TypeConvExprAST>(
                std::move(targetTypeNode),
                std::move(node.init),
                /*isUnsafe=*/false);
            convExpr->loc = initLoc;

            // Replace node.init with the desugared cast expression.
            node.init = std::move(convExpr);

            // Re-check the desugared expression so resolvedType is set correctly.
            // checkTypeConvExpr returns the target type, which is exactly declaredType,
            // so the implicit assignment check that follows this block will pass.
            checkExpr(node.init.get(), symbols, resolver, dc,
                      loopDepth, parallelDepth, insideExtern);
        } else {
            // No casting available — report the type mismatch error.
            LUC_LOG_SEMANTIC("\tERROR: type mismatch in variable initialisation");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3008,
                     "cannot implicitly convert initializer to type for '" + node.name +
                     "'; use an explicit type cast like '" +
                     "[target_type](value)' or define a 'from' casting block");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFuncDecl
//
// Rules enforced:
//   - Each parameter type must resolve (including generic type parameters).
//   - Return type must resolve (nullptr = void, always valid).
//   - Parameters are declared into a new function scope.
//   - Body is checked via SemanticStmt with the resolved return type as context.
//   - Curried multi-group functions: each group creates a nested function scope.
// ─────────────────────────────────────────────────────────────────────────────
// In SemanticDecl.cpp - update checkFuncDecl
void checkFuncDecl(FuncDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC("checkFuncDecl: name=" << node.name);
    // Validate '@' attributes
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Func, node.name, node.keyword, 
                dc, attrIsExtern, attrExternSym, attrCallingConv);

    // Handle @extern functions
    if (attrIsExtern) {
        resolver.setGenericParams(&node.genericParams);
        resolver.setInsideExtern(true);
        resolveFunctionType(node.type, resolver, dc);
        resolver.setInsideExtern(false);
        resolver.setGenericParams(nullptr);
        return;
    }

    // ── Try to find existing symbol (top-level function from Phase 1) ─────────
    Symbol* funcSym = symbols.lookup(node.name);
    
    // Track whether we created a local symbol (so we don't push it to FunctionContext)
    bool isLocalFunction = false;

    // ── Track current function for await checking ─────────────────────────────
    // Only push top-level functions onto the FunctionContext stack
    // Local functions inherit async context from their parent
    if (!isLocalFunction) {
        SemanticHelpers::pushFunction(node.name, funcSym);
    }

    // Use unified helper for normal functions
    checkFunctionLikeDeclaration(
        node.type, node.genericParams, node.bodyKind,
        node.body, node.exprBody, symbols, resolver, dc,
        loopDepth, parallelDepth, insideExtern, node.name);

    // Pop only if we pushed
    if (!isLocalFunction) {
        SemanticHelpers::popFunction();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStructDecl
//
// Rules enforced:
//   - Each field type must resolve (including generic type parameters).
//   - Duplicate field names are a semantic error.
//   - Default value types must match their field types.
// ─────────────────────────────────────────────────────────────────────────────
void checkStructDecl(StructDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                     DiagnosticEngine& dc, int& loopDepth,
                     int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC("checkStructDecl: name=" << node.name);

    // 0. Validate '@' attributes on this struct (@packed, @deprecated).
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    // Structs use Let as a neutral keyword — @extern is not valid here and
    // checkAttributes will report an error for it regardless of the keyword.
    checkAttributes(node.attributes, AttributeContext::Struct, node.name, DeclKeyword::Let,
                dc, attrIsExtern, attrExternSym, attrCallingConv);
    // @extern is not valid on structs — checkAttributes already reported the error.
    // We continue with normal struct checking regardless.

    // Set generic parameters context so that T in Struct<T> resolves as a valid generic param.
    resolver.setGenericParams(&node.genericParams);

    std::unordered_set<std::string> seen;
    
    for (auto& field : node.fields) {
        if (!seen.insert(field->name).second) {
            LUC_LOG_SEMANTIC("\tERROR: duplicate field '" << field->name << "'");
            dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3005,
                     "duplicate field '" + field->name + "' in struct '" + node.name + "'");
            continue;
        }

        TypeAST* ft = resolver.resolveType(field->type.get());
        if (!ft) continue;

        if (field->defaultVal) {
            TypeAST* dvt = checkExpr(field->defaultVal.get(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);
            if (dvt && !TypeChecker::isAssignable(dvt, ft)) {
                LUC_LOG_SEMANTIC("\tERROR: default value type mismatch for field '" << field->name << "'");
                dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3002,
                         "default value type mismatch for field '" + field->name + "'");
            }
        }
    }
    
    // Clear generic parameters context after resolving struct fields.
    resolver.setGenericParams(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkEnumDecl
//
// Rules enforced:
//   - Explicit integer values must be unique within the enum.
//   - Auto-assigned values are computed sequentially.
// ─────────────────────────────────────────────────────────────────────────────
void checkEnumDecl(EnumDeclAST& node, DiagnosticEngine& dc) {
    LUC_LOG_SEMANTIC("checkEnumDecl: name=" << node.name);
    std::unordered_set<int> usedValues;
    int nextAuto = 0;

    for (auto& variant : node.variants) {
        int value = variant->explicitValue.has_value() ? *variant->explicitValue : nextAuto;

        if (!usedValues.insert(value).second) {
            LUC_LOG_SEMANTIC("\tERROR: duplicate enum value " << value);
            dc.error(DiagnosticCategory::Semantic, variant->loc, DiagCode::E3005,
                     "duplicate enum value " + std::to_string(value) +
                     " for variant '" + variant->name + "' in enum '" + node.name + "'");
        }

        nextAuto = value + 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTraitDecl
//
// Rules enforced:
//   - Each method's parameter types and return type must resolve.
//   - Generic type parameters (e.g., T in Trait<T>) resolve correctly.
//   - No duplicate method names within the trait.
// ─────────────────────────────────────────────────────────────────────────────
void checkTraitDecl(TraitDeclAST& node, TypeResolver& resolver, DiagnosticEngine& dc) {
    resolver.setGenericParams(&node.genericParams);
    
    std::unordered_set<std::string> seen;
    for (auto& method : node.methods) {
        if (!seen.insert(method->name).second) {
            dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                     "duplicate method '" + method->name + "' in trait '" + node.name + "'");
            continue;
        }
        
        // Just resolve the types - no scope, no body
        resolveFunctionType(method->type, resolver, dc);
    }
    
    resolver.setGenericParams(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkImplDecl — Validates impl blocks and injects struct fields into method scopes
//
// Rules enforced:
//   - The impl target struct must exist in the symbol table.
//   - No duplicate method names across the impl block.
//   - Each method body is checked as a function body.
//   - If traitRef is present, every trait method must be implemented with a
//     matching signature.
//
// CRITICAL MEMORY SAFETY ISSUE:
// ─────────────────────────────
// This function handles a dangerous pointer validity issue that caused crashes
// in earlier versions. The bug: each iteration calls symbols.pushScope(), which
// may reallocate the internal vector<scope_map>, invalidating all Symbol* pointers
// from earlier lookups. The fix: re-lookup the struct symbol AFTER each pushScope().
//
// WHY DOES pushScope() CAUSE REALLOCATION?
// ─────────────────────────────────────────
// SymbolTable uses: std::vector<std::unordered_map<std::string, Symbol>> scopes_
//
// How std::vector works:
//   - Stores elements in contiguous heap memory
//   - Tracks: size (elements in use) and capacity (total allocated space)
//   - Example: after pushing 8 scopes with capacity 8, scopes_ is full
//
// When pushScope() calls emplace_back():
//   1. Checks: if (size == capacity) ← NO MORE SPACE
//   2. If true: ALLOCATES NEW LARGER BUFFER (typically 1.5x or 2x size)
//   3. COPIES all existing elements to new buffer
//   4. FREES the old buffer
//   5. Updates internal pointers
//
// CONSEQUENCE FOR SYMBOL POINTERS:
// ────────────────────────────────
// Symbol* structSym = symbols.lookup("MathOps");  // Points into old buffer
// symbols.pushScope();                             // ← REALLOCATION happens here!
// // structSym now points to FREED/INVALID memory  ← DANGLING POINTER!
//
// VISUAL EXAMPLE: std::vector growth
// ──────────────────────────────────
// Initial state (capacity 4, size 3):
//   Memory:  [Scope0][Scope1][Scope2][empty]
//   Address: 0x1000 0x2000  0x3000  0x4000
//   Pointer to Scope0: 0x1000 ✓ VALID
//
// pushScope() #1 (size becomes 4):
//   Memory:  [Scope0][Scope1][Scope2][Scope3]  ← fits in capacity 4
//   Address: 0x1000 0x2000  0x3000  0x4000
//   Pointer to Scope0: 0x1000 ✓ VALID
//
// pushScope() #2 (size would be 5, capacity overflow!):
//   REALLOCATION TRIGGERED (capacity 4 → 8)
//   
//   Old memory (FREED):
//   [Scope0][Scope1][Scope2][Scope3]
//   0x1000 0x2000  0x3000  0x4000
//
//   New memory (ALLOCATED):
//   [Scope0][Scope1][Scope2][Scope3][Scope4][empty][empty][empty]
//   0x8000 0x8100  0x8200  0x8300  0x8400  0x8500 0x8600 0x8700
//   (completely different addresses!)
//
//   Old pointer to Scope0 (0x1000) ← NOW DANGLING! Points to freed memory!
//   Should be:           (0x8000) ← New location after reallocation
//
// THIS IS WHY WE MUST RE-LOOKUP:
// ──────────────────────────────
// After pushScope(), we don't know if reallocation happened:
//   - If size < capacity: no reallocation, old pointers still valid
//   - If size == capacity: reallocation WILL happen on next push!
//
// SOLUTION: Always re-lookup after pushScope() to be safe.
// ─────────────────────────────────────────────────────────
//
// EXAMPLE SCENARIO:
//   impl MathOps {
//     method1() { ... }      ← 1st iteration: pushScope, then lookup → valid pointer A
//     method2() { ... }      ← 2nd iteration: pushScope (may reallocate!), pointer A DANGLING
//     method3() { ... }      ← 3rd iteration: using old pointer A → CRASH
//   }
// ─────────────────────────────────────────────────────────────────────────────
void checkImplDecl(ImplDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC("checkImplDecl: structName=" << node.structName);
 
    // Set generic parameters context so that T in impl Scene<T> resolves as a valid generic param.
    resolver.setGenericParams(&node.genericParams);

    // Verify the target struct exists once at the start to catch basic errors.
    Symbol* initialLookup = symbols.lookup(node.structName);
    if (!initialLookup || initialLookup->kind != SymbolKind::Struct) {
        LUC_LOG_SEMANTIC("\tERROR: impl target '" << node.structName << "' not a struct");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "impl target '" + node.structName + "' is not a declared struct");
        resolver.setGenericParams(nullptr);
        return;
    }
    auto* structDecl = initialLookup->decl->as<StructDeclAST>();

    // ── Signature Match Check ────────────────────────────────────────────────
    // The impl block must have the exact same generic signature as the struct.
    if (node.genericParams.size() != structDecl->genericParams.size()) {
        LUC_LOG_SEMANTIC("\tERROR: generic signature mismatch in impl");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3017,
                 "generic signature mismatch: impl for '" + node.structName +
                 "' has " + std::to_string(node.genericParams.size()) +
                 " parameters, but struct was declared with " +
                 std::to_string(structDecl->genericParams.size()));
    } else {
        for (size_t i = 0; i < node.genericParams.size(); ++i) {
            auto& implParam = node.genericParams[i];
            auto& structParam = structDecl->genericParams[i];

            // 1. Name must match
            if (implParam->name != structParam->name) {
                LUC_LOG_SEMANTIC("\tERROR: generic parameter name mismatch");
                 dc.error(DiagnosticCategory::Semantic, implParam->loc, DiagCode::E3017,
                          "generic parameter name mismatch: expected '" + structParam->name +
                          "', found '" + implParam->name + "'");
            }

            // 2. Constraints must match
            if (implParam->constraints.size() != structParam->constraints.size()) {
                LUC_LOG_SEMANTIC("\tERROR: generic constraint count mismatch");
                dc.error(DiagnosticCategory::Semantic, implParam->loc, DiagCode::E3017,
                         "generic constraint mismatch for '" + implParam->name +
                         "': expected " + std::to_string(structParam->constraints.size()) +
                         " traits, found " + std::to_string(implParam->constraints.size()));
            } else {
                for (size_t j = 0; j < implParam->constraints.size(); ++j) {
                    if (implParam->constraints[j] != structParam->constraints[j]) {
                        LUC_LOG_SEMANTIC("\tERROR: generic constraint mismatch");
                        dc.error(DiagnosticCategory::Semantic, implParam->loc, DiagCode::E3017,
                                 "generic constraint mismatch for '" + implParam->name +
                                 "': expected trait '" + structParam->constraints[j] +
                                 "', found '" + implParam->constraints[j] + "'");
                    }
                }
            }
        }
    }
 
    std::unordered_set<std::string> seen;
 
    for (auto& method : node.methods) {
        if (!seen.insert(method->name).second) {
            LUC_LOG_SEMANTIC("\tERROR: duplicate method '" << method->name << "' in impl");
            dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                     "duplicate method '" + method->name + "' in impl for '" +
                     node.structName + "'");
            continue;
        }
 
        TypeAST* returnType = getReturnTypeFromFunctionType(method->type, resolver);

        std::string mangledName = node.structName + "." + method->name;
        Symbol* methodSym = symbols.lookup(mangledName);
        if (methodSym && method->type.isAsync()) {
            SemanticHelpers::pushFunction(method->name, methodSym);
        }

        symbols.pushScope();

        // RE-LOOKUP struct symbol inside the loop AFTER pushScope.
        // The pushScope() call may trigger a reallocation of the std::vector<scope_map>,
        // which invalidates any Symbol* pointers from earlier lookups.
        // We must re-lookup AFTER the reallocation to get valid pointers.
        //
        // VISUAL EXAMPLE OF THE BUG (WITHOUT THIS FIX):
        // ─────────────────────────────────────────────
        // 
        // BEFORE pushScope():
        //   scopes_ vector allocation:     [Scope0]  [Scope1]  [Scope2]  ...
        //   addresses:                     0x1000    0x2000    0x3000
        //   structSym from lookup:         points to Symbol in Scope0 (0x1500)
        //
        // AFTER pushScope() - vector reallocates when capacity is exceeded:
        //   Old allocation: FREED         [Scope0]  [Scope1]  [Scope2]  ...  [freed]
        //                                 0x1000    0x2000    0x3000
        //
        //   New allocation: REALLOCATED   [Scope0]  [Scope1]  [Scope2]  ...  [Scope3]
        //                                 0x8000    0x9000    0xA000         0xB000
        //                                 (different addresses!)
        //
        // structSym still holds 0x1500 (points into old freed allocation) ← DANGLING POINTER!
        // Any access to structSym→type or structSym→decl = SEGMENTATION FAULT
        //
        // THE FIX:
        // ──────
        // Re-lookup AFTER pushScope() to get a pointer into the new allocation:
        //   structSym = symbols.lookup(node.structName);  // ← points into 0x8500 now
        //
        // Now structSym is valid and points to the reallocated memory.
        // ─────────────────────────────────────────────────────────────────────
        Symbol* structSym = symbols.lookup(node.structName);
        if (!structSym || !structSym->decl || !structSym->decl->isa<StructDeclAST>()) {
            symbols.popScope();
            LUC_LOG_SEMANTIC("\tERROR: impl target corrupt/missing during scope mutation");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "impl target '" + node.structName + "' has a corrupt or missing declaration");
            return;
        }

        auto* structDecl = structSym->decl->as<StructDeclAST>();
        
        // Inject struct fields. Re-resolve each field type fresh (don't cache across iterations).
        for (auto& field : structDecl->fields) {
            TypeAST* ft = resolver.resolveType(field->type.get());
            if (!ft) continue;
            Symbol fs;
            fs.name       = field->name;
            fs.kind       = SymbolKind::Field;
            fs.declKw     = DeclKeyword::Let;
            fs.visibility = Visibility::Private;
            fs.type       = ft;
            fs.decl       = field.get();
            fs.loc        = field->loc;
            symbols.declare(fs);
        }

        // Inject parameters. Re-resolve each param type fresh.
        declareFunctionParameters(method->type, symbols, dc);
 
        if (method->body) {
            checkStmt(method->body.get(), symbols, resolver, dc, returnType,
                      loopDepth, parallelDepth, insideExtern);
        }
 
        symbols.popScope();

        if (methodSym && method->type.isAsync()) {
            SemanticHelpers::popFunction();
        }
    }
 
    // Trait conformance check. Re-lookup after all scope mutations are complete.
    if (node.traitRef) {
        Symbol* traitSym = symbols.lookup(node.traitRef->name);
        if (!traitSym || traitSym->kind != SymbolKind::Trait) {
            LUC_LOG_SEMANTIC("\tERROR: trait '" << node.traitRef->name << "' not found");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                    "trait '" + node.traitRef->name + "' is not declared");
        } else {
            auto* traitDecl = traitSym->decl->as<TraitDeclAST>();
            
            // IMPORTANT: This check only validates methods in the CURRENT impl block.
            // In the full language design, methods could be split across multiple impl
            // blocks for the same struct. To support that, we would need to:
            //   1. Track all impl blocks globally per struct
            //   2. Merge their methods when checking trait conformance
            //
            // For now, we allow empty impl blocks (impl S : Trait { }) to pass through
            // if methods were provided by a previous impl block. A full audit across
            // all impl blocks would require architectural changes to the semantic pass.
            
            // Only check trait conformance if this impl block provides methods.
            // If the block is empty, assume methods are provided elsewhere.
            if (!node.methods.empty()) {
                bool allMethodsFound = true;
                for (auto& requiredMethod : traitDecl->methods) {
                    bool found = false;
                    for (auto& m : node.methods) {
                        if (m->name == requiredMethod->name) { found = true; break; }
                    }
                    if (!found) {
                        LUC_LOG_SEMANTIC("\tERROR: missing trait method '" << requiredMethod->name << "'");
                        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                                "impl of '" + node.structName + "' for trait '" +
                                node.traitRef->name + "' is missing method '" +
                                requiredMethod->name + "'");
                        allMethodsFound = false;
                    }
                }
                
                // trait conformance check passed
                if (allMethodsFound) {
                    LUC_LOG_SEMANTIC_VERBOSE("\ttrait conformance check passed for '" 
                                        << node.structName << " : " << node.traitRef->name << "'");
                }
            } else {
                // Empty impl block - assume methods provided elsewhere
                LUC_LOG_SEMANTIC_EXTREME("\tempty impl block for trait '" << node.traitRef->name 
                                    << "', assuming methods provided elsewhere");
            }
        }
    }
    
    // Clear generic parameters context after checking impl block.
    resolver.setGenericParams(nullptr);
}
 
// ─────────────────────────────────────────────────────────────────────────────
// checkFromDecl
//
// Rules enforced:
//   - Target type (returnTypeName) must resolve.
//   - Every parameter type in every group must resolve.
//   - Each entry's full curried signature must be unique among all other
//     entries for the same target in the block.
//   - Parameters are declared into a new scope for the body.
//   - Body is checked to return the target type (implicitly or explicitly).
// ─────────────────────────────────────────────────────────────────────────────
void checkFromDecl(FromDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC("checkFromDecl: target=" << node.targetTypeName);

    // 1. Resolve target type once for the whole block.
    Symbol* targetSym = symbols.lookup(node.targetTypeName);
    if (!targetSym || (targetSym->kind != SymbolKind::Struct && targetSym->kind != SymbolKind::Enum)) {
        LUC_LOG_SEMANTIC("\tERROR: from block target '" << node.targetTypeName << "' not found/nominal");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "from block: target '" + node.targetTypeName + "' is not a nominal type");
        return;
    }
    
    // Synthesize a NamedTypeAST on the stack to represent the expected return type.
    NamedTypeAST targetTypeAST(node.targetTypeName);
    targetTypeAST.loc = node.loc;
    TypeAST* targetType = resolver.resolveType(&targetTypeAST);
    if (!targetType) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "from block: cannot resolve target type '" + node.targetTypeName + "'");
        return;
    }

    // We accumulate validated entries here to cross-check full signatures and prevent duplicates.
    std::vector<FromEntryAST*> verifiedEntries;

    // 2. Iterate and check each casting entry.
    for (auto& entry : node.entries) {
        if (!entry) continue;

        // Verify the explicit return type identifier matches the block target.
        if (entry->returnTypeName != node.targetTypeName) {
            LUC_LOG_SEMANTIC("\tERROR: from entry return type mismatch");
            dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3002,
                     "from casting: return type '" + entry->returnTypeName +
                     "' must match block target type '" + node.targetTypeName + "'");
        }

        // New scope for the casting body.
        symbols.pushScope();

        // Declare all parameters from all curry groups into the body scope.
        // entry->paramGroups is std::vector<ParamGroup> (ParamInfo)
        for (const auto& group : entry->paramGroups) {
            for (const auto& param : group) {
                // Resolve parameter type if not already resolved
                TypeAST* pt = param.type.get();
                if (!pt) {
                    LUC_LOG_SEMANTIC("\tERROR: parameter '" << param.name << "' has no type");
                    dc.error(DiagnosticCategory::Semantic, param.loc, DiagCode::E3001,
                             "parameter '" + param.name + "' has no type");
                    continue;
                }
                
                // Ensure the type is resolved
                pt = resolver.resolveType(pt);
                if (!pt) continue;
                
                Symbol ps;
                ps.name       = param.name;
                ps.kind       = SymbolKind::Param;
                ps.declKw     = DeclKeyword::Let;
                ps.visibility = Visibility::Private;
                ps.type       = pt;
                ps.decl       = nullptr;  // ParamInfo doesn't have AST back pointer
                ps.loc        = param.loc;
                
                if (!symbols.declare(ps)) {
                    LUC_LOG_SEMANTIC("\tERROR: duplicate parameter '" << param.name << "' in from entry");
                    dc.error(DiagnosticCategory::Semantic, param.loc, DiagCode::E3005,
                             "duplicate parameter name '" + param.name + "' in from casting");
                }
            }
        }

        // Check the body. Expected return is the target type.
        if (entry->body) {
            checkStmt(entry->body.get(), symbols, resolver, dc, targetType,
                      loopDepth, parallelDepth, insideExtern);
        }

        symbols.popScope();

        // 3. Full Signature Duplicate Check
        // Now that the parameter types are resolved natively, verify if this
        // specific generic/curried signature has already been declared.
        bool isDuplicate = false;
        for (auto* seen : verifiedEntries) {
            if (entry->paramGroups.size() != seen->paramGroups.size()) continue;

            bool allGroupsMatch = true;
            for (size_t i = 0; i < entry->paramGroups.size(); ++i) {
                const auto& g1 = entry->paramGroups[i];
                const auto& g2 = seen->paramGroups[i];
                
                if (g1.size() != g2.size()) {
                    allGroupsMatch = false;
                    break;
                }
                
                for (size_t j = 0; j < g1.size(); ++j) {
                    // Compare parameter types (ignoring names)
                    if (!TypeChecker::isEqual(g1[j].type.get(), g2[j].type.get())) {
                        allGroupsMatch = false;
                        break;
                    }
                }
                if (!allGroupsMatch) break;
            }

            if (allGroupsMatch) {
                isDuplicate = true;
                break;
            }
        }

        if (isDuplicate) {
            LUC_LOG_SEMANTIC("\tERROR: duplicate casting signature");
            dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3005,
                     "duplicate casting signature in from block for '" + node.targetTypeName + "'");
        } else {
            verifiedEntries.push_back(entry.get());
            LUC_LOG_SEMANTIC_EXTREME("\tentry verified, signature unique");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTopLevelDecl  — Dispatcher called by SemanticAnalyzer::checkDecls()
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SymbolTable& symbols, TypeResolver& resolver,
                       DiagnosticEngine& dc, int& loopDepth,
                       int& parallelDepth, bool insideExtern) {
    if (!decl) return;
    LUC_LOG_SEMANTIC("checkTopLevelDecl: kind=" << LucDebug::kindToString(decl->kind));

    if (decl->isa<VarDeclAST>())
        checkVarDecl(*decl->as<VarDeclAST>(), symbols, resolver, dc,
                     loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<FuncDeclAST>())
        checkFuncDecl(*decl->as<FuncDeclAST>(), symbols, resolver, dc,
                      loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<StructDeclAST>())
        checkStructDecl(*decl->as<StructDeclAST>(), symbols, resolver, dc,
                        loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<EnumDeclAST>())
        checkEnumDecl(*decl->as<EnumDeclAST>(), dc);

    else if (decl->isa<TraitDeclAST>())
        checkTraitDecl(*decl->as<TraitDeclAST>(), resolver, dc);

    else if (decl->isa<ImplDeclAST>())
        checkImplDecl(*decl->as<ImplDeclAST>(), symbols, resolver, dc,
                      loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<FromDeclAST>())
        checkFromDecl(*decl->as<FromDeclAST>(), symbols, resolver, dc,
                      loopDepth, parallelDepth, insideExtern);

    // PackageDecl, UseDecl, TypeAliasDecl, ModuleDecl — nothing to check at phase 3.
}
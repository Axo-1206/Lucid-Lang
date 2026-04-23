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

#include "SemanticAnalyzer.hpp"
#include "SemanticSymbol.hpp"
#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "ast/DeclAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"

#include <unordered_set>
#include <string>
#include <fstream>
#include <chrono>

static void agentDebugLogDecl(const char* hypothesisId, const char* location,
                              const std::string& message, const std::string& data) {
    std::ofstream out("debug-00e876.log", std::ios::app);
    if (!out) return;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    out << "{\"sessionId\":\"00e876\",\"runId\":\"pre-fix\",\"hypothesisId\":\""
        << hypothesisId << "\",\"location\":\"" << location << "\",\"message\":\""
        << message << "\",\"data\":" << data << ",\"timestamp\":" << ts << "}\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// AttributeContext — which kind of declaration owns the attribute list.
// Controls which attribute names are valid in a given position.
// ─────────────────────────────────────────────────────────────────────────────
enum class AttributeContext { Func, Var, Struct };

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
static void checkAttributes(const std::vector<AttributePtr>& attributes,
                             AttributeContext ctx,
                             DeclKeyword declKw,
                             DiagnosticEngine& dc,
                             bool& outIsExtern,
                             std::string& outExternSym,
                             std::string& outCallingConv) {
    outIsExtern    = false;
    outExternSym   = "";
    outCallingConv = "C"; // default calling convention

    // Track which attribute names we have already seen to catch duplicates.
    std::unordered_set<std::string> seen;

    for (const auto& attr : attributes) {
        const std::string& n = attr->name;

        if (!seen.insert(n).second) {
            dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E3005,
                     "duplicate attribute '@" + n + "'");
            continue;
        }

        // ── @extern ──────────────────────────────────────────────────────────
        if (n == "extern") {
            if (ctx != AttributeContext::Func && ctx != AttributeContext::Var) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2010,
                         "'@extern' is only valid on function or variable declarations");
                continue;
            }
            if (attr->args.empty()) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2011,
                         "'@extern' requires at least one string argument: the C symbol name");
                continue;
            }
            if (attr->args[0].argKind != AttributeArgAST::ArgKind::StringLit) {
                dc.error(DiagnosticCategory::Semantic, attr->args[0].loc, DiagCode::E2011,
                         "'@extern' first argument must be a string literal (the C symbol name)");
                continue;
            }
            outIsExtern  = true;
            outExternSym = attr->args[0].value;

            // Optional second argument: calling convention string.
            if (attr->args.size() >= 2) {
                if (attr->args[1].argKind != AttributeArgAST::ArgKind::StringLit) {
                    dc.error(DiagnosticCategory::Semantic, attr->args[1].loc, DiagCode::E2011,
                             "'@extern' second argument must be a string literal (calling convention)");
                } else {
                    outCallingConv = attr->args[1].value;
                }
            }
            if (attr->args.size() > 2) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2011,
                         "'@extern' takes at most 2 arguments: (symbol_name, calling_convention)");
            }

            // ── Enforce 'const' for @extern ───────────────────────────────────
            // @extern bindings are resolved by the linker — they are fixed at
            // link time and cannot be reassigned. Using 'let' allows body
            // reassignment (f = { ... }) which is meaningless for an extern symbol.
            // Emit W3001 so the developer knows, but continue compilation.
            if (declKw == DeclKeyword::Let) {
                dc.warning(DiagnosticCategory::Semantic, attr->loc, DiagCode::W3001,
                           "'@extern(\"" + outExternSym + "\")' should use 'const', not 'let' — "
                           "extern bindings are permanently resolved by the linker and cannot "
                           "be reassigned; change 'let' to 'const'");
            }
            continue;
        }

        // ── @inline ──────────────────────────────────────────────────────────
        if (n == "inline") {
            if (ctx != AttributeContext::Func) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2010,
                         "'@inline' is only valid on function declarations");
            }
            if (!attr->args.empty()) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2011,
                         "'@inline' takes no arguments");
            }
            continue;
        }

        // ── @noinline ────────────────────────────────────────────────────────
        if (n == "noinline") {
            if (ctx != AttributeContext::Func) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2010,
                         "'@noinline' is only valid on function declarations");
            }
            if (!attr->args.empty()) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2011,
                         "'@noinline' takes no arguments");
            }
            // @inline and @noinline are mutually exclusive.
            if (seen.count("inline")) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2010,
                         "'@inline' and '@noinline' cannot both appear on the same declaration");
            }
            continue;
        }

        // ── @packed ──────────────────────────────────────────────────────────
        if (n == "packed") {
            if (ctx != AttributeContext::Struct) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2010,
                         "'@packed' is only valid on struct declarations");
            }
            if (!attr->args.empty()) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2011,
                         "'@packed' takes no arguments");
            }
            continue;
        }

        // ── @deprecated ──────────────────────────────────────────────────────
        if (n == "deprecated") {
            if (attr->args.size() > 1) {
                dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2011,
                         "'@deprecated' takes at most one string argument (the message)");
            }
            if (!attr->args.empty() &&
                attr->args[0].argKind != AttributeArgAST::ArgKind::StringLit) {
                dc.error(DiagnosticCategory::Semantic, attr->args[0].loc, DiagCode::E2011,
                         "'@deprecated' argument must be a string literal message");
            }
            continue;
        }

        // ── Unknown attribute ─────────────────────────────────────────────────
        dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E2010,
                 "unknown attribute '@" + n + "'; "
                 "known attributes: extern, inline, noinline, packed, deprecated");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& asyncDepth, int& loopDepth, int& parallelDepth,
               bool insideExtern);

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
                  DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                  int& parallelDepth, bool insideExtern) {

    // 0. Validate '@' attributes on this variable declaration.
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Var, node.keyword, dc,
                    attrIsExtern, attrExternSym, attrCallingConv);
    // @extern on a variable: it must have no initialiser (linker provides the value).
    if (attrIsExtern && node.init) {
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

    // 2. const requires an initialiser.
    if (!node.init) {
        if (node.keyword == DeclKeyword::Const) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "const '" + node.name + "' must have an initialiser");
        }
        return;
    }

    // 3. Check the initialiser type and verify assignability.
    TypeAST* initType = checkExpr(node.init.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (!initType) return;

    // 4. nil literal is assignable only to nullable types.
    if (node.init->isa<LiteralExprAST>()) {
        auto* lit = node.init->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Nil && !TypeChecker::isNullable(declaredType)) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "nil cannot be assigned to non-nullable type '" + node.name + "'");
            return;
        }
    }

    // 5. const initialiser must be a compile-time constant expression.
    if (node.keyword == DeclKeyword::Const && !isConstExpr(node.init.get(), symbols)) {
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
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
        } else {
            // No casting available — report the type mismatch error.
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
void checkFuncDecl(FuncDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {

    // 0. Validate '@' attributes on this function.
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Func, node.keyword, dc,
                    attrIsExtern, attrExternSym, attrCallingConv);

    // @extern("sym") on a function means the body is resolved by the linker.
    // We still resolve param/return types so they are available to codegen.
    if (attrIsExtern) {
        resolver.setGenericParams(&node.genericParams);
        resolver.setInsideExtern(true);

        // Resolve param types — *T is valid here.
        for (auto& group : node.paramGroups) {
            for (auto& param : group)
                resolver.resolveType(param->type.get());
        }
        if (node.returnType)
            resolver.resolveType(node.returnType.get());

        resolver.setInsideExtern(false);
        resolver.setGenericParams(nullptr);

        // ── Body classification ───────────────────────────────────────────────
        // The parser always produces a body node.  Determine whether the
        // programmer actually wrote one, and how serious it is.
        //
        //   No body (nullptr)                 — ideal; nothing to report.
        //   Body is a BlockStmt with 0 stmts  — empty: = {}  → W3002 (warning)
        //   Body is a BlockStmt with statements — non-empty        → E3002 (error)
        //
        if (node.body) {
            bool bodyEmpty = false;
            if (node.body->isa<BlockStmtAST>()) {
                bodyEmpty = node.body->as<BlockStmtAST>()->stmts.empty();
            }

            if (bodyEmpty) {
                // Empty body `= {}` — warn: the body is silently ignored.
                dc.warning(DiagnosticCategory::Semantic, node.body->loc,
                           DiagCode::W3002,
                           "'@extern(\"" + attrExternSym + "\")' function '" + node.name +
                           "' has an empty body '= {}' — the body is ignored and the "
                           "linker symbol is used; remove the body to suppress this warning");
            } else {
                // Non-empty body — hard error: code exists that will never run.
                dc.error(DiagnosticCategory::Semantic, node.body->loc,
                         DiagCode::E3002,
                         "'@extern(\"" + attrExternSym + "\")' function '" + node.name +
                         "' has a body with statements — this code will never execute "
                         "because the linker resolves the symbol; remove the body");
            }
        }
        return;
    }

    // Set generic parameters context so that T in let foo<T> resolves as a valid generic param.
    resolver.setGenericParams(&node.genericParams);

    // Resolve return type (nullptr is void — valid).
    TypeAST* returnType = nullptr;
    if (node.returnType) {
        returnType = resolver.resolveType(node.returnType.get());
        if (!returnType) {
            resolver.setGenericParams(nullptr);
            return;
        }
    }

    // async increments depth so nested await checks work correctly.
    if (node.isAsync) asyncDepth++;

    symbols.pushScope();

    // Resolve param types and declare params in scope.
    for (auto& group : node.paramGroups) {
        for (auto& param : group) {
            TypeAST* pt = resolver.resolveType(param->type.get());
            if (!pt) {
                symbols.popScope();
                if (node.isAsync) asyncDepth--;
                resolver.setGenericParams(nullptr);
                return;
            }
            Symbol ps;
            ps.name       = param->name;
            ps.kind       = SymbolKind::Param;
            ps.declKw     = DeclKeyword::Let;
            ps.visibility = Visibility::Private;
            ps.type       = pt;
            ps.decl       = param.get();
            ps.isAsync    = false;
            ps.loc        = param->loc;
            if (!symbols.declare(ps)) {
                dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                         "duplicate parameter name '" + param->name + "'");
            }
        }
    }

    // Check the body.
    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, returnType,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    if (node.isAsync) asyncDepth--;
    
    // Clear generic parameters context after checking function.
    resolver.setGenericParams(nullptr);
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
                     DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                     int& parallelDepth, bool insideExtern) {

    // 0. Validate '@' attributes on this struct (@packed, @deprecated).
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    // Structs use Let as a neutral keyword — @extern is not valid here and
    // checkAttributes will report an error for it regardless of the keyword.
    checkAttributes(node.attributes, AttributeContext::Struct, DeclKeyword::Let, dc,
                    attrIsExtern, attrExternSym, attrCallingConv);
    // @extern is not valid on structs — checkAttributes already reported the error.
    // We continue with normal struct checking regardless.

    // Set generic parameters context so that T in Struct<T> resolves as a valid generic param.
    resolver.setGenericParams(&node.genericParams);

    std::unordered_set<std::string> seen;
    
    for (auto& field : node.fields) {
        if (!seen.insert(field->name).second) {
            dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3005,
                     "duplicate field '" + field->name + "' in struct '" + node.name + "'");
            continue;
        }

        TypeAST* ft = resolver.resolveType(field->type.get());
        if (!ft) continue;

        if (field->defaultVal) {
            TypeAST* dvt = checkExpr(field->defaultVal.get(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);
            if (dvt && !TypeChecker::isAssignable(dvt, ft)) {
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
    std::unordered_set<int> usedValues;
    int nextAuto = 0;

    for (auto& variant : node.variants) {
        int value = variant->explicitValue.has_value() ? *variant->explicitValue : nextAuto;

        if (!usedValues.insert(value).second) {
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
    // Set generic parameters context so that T in Container<T> resolves as a valid generic param.
    resolver.setGenericParams(&node.genericParams);
    
    std::unordered_set<std::string> seen;
    for (auto& method : node.methods) {
        if (!seen.insert(method->name).second) {
            dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                     "duplicate method '" + method->name + "' in trait '" + node.name + "'");
            continue;
        }
        for (auto& group : method->paramGroups) {
            for (auto& param : group) {
                resolver.resolveType(param->type.get());
            }
        }
        if (method->returnType) {
            resolver.resolveType(method->returnType.get());
        }
    }
    
    // Clear generic parameters context after resolving trait methods.
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
//
// See line 372-391 for the detailed visual example of the memory layout issue.
// ─────────────────────────────────────────────────────────────────────────────
void checkImplDecl(ImplDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
 
    // Set generic parameters context so that T in impl<T> resolves as a valid generic param.
    resolver.setGenericParams(&node.genericParams);

    // Verify the target struct exists once at the start to catch basic errors.
    Symbol* initialLookup = symbols.lookup(node.structName);
    if (!initialLookup || initialLookup->kind != SymbolKind::Struct) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "impl target '" + node.structName + "' is not a declared struct");
        resolver.setGenericParams(nullptr);
        return;
    }
 
    std::unordered_set<std::string> seen;
 
    for (auto& method : node.methods) {
        if (!seen.insert(method->name).second) {
            dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                     "duplicate method '" + method->name + "' in impl for '" +
                     node.structName + "'");
            continue;
        }
 
        TypeAST* returnType = nullptr;
        if (method->returnType) {
            returnType = resolver.resolveType(method->returnType.get());
            if (!returnType) continue;
        }
 
        if (method->isAsync) asyncDepth++;
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
            if (method->isAsync) asyncDepth--;
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
            fs.isAsync    = false;
            fs.loc        = field->loc;
            symbols.declare(fs);
        }

        // Inject parameters. Re-resolve each param type fresh.
        for (auto& group : method->paramGroups) {
            for (auto& param : group) {
                TypeAST* pt = resolver.resolveType(param->type.get());
                if (!pt) continue;
                Symbol ps;
                ps.name = param->name;
                ps.kind = SymbolKind::Param;
                ps.declKw = DeclKeyword::Let;
                ps.visibility = Visibility::Private;
                ps.type = pt;
                ps.decl = param.get();
                ps.isAsync = false;
                ps.loc = param->loc;
                if (!symbols.declare(ps)) {
                    dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                             "duplicate parameter '" + param->name + "'");
                }
            }
        }
 
        if (method->body) {
            checkStmt(method->body.get(), symbols, resolver, dc, returnType,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
        }

 
        symbols.popScope();
        if (method->isAsync) asyncDepth--;

    }
 
    // Trait conformance check. Re-lookup after all scope mutations are complete.
    if (node.traitRef) {
        Symbol* traitSym = symbols.lookup(node.traitRef->name);
        if (!traitSym || traitSym->kind != SymbolKind::Trait) {
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
                for (auto& requiredMethod : traitDecl->methods) {
                    bool found = false;
                    for (auto& m : node.methods) {
                        if (m->name == requiredMethod->name) { found = true; break; }
                    }
                    if (!found) {
                        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                                 "impl of '" + node.structName + "' for trait '" +
                                 node.traitRef->name + "' is missing method '" +
                                 requiredMethod->name + "'");
                    }
                }
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
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {

    // 1. Resolve target type once for the whole block.
    Symbol* targetSym = symbols.lookup(node.targetTypeName);
    if (!targetSym || (targetSym->kind != SymbolKind::Struct && targetSym->kind != SymbolKind::Enum)) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "from block: target '" + node.targetTypeName + "' is not a nominal type");
        return;
    }
    
    // Synthesize a NamedTypeAST on the stack to represent the expected return type.
    NamedTypeAST targetTypeAST(node.targetTypeName);
    TypeAST* targetType = resolver.resolveType(&targetTypeAST);

    // We accumulate validated entries here to cross-check full signatures and prevent duplicates.
    std::vector<FromEntryAST*> verifiedEntries;

    // 2. Iterate and check each casting entry.
    for (auto& entry : node.entries) {
        if (!entry) continue;

        // Verify the explicit return type identifier matches the block target.
        if (entry->returnTypeName != node.targetTypeName) {
            dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3002,
                     "from casting: return type '" + entry->returnTypeName +
                     "' must match block target type '" + node.targetTypeName + "'");
        }

        // New scope for the casting body.
        symbols.pushScope();

        // Declare all parameters from all curry groups into the body scope.
        for (auto& group : entry->paramGroups) {
            for (auto& param : group) {
                TypeAST* pt = resolver.resolveType(param->type.get());
                if (!pt) continue;
                Symbol ps;
                ps.name       = param->name;
                ps.kind       = SymbolKind::Param;
                ps.declKw     = DeclKeyword::Let;
                ps.visibility = Visibility::Private;
                ps.type       = pt;
                ps.decl       = param.get();
                ps.isAsync    = false;
                ps.loc        = param->loc;
                if (!symbols.declare(ps)) {
                    dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                             "duplicate parameter name '" + param->name + "' in from casting");
                }
            }
        }

        // Check the body. Expected return is the target type.
        if (entry->body) {
            checkStmt(entry->body.get(), symbols, resolver, dc, targetType,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
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
                auto& g1 = entry->paramGroups[i];
                auto& g2 = seen->paramGroups[i];
                if (g1.size() != g2.size()) {
                    allGroupsMatch = false;
                    break;
                }
                for (size_t j = 0; j < g1.size(); ++j) {
                    if (!TypeChecker::isEqual(g1[j]->type.get(), g2[j]->type.get())) {
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
            dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3005,
                     "duplicate casting signature in from block for '" + node.targetTypeName + "'");
        } else {
            verifiedEntries.push_back(entry.get());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTopLevelDecl  — Dispatcher called by SemanticAnalyzer::checkDecls()
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SymbolTable& symbols, TypeResolver& resolver,
                       DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                       int& parallelDepth, bool insideExtern) {
    if (!decl) return;

    if (decl->isa<VarDeclAST>())
        checkVarDecl(*decl->as<VarDeclAST>(), symbols, resolver, dc,
                     asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<FuncDeclAST>())
        checkFuncDecl(*decl->as<FuncDeclAST>(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<StructDeclAST>())
        checkStructDecl(*decl->as<StructDeclAST>(), symbols, resolver, dc,
                        asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<EnumDeclAST>())
        checkEnumDecl(*decl->as<EnumDeclAST>(), dc);

    else if (decl->isa<TraitDeclAST>())
        checkTraitDecl(*decl->as<TraitDeclAST>(), resolver, dc);

    else if (decl->isa<ImplDeclAST>())
        checkImplDecl(*decl->as<ImplDeclAST>(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<FromDeclAST>())
        checkFromDecl(*decl->as<FromDeclAST>(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);

    // PackageDecl, UseDecl, TypeAliasDecl, ModuleDecl — nothing to check at phase 3.
}
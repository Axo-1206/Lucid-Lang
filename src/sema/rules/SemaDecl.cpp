/// @file SemaDecl.cpp
/// @brief Implements Sema.hpp's "Declarations" section — registration and resolution.
/// 
/// This file is split into two phases:
///   Phase 1: register*Name() - Register names in symbol table (no type resolution)
///   Phase 2: resolve*Decl() - Resolve types, check bodies, AND evaluate consts
/// 
/// @architectural_note Two-Pass Approach
///   All names are registered first, then types are resolved. This enables
///   forward references (names can be used before they're defined).
/// 
/// @architectural_note Registration Rules
///   - Top-level declarations: Registered in Phase 1 (register*Name)
///   - Nested declarations: Registered in Phase 2 (resolveDecl)
///   - Parameters: Registered ONLY in Phase 2 (resolveParam) 
///     (Parameters are not needed for Phase 1 name resolution)
///   - Generic params: Registered in Phase 1 (registerFuncName) and Phase 2 (resolveGenericParam)

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ASTStrings.hpp"
#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../const_eval/ConstEvaluator.hpp"
#include "../support/CaptureAnalysis.hpp"
#include "../support/CaptureAnalysis.hpp"
#include "../support/MangledName.hpp"
#include "../registry/AttributeValidator.hpp"
#include "sema/types/SemaType.hpp"


namespace sema {

// =============================================================================
// PHASE 1: Name Registration
// =============================================================================
//
// Phase 1 registers all names that need to be visible for forward references.
// Only top-level declarations and their generic parameters need to be registered
// here. Parameters and local variables are registered in Phase 2.
//
// IMPORTANT: Parameters are NOT registered in Phase 1 because:
//   1. They are only used inside the function body (resolved in Phase 2)
//   2. They don't need to be visible for forward references
//   3. They are scoped to the function and resolved when the body is processed

void registerImportName(ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) return;  // Error will be reported in Phase 2
    ctx.addImportAlias(decl->alias, target, decl);
}

void registerVarName(VarDeclAST* decl, SemaContext& ctx) {
    ctx.insertValue(decl);
}

void registerFuncName(FuncDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Register the function itself ──────────────────────────────────────
    ctx.insertValue(decl);

    // ─── 2. Generic parameters are NOT registered in Phase 1 ──────────────────
    // Generic parameters are only valid inside the function body, which is
    // resolved in Phase 2. They are registered when resolveFuncDecl is called.
    // 
    // If we register them at module level here, they'll leak into other
    // functions and cause name conflicts in nested functions.

    // ─── 3. Parameters are NOT registered in Phase 1 ─────────────────────────
    // Parameters are only needed inside the function body, which is resolved
    // in Phase 2. They are registered when resolveFuncDecl calls resolveParam.
}

void registerEnumName(EnumDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (EnumVariantAST* variant : decl->variants) {
        if (!variant->name.isEmpty()) {
            ctx.insertValue(variant);
        }
    }
}

void registerTraitName(TraitDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
}

void registerStructName(StructDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    /// NOTE: Do not ignore this comment
    /// ─── REMOVED: Field registration at module level ──────────────────────
    // Fields are NOT registered in the global symbol table.
    // They are accessed through self.field or instance.field.
    // Field lookup is done via the struct's field list, not the symbol table.
}

// =============================================================================
// PHASE 2: Declaration Resolution
// =============================================================================

void resolveImportDecl(ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedModule, decl,
                              "undefined module '", ctx.pool.lookup(decl->path), "'");
    }
}

// ─── resolveVarDecl ──────────────────────────────────────────────────────────

void resolveVarDecl(VarDeclAST* decl, SemaContext& ctx) {
    validateAllAttributes(decl, ctx);

    // ─── 1. Resolve the declared type ──────────────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType) {
        return;
    }

    // ─── 2. Validate const type ────────────────────────────────────────────
    if (decl->keyword == DeclKeyword::Const) {
        if (!validateConstType(declaredType, decl->name, "variable", ctx)) {
            return;
        }
    }

    // ─── 3. Arena type special validation ──────────────────────────────────
    if (isArenaType(declaredType)) {
        // ─── 3a. Arena bindings must be declared with `const` ──────────────
        if (decl->keyword == DeclKeyword::Let) {
            ctx.diagnostics.error(DiagCode::Sem_ConstRequired, decl,
                                "Arena bindings must be declared with `const`");
            ctx.diagnostics.note(decl,
                                "Reassigning an Arena binding would orphan slices "
                                "into its backing region");
            return;
        }
        
        // ─── 3b. Arena cannot be declared at module level ───────────────────
        if (ctx.isAtModuleLevel()) {
            ctx.diagnostics.error(DiagCode::Sem_BuiltinTypeMisuse, decl,
                                "Arena cannot be declared at top level");
            ctx.diagnostics.note(decl,
                                "Arena is scope-confined and should be declared inside "
                                "a function or block where it will be properly scoped");
            return;
        }
        
        // ─── 3c. Initializer must exist ─────────────────────────────────────
        if (!decl->init) {
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                                "Arena binding must be initialized with "
                                "Arena::create(size) or Arena::empty()");
            return;
        }
        
        // ─── 3d. Validate the initializer ───────────────────────────────────
        // The only valid initializers for an Arena binding are:
        //   - Arena::create(size)  (fallible)
        //   - Arena::empty()       (non-fallible)
        // 
        // This rejects:
        //   - const b Arena = a;               (copy of existing arena)
        //   - const b Arena = someFunction();  (any other expression)
        //   - const b Arena = Arena{};         already rejected by type system
        if (!validateArenaInitializer(decl->init, ctx)) {
            return;
        }
    }

    // ─── 4. Check initializer ──────────────────────────────────────────────
    if (decl->init) {
        TypeAST* initType = resolveExprWithTarget(decl->init, declaredType, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            return;
        }

        if (decl->keyword == DeclKeyword::Let) {
            checkLetSelfReference(decl->init, decl->name, ctx);
        }

        // ─── 5. CONST EVALUATION ──────────────────────────────────────────
        if (decl->keyword == DeclKeyword::Const) {
            ConstantValue val = ConstEvaluator::evaluateDecl(ctx, decl);
            if (!val.isError()) {
                decl->init->isConst = true;
            }
        }
    } else {
        // ─── NO INITIALIZER: Set default value state ──────────────────────
        // (Only for nullable/fallible/combined types - Arena is handled above)
        UnknownExprAST* defaultExpr = ctx.arena.make<UnknownExprAST>();
        defaultExpr->resolvedType = declaredType;
        defaultExpr->isLValue = false;
        defaultExpr->isConst = true;
        
        if (isNullableType(declaredType)) {
            defaultExpr->valueState = ValueState::Nil;
            decl->init = defaultExpr;
            Trace::info("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to nil (nullable type)");
            return;
        }
        
        if (isFallibleType(declaredType)) {
            defaultExpr->valueState = ValueState::Err;
            decl->init = defaultExpr;
            Trace::info("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to err (fallible type)");
            return;
        }
        
        if (declaredType->isa<CombinedTypeAST>()) {
            defaultExpr->valueState = ValueState::Nil;
            decl->init = defaultExpr;
            Trace::detail("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to nil (combined type)");
            return;
        }
        
        if (declaredType->isa<FuncTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                                  "function type variable '", ctx.pool.lookup(decl->name),
                                  "' must be initialized with a function value");
            return;
        }
        
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                              "variable '", ctx.pool.lookup(decl->name),
                              "' of type '", typeToString(declaredType, ctx.pool),
                              "' must be initialized (type is not nullable or fallible)");
        ctx.diagnostics.note(decl, 
                             "Consider using a nullable type (T?) or fallible type (T!) ",
                             "if you want default initialization to nil/err, or provide an initializer");
        return;
    }

    // ─── 6. Generate mangled name for exported globals ───────────────────
    bool isModuleLevel = isModuleLevelDeclaration(decl, ctx);
    bool isExported = false;
    InternedString exportName = ctx.pool.intern("export");
    for (const AttributeAST* attr : decl->attributes) {
        if (attr->name == exportName) {
            isExported = true;
            break;
        }
    }
    
    if (isModuleLevel && isExported) {
        InternedString mangled = generateMangledName(decl, ctx);
        if (mangled.isValid()) {
            decl->mangledName = mangled;
        }
    }
}

// ─── resolveFuncDecl ──────────────────────────────────────────────────────────
//
// ─── Two Scopes, Two Purposes ─────────────────────────────────────────────────
//
// Resolving a function declaration requires *two* distinct scopes, pushed
// at two different moments, and the split is not obvious from the code
// unless you know which names live where. This comment records the split
// so it isn't "simplified" away by a future reader who sees two scope
// pushes and assumes one of them is redundant.
//
//   Scope A — pushed here, in resolveFuncDecl, before anything else.
//             Holds the function's *generic parameters* (`<T>`, `<K, V>`).
//             Inserted by resolveGenericParam, one per parameter.
//
//             Exists so that resolving the declared function type can
//             resolve references to `T`:
//
//                 const factorial<T> (base T, p T) -> int = { ... }
//                                     ↑
//                          resolving this type needs `T` in scope,
//                          or lookupTypeDecl("T") returns nullptr and
//                          the type resolver emits Sem_UndefinedType.
//
//             Also visible inside the body, so `let x T = ...` works.
//
//   Scope B — pushed by ScopedFunction inside resolveAnonFuncExpr, later,
//             when resolveExprWithTarget dispatches into the AnonFuncExprAST
//             at `init`. Holds the function's *runtime parameters* (`base`,
//             `p`).
//
//             Exists so that resolving the body can resolve references to
//             parameter *names*:
//
//                 const factorial<T> (base T, p T) -> int = {
//                     return factorial(base, p - 1);
//                                     ↑     ↑
//                          resolving these names needs the runtime
//                          ParamAST nodes in scope. The type-only
//                          ParamAST nodes on decl->funcType are a
//                          different set of nodes and would not be
//                          found by the same lookup — and even if
//                          they were, CodeGen would later fail to
//                          connect them to the argument allocas it
//                          actually stores. See FuncDeclAST's doc-
//                          comment for the full reasoning.
//
// ─── Why Not One Scope? ───────────────────────────────────────────────────────
//
// The two scopes hold disjoint name sets — type parameters vs. value
// parameters — and they answer different lookup questions
// (lookupTypeDecl vs. lookupValue). Collapsing them into one would
// require pushing the generic parameters into the same map that
// resolveParam writes to, which would make `T` (a type) and `base` (a
// value) siblings in one namespace. That conflicts with the language's
// two-namespace model (see BaseAST.hpp's NAMESPACE SEPARATION note) and
// would make `lookupTypeDecl` and `lookupValue` return each other's
// entries.
//
// ─── Nesting Order ────────────────────────────────────────────────────────────
//
// At body-resolution time the scope stack is:
//
//     [module scope]        ← module-level declarations
//       [Scope A]           ← generic parameters (T)
//         [Scope B]         ← runtime parameters (base, p)
//           [block scope]   ← the body's own `{ ... }`
//
// Name lookup walks outward, so a body reference to `base` finds Scope B,
// a body reference to `T` finds Scope A, and a recursive reference to
// `factorial` finds the module scope (registered in Phase 1). All three
// resolve, and none of them leak: Scope A pops when resolveFuncDecl
// returns, Scope B pops when the anon's ScopedFunction destructor fires.
//
// ─── Why Phase 1 Does Not Register Generic Parameters ─────────────────────────
//
// registerFuncName does not call ctx.insertGenericParam. Registering `T`
// at module scope would:
//   1. Collide when two functions both declare `<T>` in the same module.
//   2. Shadow any module-level type named `T` for every other declaration.
//   3. Make `T` visible to declarations that have nothing to do with this
//      function.
//
// Generic parameters are lexical to the function that declares them, so
// they are registered in a scope that exists only for that function's
// resolution. That scope is Scope A.
//
// ─── Why the Push Is Unconditional ────────────────────────────────────────────
//
// SymbolScope is pushed whether or not decl->genericParams is empty.
// For a non-generic function Scope A is empty and does nothing useful,
// but the uniformity is deliberate: a conditional push would need to be
// kept in sync with every other place a FuncDeclAST is resolved, and
// the cost of an empty std::vector entry is negligible compared to the
// maintenance hazard of a conditional that can drift out of agreement
// with reality.
//
// ─── Standalone func_literals Do Not Get Scope A ──────────────────────────────
//
// A `func_literal` used as a value (passed as an argument, stored in a
// struct field, returned from another function) never goes through
// resolveFuncDecl. It's resolved directly by resolveExprWithTarget
// dispatching to resolveAnonFuncExpr, which pushes only Scope B. That's
// correct: AnonFuncExprAST has no genericParams, so there is nothing for
// Scope A to hold. Scope B alone is what the body needs.
//
// ─── Curried Functions and Nested Scopes ──────────────────────────────────────
//
// For a curried declaration like
//
//     const add<T> (a T) -> (b T) -> T = {
//         return (b T) -> T { return a + b; };
//     };
//
// the scope stack while resolving the inner anon is:
//
//     [module scope]
//       [Scope A]              ← T (generic parameter)
//         [Scope B outer]      ← a (outer runtime parameter)
//           [block outer]
//             [Scope B inner]  ← b (inner runtime parameter)
//               [block inner]
//
// The inner body sees both `a` (from Scope B outer, still on the stack
// because the outer anon hasn't finished resolving) and `b` (from Scope
// B inner). T is visible to both from Scope A. This is what makes
// currying work: each curry stage gets its own parameter scope, and
// outer stages remain visible to inner stages by virtue of the stack
// discipline.
//
// ─── Name Collisions Between Scopes ───────────────────────────────────────────
//
// A parameter and a generic parameter can share a name:
//
//     const f<T> (T T) -> T = { ... }   // legal, if confusing
//
// Scope B's `T` (the value parameter) shadows Scope A's `T` (the type
// parameter) for lookupValue, while lookupTypeDecl still finds Scope A's
// `T` because ParamAST is not consulted by lookupTypeDecl. The result is
// that the value `T` is the parameter and the type `T` is the generic —
// which is the coherent reading, if a confusing one. Lucid does not
// forbid this today. If it starts appearing in practice, a warning
// would be the right response, not an error: the semantics are
// well-defined, the surprise is just the naming.
//
// ─── Summary ──────────────────────────────────────────────────────────────────
//
// | Scope | Pushed by                    | Holds                | Popped by                     |
// | ----- | ---------------------------- | -------------------- | ----------------------------- |
// | A     | SymbolScope in this function | generic parameters   | SymbolScope's destructor      |
// | B     | ScopedFunction in the anon   | runtime parameters   | ScopedFunction's destructor   |
//
// Neither is redundant. Removing either breaks a distinct class of name
// resolution, and the failure modes are different (A fails to resolve
// parameter *types*; B fails to resolve parameter *values* and, later,
// CodeGen's binding lookup).
//
// ──────────────────────────────────────────────────────────────────────────────
void resolveFuncDecl(FuncDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        if (decl->type) {
            decl->type = ctx.getUnknownType();
        }
        return;
    }

    // ─── Defensive: generic ⇒ const ───────────────────────────────────────
    AST_ASSERT_MSG(!decl->isGeneric() || decl->keyword == DeclKeyword::Const,
                   "FuncDeclAST invariant violated: generic function with let keyword");

    validateAllAttributes(decl, ctx);

    // ─── 1. Check @[foreign] ──────────────────────────────────────────────
    InternedString foreignName = ctx.pool.intern("foreign");
    for (AttributeAST* attr : decl->attributes) {
        if (attr->name == foreignName) {
            decl->isForeignFunction = true;
        }
    }

    // ─── 2. Push the function's own scope ─────────────────────────────────
    //
    // Holds the function's generic parameters (`<T>`) for the duration of
    // this declaration's resolution. The runtime parameters belong to the
    // AnonFuncExprAST at `init` and are handled when the init is resolved.
    //
    // For a generic function, we will NOT resolve the init here, so this
    // scope only needs to stay alive long enough to resolve the signature
    // and the generic parameter constraints. It is popped at the end of
    // this function via SymbolScope's destructor.
    SymbolScope funcScope(ctx);

    // ─── 3. Resolve generic parameters ────────────────────────────────────
    //
    // Must run before resolveFuncType: the signature may reference `T`,
    // and `T` must be in scope for that resolution to succeed.
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 4. Resolve the declared function type ────────────────────────────
    //
    // Both kinds of function (generic and non-generic) resolve their
    // signature at declaration time. For a generic function, the signature
    // is a *template*: parameter types may reference the function's own
    // generic parameters, and those references are resolved against the
    // GenericParamDeclAST nodes registered in step 3.
    //
    // This is the only place a generic function's signature is validated
    // as a template. Each specialization's signature is derived from this
    // one by substitution during instantiation.
    FuncTypeAST* funcType = decl->funcType;
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }

    // ─── 5. Foreign functions: no body, no init ───────────────────────────
    if (decl->isForeignFunction) {
        decl->mangledName = decl->name;
        Trace::info("Foreign function '", ctx.pool.lookup(decl->name),
                 "' uses symbol name: ", ctx.pool.lookup(decl->mangledName));
        return;
    }

    // ─── 6. Generate mangled name ─────────────────────────────────────────
    //
    // For a generic function, this mangles the *template's* name. Each
    // specialization gets its own mangled name in
    // createInstantiatedFunction, derived from the template's name plus
    // the concrete type arguments. The template's mangled name is only
    // used in diagnostics that reference the template itself.
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }

    // ─── 7. Non-foreign functions must have an init ───────────────────────
    if (!decl->init) {
        ctx.diagnostics.error(DiagCode::Sem_MissingFuncBody, decl,
                              "function '", ctx.pool.lookup(decl->name), "' has no body");
        return;
    }

    // ─── 8a. Generic functions: do NOT resolve the body here ──────────────
    //
    // A generic function declaration is not a function — it is a *family*
    // of functions, one per distinct concrete set of type arguments.
    //
    // The template's body is a recipe: a parameterized program, not a
    // program. It cannot be resolved while `T` is abstract, because the
    // body may contain operations that require `T` to be concrete:
    //
    //   - a recursive call to the function itself: `factorial<T>(...)`.
    //     `factorial<T>` is not an instantiation — it's a "self-reference
    //     to the enclosing family", and there is no concrete type to
    //     instantiate with at template-resolution time.
    //   - a call to another generic function with `T` as an argument:
    //     `other<T>(...)`. Same problem.
    //   - arithmetic or comparison on `T`-typed values: `p <= 1` where `p`
    //     has type `T`. This only type-checks if `T` is known to be numeric.
    //   - field access on a `T`-typed value: `x.field` where `x: T`. This
    //     only type-checks if `T` is constrained to a trait providing that
    //     field.
    //
    // None of these can be resolved at declaration time. They all resolve
    // naturally during instantiation, when `T` has been substituted by the
    // concrete type argument and the body reads `factorial<int>(...)`,
    // `other<int>(...)`, `p <= 1` where `p: int`, and so on.
    //
    // So the template body is left unresolved here. Its resolution happens
    // once per specialization, in finalizeInstantiatedFunction, after
    // substitution has replaced every occurrence of `T` with its concrete
    // argument.
    //
    // Consequence: errors in a generic function's body are reported at
    // first instantiation, not at declaration. This matches the behavior
    // of every specialization-only generic system (C++ templates, Rust
    // monomorphization, Zig comptime): the body is not checked until the
    // compiler actually has a concrete type to check it against.
    if (decl->isGeneric()) {
        return;
    }

    // ─── 8b. Non-generic functions: resolve the body ──────────────────────
    //
    // A non-generic function is a function. Its body resolves normally:
    // push a ScopedFunction (done inside resolveAnonFuncExpr when the init
    // is an AnonFuncExprAST), register the runtime parameters, resolve the
    // body, run capture analysis.
    //
    // Any call in the body that instantiates a generic function —
    // `factorial<int>(5)` — will trigger instantiation via
    // resolveIdentifierExpr / resolveCallExpr, which call into
    // finalizeInstantiatedFunction. That's the same path as before.
    TypeAST* initType = resolveExprWithTarget(decl->init, funcType, ctx);
    if (!initType || initType->isa<UnknownTypeAST>()) {
        // resolveExprWithTarget already emitted a diagnostic
        return;
    }
}

// ─── resolveParam ─────────────────────────────────────────────────────────────

/// @brief Resolve a parameter type and register it in the current scope.
///
/// Parameters are registered in Phase 2 (resolveFuncDecl) because they are
/// only needed when resolving the function body. Unlike top-level declarations,
/// parameters don't need to be visible for forward references.
///
/// @note This is called from resolveFuncDecl, NOT from registerFuncName.
void resolveParam(ParamAST* param, SemaContext& ctx) {
    if (!param) return;

    if (!param->name.isEmpty()) {
        ctx.insertValue(param);
    }
    if (param->hasSyntaxError) {
        param->type = ctx.getUnknownType();
        return;
    }

    // ─── 1. Resolve the parameter type ──────────────────────────────────────
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        param->type = ctx.getUnknownType();
        return;
    }
    
    // ─── 2. Validate const parameter ────────────────────────────────────────
    if (param->isConstParam) {
        if (!validateConstType(paramType, param->name, "parameter", ctx)) {
            return;
        }
    }
    
}

// ─── resolveGenericParam ──────────────────────────────────────────────────────

void resolveGenericParam(GenericParamDeclAST* param, SemaContext& ctx) {
    // ─── Push generic constraint context using RAII guard ──────────────────
    ScopedSemanticContext constraintCtx(ctx, ContextKind::GenericConstraint, param);
    
    for (NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
    
    // ─── Register this generic parameter in the current scope ──────────────
    ctx.insertGenericParam(param);
    
    // ─── ScopedSemanticContext destructor automatically pops the context ───
}

// ─── resolveEnumDecl ──────────────────────────────────────────────────────────

void resolveEnumDecl(EnumDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        return;
    }

    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerEnumName() ──────────────
    // Do NOT call ctx.insertType() here.

    // ─── 1. Resolve backing type ────────────────────────────────────────────
    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, decl,
                                  "invalid backing type for enum '",
                                  ctx.pool.lookup(decl->name), "'");
        }
    }

    // ─── 2. Validate enum variants ──────────────────────────────────────────
    // Note: EnumVariantAST doesn't have a resolvedType field because variants
    // are values of the enum type. The enum type itself is the type.
    for (EnumVariantAST* variant : decl->variants) {
        validateAllAttributes(variant, ctx);

        // Check duplicate variant values
        for (EnumVariantAST* existing : decl->variants) {
            if (existing == variant) {
                ctx.diagnostics.error(DiagCode::Sem_DuplicateValue, variant,
                                      "duplicate enum variant ", ctx.pool.lookup(variant->name));
                break;
            }
            if (existing->value == variant->value) {
                ctx.diagnostics.error(DiagCode::Sem_DuplicateValue, variant,
                                      "duplicate enum value ", std::to_string(variant->value),
                                      " (also used by '", ctx.pool.lookup(existing->name), "')");
                break;
            }
        }
    }

    // ─── 3. Generate mangled name ────────────────────────────────────────────
    // This is CRITICAL for CodeGen to create unique enum types.
    // Without mangling, two enums with the same name in different modules
    // would collide in LLVM.
    //
    // Example:
    //   module1: enum Status { Ok = 0, Err = 1 }
    //   module2: enum Status { Active = 0, Inactive = 1 }
    //   Both would be named "Status" without mangling → conflict!
    //
    // With mangling:
    //   module1 → _Lmodule1_Status_Bi_V2
    //   module2 → _Lmodule2_Status_Bi_V2
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }
}

// ─── resolveTraitDecl ─────────────────────────────────────────────────────────

void resolveTraitDecl(TraitDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        return;
    }

    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerTraitName() ─────────────
    // Do NOT call ctx.insertType() here.

    // ─── Push a scope for the trait's own contents ─────────────────────
    SymbolScope traitScope(ctx);

    // ─── 1. Resolve generic parameters ──────────────────────────────────────
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 2. Resolve trait fields ────────────────────────────────────────────
    for (TraitFieldDeclAST* field : decl->fields) {
        validateAllAttributes(field, ctx);

        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── Arena validation: Cannot store Arena in struct fields ──────
        if (isArenaType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name), "' cannot be of type Arena");
            ctx.diagnostics.note(field,
                                 "Arena is scope-confined and cannot be stored in traits");
            continue;
        }

        // ─── Validate const trait field ──────────────────────────────────
        // const trait fields must have a definite type (not nullable or fallible)
        if (field->isConst()) {
            if (!validateConstType(fieldType, field->name, "trait field", ctx)) {
                continue;
            }
        }
    }

    // ─── 3. Validate generic parameter usage ───────────────────────────────
    std::vector<TypeAST*> types;
    for (TraitFieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

// ─── resolveStructDecl ────────────────────────────────────────────────────────

void resolveStructDecl(StructDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        return;
    }

    validateAllAttributes(decl, ctx);

    ScopedTypeDefinition defining(ctx, decl);

    // ─── Push a scope for the struct's own contents ────────────────────
    // This scope holds:
    //   - the struct's generic parameters (registered below)
    //   - the struct's fields (registered after the generics)
    // It's a single scope for both, because a field's type may reference
    // the struct's generic parameters (`value T`, `next Node<T>?`), and
    // both are local to the struct's own declaration.
    SymbolScope structScope(ctx);

    // ─── 1. Resolve generic parameters FIRST ──────────────────────────────
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 2. Register all fields in the struct scope ──────────────────────────
    for (FieldDeclAST* field : decl->fields) {
        if (!field->name.isEmpty()) {
            ctx.insertValue(field);
        }
    }

    // ─── 3. Resolve fields and compute logical layout ──────────────────────
    resolveStructFields(decl, ctx);

    // ─── 4. Validate trait implementations ──────────────────────────────────
    if (!validateAllTraitImplementations(decl, ctx)) {
        // Error already reported
    }

    // ─── 5. Validate generic parameter usage ───────────────────────────────
    std::vector<TypeAST*> types;
    for (FieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);

    // ─── 6. Generate mangled name ───────────────────────────────────────────
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }
}

// ─── resolveStructFields ──────────────────────────────────────────────────────

void resolveStructFields(StructDeclAST* decl, SemaContext& ctx) {
    // ─── Phase 1: Resolve field types and validate ──────────────────────────
    for (FieldDeclAST* field : decl->fields) {
        if (field->hasSyntaxError) continue;

        validateAllAttributes(field, ctx);

        // ─── 1. Resolve the field's type ──────────────────────────────────
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) continue;

        // ─── Arena validation: Cannot store Arena in struct fields ──────
        if (isArenaType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name), "' cannot be of type Arena");
            ctx.diagnostics.note(field,
                                 "Arena is scope-confined and cannot be stored in structs");
            continue;
        }

        // ─── 2. Downward Flow Rule: Check borrowed types ──────────────────
        if (isBorrowedType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name),
                                  "' has borrowed type (",
                                  typeToString(fieldType, ctx.pool),
                                  ") — struct fields cannot contain &T or [_]T");
            continue;
        }

        // ─── 3. Validate self-reference ──────────────────────────────────────
        isValidStructSelfReference(fieldType, decl, ctx);

        // ─── 4. Validate const field type ──────────────────────────────────
        if (field->isConst()) {
            if (!validateConstType(fieldType, field->name, "struct field", ctx)) {
                continue;
            }
        }

        // ─── 5. Handle default value ────────────────────────────────────────
        //
        // Under the new AST design, `defaultVal` is the single field that
        // holds a default, regardless of whether the user wrote it as a
        // value or as a block. A block default was wrapped by the parser
        // into an AnonFuncExprAST whose funcType is `fieldType` (with
        // `self: &StructName` prepended, if the field is function-typed).
        //
        // So there is exactly one case here: resolve `defaultVal` against
        // `fieldType`. The block-vs-expression distinction the old code
        // made has been eliminated — it's all just expressions now.
        if (field->defaultVal) {
            TypeAST* initType = resolveExprWithTarget(field->defaultVal, fieldType, ctx);
            if (!initType || initType->isa<UnknownTypeAST>()) {
                // resolveExprWithTarget already emitted a diagnostic
                continue;
            }

            // ─── Semantic check: function-typed fields need function values ──
            // Even though the type system should have caught this (a
            // non-function value isn't assignable to a function type),
            // we check explicitly because the error message is clearer.
            // This mirrors the old code's expression-default branch.
            bool isFunctionType = fieldType->isa<FuncTypeAST>();
            if (isFunctionType && !isFunctionValue(field->defaultVal, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, field,
                                      "field '", ctx.pool.lookup(field->name),
                                      "' default value must be a function value");
                continue;
            }
        }
        // ─── No default value ─────────────────────────────────────────────
        // The struct literal must supply a value for this field.
    }

    // ─── Phase 2: Compute logical layout ────────────────────────────────────
    for (size_t i = 0; i < decl->fields.size(); ++i) {
        FieldDeclAST* field = decl->fields[i];
        field->fieldIndex = i;
    }
}

} // namespace sema
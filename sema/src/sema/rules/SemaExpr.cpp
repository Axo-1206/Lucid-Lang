/// @file SemaExpr.cpp
/// @brief Expression resolution.
///
/// ─── Design: resolveExprWithTarget is the entry point ─────────────────────
/// Every expression in the compiler is resolved by `resolveExprWithTarget`,
/// which takes the expression and the type the surrounding context
/// expects. A `nullptr` target means "no expected type"; an
/// `UnknownTypeAST` target is treated the same way. The two-argument
/// form is what the resolver needs to disambiguate forms whose type is
/// not inferable from the expression alone:
///
///   - an empty array literal `[]` takes its element type from `target`;
///   - an integer or float literal adopts the concrete numeric type the
///     target requires (§5.8);
///   - `nil` adopts the target when it is a nullable type;
///   - a lambda adopts the parameter and return types of the target when
///     the target is a function type.
///
/// `resolveExpr` is a thin wrapper that passes `nullptr`.
///
/// ─── Design: the target is a hint, not a demand ───────────────────────────
/// `resolveExprWithTarget` calls the per-form resolver for the
/// expression's kind, passing the target along. The per-form resolver
/// uses the target *if it helps*; if the target does not bear on the
/// expression's type (a binary expression, a call), the resolver ignores
/// it and returns the expression's natural type. After the per-form
/// resolver returns, `resolveExprWithTarget` checks whether the natural
/// type is assignable to the target; if not, it emits a mismatch and
/// returns `UnknownTypeAST`.
///
/// This is why a per-form resolver never has to know "am I being called
/// in a target position?" — it always answers with its own type, and the
/// wrapper decides whether that is acceptable.
///
/// ─── Design: expression resolution is idempotent-ish ──────────────────────
/// A resolver that has already resolved an expression writes
/// `expr->resolvedType` and does not re-resolve. `resolveExprWithTarget`
/// checks for a pre-existing `resolvedType` and returns it, subject to
/// the assignability check against the target. This makes the resolution
/// robust to being called twice on the same expression — which happens
/// when a caller resolves an expression for its type and then a
/// different caller wants the same expression's type in a different
/// context. The re-check against the target is what makes the second
/// call a genuine check, not a silent return.
///
/// ─── Design: the field-access classification ──────────────────────────────
/// `a.b` in the new grammar is one of several things depending on what
/// `a` is. `resolveFieldAccessExpr` is the largest function in this file
/// because it has to classify the access and produce the right result
/// type for each case. The cases are enumerated in the function's own
/// comment.

#include "sema/Sema.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/support/TypeNarrowHelpers.hpp"
#include "sema/types/SemaType.hpp"

#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace lucid::diag;

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// Public entry points
// ═════════════════════════════════════════════════════════════════════════════

TypeAST* resolveExpr(ExprAST* expr, SemaContext& ctx) {
    return resolveExprWithTarget(expr, nullptr, ctx);
}

TypeAST* resolveExprWithTarget(ExprAST* expr, TypeAST* targetType,
                               SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // A parser error-recovery node has no meaningful type. Short-circuit
    // and let the caller's error-suppression logic skip the construct.
    if (expr->hasSyntaxError) {
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // A target that is the unknown singleton is treated as no target.
    // The unknown singleton is what a caller passes when it does not
    // know the expected type; treating it as a real target would
    // produce spurious mismatches.
    if (targetType && targetType->isa<UnknownTypeAST>()) {
        targetType = nullptr;
    }

    // ─── If the expression is already resolved, apply the target check ──
    //
    // A resolver that ran on this node previously wrote its resolved
    // type. Rather than re-resolving (which would be wasteful, and
    // could produce a different answer if the scope has changed), the
    // cached type is checked against the target directly.
    if (expr->resolvedType) {
        TypeAST* cached = expr->resolvedType;
        if (targetType && !isAssignable(targetType, cached, ctx)) {
            ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                                  "type mismatch: expected ",
                                  typeToString(targetType, ctx.pool),
                                  ", got ",
                                  typeToString(cached, ctx.pool));
            return ctx.getUnknownType();
        }
        return cached;
    }

    // ─── Dispatch on the expression's kind ──────────────────────────────
    TypeAST* result = nullptr;
    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = resolveLiteralExpr(expr->as<LiteralExprAST>(), targetType, ctx);
            break;
        case ASTKind::IdentifierExpr:
            result = resolveIdentifierExpr(expr->as<IdentifierExprAST>(), targetType, ctx);
            break;
        case ASTKind::ArrayLiteralExpr:
            result = resolveArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), targetType, ctx);
            break;
        case ASTKind::FieldAccessExpr:
            result = resolveFieldAccessExpr(expr->as<FieldAccessExprAST>(), targetType, ctx);
            break;
        case ASTKind::IndexExpr:
            result = resolveIndexExpr(expr->as<IndexExprAST>(), targetType, ctx);
            break;
        case ASTKind::CallExpr:
            result = resolveCallExpr(expr->as<CallExprAST>(), targetType, ctx);
            break;
        case ASTKind::LambdaExpr:
            result = resolveLambdaExpr(expr->as<LambdaExprAST>(), targetType, ctx);
            break;
        case ASTKind::StartExpr:
            result = resolveStartExpr(expr->as<StartExprAST>(), targetType, ctx);
            break;
        case ASTKind::UnaryExpr:
            result = resolveUnaryExpr(expr->as<UnaryExprAST>(), targetType, ctx);
            break;
        case ASTKind::BinaryExpr:
            result = resolveBinaryExpr(expr->as<BinaryExprAST>(), targetType, ctx);
            break;
        case ASTKind::ParenExpr:
            result = resolveParenExpr(expr->as<ParenExprAST>(), targetType, ctx);
            break;
        case ASTKind::RangeExpr:
            result = resolveRangeExpr(expr->as<RangeExprAST>(), targetType, ctx);
            break;

        default:
            AST_ASSERT_MSG(false,
                "resolveExprWithTarget: unrecognized ExprAST kind");
            expr->resolvedType = ctx.getUnknownType();
            return ctx.getUnknownType();
    }

    if (!result || result->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Validate against the target, if the per-form resolver did not ──
    //
    // Some per-form resolvers already check the target (array literals,
    // lambdas — those that adopt the target's shape). Others ignore the
    // target entirely (binary expressions, calls). The check here is
    // what makes `resolveExprWithTarget` uniform: whatever the
    // per-form resolver returns, it must be assignable to the target.
    //
    // A per-form resolver that already validated its result against the
    // target has set `expr->resolvedType`, and the check here returns
    // early because the target check has already been done. To avoid a
    // double diagnostic, per-form resolvers that validate the target
    // themselves set `expr->resolvedType` on failure, so this branch
    // sees an already-set field.
    if (targetType) {
        if (!isAssignable(targetType, result, ctx)) {
            ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                                  "type mismatch: expected ",
                                  typeToString(targetType, ctx.pool),
                                  ", got ",
                                  typeToString(result, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            return ctx.getUnknownType();
        }
    }

    expr->resolvedType = result;
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveLiteralExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// Literals are the one place the target type is consulted for
// disambiguation rather than just checked. An integer literal in a
// `uint` context is a `uint`; the same literal in a `long` context is a
// `long`; with no context, it is the default `int` (Int32). The same
// holds for float literals and the default `float` (Float32).
//
// `nil` is special: it has no type of its own, and its resolved type is
// whatever nullable type the target names. A `nil` with no target is an
// error (the language has no untyped nil).

TypeAST* resolveLiteralExpr(LiteralExprAST* expr, TypeAST* target,
                                   SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    TypeAST* result = nullptr;

    switch (expr->kind) {
        // ─── Boolean literals ───────────────────────────────────────────
        case LiteralKind::True:
        case LiteralKind::False:
            result = ctx.getPrimitiveType(PrimitiveKind::Bool);
            break;

        // ─── Integer literals (all radices) ─────────────────────────────
        //
        // An integer literal adopts the target's integer kind if the
        // target is an integer primitive, else `int` (Int32).
        //
        // If the target is a nullable integer (`int?`), the literal
        // adapts to the inner type and the result is the nullable type,
        // with a non-nil value. This is §5.8's "A literal adapts to a
        // nilable numeric type as well".
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary:
        case LiteralKind::Octal: {
            if (target) {
                TypeAST* innerTarget = unwrapNullable(target);
                if (innerTarget && isIntegerType(innerTarget)) {
                    result = (innerTarget != target) ? target : innerTarget;
                }
            }
            if (!result) {
                result = ctx.getPrimitiveType(PrimitiveKind::Int32);
            }
            break;
        }

        // ─── Float literal ──────────────────────────────────────────────
        case LiteralKind::Float: {
            if (target) {
                TypeAST* innerTarget = unwrapNullable(target);
                if (innerTarget && isFloatType(innerTarget)) {
                    result = (innerTarget != target) ? target : innerTarget;
                }
            }
            if (!result) {
                result = ctx.getPrimitiveType(PrimitiveKind::Float32);
            }
            break;
        }

        // ─── String literal ─────────────────────────────────────────────
        case LiteralKind::String:
        case LiteralKind::RawString:
            result = ctx.getPrimitiveType(PrimitiveKind::String);
            break;

        // ─── Char literal ───────────────────────────────────────────────
        case LiteralKind::Char:
            result = ctx.getPrimitiveType(PrimitiveKind::Char);
            break;

        // ─── Nil ────────────────────────────────────────────────────────
        //
        // `nil` has no type of its own; it takes the target's type and
        // requires the target to be nullable (a `T?`) or a row
        // reference (`&T`, which is inherently nilable). With no target
        // or a non-nilable target, this is an error.
        //
        // The two acceptable targets:
        //   - a `NullableTypeAST` (`int?`, `[int]?`, `SpriteRef?`);
        //   - a `RowRefTypeAST` (`&Person`, which admits `nil`).
        //
        // A `nil` in a context that wants a bare table, a function
        // value, or a non-nilable primitive is rejected.
        case LiteralKind::Nil:
            if (!target) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                                      "'nil' requires a nullable context");
                return ctx.getUnknownType();
            }
            if (isNullableType(target) || isRowRefType(target)) {
                result = target;
            } else {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                                      "'nil' cannot be used where '",
                                      typeToString(target, ctx.pool),
                                      "' is expected — the target is not "
                                      "nullable");
                return ctx.getUnknownType();
            }
            break;

        // ─── Fallback ───────────────────────────────────────────────────
        default:
            AST_ASSERT_MSG(false,
                "resolveLiteralExpr: unrecognized LiteralKind");
            return ctx.getUnknownType();
    }

    expr->isLValue = false;
    expr->isConst  = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// functionTypeOf
// ─────────────────────────────────────────────────────────────────────────────
//
// Synthesize the function type of a `FnDeclAST` from its parameter list
// and return type. The AST does not store a `TypeAST*` for the function
// itself; the type is derived from the declaration's shape. The
// synthesizer goes through the context's type cache, so two references
// to the same function type share one node.
//
// A `FnDeclAST`'s parameters are `ParamAST*` (with names, `const`
// qualifiers, and optional `isVariadic`), but a function *type*'s
// parameters are `TypeAST*` (unnamed, no qualifiers). The
// synthesizer drops the qualifiers and keeps only the types. A
// variadic parameter's type is already a `[T]` array (set by
// `resolveParam`); the array type is the parameter's type in the
// function type as well.
static TypeAST* functionTypeOf(FnDeclAST* fn, SemaContext& ctx) {
    if (!fn) return ctx.getUnknownType();

    std::vector<TypeAST*> params;
    params.reserve(fn->params.size());
    for (ParamAST* p : fn->params) {
        if (p && p->type) params.push_back(p->type);
    }
    ArenaSpan<TypeAST*> paramSpan = ctx.arena.makeSpan<TypeAST*>(params);

    // A function with no `-> T` returns `unit`. The AST stores a null
    // `returnType` for that case; the synthesized function type uses
    // the `unit` singleton.
    TypeAST* ret = fn->returnType
                 ? fn->returnType
                 : ctx.getUnitType();

    return ctx.getFunctionType(paramSpan, ret);
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveIdentifierExpr
// ═════════════════════════════════════════════════════════════════════════════

TypeAST* resolveIdentifierExpr(IdentifierExprAST* expr, TypeAST* target,
                               SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── `_` is a discard placeholder, not a name ───────────────────────
    if (ctx.pool.lookupView(expr->name) == "_") {
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Look up the name in the value namespace ────────────────────────
    //
    // `lookupValue` walks local scopes outward, then the current module
    // table's variables, functions, and tables. It does not follow
    // imports; a name from another module reaches here through a
    // qualified field access (`math.sqrt`), which is a
    // `FieldAccessExprAST` and goes through `resolveFieldAccessExpr`.
    ValueLookup lv = ctx.lookupValue(expr->name);
    if (!lv.found()) {
        ctx.diagnostics.error(DiagCode::Name_UndefinedValue, expr,
                              "undefined value '",
                              ctx.pool.lookup(expr->name), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Get the declaration's type, per form ───────────────────────────
    //
    // The three value forms each have a different "what is my type?"
    // answer:
    //
    //   - A `VarDeclAST` binding: its declared type, `var->type`.
    //   - A `FnDeclAST` declaration: a function value. Its type is
    //     synthesized from the declaration's parameter and return
    //     types — the AST does not store a `TypeAST*` on the function
    //     itself because the function type is derived from the
    //     declaration's shape.
    //   - A `TableDeclAST` name: the sheet itself. Its type is the
    //     table's `NamedTypeAST`.
    //   - A `ParamAST`: a parameter. Its type is `param->type`.
    //
    // The `ValueLookup` struct carries the discriminator, so this
    // function reads one field per case instead of `isa<>`-checking a
    // `ValueDeclAST*` and casting.
    TypeAST* declType = nullptr;
    DeclAST* resolvedDecl = nullptr;

    switch (lv.kind) {
        case ValueLookup::Kind::Variable: {
            VarDeclAST* var = lv.variable;
            resolvedDecl = var;
            declType = var->type;
            break;
        }
        case ValueLookup::Kind::Function: {
            FnDeclAST* fn = lv.function;
            resolvedDecl = fn;
            // The function's type is its signature. A `FnDeclAST` does
            // not carry a `type` field the way a `VarDeclAST` does; the
            // function type is derived from the parameters and return
            // type. `resolveFnDecl` has already resolved both.
            declType = functionTypeOf(fn, ctx);
            break;
        }
        case ValueLookup::Kind::Table: {
            TableDeclAST* table = lv.table;
            resolvedDecl = table;
            NamedTypeAST* tableType = ctx.getNamedType(table->name);
            tableType->resolvedDecl = table;
            declType = tableType;
            break;
        }
        case ValueLookup::Kind::Param: {
            ParamAST* param = lv.param;
            resolvedDecl = param;
            declType = param->type;
            break;
        }
        case ValueLookup::Kind::None:
            // Handled above by the `found()` check.
            break;
    }

    if (!declType || declType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // Store the resolved declaration. The `IdentifierExprAST` field is
    // typed `DeclAST*`; the four forms above are all `DeclAST`s.
    expr->resolvedDecl = resolvedDecl;

    // ─── Apply the narrowings in scope ──────────────────────────────────
    TypeAST* narrowed = ctx.stack.getNarrowedType(expr->name);
    if (narrowed) {
        declType = narrowed;
    }

    // ─── Mutability ─────────────────────────────────────────────────────
    //
    // An identifier is an lvalue iff its declaration is mutable:
    //   - a `let` variable: lvalue;
    //   - a `const` variable: not an lvalue;
    //   - a non-`const` parameter: lvalue;
    //   - a `const` parameter: not an lvalue;
    //   - a `FN` declaration: not an lvalue;
    //   - a `TABLE` name: not an lvalue.
    switch (lv.kind) {
        case ValueLookup::Kind::Variable:
            expr->isLValue = !lv.variable->isConst;
            break;
        case ValueLookup::Kind::Param:
            expr->isLValue = !lv.param->isConst;
            break;
        case ValueLookup::Kind::Function:
        case ValueLookup::Kind::Table:
        case ValueLookup::Kind::None:
            expr->isLValue = false;
            break;
    }

    return declType;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveArrayLiteralExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// The array literal is where the target type does the most work. An
// empty `[]` has no element type of its own, and a non-empty literal's
// element type comes from the elements' common type — but if the target
// names a fixed-size array, its size disambiguates whether the literal
// is a fixed array of that size, and the target's element type is used
// as the target for each element.
//
// The rule, in order:
//
//   1. If the target is an array type, its element type is the target
//      for each element, and the result is an array type of the same
//      kind (dynamic or fixed) and size as the target.
//   2. If the target is not an array type (or is missing), the first
//      element's type is the element type, and the result is a dynamic
//      array of that type. If the literal is empty and there is no
//      target, this is an error.
//   3. If the target is a nullable array type (`[int]?`), the same
//      logic applies to the inner array type.

TypeAST* resolveArrayLiteralExpr(ArrayLiteralExprAST* expr,
                                        TypeAST* target, SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── Determine the target array (unwrapping nullable) ───────────────
    ArrayTypeAST* targetArray = nullptr;
    bool targetWasNullable = false;
    if (target) {
        TypeAST* inner = unwrapNullable(target);
        if (inner && inner->isa<ArrayTypeAST>()) {
            targetArray = inner->as<ArrayTypeAST>();
            targetWasNullable = (inner != target);
        }
    }

    // ─── Empty literal ──────────────────────────────────────────────────
    if (expr->elements.empty()) {
        if (!targetArray) {
            ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                                  "an empty array literal requires an "
                                  "array type in the surrounding context");
            return ctx.getUnknownType();
        }
        // The literal is an empty array of the target's kind and size.
        // A fixed array with size 0 is valid; a dynamic array of any
        // element type is valid.
        ArrayTypeAST* resultType = ctx.getArrayType(targetArray->arrayKind,
                                                   targetArray->fixedSize,
                                                   targetArray->element);
        // If the target was nullable, the literal adapts to the
        // nullable array type (the literal is not nil, but the target
        // is a `[T]?` and the literal must produce that type).
        TypeAST* finalType = targetWasNullable ? target : resultType;
        expr->isLValue = false;
        expr->isConst  = true;
        return finalType;
    }

    // ─── Resolve the first element to determine the element type ────────
    TypeAST* elementTarget = targetArray ? targetArray->element : nullptr;
    TypeAST* firstType = resolveExprWithTarget(expr->elements[0],
                                               elementTarget, ctx);
    if (!firstType || firstType->isa<UnknownTypeAST>()) {
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Check every other element against the same element type ────────
    bool allConstant = expr->elements[0]->isConst;
    for (size_t i = 1; i < expr->elements.size(); ++i) {
        TypeAST* elemType = resolveExprWithTarget(expr->elements[i],
                                                  firstType, ctx);
        if (!elemType || elemType->isa<UnknownTypeAST>()) {
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
        if (!typesEqual(firstType, elemType)) {
            ctx.diagnostics.error(DiagCode::Type_Mismatch, expr->elements[i],
                                  "array literal element has type ",
                                  typeToString(elemType, ctx.pool),
                                  ", expected ",
                                  typeToString(firstType, ctx.pool));
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
        if (!expr->elements[i]->isConst) {
            allConstant = false;
        }
    }

    // ─── Determine the result type ──────────────────────────────────────
    //
    // If the target names an array, the literal is an array of the
    // target's kind and size, with the resolved element type. The
    // literal's size must match a fixed target's size, or the literal
    // is not assignable to the target.
    ArrayKind resultKind = ArrayKind::Dynamic;
    uint64_t  resultSize = 0;
    if (targetArray) {
        resultKind = targetArray->arrayKind;
        resultSize = targetArray->fixedSize;
        if (resultKind == ArrayKind::Fixed &&
            resultSize != expr->elements.size()) {
            ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                                  "fixed-size array target requires ",
                                  resultSize,
                                  " element(s), got ",
                                  expr->elements.size());
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
    } else {
        // No target: the literal is a dynamic array. Its size is
        // whatever the source wrote.
        resultKind = ArrayKind::Dynamic;
        resultSize = 0;
    }

    TypeAST* arrayType = ctx.getArrayType(resultKind, resultSize, firstType);
    TypeAST* finalType = targetWasNullable ? target : arrayType;

    expr->isLValue = false;
    expr->isConst  = allConstant;
    return finalType;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveFieldAccessExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// The most complex resolver in the file. `a.b` is classified by what
// `a` is:
//
//   ─── a is a module alias ───────────────────────────────────────────────
//   `math.sqrt`, `input.isDown` — the object is a bare identifier whose
//   name resolves to an import alias. The field is looked up in the
//   imported module's value namespace, and `isModuleAccess` is set. The
//   result is the member's type. `@export` is required.
//
//   ─── a is a table name ─────────────────────────────────────────────────
//   `Person.age`, `Person.ADD`, `Direction.North`, `Person.byId`:
//
//     - a column name → a *column view* (`isColumnView`), iterable in a
//       `for` loop and convertible via `.TOARRAY()`. It has no storable
//       type (§5.6), so the resolver returns a special marker type for
//       it — but in practice a column view only appears as the object
//       of `.TOARRAY()` or as a `for` iterable, both of which handle it
//       specially without needing its type.
//
//     - a built-in method name (`ADD`, `FIND`, ...) → a *function value*
//       whose signature is derived from the method and the table's
//       shape (`isTableMethod`). The result is a function type; the
//       ordinary call machinery in `resolveCallExpr` checks the
//       arguments against it.
//
//     - a fixed-table member (`Direction.North`) → a *compile-time row reference*
//       that resolves at compile time to a row reference. The result is
//       `&T`. The flag `isCompileTimeRowRef` is set so later passes (the
//       switch coverage check, the constant evaluator) can recognize it.
//
//     - a `by<Column>` name → a *primary lookup* function value. The
//       result is a function type `(K) -> &T` where `K` is the key
//       column's type. A new flag `isPrimaryLookup` marks it.
//
//     - anything else → a name-resolution error.
//
//   ─── a is a row reference (&T) ─────────────────────────────────────────
//   `row.name`, `slot.item` — cell access. The field is looked up in
//   `T`'s columns. The result is the column's type; `isLValue` follows
//   the row reference's mutability and the column's `@readonly` flag.
//
//   ─── a is an array ─────────────────────────────────────────────────────
//   `arr.ADD`, `arr.SORT`, `arr.CONTAINS` — array methods. Same
//   treatment as table methods: a function value whose signature is
//   derived from the method and the array's element type.
//
//   ─── a is a host-backed table ──────────────────────────────────────────
//   A host-backed table has no columns. Any `.` access is an error.
//
//   ─── anything else ─────────────────────────────────────────────────────
//   A `.` on a primitive, a nullable value, a function value, or an
//   unsupported type is an error.

TypeAST* resolveFieldAccessExpr(FieldAccessExprAST* expr,
                                       TypeAST* target, SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── Classify the object before resolving it ────────────────────────
    //
    // The object may be a bare identifier that names a module alias.
    // That is the only classification that is done on the *syntactic*
    // object, before it is resolved to a type. Everything else is
    // classified after the object's type is known.
    //
    // A module alias is not a value. If `resolveExpr` ran on the object
    // first, it would look up `math` in the value namespace, fail to
    // find it, and emit an "undefined value" diagnostic — the wrong
    // diagnostic, because `math` is a module alias, not a value.
    // Checking the alias case first avoids that.
    if (expr->object->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = expr->object->as<IdentifierExprAST>();
        ModuleAST* module = ctx.lookupImport(id->name);
        if (module) {
            return resolveModuleMemberAccess(expr, id, module, target, ctx);
        }
    }

    // ─── Resolve the object normally ────────────────────────────────────
    TypeAST* objectType = resolveExpr(expr->object, ctx);
    if (!objectType || objectType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Nullable / row-ref-in-nullable rejection ───────────────────────
    //
    // Accessing a field on a nullable value requires narrowing first.
    // `x.name` where `x: Person?` is an error; the user must write
    // `x != nil` first. A row reference is inherently nilable, but
    // accessing a field on a `&T` is legal (the resolver dereferences
    // the reference and produces a cell access); dereferencing a `nil`
    // `&T` panics at runtime, not at compile time.
    if (isNullableType(objectType)) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, expr->object,
                              "cannot access a field on a nullable value ('",
                              typeToString(objectType, ctx.pool),
                              "') — narrow it first using 'if x != nil' "
                              "or 'x ?? default'");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Classify based on the object's type ────────────────────────────
    if (isRowRefType(objectType)) {
        return resolveCellAccess(expr, objectType->as<RowRefTypeAST>(), target, ctx);
    }

    if (isTableType(objectType, ctx)) {
        return resolveTableMemberAccess(expr, objectType, target, ctx);
    }

    if (isArrayType(objectType)) {
        return resolveArrayMethodAccess(expr, objectType->as<ArrayTypeAST>(), target, ctx);
    }

    // A `NamedTypeAST` whose resolvedDecl is a host-backed table is a
    // host type; host types have no fields.
    if (objectType->isa<NamedTypeAST>()) {
        NamedTypeAST* named = objectType->as<NamedTypeAST>();
        if (named->resolvedDecl && named->resolvedDecl->isa<TableDeclAST>()) {
            TableDeclAST* table = named->resolvedDecl->as<TableDeclAST>();
            if (table->isHostBacked) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                                      "cannot access a field on host type '",
                                      ctx.pool.lookup(table->name),
                                      "' — host types are opaque");
                expr->resolvedType = ctx.getUnknownType();
                expr->isLValue = false;
                return ctx.getUnknownType();
            }
        }
    }

    ctx.diagnostics.error(DiagCode::Name_FieldNotFound, expr,
                          "field access on unsupported type ",
                          typeToString(objectType, ctx.pool));
    expr->resolvedType = ctx.getUnknownType();
    expr->isLValue = false;
    return ctx.getUnknownType();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveModuleMemberAccess
// ─────────────────────────────────────────────────────────────────────────────
//
// `math.sqrt` where `math` is an import alias.

TypeAST* resolveModuleMemberAccess(FieldAccessExprAST* expr,
                                   IdentifierExprAST* /*objId*/,
                                   ModuleAST* module,
                                   TypeAST* target,
                                   SemaContext& ctx) {
    expr->isModuleAccess = true;

    // ─── Try a value member first ───────────────────────────────────────
    ValueDeclAST* valueMember = ctx.lookupModuleValueMember(module, expr->fieldName);
    TableDeclAST* tableMember = nullptr;

    if (!valueMember) {
        tableMember = ctx.lookupModuleTableMember(module, expr->fieldName);
    }

    if (!valueMember && !tableMember) {
        ctx.diagnostics.error(DiagCode::Name_UndefinedMember, expr,
                              "module '", ctx.pool.lookup(module->filePath),
                              "' has no member named '",
                              ctx.pool.lookup(expr->fieldName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Export check ───────────────────────────────────────────────────
    DeclAST* member = valueMember
                    ? static_cast<DeclAST*>(valueMember)
                    : static_cast<DeclAST*>(tableMember);

    if (!member->isExported) {
        ctx.diagnostics.error(DiagCode::Name_PrivateMember, expr,
                              "member '", ctx.pool.lookup(expr->fieldName),
                              "' in module '",
                              ctx.pool.lookup(module->filePath),
                              "' is not @export'ed");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    expr->resolvedDecl = member;

    // ─── Determine the member's type ────────────────────────────────────
    TypeAST* memberType = nullptr;
    if (tableMember) {
        NamedTypeAST* t = ctx.getNamedType(tableMember->name);
        t->resolvedDecl = tableMember;
        memberType = t;
    } else if (valueMember->isa<ValueDeclAST>()) {
        memberType = valueMember->as<ValueDeclAST>()->type;
    }

    if (!memberType || memberType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Name_UndefinedMember, expr,
                              "member '", ctx.pool.lookup(expr->fieldName),
                              "' has no resolvable type");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Mutability ─────────────────────────────────────────────────────
    if (valueMember && valueMember->isa<VarDeclAST>()) {
        expr->isLValue = !valueMember->as<VarDeclAST>()->isConst;
    } else {
        expr->isLValue = false;
    }

    (void)target;
    return memberType;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveCellAccess
// ─────────────────────────────────────────────────────────────────────────────
//
// `row.name`, `slot.item` — cell access on a row reference.

TypeAST* resolveCellAccess(FieldAccessExprAST* expr,
                                  RowRefTypeAST* rowRef,
                                  TypeAST* target,
                                  SemaContext& ctx) {
    // ─── Follow the row reference to its table ──────────────────────────
    if (!rowRef->inner || !rowRef->inner->isa<NamedTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Name_FieldNotFound, expr,
                              "row reference has no resolvable table");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    NamedTypeAST* named = rowRef->inner->as<NamedTypeAST>();
    if (!named->resolvedDecl || !named->resolvedDecl->isa<TableDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Name_FieldNotFound, expr,
                              "row reference's table is not resolved");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    TableDeclAST* table = named->resolvedDecl->as<TableDeclAST>();
    if (table->isHostBacked) {
        ctx.diagnostics.error(DiagCode::Name_FieldNotFound, expr,
                              "cannot access a field on host type '",
                              ctx.pool.lookup(table->name),
                              "' — host types are opaque");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Look up the column ─────────────────────────────────────────────
    for (ColumnDeclAST* column : table->columns) {
        if (column && column->name == expr->fieldName) {
            expr->resolvedColumn = column;
            // A cell access's mutability: the field is writable iff
            //   - the object is an lvalue (the reference binding is
            //     mutable), and
            //   - the column is not @readonly, and
            //   - the table is not @readonly.
            const bool objectIsLValue = expr->object->isLValue;
            const bool columnWritable = !column->isReadonly;
            const bool tableWritable  = !table->isReadonly;
            expr->isLValue = objectIsLValue && columnWritable && tableWritable;
            (void)target;
            return column->type;
        }
    }

    ctx.diagnostics.error(DiagCode::Name_ColumnNotFound, expr,
                          "table '", ctx.pool.lookup(table->name),
                          "' has no column named '",
                          ctx.pool.lookup(expr->fieldName), "'");
    expr->resolvedType = ctx.getUnknownType();
    expr->isLValue = false;
    return ctx.getUnknownType();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveTableMemberAccess
// ─────────────────────────────────────────────────────────────────────────────
//
// `Person.age`, `Person.ADD`, `Direction.North`, `Person.byId` — the
// object is a table name (or a value whose type is a table name, which
// in the new grammar is the same thing).

TypeAST* resolveTableMemberAccess(FieldAccessExprAST* expr,
                                         TypeAST* objectType,
                                         TypeAST* target,
                                         SemaContext& ctx) {
    // ─── Get the table declaration ──────────────────────────────────────
    TableDeclAST* table = nullptr;
    if (objectType->isa<NamedTypeAST>()) {
        NamedTypeAST* named = objectType->as<NamedTypeAST>();
        if (named->resolvedDecl && named->resolvedDecl->isa<TableDeclAST>()) {
            table = named->resolvedDecl->as<TableDeclAST>();
        }
    }
    if (!table) {
        ctx.diagnostics.error(DiagCode::Name_FieldNotFound, expr,
                              "table name does not resolve to a table "
                              "declaration");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Host-backed table: no fields ───────────────────────────────────
    if (table->isHostBacked) {
        ctx.diagnostics.error(DiagCode::Name_FieldNotFound, expr,
                              "cannot access a field on host type '",
                              ctx.pool.lookup(table->name),
                              "' — host types are opaque");
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Column view? ───────────────────────────────────────────────────
    for (ColumnDeclAST* column : table->columns) {
        if (column && column->name == expr->fieldName) {
            expr->isColumnView   = true;
            expr->resolvedColumn = column;
            // A column view is a live view over the column's values.
            // It has no storable type (§5.6); the resolver returns a
            // marker that the small set of column-view-only resolvers
            // (`for` iterable, `.TOARRAY()`) recognize. For the common
            // case of a column view used directly in an expression,
            // this is an error.
            //
            // The marker is the column's own type, with `isColumnView`
            // set on the node — a caller that wants to know "is this a
            // column view?" checks the flag, and a caller that just
            // wants the type sees the column's element type.
            (void)target;
            return column->type;
        }
    }

    // ─── Compile-time row reference? ────────────────────────────────────
    //
    // On a table whose row set is fixed at declaration (`@fixed`,
    // `@readonly`, or `@packed`), `T.Member` resolves to the row whose
    // first `string` cell equals `"Member"`, at compile time.
    //
    // The guard is `hasFixedRowSet()` — the derived property, not the
    // `@fixed` attribute. A fixed-row-set table must have an initializer
    // (§4.1.1b), so its rows are always the inline `= [ ... ]` rows
    // written at the declaration site; a growing table may have an
    // initializer too, but `hasFixedRowSet()` is false for it and this
    // block is skipped, because a growing table's rows can be removed
    // and a `T.Member` reference would not have the compile-time
    // permanence the form promises.
    //
    // The lookup walks those inline rows and looks for one whose first
    // cell is a string literal equal to the field's name. If one is
    // found, the access is a `&T` referencing that row.
    //
    // The `isCompileTimeRowRef` flag is set so downstream passes (the
    // switch coverage check, the constant evaluator) can recognize this
    // form.
    if (table->hasFixedRowSet()) {
        InternedString member = expr->fieldName;
        for (size_t i = 0; i < table->rows.size(); ++i) {
            RowAST* row = table->rows[i];
            if (!row || row->cells.empty()) continue;
            ExprAST* firstCell = row->cells[0];
            if (!firstCell || !firstCell->isa<LiteralExprAST>()) continue;
            LiteralExprAST* lit = firstCell->as<LiteralExprAST>();
            if (lit->kind != LiteralKind::String &&
                lit->kind != LiteralKind::RawString) continue;
            if (lit->value != member) continue;

            // Found the row.
            expr->isCompileTimeRowRef = true;
            expr->isConst         = true;
            expr->isLValue        = false;
            expr->hasCompileTimeRow   = true;
            expr->compileTimeRowIndex = static_cast<uint32_t>(i);

            // The row reference's type is `&T`.
            NamedTypeAST* tableType = ctx.getNamedType(table->name);
            tableType->resolvedDecl = table;
            TypeAST* rowRefType = ctx.getRowRefType(tableType);
            (void)target;
            return rowRefType;
        }
    }

    // ─── Built-in method? ───────────────────────────────────────────────
    //
    // The method's name must be an ALLCAPS method registered in the
    // method registry. The set is fixed: `ADD`, `REMOVE`, `CLEAR`,
    // `SHRINK`, `AT`, `COUNT`, `VERSION`, `FIND`, `TOARRAY`, `SORT`,
    // `CONTAINS`. The registry answers "is this a recognized method on
    // this receiver?" — this function only needs to know whether to
    // consult it.
    TypeAST* methodType = tryResolveTableMethod(expr, table, ctx);
    if (methodType) {
        (void)target;
        return methodType;
    }

    // ─── by<Column>? ────────────────────────────────────────────────────
    //
    // A generated lookup name: `by` followed by a column name with its
    // first letter uppercased (`byId` for `@primary id`). The check
    // recognizes the shape, then resolves the column name against the
    // table's primary column.
    TypeAST* byColumnType = tryResolveByColumnLookup(expr, table, ctx);
    if (byColumnType) {
        (void)target;
        return byColumnType;
    }

    // ─── Nothing matched ────────────────────────────────────────────────
    ctx.diagnostics.error(DiagCode::Name_FieldNotFound, expr,
                          "table '", ctx.pool.lookup(table->name),
                          "' has no member named '",
                          ctx.pool.lookup(expr->fieldName), "'");
    expr->resolvedType = ctx.getUnknownType();
    expr->isLValue = false;
    return ctx.getUnknownType();
}

// ─────────────────────────────────────────────────────────────────────────────
// tryResolveTableMethod
// ─────────────────────────────────────────────────────────────────────────────
//
// Recognize a built-in table method by name and produce its function
// type. The registry answers "is this a recognized method on a table
// sheet?"; this function answers "is this particular table permitted to
// use it?" and "what is its concrete signature on this table?".
//
// The signature is built from the *shape* the registry records, not from
// a hardcoded name. Every `AddValue` method is built the same way
// regardless of its name; every `OneIndex` method is built the same way;
// and so on. The only per-method knowledge left in this function is the
// small set of use-site restrictions:
//
//   - `ADD`/`REMOVE`/`CLEAR`/`SHRINK` require a growing table
//     (`!hasFixedRowSet()`).
//   - Nothing else has a table-shape restriction.
//
// A method with no entry in this function's receiver-policy section is
// assumed to be legal on any table the registry says has it.

TypeAST* tryResolveTableMethod(FieldAccessExprAST* expr,
                                      TableDeclAST* table,
                                      SemaContext& ctx) {
    if (!expr) return nullptr;

    // ─── Is this a registered method? ───────────────────────────────────
    std::string_view methodName = ctx.pool.lookupView(expr->fieldName);
    const BuiltinMethodInfo* info =
        ctx.builtinMethodRegistry.getInfo(methodName);
    if (!info) return nullptr;

    // ─── Does it apply to a table sheet? ────────────────────────────────
    //
    // A name the registry knows but that is not registered for
    // `TableSheet` is not a table method. (`TOARRAY`, for instance, is
    // `ColumnView`-only; `CONTAINS` and `SORT` are array-only.) Return
    // null so the caller's fall-through reports "no such member".
    if (!ctx.builtinMethodRegistry.isForReceiver(methodName,
                                                 ReceiverKind::TableSheet)) {
        return nullptr;
    }

    // ─── Use-site policy: growing-table requirements ────────────────────
    //
    // Four table methods change the table's row set or storage and are
    // only available on a growing table (no `@fixed`, no `@readonly`,
    // no `@packed`). The registry's receiver list does not encode this
    // — the restriction is a property of the table, not of the method
    // name — so it lives here.
    //
    // The check names the four methods explicitly. There is no way to
    // fold them into a single registry predicate without adding a
    // "requiresGrowingTable" boolean to `BuiltinMethodInfo`, which
    // would be a registry change for one policy. The four strings
    // appear here in one place; that is the honest expression of the
    // rule.
    const bool requiresGrowingTable =
        methodName == "ADD"    ||
        methodName == "REMOVE" ||
        methodName == "CLEAR"  ||
        methodName == "SHRINK";

    if (requiresGrowingTable) {
        if (table->hasFixedRowSet()) {
            ctx.diagnostics.error(DiagCode::Table_AddOnFixed, expr,
                                  "'", methodName, "' is not available on a "
                                  "fixed-row-set table ('",
                                  ctx.pool.lookup(table->name), "')");
            return nullptr;
        }
        if (table->isReadonly) {
            ctx.diagnostics.error(DiagCode::Table_AddOnReadonly, expr,
                                  "'", methodName, "' is not available on a "
                                  "@readonly table ('",
                                  ctx.pool.lookup(table->name), "')");
            return nullptr;
        }
    }

    // ─── Build the concrete signature ───────────────────────────────────
    //
    // Every method's signature is derived from the table's row type (for
    // row-returning methods) or from the table's columns (for `ADD`).
    // The registry's `argShape` and `resultShape` tell us which case we
    // are in; the receiver supplies the concrete types.
    NamedTypeAST* tableType = ctx.getNamedType(table->name);
    tableType->resolvedDecl = table;
    TypeAST* rowRefType = ctx.getRowRefType(tableType);

    expr->isTableMethod = true;

    switch (info->argShape) {
        // ─── No arguments ───────────────────────────────────────────────
        //
        // `T.CLEAR()`, `T.SHRINK()`, `T.COUNT()`, `T.VERSION()`.
        case MethodArgShape::None: {
            ArenaSpan<TypeAST*> emptySpan = ctx.arena.emptySpan<TypeAST*>();
            switch (info->resultShape) {
                case MethodResultShape::Unit:
                    return ctx.getFunctionType(emptySpan, ctx.getUnitType());
                case MethodResultShape::Primitive: {
                    // Which primitive? The registry says "a primitive
                    // derived from the receiver"; for a table, `COUNT`
                    // is `uint` and `VERSION` is `uint64`. The
                    // distinction is per-method, so this is the one
                    // place the name is consulted for a *type*.
                    if (methodName == "COUNT") {
                        return ctx.getFunctionType(emptySpan, ctx.getUint32Type());
                    }
                    if (methodName == "VERSION") {
                        return ctx.getFunctionType(emptySpan, ctx.getUint64Type());
                    }
                    // A future primitive-returning table method with
                    // no argument needs a case here. Assert rather
                    // than silently mistype it.
                    AST_ASSERT_MSG(false,
                        "tryResolveTableMethod: Primitive result shape "
                        "with no known concrete type");
                    return nullptr;
                }
                default:
                    AST_ASSERT_MSG(false,
                        "tryResolveTableMethod: unexpected result shape "
                        "for a no-argument table method");
                    return nullptr;
            }
        }

        // ─── One index argument ─────────────────────────────────────────
        //
        // `T.REMOVE(i) -> unit`, `T.AT(i) -> &T`.
        case MethodArgShape::OneIndex: {
            ArenaSpan<TypeAST*> indexSpan =
                ctx.arena.makeSpan<TypeAST*>({ ctx.getUint32Type() });
            switch (info->resultShape) {
                case MethodResultShape::Unit:
                    return ctx.getFunctionType(indexSpan, ctx.getUnitType());
                case MethodResultShape::RowRef:
                    return ctx.getFunctionType(indexSpan, rowRefType);
                default:
                    AST_ASSERT_MSG(false,
                        "tryResolveTableMethod: unexpected result shape "
                        "for a OneIndex table method");
                    return nullptr;
            }
        }

        // ─── One predicate argument ─────────────────────────────────────
        //
        // `T.FIND(pred) -> T`. The predicate is `(&T) -> bool`.
        case MethodArgShape::OnePredicate: {
            ArenaSpan<TypeAST*> predicateParams =
                ctx.arena.makeSpan<TypeAST*>({ rowRefType });
            TypeAST* predicateType = ctx.getFunctionType(predicateParams,
                                                         ctx.getBoolType());
            ArenaSpan<TypeAST*> findParams =
                ctx.arena.makeSpan<TypeAST*>({ predicateType });
            return ctx.getFunctionType(findParams, tableType);
        }

        // ─── One argument per column ────────────────────────────────────
        //
        // `T.ADD(args...) -> &T`. The signature is `(C1, C2, ...) -> &T`.
        case MethodArgShape::AddValue: {
            std::vector<TypeAST*> params;
            params.reserve(table->columns.size());
            for (ColumnDeclAST* column : table->columns) {
                if (column && column->type) {
                    params.push_back(column->type);
                }
            }
            ArenaSpan<TypeAST*> addParams =
                ctx.arena.makeSpan<TypeAST*>(params);
            return ctx.getFunctionType(addParams, rowRefType);
        }

        // ─── Not applicable to a table ──────────────────────────────────
        //
        // A `OneElement` method is an array-only shape; a
        // `ZeroOrOneComparator` is `SORT`, which is array-only; a
        // `PrimaryKey` method is `by<Column>`, which is handled by
        // `tryResolveByColumnLookup` before this function is called.
        // Reaching here means a table-registered method has a shape
        // this function does not handle.
        case MethodArgShape::OneElement:
        case MethodArgShape::ZeroOrOneComparator:
        case MethodArgShape::PrimaryKey:
        default:
            AST_ASSERT_MSG(false,
                "tryResolveTableMethod: a table-registered method has a "
                "shape that is not legal for a table");
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// tryResolveByColumnLookup
// ─────────────────────────────────────────────────────────────────────────────
//
// Recognize `by<Column>` and produce the function type `(K) -> &T`
// where K is the primary column's key type.
//
// The registry's `isPrimaryLookupName` answers "does this name have the
// `by...` shape?" — a purely syntactic question. This function answers
// "does *this table* have a matching primary column?" — a semantic
// question the registry cannot decide.

TypeAST* tryResolveByColumnLookup(FieldAccessExprAST* expr,
                                         TableDeclAST* table,
                                         SemaContext& ctx) {
    if (!expr) return nullptr;

    std::string_view name = ctx.pool.lookupView(expr->fieldName);
    if (!BuiltinMethodRegistry::isPrimaryLookupName(name)) {
        return nullptr;
    }

    // ─── Find the primary column ────────────────────────────────────────
    ColumnDeclAST* primary = nullptr;
    for (ColumnDeclAST* column : table->columns) {
        if (column && column->isPrimary) {
            primary = column;
            break;
        }
    }
    if (!primary) {
        // `by<Column>`-shaped name on a table with no `@primary` column.
        // This is a name-resolution error, but only if the user meant
        // it as a lookup; if the table also has no column named `byXxx`,
        // this is the right diagnostic.
        return nullptr;
    }

    // ─── Confirm the name matches `by` + primary's name ─────────────────
    //
    // The rule (§4.1.5): the method name is `by` plus the column's name
    // with its first letter uppercased. The registry's
    // `primaryLookupColumnName` returns the suffix; the caller compares
    // it against the primary column's name with the same casing rule.
    std::string_view columnName = ctx.pool.lookupView(primary->name);
    std::string expected;
    expected.reserve(2 + columnName.size());
    expected += "by";
    if (!columnName.empty()) {
        expected += static_cast<char>(std::toupper(
            static_cast<unsigned char>(columnName[0])));
        expected.append(columnName.substr(1));
    }
    if (name != expected) {
        return nullptr;
    }

    // ─── Build the function type ────────────────────────────────────────
    //
    // `T.by<Column>(key: K) -> &T`.
    NamedTypeAST* tableType = ctx.getNamedType(table->name);
    tableType->resolvedDecl = table;
    TypeAST* rowRefType = ctx.getRowRefType(tableType);

    ArenaSpan<TypeAST*> params =
        ctx.arena.makeSpan<TypeAST*>({ primary->type });
    expr->isPrimaryLookup = true;
    return ctx.getFunctionType(params, rowRefType);
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveArrayMethodAccess
// ─────────────────────────────────────────────────────────────────────────────
//
// `arr.ADD`, `arr.SORT`, `arr.CONTAINS`, `arr.COUNT`, `arr.REMOVE`,
// `arr.CLEAR` — the array methods. Same treatment as table methods: the
// registry answers "is this a recognized method on an array?", this
// function answers "what is its concrete signature on this element type?".
//
// The receiver-kind check is `DynamicArray` or `FixedArray`. A method
// that requires a dynamic receiver (`ADD`/`REMOVE`/`CLEAR`) is rejected
// on a fixed-size array; the registry's receiver list for those three
// methods is `DynamicArray` only, so `isForReceiver` rejects them
// naturally. No separate length-changing check is needed.

TypeAST* resolveArrayMethodAccess(FieldAccessExprAST* expr,
                                         ArrayTypeAST* arrayType,
                                         TypeAST* target,
                                         SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();
    (void)target;

    std::string_view name = ctx.pool.lookupView(expr->fieldName);
    const BuiltinMethodInfo* info =
        ctx.builtinMethodRegistry.getInfo(name);
    if (!info) {
        ctx.diagnostics.error(DiagCode::Name_MethodNotFound, expr,
                              "array has no method named '", name, "'");
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Receiver-kind check ────────────────────────────────────────────
    //
    // `DynamicArray` and `FixedArray` are the two array receivers. A
    // method registered for `DynamicArray` only is not legal on a
    // fixed-size array; `isForReceiver` reports it. This replaces the
    // hand-written "length-changing method on fixed array" check with
    // the registry's own receiver list.
    const ReceiverKind kind = (arrayType->arrayKind == ArrayKind::Fixed)
                            ? ReceiverKind::FixedArray
                            : ReceiverKind::DynamicArray;
    if (!ctx.builtinMethodRegistry.isForReceiver(name, kind)) {
        ctx.diagnostics.error(DiagCode::Name_MethodNotFound, expr,
                              "'", name, "' is not available on a ",
                              (kind == ReceiverKind::FixedArray
                                   ? "fixed-size array"
                                   : "dynamic array"));
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Build the concrete signature from the shape ────────────────────
    TypeAST* elementType = arrayType->element;

    switch (info->argShape) {
        // ─── No arguments ───────────────────────────────────────────────
        //
        // `arr.COUNT() -> uint`, `arr.CLEAR() -> unit`.
        case MethodArgShape::None: {
            ArenaSpan<TypeAST*> emptySpan = ctx.arena.emptySpan<TypeAST*>();
            switch (info->resultShape) {
                case MethodResultShape::Unit:
                    return ctx.getFunctionType(emptySpan, ctx.getUnitType());
                case MethodResultShape::Primitive: {
                    // `COUNT` is the only no-argument primitive-returning
                    // array method today. A future method with this
                    // shape needs its own case; assert rather than
                    // silently mistype it.
                    if (name == "COUNT") {
                        return ctx.getFunctionType(emptySpan, ctx.getUint32Type());
                    }
                    AST_ASSERT_MSG(false,
                        "resolveArrayMethodAccess: Primitive result shape "
                        "with no known concrete type");
                    return ctx.getUnknownType();
                }
                default:
                    AST_ASSERT_MSG(false,
                        "resolveArrayMethodAccess: unexpected result shape "
                        "for a no-argument array method");
                    return ctx.getUnknownType();
            }
        }

        // ─── One index argument ─────────────────────────────────────────
        //
        // `arr.REMOVE(i) -> unit`.
        case MethodArgShape::OneIndex: {
            ArenaSpan<TypeAST*> indexSpan =
                ctx.arena.makeSpan<TypeAST*>({ ctx.getUint32Type() });
            return ctx.getFunctionType(indexSpan, ctx.getUnitType());
        }

        // ─── One element argument ───────────────────────────────────────
        //
        // `arr.ADD(x) -> unit`, `arr.CONTAINS(x) -> bool`.
        case MethodArgShape::OneElement: {
            ArenaSpan<TypeAST*> elementSpan =
                ctx.arena.makeSpan<TypeAST*>({ elementType });
            switch (info->resultShape) {
                case MethodResultShape::Unit:
                    return ctx.getFunctionType(elementSpan, ctx.getUnitType());
                case MethodResultShape::Bool:
                    return ctx.getFunctionType(elementSpan, ctx.getBoolType());
                default:
                    AST_ASSERT_MSG(false,
                        "resolveArrayMethodAccess: unexpected result shape "
                        "for a OneElement array method");
                    return ctx.getUnknownType();
            }
        }

        // ─── Zero or one comparator ─────────────────────────────────────
        //
        // `arr.SORT()` (natural order) or
        // `arr.SORT(less: (E, E) -> bool)`.
        //
        // The two overloads are distinguished by the argument count at
        // the call site. The function type produced here is the
        // one-argument form `((E, E) -> bool) -> unit`. The call
        // resolver accepts zero arguments for this method as a special
        // case (see `resolveCallExpr`'s handling of `ZeroOrOneComparator`
        // methods).
        case MethodArgShape::ZeroOrOneComparator: {
            ArenaSpan<TypeAST*> comparatorParams =
                ctx.arena.makeSpan<TypeAST*>({ elementType, elementType });
            TypeAST* comparatorType = ctx.getFunctionType(comparatorParams,
                                                          ctx.getBoolType());
            ArenaSpan<TypeAST*> sortParams =
                ctx.arena.makeSpan<TypeAST*>({ comparatorType });
            return ctx.getFunctionType(sortParams, ctx.getUnitType());
        }

        // ─── Not applicable to an array ─────────────────────────────────
        //
        // `OnePredicate` is table-only (`FIND`); `AddValue` is a table
        // form (`ADD` on a table takes one argument per column; on an
        // array it is `OneElement`, and the registry distinguishes the
        // two by receiver); `PrimaryKey` is a table-only shape
        // (`by<Column>`). Reaching here means an array-registered
        // method has a shape this function does not handle.
        case MethodArgShape::OnePredicate:
        case MethodArgShape::AddValue:
        case MethodArgShape::PrimaryKey:
        default:
            AST_ASSERT_MSG(false,
                "resolveArrayMethodAccess: an array-registered method "
                "has a shape that is not legal for an array");
            return ctx.getUnknownType();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveIndexExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// `container[index]`. Two cases:
//
//   - `Person[i]` — indexing a table name (or a table value) by a
//     `uint`. The result is `&T` (the row at slot i). Panics on
//     out-of-bounds; `T.AT(i)` is the nil-returning form.
//   - `arr[i]` — indexing an array by an integer. The result is the
//     array's element type. Panics on out-of-bounds.

TypeAST* resolveIndexExpr(IndexExprAST* expr, TypeAST* /*target*/,
                                 SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    TypeAST* targetType = resolveExpr(expr->target, ctx);
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Table indexing ─────────────────────────────────────────────────
    if (isTableType(targetType, ctx)) {
        // `Person[i]` — the index is a `uint`, the result is `&T`.
        TypeAST* indexType = resolveExprWithTarget(
            expr->index, ctx.getPrimitiveType(PrimitiveKind::Uint32), ctx);
        if (!indexType || indexType->isa<UnknownTypeAST>()) {
            expr->resolvedType = ctx.getUnknownType();
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        TypeAST* tableType = targetType;
        TypeAST* rowRefType = ctx.getRowRefType(tableType);
        expr->isLValue = false;   // A `&T` is not itself an lvalue; its cells are.
        return rowRefType;
    }

    // ─── Array indexing ─────────────────────────────────────────────────
    if (isArrayType(targetType)) {
        ArrayTypeAST* array = targetType->as<ArrayTypeAST>();
        TypeAST* indexType = resolveExprWithTarget(
            expr->index, ctx.getPrimitiveType(PrimitiveKind::Int32), ctx);
        if (!indexType || indexType->isa<UnknownTypeAST>()) {
            expr->resolvedType = ctx.getUnknownType();
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        // An array index is an lvalue iff the array it indexes is an
        // lvalue. `arr[0] = x` is legal when `arr` is a `let` binding;
        // a literal array `[1, 2, 3][0] = x` is not.
        expr->isLValue = expr->target->isLValue;
        return array->element;
    }

    ctx.diagnostics.error(DiagCode::Type_Mismatch, expr->target,
                          "indexing requires a table or an array, got ",
                          typeToString(targetType, ctx.pool));
    expr->resolvedType = ctx.getUnknownType();
    expr->isLValue = false;
    return ctx.getUnknownType();
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveCallExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// `callee(args)`. The callee is a function value — a named `FN`, a
// lambda, a table method (`Person.ADD`), or a module function
// (`math.sqrt`). The resolver checks the arguments against the callee's
// signature.
//
// Variadic parameters: the callee's last parameter may be a `[T]` array
// marked `isVariadic`. The caller passes zero or more `T`s; the callee
// receives one `[T]`.

TypeAST* resolveCallExpr(CallExprAST* expr, TypeAST* /*target*/,
                                SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();
    if (!expr->callee) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                              "call expression has no callee");
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Resolve the callee to a function type ──────────────────────────
    TypeAST* calleeType = resolveExpr(expr->callee, ctx);
    if (!calleeType || calleeType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }
    if (!calleeType->isa<FunctionTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Name_NotCallable, expr->callee,
                              "expression is not callable — its type is ",
                              typeToString(calleeType, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    FunctionTypeAST* fnType = calleeType->as<FunctionTypeAST>();

    // ─── Sequence-call restrictions ─────────────────────────────────────
    //
    // A `@sequence` function cannot be called directly; it must be
    // launched with `start`. Reject a plain call whose callee resolves
    // to a `@sequence` `FnDeclAST`.
    //
    // The check reads `expr->callee`'s resolved declaration. The callee
    // is an `IdentifierExprAST` or a `FieldAccessExprAST`; either can
    // have a `resolvedDecl` pointing at a `FnDeclAST`.
    if (expr->callee->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = expr->callee->as<IdentifierExprAST>();
        if (id->resolvedDecl && id->resolvedDecl->isa<FnDeclAST>()) {
            FnDeclAST* fn = id->resolvedDecl->as<FnDeclAST>();
            if (fn->isSequence) {
                ctx.diagnostics.error(DiagCode::Seq_SequenceCalledDirectly,
                                      expr->callee,
                                      "'", ctx.pool.lookup(fn->name),
                                      "' is a @sequence function and must be "
                                      "launched with 'start'");
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
        }
    } else if (expr->callee->isa<FieldAccessExprAST>()) {
        FieldAccessExprAST* fa = expr->callee->as<FieldAccessExprAST>();
        if (fa->resolvedDecl && fa->resolvedDecl->isa<FnDeclAST>()) {
            FnDeclAST* fn = fa->resolvedDecl->as<FnDeclAST>();
            if (fn->isSequence) {
                ctx.diagnostics.error(DiagCode::Seq_SequenceCalledDirectly,
                                      expr->callee,
                                      "a @sequence function must be launched "
                                      "with 'start'");
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
        }
    }

    // ─── Argument-count check (with variadic) ───────────────────────────
    //
    // A variadic function's last parameter absorbs zero or more
    // trailing arguments. The rule:
    //   - the number of fixed parameters is `params.size() - 1` if the
    //     last is variadic, else `params.size()`;
    //   - the caller must provide at least the fixed count;
    //   - if the function is not variadic, the caller must provide
    //     exactly the total count.
    const size_t totalParams = fnType->params.size();
    bool hasVariadic = false;
    size_t variadicIndex = totalParams;
    for (size_t i = 0; i < totalParams; ++i) {
        TypeAST* paramType = fnType->params[i];
        if (paramType->isa<ArrayTypeAST>() &&
            fnType->params[i] == fnType->params.back() /* sanity */) {
            // The variadic marker is not on the type; it is on the
            // declaration's `ParamAST::isVariadic`. But a function
            // *type*'s parameter list is a span of `TypeAST*`, not
            // `ParamAST*`, so the type alone does not carry the
            // variadic flag.
            //
            // Resolution: the AST's `FunctionTypeAST::params` is a span
            // of `TypeAST*`, and variadic-ness is a property of the
            // *declaration's* parameter list, not of the function type.
            // The call resolver therefore does not see variadic markers
            // through the function type; it must consult the callee's
            // `FnDeclAST` if it wants to know.
            //
            // Since the callee is a resolved node, the resolver can
            // follow `resolvedDecl` to the `FnDeclAST` and check
            // `params[i]->isVariadic`. That check is done below; this
            // loop is only about the *type* shape.
        }
    }

    // ─── Variadic check via the callee's declaration ────────────────────
    FnDeclAST* fnDecl = nullptr;
    if (expr->callee->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = expr->callee->as<IdentifierExprAST>();
        if (id->resolvedDecl && id->resolvedDecl->isa<FnDeclAST>()) {
            fnDecl = id->resolvedDecl->as<FnDeclAST>();
        }
    } else if (expr->callee->isa<FieldAccessExprAST>()) {
        FieldAccessExprAST* fa = expr->callee->as<FieldAccessExprAST>();
        if (fa->resolvedDecl && fa->resolvedDecl->isa<FnDeclAST>()) {
            fnDecl = fa->resolvedDecl->as<FnDeclAST>();
        }
    }

    if (fnDecl && !fnDecl->params.empty() &&
        fnDecl->params.back()->isVariadic) {
        hasVariadic = true;
        variadicIndex = fnDecl->params.size() - 1;
    }

    const size_t fixedCount = hasVariadic ? variadicIndex : totalParams;
    const size_t argCount = expr->args.size();

    if (hasVariadic) {
        if (argCount < fixedCount) {
            ctx.diagnostics.error(DiagCode::Type_ArgCountMismatch, expr,
                                  "function expects at least ", fixedCount,
                                  " argument(s), got ", argCount);
            expr->resolvedType = ctx.getUnknownType();
            return ctx.getUnknownType();
        }
    } else {
        if (argCount != totalParams) {
            ctx.diagnostics.error(DiagCode::Type_ArgCountMismatch, expr,
                                  "function expects ", totalParams,
                                  " argument(s), got ", argCount);
            expr->resolvedType = ctx.getUnknownType();
            return ctx.getUnknownType();
        }
    }

    // ─── Check each argument ────────────────────────────────────────────
    for (size_t i = 0; i < argCount; ++i) {
        TypeAST* expectedType = nullptr;
        if (hasVariadic && i >= variadicIndex) {
            // The variadic parameter's type is `[E]`; each trailing
            // argument is an `E`.
            TypeAST* variadicType = fnType->params[variadicIndex];
            if (!variadicType->isa<ArrayTypeAST>()) {
                // A variadic parameter's type should be an array type.
                // If it is not, the resolver has a stale function type.
                AST_ASSERT_MSG(false,
                    "resolveCallExpr: variadic parameter's type is not "
                    "an array");
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
            expectedType = variadicType->as<ArrayTypeAST>()->element;
        } else {
            expectedType = fnType->params[i];
        }

        TypeAST* argType = resolveExprWithTarget(expr->args[i], expectedType, ctx);
        if (!argType || argType->isa<UnknownTypeAST>()) {
            expr->resolvedType = ctx.getUnknownType();
            return ctx.getUnknownType();
        }
    }

    // ─── Sequence-call resolution ───────────────────────────────────────
    //
    // If the callee resolved to a `@sequence` declaration, reject the
    // call — the previous check already did this. If the callee resolved
    // to an ordinary function, the call is legal.
    //
    // `start` is the only way to invoke a sequence; that path is handled
    // by `resolveStartExpr`.

    return fnType->returnType;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveLambdaExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// A lambda's parameter types and return type come from the target
// function type when it is available. With no target, a lambda is
// resolved in "inference from body" mode — parameter types are
// inferred from how the body uses them, and the return type from the
// body's type. That mode is rarely useful (an unannotated lambda with
// no target is a mostly-blind inference) but it is the grammar's rule.

TypeAST* resolveLambdaExpr(LambdaExprAST* expr, TypeAST* target,
                                  SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── Collect the lambda ─────────────────────────────────────────────
    //
    // The lambda is recorded before any resolution, so the bytecode
    // compiler's LambdaLift sees every lambda that appears in the
    // source — including one whose body has a resolution error. A
    // lambda that fails to resolve still needs a synthesized function
    // slot; emitting a call to a non-existent function index is a
    // much worse failure than a diagnostic on the lambda's body.
    //
    // Appending here (rather than in `resolveExprWithTarget`'s dispatch
    // or in `Sema.cpp`) is what makes the collection complete: this is
    // the one function every lambda passes through, in every pass that
    // resolves an expression.
    ctx.pendingLambdas[ctx.currentModule].push_back(expr);

    // ─── Determine the parameter types ──────────────────────────────────
    //
    // If a target function type is given, its parameters are the
    // lambda's parameters. Otherwise the lambda's own parameter types
    // are what the source wrote (unannotated parameters get `unknown`,
    // which will fail further inference).
    FunctionTypeAST* targetFn = nullptr;
    if (target && target->isa<FunctionTypeAST>()) {
        targetFn = target->as<FunctionTypeAST>();
    }

    if (targetFn && targetFn->params.size() != expr->params.size()) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, expr,
                              "lambda takes ", expr->params.size(),
                              " parameter(s), target expects ",
                              targetFn->params.size());
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Push the lambda's scope ────────────────────────────────────────
    //
    // A lambda's body sees only its own parameters and the module's
    // top-level declarations — no enclosing locals. The scope for the
    // lambda is fresh; nothing from an enclosing block is visible.
    //
    // The `ScopedFunction` guard here is a "scope for the parameters"
    // push. It pushes `FuncBody` because a lambda is not a `@sequence`
    // function (the grammar does not allow `wait*` inside a lambda; a
    // lambda's body is a single expression, so it cannot contain a
    // statement anyway).
    SymbolScope lambdaScope(ctx);

    for (size_t i = 0; i < expr->params.size(); ++i) {
        ParamAST* param = expr->params[i];

        TypeAST* paramType = nullptr;
        if (targetFn) {
            paramType = targetFn->params[i];
        } else {
            paramType = resolveType(param->type, ctx);
            if (!paramType || paramType->isa<UnknownTypeAST>()) {
                paramType = ctx.getUnknownType();
            }
        }
        param->type = paramType;
        if (!param->name.isEmpty()) {
            ctx.insertLocal(param);
        }
    }

    // ─── Resolve the body against the target's return type ──────────────
    TypeAST* bodyTarget = targetFn ? targetFn->returnType : nullptr;
    TypeAST* bodyType = resolveExprWithTarget(expr->body, bodyTarget, ctx);
    if (!bodyType || bodyType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Build the lambda's function type ───────────────────────────────
    //
    // The parameter types are whatever the lambda's parameters resolved
    // to; the return type is the body's type.
    std::vector<TypeAST*> paramTypes;
    paramTypes.reserve(expr->params.size());
    for (ParamAST* param : expr->params) {
        paramTypes.push_back(param->type);
    }
    auto span = ctx.arena.makeSpan<TypeAST*>(paramTypes);
    return ctx.getFunctionType(span, bodyType);
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveStartExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// `start f(args)`. The callee must be a `@sequence` function. The
// result is a `&Coroutine` handle.

TypeAST* resolveStartExpr(StartExprAST* expr, TypeAST* /*target*/,
                          SemaContext& ctx) {
    if (!expr || !expr->call) {
        if (expr) expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Check the callee is a @sequence function ───────────────────────
    //
    // A bare identifier resolves to a `FnDeclAST` via `lookupFunction`,
    // which returns the function specifically (not a `ValueLookup`
    // discriminated result). The field-access case reads its
    // `resolvedDecl`, which is set by the field-access resolver.
    FnDeclAST* fn = nullptr;
    if (expr->call->callee->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = expr->call->callee->as<IdentifierExprAST>();
        fn = ctx.lookupFunction(id->name);
    } else if (expr->call->callee->isa<FieldAccessExprAST>()) {
        FieldAccessExprAST* fa = expr->call->callee->as<FieldAccessExprAST>();
        if (fa->resolvedDecl && fa->resolvedDecl->isa<FnDeclAST>()) {
            fn = fa->resolvedDecl->as<FnDeclAST>();
        }
    }

    if (!fn) {
        ctx.diagnostics.error(DiagCode::Seq_NonSequenceStarted, expr,
                              "'start' requires a call to a @sequence "
                              "function");
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }
    if (!fn->isSequence) {
        ctx.diagnostics.error(DiagCode::Seq_NonSequenceStarted, expr,
                              "'", ctx.pool.lookup(fn->name),
                              "' is not a @sequence function");
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // ─── Resolve the call's arguments ───────────────────────────────────
    //
    // The call is resolved the ordinary way — a `@sequence` function's
    // parameter list is checked the same as any other function's. The
    // only difference is that a `@sequence` function always returns
    // `unit`, so the call's result is discarded; the `start`
    // expression's own result is the handle.
    TypeAST* callType = resolveExpr(expr->call, ctx);
    (void)callType;

    // ─── The handle's type is `&Coroutine` ──────────────────────────────
    //
    // `Coroutine` is a host-backed table declared in the standard
    // library. The resolver needs it to be in scope; the caller is
    // responsible for having imported `core.coroutine` or whatever
    // module declares it. If the type is not in scope, the resolver
    // emits an error and returns unknown.
    TypeDeclAST* coroutineDecl = ctx.lookupType(ctx.pool.intern("Coroutine"));
    if (!coroutineDecl || !coroutineDecl->isa<TableDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Seq_SequenceAsFunctionValue, expr,
                              "'start' produces a '&Coroutine' handle, but "
                              "'Coroutine' is not declared in the current "
                              "module — import the module that declares it");
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    NamedTypeAST* coroutineType = ctx.getNamedType(ctx.pool.intern("Coroutine"));
    coroutineType->resolvedDecl = coroutineDecl;
    return ctx.getRowRefType(coroutineType);
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveUnaryExpr
// ═════════════════════════════════════════════════════════════════════════════

TypeAST* resolveUnaryExpr(UnaryExprAST* expr, TypeAST* /*target*/,
                                 SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    TypeAST* operandType = resolveExpr(expr->operand, ctx);
    if (!operandType || operandType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    switch (expr->op) {
        case UnaryOp::Neg:
            if (!isNumericType(operandType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidUnary, expr,
                                      "unary '-' requires a numeric operand, "
                                      "got ",
                                      typeToString(operandType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                expr->isLValue = false;
                return ctx.getUnknownType();
            }
            expr->isLValue = false;
            return operandType;

        case UnaryOp::Not:
            if (!isBoolType(operandType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidUnary, expr,
                                      "'not' requires a bool operand, got ",
                                      typeToString(operandType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                expr->isLValue = false;
                return ctx.getUnknownType();
            }
            expr->isLValue = false;
            return ctx.getPrimitiveType(PrimitiveKind::Bool);

        case UnaryOp::BitNot:
            if (!isIntegerType(operandType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidUnary, expr,
                                      "'~' requires an integer operand, got ",
                                      typeToString(operandType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                expr->isLValue = false;
                return ctx.getUnknownType();
            }
            expr->isLValue = false;
            return operandType;
    }

    AST_ASSERT_MSG(false, "resolveUnaryExpr: unrecognized UnaryOp");
    return ctx.getUnknownType();
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveBinaryExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// The binary operators: arithmetic, comparison, logical, bitwise,
// null-coalescing. The operator is a field on the node; the resolver
// dispatches on it and on the operands' types.

TypeAST* resolveBinaryExpr(BinaryExprAST* expr, TypeAST* /*target*/,
                                  SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── If-condition narrowing detection ───────────────────────────────
    //
    // When this binary expression is the condition of an `if`, the
    // narrowing detector runs first. `x != nil` and `x == nil` are the
    // two forms that produce a narrowing; the detector recognizes them
    // and stores the narrowing on the current if-frame.
    //
    // The detector is called *before* the operands are resolved, so
    // that the shape is read from the AST (an `IdentifierExprAST` on
    // one side, a `nil` literal on the other), not from the resolved
    // types. The resolved types are the same for `x != nil` and
    // `x != 0` — a `bool` — so the shape is what distinguishes a
    // narrowing site.
    if (ctx.stack.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.stack.setPendingNarrowing(info);
        }
    }

    // ─── Resolve both operands ──────────────────────────────────────────
    TypeAST* leftType = resolveExpr(expr->left, ctx);
    if (!leftType || leftType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    TypeAST* rightType = resolveExpr(expr->right, ctx);
    if (!rightType || rightType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    expr->isLValue = false;

    // ─── Dispatch on operator ───────────────────────────────────────────
    switch (expr->op) {
        // ─── Arithmetic ─────────────────────────────────────────────────
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
        case BinaryOp::Pow: {
            // `+` is overloaded: numeric addition and string concatenation.
            if (expr->op == BinaryOp::Add &&
                isStringType(leftType) && isStringType(rightType)) {
                return ctx.getPrimitiveType(PrimitiveKind::String);
            }
            if (!isNumericType(leftType) || !isNumericType(rightType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr,
                                      "arithmetic operator requires numeric "
                                      "operands, got ",
                                      typeToString(leftType, ctx.pool),
                                      " and ",
                                      typeToString(rightType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
            // Numeric operands must be the same concrete type (§5.8:
            // no implicit coercion between two already-typed values).
            // The exception is an untyped literal, which the resolver
            // has already adapted to the target's type — so by the
            // time both operands are resolved, they are the same type
            // or the user made a mistake.
            if (!typesEqual(leftType, rightType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr,
                                      "arithmetic operands must have the same "
                                      "type, got ",
                                      typeToString(leftType, ctx.pool),
                                      " and ",
                                      typeToString(rightType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
            return leftType;
        }

        // ─── Comparison ─────────────────────────────────────────────────
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge: {
            // Ordering operators (< <= > >=) require numeric, string, or
            // char operands of the same type. Equality operators (== !=)
            // are more permissive — they work on any pair of values of
            // the same type, plus `&T` against `nil`, plus `T?` against
            // `nil` and against `T?` of the same `T`.
            const bool isOrdering = (expr->op == BinaryOp::Lt ||
                                     expr->op == BinaryOp::Le ||
                                     expr->op == BinaryOp::Gt ||
                                     expr->op == BinaryOp::Ge);

            if (isOrdering) {
                const bool isOrderableLeft  = isNumericType(leftType) ||
                                              isStringType(leftType)  ||
                                              isCharType(leftType);
                const bool isOrderableRight = isNumericType(rightType) ||
                                              isStringType(rightType)  ||
                                              isCharType(rightType);
                if (!isOrderableLeft || !isOrderableRight ||
                    !typesEqual(leftType, rightType)) {
                    ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr,
                                          "ordering operator requires operands "
                                          "of the same numeric, string, or "
                                          "char type, got ",
                                          typeToString(leftType, ctx.pool),
                                          " and ",
                                          typeToString(rightType, ctx.pool));
                    expr->resolvedType = ctx.getUnknownType();
                    return ctx.getUnknownType();
                }
            } else {
                // `==` / `!=` — the operands must be comparable. The
                // comparability rules:
                //   - identical types → comparable
                //   - `&T` and `nil` → comparable (identity check)
                //   - `T?` and `nil` → comparable (nil-check)
                //   - two `T?` of the same T → comparable
                //   - anything else → not comparable
                const bool bothSame = typesEqual(leftType, rightType);
                const bool leftIsNilLit =
                    expr->left->isa<LiteralExprAST>() &&
                    expr->left->as<LiteralExprAST>()->kind == LiteralKind::Nil;
                const bool rightIsNilLit =
                    expr->right->isa<LiteralExprAST>() &&
                    expr->right->as<LiteralExprAST>()->kind == LiteralKind::Nil;
                const bool nilVsRef =
                    (leftIsNilLit && isRowRefType(rightType)) ||
                    (rightIsNilLit && isRowRefType(leftType));
                const bool nilVsNullable =
                    (leftIsNilLit && isNullableType(rightType)) ||
                    (rightIsNilLit && isNullableType(leftType));
                if (!bothSame && !nilVsRef && !nilVsNullable) {
                    ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr,
                                          "cannot compare ",
                                          typeToString(leftType, ctx.pool),
                                          " and ",
                                          typeToString(rightType, ctx.pool));
                    expr->resolvedType = ctx.getUnknownType();
                    return ctx.getUnknownType();
                }
            }
            return ctx.getPrimitiveType(PrimitiveKind::Bool);
        }

        // ─── Logical ────────────────────────────────────────────────────
        case BinaryOp::And:
        case BinaryOp::Or:
            if (!isBoolType(leftType) || !isBoolType(rightType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr,
                                      "logical operator requires bool operands, "
                                      "got ",
                                      typeToString(leftType, ctx.pool),
                                      " and ",
                                      typeToString(rightType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
            return ctx.getPrimitiveType(PrimitiveKind::Bool);

        // ─── Bitwise ────────────────────────────────────────────────────
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr:
            if (!isIntegerType(leftType) || !isIntegerType(rightType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr,
                                      "bitwise operator requires integer "
                                      "operands, got ",
                                      typeToString(leftType, ctx.pool),
                                      " and ",
                                      typeToString(rightType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
            if (!typesEqual(leftType, rightType)) {
                ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr,
                                      "bitwise operands must have the same "
                                      "type, got ",
                                      typeToString(leftType, ctx.pool),
                                      " and ",
                                      typeToString(rightType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
            return leftType;

        // ─── Null coalescing ────────────────────────────────────────────
        case BinaryOp::NullCoalesce: {
            // The LHS must be a nullable type (`T?`) or a row reference
            // (`&T`, which is inherently nilable). The RHS must be the
            // corresponding non-nil type (`T`). The result is `T`.
            TypeAST* innerType = nullptr;
            if (isNullableType(leftType)) {
                innerType = unwrapNullable(leftType);
            } else if (isRowRefType(leftType)) {
                // A row reference's non-nil type is itself — the
                // reference may be `nil`, but it is still a `&T`.
                innerType = leftType;
            } else {
                ctx.diagnostics.error(DiagCode::Type_InvalidBinary, expr->left,
                                      "'?\?' requires a nullable or "
                                      "row-reference LHS, got ",
                                      typeToString(leftType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }

            if (!isAssignable(innerType, rightType, ctx)) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, expr->right,
                                      "'?\?' fallback must be assignable to ",
                                      typeToString(innerType, ctx.pool),
                                      ", got ",
                                      typeToString(rightType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
            return innerType;
        }
    }

    AST_ASSERT_MSG(false, "resolveBinaryExpr: unrecognized BinaryOp");
    return ctx.getUnknownType();
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveParenExpr / resolveRangeExpr
// ═════════════════════════════════════════════════════════════════════════════

TypeAST* resolveParenExpr(ParenExprAST* expr, TypeAST* target,
                                 SemaContext& ctx) {
    if (!expr || !expr->inner) return ctx.getUnknownType();
    // Parentheses are transparent to resolution — the tree's shape
    // already encodes precedence. The inner expression is resolved
    // exactly as if the parentheses were absent.
    TypeAST* inner = resolveExprWithTarget(expr->inner, target, ctx);
    expr->isLValue = expr->inner->isLValue;
    expr->isConst  = expr->inner->isConst;
    return inner;
}

TypeAST* resolveRangeExpr(RangeExprAST* expr, TypeAST* /*target*/,
                                 SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // A range's bounds are integers of the same type. The default is
    // `int` if no context fixes them; the range's own type is the
    // bounds' type.
    TypeAST* intType = ctx.getPrimitiveType(PrimitiveKind::Int32);
    TypeAST* loType = resolveExprWithTarget(expr->lo, intType, ctx);
    TypeAST* hiType = resolveExprWithTarget(expr->hi, intType, ctx);
    if (!loType || !hiType) {
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }
    if (!typesEqual(loType, hiType)) {
        ctx.diagnostics.error(DiagCode::Type_RangeBoundTypeMismatch, expr,
                              "range bounds must have the same type, got ",
                              typeToString(loType, ctx.pool),
                              " and ",
                              typeToString(hiType, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        return ctx.getUnknownType();
    }

    // A step, if present, is the same type as the bounds.
    if (expr->step) {
        TypeAST* stepType = resolveExprWithTarget(expr->step, loType, ctx);
        if (!stepType || stepType->isa<UnknownTypeAST>()) {
            expr->resolvedType = ctx.getUnknownType();
            return ctx.getUnknownType();
        }
        // Zero step is a compile error.
        if (expr->step->isConst && expr->step->constValue.isInt()) {
            if (expr->step->constValue.asInt() == 0) {
                ctx.diagnostics.error(DiagCode::Type_RangeStepZero, expr->step,
                                      "range step must not be zero");
                expr->resolvedType = ctx.getUnknownType();
                return ctx.getUnknownType();
            }
        }
    }

    expr->isLValue = false;
    return loType;
}

} // namespace lucid::sema
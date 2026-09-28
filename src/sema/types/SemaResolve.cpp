/// @file SemaResolve.cpp
/// @brief Type resolution: turning a syntactic TypeAST into a semantic one.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations of the per-form resolvers.
//
// These are static — nothing outside this file calls them. resolveType is
// the only public entry point.
// ─────────────────────────────────────────────────────────────────────────────

static TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx);
static TypeAST* resolveNamedType    (NamedTypeAST* type,     SemaContext& ctx);
static TypeAST* resolveArrayType    (ArrayTypeAST* type,     SemaContext& ctx);
static TypeAST* resolveRowRefType   (RowRefTypeAST* type,    SemaContext& ctx);
static TypeAST* resolveNullableType (NullableTypeAST* type,  SemaContext& ctx);
static TypeAST* resolveFuncType     (FunctionTypeAST* type,      SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// resolveType — the public entry point
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* resolveType(TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // A type that came from a broken parse short-circuits. The parser
    // already reported the syntax error; Sema must not pile on.
    if (type->hasSyntaxError) {
        return ctx.getUnknownType();
    }

    switch (type->kind) {
        case ASTKind::PrimitiveType: return resolvePrimitiveType(type->as<PrimitiveTypeAST>(), ctx);
        case ASTKind::NamedType:     return resolveNamedType    (type->as<NamedTypeAST>(),     ctx);
        case ASTKind::ArrayType:     return resolveArrayType    (type->as<ArrayTypeAST>(),     ctx);
        case ASTKind::RowRefType:    return resolveRowRefType   (type->as<RowRefTypeAST>(),    ctx);
        case ASTKind::NullableType:  return resolveNullableType (type->as<NullableTypeAST>(),  ctx);
        case ASTKind::FunctionType:  return resolveFuncType     (type->as<FunctionTypeAST>(),      ctx);

        case ASTKind::UnknownType:
            return ctx.getUnknownType();

        default:
            // A TypeAST node of an unrecognized kind is a compiler bug,
            // not a user error — the parser only produces the seven
            // forms above.
            AST_ASSERT_MSG(false,
                "resolveType: unrecognized TypeAST kind — a new type form "
                "was added without extending the dispatch");
            return ctx.getUnknownType();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Primitive
// ─────────────────────────────────────────────────────────────────────────────

/// A primitive is already concrete when the parser produces it: the
/// lexical form has been folded to a PrimitiveKind, and aliases (int /
/// int32, long / int64, ...) have been resolved. There is nothing to do.
/// The function exists so the dispatch reads uniformly.
static TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx) {
    (void)ctx;
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// Named — a table name, optionally module-qualified
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve a `NamedTypeAST` to the `TableDeclAST` it names.
///
/// Two shapes reach here:
///
///   - `Table` — an unqualified name. Look it up in the current module's
///     type namespace.
///   - `alias.Table` — a qualified name. Look up `alias` among the current
///     module's import aliases, then look up `Table` in the target module's
///     exported type namespace. §5 fixes this at exactly one level of
///     qualification: `[ IDENTIFIER '.' ] IDENTIFIER`. A deeper path is a
///     parser error, not a Sema one.
///
/// A `NamedTypeAST` can also name a host-backed table (`TABLE X = host("...")`).
/// The name resolution is identical; the difference is that a host-backed
/// table's `TableDeclAST::isHostBacked` is true and its `columns` is empty.
/// Sema checks that distinction at the *use* site, not here — the type
/// resolver's job is only to bind the name.
static TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeDeclAST* decl = nullptr;

    if (type->isQualified()) {
        // ─── Qualified: alias.Table ─────────────────────────────────────
        ModuleAST* module = ctx.lookupImport(type->qualifier);
        if (!module) {
            ctx.diagnostics.error(DiagCode::Name_UndefinedModule, type,
                                  "undefined module alias '",
                                  ctx.pool.lookup(type->qualifier), "'");
            return ctx.getUnknownType();
        }

        decl = ctx.lookupImportedType(type->qualifier, type->name);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Name_UndefinedType, type,
                                  "module '", ctx.pool.lookup(type->qualifier),
                                  "' has no type named '",
                                  ctx.pool.lookup(type->name), "'");
            return ctx.getUnknownType();
        }

        if (!decl->isExported) {
            ctx.diagnostics.error(DiagCode::Name_PrivateMember, type,
                                  "type '", ctx.pool.lookup(type->name),
                                  "' in module '",
                                  ctx.pool.lookup(type->qualifier),
                                  "' is not exported");
            return ctx.getUnknownType();
        }
    } else {
        // ─── Unqualified: Table ─────────────────────────────────────────
        decl = ctx.lookupType(type->name);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Name_UndefinedType, type,
                                  "undefined type '",
                                  ctx.pool.lookup(type->name), "'");
            return ctx.getUnknownType();
        }
    }

    // The only user-declarable type in the new grammar is a table. A
    // NamedTypeAST that resolves to anything else is a Sema bug — the
    // type namespace can only contain TableDeclAST.
    AST_ASSERT_MSG(decl->isa<TableDeclAST>(),
                   "resolveNamedType: a type name resolved to a non-table "
                   "declaration — the type namespace should contain only "
                   "TableDeclAST nodes");

    // Bind the resolved declaration back onto the syntactic node, then
    // canonicalize through the cache so two references to the same table
    // share a node. Canonicalization is what makes typesEqual's pointer
    // fast-path hit.
    type->resolvedDecl = decl;

    NamedTypeAST* canonical = ctx.getNamedType(type->name);
    canonical->qualifier    = type->qualifier;
    canonical->resolvedDecl = decl;
    return canonical;
}

// ─────────────────────────────────────────────────────────────────────────────
// Array
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve `[T]` or `[N, T]`. The element type is resolved recursively;
/// the array's own kind and size are already concrete from the parser.
///
/// A `[N, T]` with N == 0 is legal — it is an empty fixed-size array,
/// which has uses as a zero-length placeholder. The parser accepts any
/// non-negative integer literal; Sema does not further restrict it.
///
/// The element type is not otherwise constrained: `[int]`, `[&Person]`,
/// `[[int]]`, `[[5, int]]`, `[Person?]` are all legal. The runtime
/// stores row references and nested arrays with the shared-buffer layout
/// of §7.8, so nothing is rejected here on representation grounds.
static TypeAST* resolveArrayType(ArrayTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* element = resolveType(type->element, ctx);
    if (!element || element->isa<UnknownTypeAST>()) {
        // The recursive resolve already reported the specific error (an
        // undefined table name, a bad `&` target, etc.). Return unknown
        // so the caller's error-suppression logic kicks in.
        return ctx.getUnknownType();
    }

    type->element = element;

    // Canonicalize through the cache. Two `[int]` written at different
    // sites share a node.
    ArrayTypeAST* canonical = ctx.getArrayType(type->arrayKind,
                                              type->fixedSize,
                                              element);
    canonical->loc = type->loc;
    return canonical;
}

// ─────────────────────────────────────────────────────────────────────────────
// Row reference — &T
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve `&T`. The inner type must be a table — a columned table or a
/// host-backed table. `&int`, `&[int]`, `&&T`, and `&SomeFunctionType`
/// are all errors (§5.1.1).
///
/// `&T` is inherently nilable (§5.2), so a `&T?` written in source is
/// accepted and treated as `&T` — the `?` is a readability hint, not a
/// distinct type. That rule is enforced in `resolveNullableType`, which
/// detects a nullable inner of a row-ref and short-circuits. This
/// function only sees `&T` without the redundant suffix.
static TypeAST* resolveRowRefType(RowRefTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner || inner->isa<UnknownTypeAST>()) {
        return ctx.getUnknownType();
    }

    if (!isTableType(inner, ctx)) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, type,
                              "'&' requires a table type, got ",
                              typeToString(inner, ctx.pool));
        ctx.diagnostics.note(type,
                             "'&T' is a reference to one row of table T. "
                             "Primitives are always copied (§5.1.1).");
        return ctx.getUnknownType();
    }

    type->inner = inner;

    RowRefTypeAST* canonical = ctx.getRowRefType(inner);
    canonical->loc = type->loc;
    return canonical;
}

// ─────────────────────────────────────────────────────────────────────────────
// Nullable — T?
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve `T?`.
///
/// Nilability is meaningful for a primitive, a host-backed table, or an
/// array. It is redundant (and accepted silently) on a row reference —
/// `&T?` is the same type as `&T`. It is an error on a bare columned
/// table (§5.2), on a function type (§5.0), and on `unit` (§5.3).
///
/// Note the "inner" a `?` wraps is the *base* type — the parser attaches
/// the `?` to the innermost base. `(int) -> string?` is a function whose
/// return type is `string?`; `[int?]` is an array whose element type is
/// `int?`; `[int]?` is a nullable array. The parser has already grouped
/// the `?` with the right base by the time this function sees the node.
static TypeAST* resolveNullableType(NullableTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner || inner->isa<UnknownTypeAST>()) {
        return ctx.getUnknownType();
    }

    // ─── Redundant on a row reference: accept as-is ────────────────────
    // `&T?` is `&T`. Return the row-ref type directly, not a wrapped
    // nullable, so downstream code never has to special-case it.
    if (isRowRefType(inner)) {
        return inner;
    }

    // ─── Error on a function type ──────────────────────────────────────
    if (isFunctionType(inner)) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, type,
                              "function types cannot be nullable — a "
                              "function value is always a valid code address");
        return ctx.getUnknownType();
    }

    // ─── Error on a bare columned table ────────────────────────────────
    // A table is a global; "the table is nil" is meaningless.
    if (isColumnedTableType(inner, ctx)) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, type,
                              "a bare table type cannot be nullable — "
                              "'", typeToString(inner, ctx.pool),
                              "' names a global sheet, not a value");
        ctx.diagnostics.note(type,
                             "Use '&T' if you mean a reference to one row.");
        return ctx.getUnknownType();
    }

    // ─── Error on unit ─────────────────────────────────────────────────
    if (isUnitType(inner)) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, type,
                              "'unit?' is redundant — 'unit' already means "
                              "'no value'");
        return ctx.getUnknownType();
    }

    type->inner = inner;

    NullableTypeAST* canonical = ctx.getNullableType(inner);
    canonical->loc = type->loc;
    return canonical;
}

// ─────────────────────────────────────────────────────────────────────────────
// Function — (T, U) -> R
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve a function type.
///
/// The parameter types and return type are resolved recursively. The
/// return type may itself be nilable (`(int) -> string?`) — that is a
/// function whose return is a nullable string, not a nullable function,
/// and it is legal. The parameter types are unnamed: the AST stores a
/// span of `TypeAST*`, not a span of `ParamAST*` (see `FunctionTypeAST`).
///
/// The return type is never `nullptr` in the grammar — every function
/// type has an explicit `-> R`, with `unit` when the function returns
/// nothing. So this function does not have to invent a default.
///
/// A `@sequence` function is not a function value and cannot appear in a
/// function type (§9.2.5). That rule is enforced at the use site, not
/// here — a `FunctionTypeAST` produced from a source `(T) -> R` never
/// names a sequence by construction.
static TypeAST* resolveFuncType(FunctionTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── Resolve each parameter type ───────────────────────────────────
    //
    // ArenaSpan is immutable, so a new span is built through the arena's
    // SpanBuilder rather than mutating `type->params` in place. The new
    // span replaces the old one; the parser's original buffer is left
    // untouched (and is reclaimed by the arena when the session ends).
    auto paramBuilder = ctx.arena.makeBuilder<TypeAST*>(type->params.size());
    for (TypeAST* p : type->params) {
        TypeAST* resolved = resolveType(p, ctx);
        if (!resolved || resolved->isa<UnknownTypeAST>()) {
            return ctx.getUnknownType();
        }
        paramBuilder.push_back(resolved);
    }
    ArenaSpan<TypeAST*> resolvedParams = paramBuilder.build();

    // ─── Resolve the return type ───────────────────────────────────────
    TypeAST* resolvedReturn = resolveType(type->returnType, ctx);
    if (!resolvedReturn || resolvedReturn->isa<UnknownTypeAST>()) {
        return ctx.getUnknownType();
    }

    // ─── Canonicalize ──────────────────────────────────────────────────
    FunctionTypeAST* canonical = ctx.getFunctionType(resolvedParams, resolvedReturn);
    canonical->loc = type->loc;
    return canonical;
}

} // namespace lucid::sema
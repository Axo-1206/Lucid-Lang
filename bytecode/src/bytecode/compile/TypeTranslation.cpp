/// @file compile/TypeTranslation.cpp
/// @brief Translate a Sema-resolved TypeAST into a TypeDescriptor.

#include "TypeTranslation.hpp"

#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"   // for NamedTypeAST::resolvedDecl
#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/memory/StringPool.hpp"

using namespace lucid::contract;

namespace lucid::bytecode::compile {

TypeDescriptor translateType(const TypeAST* type, StringPool& pool) {
    AST_ASSERT_MSG(type != nullptr,
        "translateType: null type — Sema should have resolved every "
        "type before the compiler runs");

    // ─── Primitive ─────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        const auto* prim = type->as<PrimitiveTypeAST>();
        return TypeDescriptor::makePrimitive(prim->primitiveKind);
    }

    // ─── Named ─────────────────────────────────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        const auto* named = type->as<NamedTypeAST>();
        AST_ASSERT_MSG(named->resolvedDecl != nullptr,
            "translateType: a NamedTypeAST has no resolvedDecl — "
            "Sema should have resolved every named type");

        const auto* decl = named->resolvedDecl;
        AST_ASSERT_MSG(decl->mangledName.isValid(),
            "translateType: a resolved named type has no mangled name — "
            "Sema's mangling pass should have run for every declaration");

        TypeDescriptor d;
        d.kind         = TypeDescriptor::Kind::Named;
        d.namedMangled = pool.lookup(decl->mangledName);

        // A host type is a table declared with `= host("...")`. A
        // table reference (an ordinary TABLE) is not a host type.
        // The distinction drives resource-plan classification: a
        // host type is Refcounted, an ordinary table is a reference.
        if (decl->isa<TableDeclAST>()) {
            const auto* table = decl->as<TableDeclAST>();
            d.isHostType = table->isHostBacked;
        }

        return d;
    }

    // ─── Array ─────────────────────────────────────────────────────────
    if (type->isa<ArrayTypeAST>()) {
        const auto* arr = type->as<ArrayTypeAST>();
        AST_ASSERT_MSG(arr->element != nullptr,
            "translateType: an array type has no element type — "
            "Sema should have resolved it");

        TypeDescriptor elem = translateType(arr->element, pool);
        return TypeDescriptor::makeArray(arr->arrayKind,
                                         arr->fixedSize,
                                         std::move(elem));
    }

    // ─── Row reference ─────────────────────────────────────────────────
    if (type->isa<RowRefTypeAST>()) {
        const auto* ref = type->as<RowRefTypeAST>();
        AST_ASSERT_MSG(ref->inner != nullptr,
            "translateType: a row-reference type has no inner type — "
            "Sema should have resolved it");

        // The inner type is a NamedTypeAST (the table). Translate it
        // and wrap.
        TypeDescriptor inner = translateType(ref->inner, pool);
        AST_ASSERT_MSG(inner.isNamed(),
            "translateType: a row-reference's inner type is not a named "
            "type — Sema should have rejected this");
        return TypeDescriptor::makeRowRef(std::move(inner));
    }

    // ─── Function ──────────────────────────────────────────────────────
    if (type->isa<FunctionTypeAST>()) {
        const auto* fn = type->as<FunctionTypeAST>();

        std::vector<TypeDescriptor> params;
        params.reserve(fn->params.size());
        for (const auto* p : fn->params) {
            params.push_back(translateType(p, pool));
        }

        AST_ASSERT_MSG(fn->returnType != nullptr,
            "translateType: a function type has no return type — "
            "Sema should have resolved it");
        TypeDescriptor ret = translateType(fn->returnType, pool);

        return TypeDescriptor::makeFunction(std::move(params),
                                            std::move(ret));
    }

    // ─── Nullable ──────────────────────────────────────────────────────
    if (type->isa<NullableTypeAST>()) {
        const auto* nullable = type->as<NullableTypeAST>();
        AST_ASSERT_MSG(nullable->inner != nullptr,
            "translateType: a nullable type has no inner type — "
            "Sema should have resolved it");
        TypeDescriptor inner = translateType(nullable->inner, pool);
        return TypeDescriptor::makeNullable(std::move(inner));
    }

    // ─── Unknown ───────────────────────────────────────────────────────
    // An UnknownTypeAST reaching the compiler is a Sema bug: Sema
    // rejects a module with an unresolved type before the compiler
    // runs. The translation produces Unknown so a downstream check
    // fires, rather than silently producing a wrong type.
    if (type->isa<UnknownTypeAST>()) {
        AST_ASSERT_MSG(false,
            "translateType: an UnknownTypeAST reached the compiler — "
            "Sema should have rejected the module before codegen");
        TypeDescriptor d;
        d.kind = TypeDescriptor::Kind::Unknown;
        return d;
    }

    // Any other TypeAST subclass is a compiler bug — every concrete
    // type node is handled above.
    AST_ASSERT_MSG(false,
        "translateType: unhandled TypeAST subclass — the compiler's "
        "type translation is out of sync with the AST");
    TypeDescriptor d;
    d.kind = TypeDescriptor::Kind::Unknown;
    return d;
}

} // namespace lucid::bytecode::compile
/// @file sema/context/Generic.hpp
/// @brief Generic instantiation and substitution utilities for Sema.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "core/memory/InternedString.hpp"
#include "sema/context/SemaContext.hpp"

#include <unordered_map>
#include <unordered_set>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// TypeTagRegistry
// ─────────────────────────────────────────────────────────────────────────────

struct TypeTagRegistry {
    std::unordered_map<TypeAST*, uint32_t> typeToTag;
    std::unordered_map<uint32_t, TypeAST*> tagToType;
    uint32_t nextTag = 1;

    uint32_t getTag(TypeAST* type) {
        auto it = typeToTag.find(type);
        if (it != typeToTag.end()) {
            return it->second;
        }
        uint32_t tag = nextTag++;
        typeToTag[type] = tag;
        tagToType[tag] = type;
        return tag;
    }

    TypeAST* getType(uint32_t tag) const {
        auto it = tagToType.find(tag);
        return it != tagToType.end() ? it->second : nullptr;
    }

    bool hasTag(TypeAST* type) const {
        return typeToTag.find(type) != typeToTag.end();
    }

    void clear() {
        typeToTag.clear();
        tagToType.clear();
        nextTag = 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// GenericSubstitution
// ─────────────────────────────────────────────────────────────────────────────

struct GenericSubstitution {
    const ArenaSpan<GenericParamDeclAST*>& genericParams;
    const ArenaSpan<TypeAST*>& typeArgs;

    GenericSubstitution(
        const ArenaSpan<GenericParamDeclAST*>& params,
        const ArenaSpan<TypeAST*>& args)
        : genericParams(params), typeArgs(args) {}

    TypeAST* lookup(InternedString name) const {
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (genericParams[i]->name == name && i < typeArgs.size()) {
                return typeArgs[i];
            }
        }
        return nullptr;
    }

    bool isParam(InternedString name) const {
        for (auto p : genericParams) {
            if (p->name == name) return true;
        }
        return false;
    }

    size_t paramCount() const { return genericParams.size(); }
    size_t argCount() const { return typeArgs.size(); }
    bool isComplete() const { return typeArgs.size() == genericParams.size(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Type Substitution Helpers (declarations only)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Substitute generic parameters in a type.
TypeAST* substituteType(TypeAST* type, const GenericSubstitution& subst, SemaContext& ctx);

/// @brief Substitute generic parameters in a statement.
StmtAST* substituteStmt(StmtAST* stmt, const GenericSubstitution& subst, SemaContext& ctx);

/// @brief Substitute generic parameters in an expression.
ExprAST* substituteExpr(ExprAST* expr, const GenericSubstitution& subst, SemaContext& ctx);

/// @brief Check if a type contains any generic parameters.
bool containsGenericParams(TypeAST* type, const GenericSubstitution& subst);

// ─────────────────────────────────────────────────────────────────────────────
// Specialized Struct/Function Creation (declarations only)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create a specialized struct declaration from a generic template.
StructDeclAST* createSpecializedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx);

/// @brief Create a specialized function declaration from a generic template.
FuncDeclAST* createSpecializedFunction(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx);

} // namespace sema
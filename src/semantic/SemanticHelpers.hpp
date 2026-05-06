/**
 * @file SemanticHelpers.hpp
 *
 * @nutshell Shared utility functions for semantic analysis passes (Phases 1-3).
 *
 * @reason Eliminates code duplication across SemanticDecl.cpp, SemanticStmt.cpp,
 *   and SemanticExpr.cpp by providing a single source of truth for primitive
 *   type singletons, type cloning, and function signature building.
 *
 * @responsibility Centralized helper functions that don't logically belong to
 *   TypeResolver but are needed across multiple semantic files.
 *
 * @usage Include this header in SemanticDecl.cpp, SemanticStmt.cpp, and
 *   SemanticExpr.cpp where shared functionality is needed.
 *
 * @design_principles
 *   1. All functions are inline to avoid linking issues across TU boundaries
 *   2. No dependencies on TypeResolver or other components that would create cycles
 *   3. Primitive type singletons are static locals for thread-safe lazy initialization
 *
 * @related SemanticDecl.cpp, SemanticStmt.cpp, SemanticExpr.cpp, TypeResolver.cpp
 */

#pragma once

#include "SemanticSymbol.hpp"
#include "SymbolTable.hpp"
#include "ast/TypeAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "debug/DebugMacros.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"

namespace SemanticHelpers {


inline void printTypeAST(const std::string& label, TypeAST* t, int indent = 0) {
    if (!t) {
        LUC_LOG_SEMANTIC_EXTREME(std::string(indent, ' ') << label << " = nullptr");
        return;
    }
    
    std::string indentStr(indent, ' ');
    
    switch (t->kind) {
        case ASTKind::PrimitiveType: {
            auto* p = t->as<PrimitiveTypeAST>();
            std::string typeName;
            switch (p->primitiveKind) {
                case PrimitiveKind::Bool:   typeName = "bool"; break;
                case PrimitiveKind::Int:    typeName = "int"; break;
                case PrimitiveKind::Float:  typeName = "float"; break;
                case PrimitiveKind::Double: typeName = "double"; break;
                case PrimitiveKind::String: typeName = "string"; break;
                case PrimitiveKind::Uint8:  typeName = "uint8"; break;
                case PrimitiveKind::Uint64: typeName = "uint64"; break;
                case PrimitiveKind::Any:    typeName = "any"; break;
                default: typeName = "other(" + std::to_string(static_cast<int>(p->primitiveKind)) + ")";
            }
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = PrimitiveType(" << typeName << ")");
            break;
        }
        case ASTKind::NamedType: {
            auto* n = t->as<NamedTypeAST>();
            std::string msg = indentStr + label + " = NamedType(" + n->name + ")";
            if (n->isGenericParam) msg += " [generic param]";
            LUC_LOG_SEMANTIC_EXTREME(msg);
            break;
        }
        case ASTKind::NullableType: {
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = NullableType");
            printTypeAST("  inner", t->as<NullableTypeAST>()->inner.get(), indent + 2);
            break;
        }
        case ASTKind::PtrType: {
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = PtrType");
            printTypeAST("  inner", t->as<PtrTypeAST>()->inner.get(), indent + 2);
            break;
        }
        case ASTKind::FuncType: {
            auto* f = t->as<FuncTypeAST>();
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = FuncType(nullable=" << f->isNullable << ")");
            break;
        }
        default:
            LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = " << LucDebug::kindToString(t->kind));
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Primitive Type Singletons
// ─────────────────────────────────────────────────────────────────────────────
//
// Returns a pointer to a static singleton PrimitiveTypeAST for the given kind.
// These singletons are safe to use across the entire semantic pass because:
//   1. PrimitiveTypeAST nodes are immutable after construction
//   2. The static storage duration ensures they live for the program's lifetime
//   3. Multiple returns of the same pointer allow pointer equality comparisons
//
// Usage:
//   TypeAST* intType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int);
//
// IMPORTANT: Do NOT delete or take ownership of the returned pointer.
// These singletons are owned by this function and must not be freed.
// ─────────────────────────────────────────────────────────────────────────────

inline PrimitiveTypeAST* getPrimitiveType(PrimitiveKind k) {
    // One singleton per kind, lazily constructed on first access.
    // Using function-local statics guarantees thread-safe initialization (C++11).
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
// Type Cloning — Deep copy a TypeAST node
// ─────────────────────────────────────────────────────────────────────────────
//
// Creates a deep copy of the given TypeAST node. All child nodes are recursively
// cloned. Required for:
//   - Building function signatures where the same type node appears in multiple places
//   - Generic instantiation where we need to create a concrete copy of a template
//   - Storing resolved types without aliasing the original AST nodes
//
// Ownership: Returns a unique_ptr that the caller must manage.
// The cloned nodes are completely independent of the original.
//
// IMPORTANT: Does NOT clone the resolvedType pointer (that's semantic state,
// not part of the type's syntactic structure). The cloned type will have
// resolvedType = nullptr.
// ─────────────────────────────────────────────────────────────────────────────

inline std::unique_ptr<TypeAST> cloneType(const TypeAST* type) {
    if (!type) return nullptr;

    // EXTREME level is appropriate here because cloneType is called frequently,
    // and EXTREME logging is disabled by default. Only enable when debugging
    // memory ownership issues.
    LUC_LOG_SEMANTIC_EXTREME("cloneType: kind=" << LucDebug::kindToString(type->kind));

    switch (type->kind) {
        case ASTKind::PrimitiveType: {
            auto* p = static_cast<const PrimitiveTypeAST*>(type);
            return std::make_unique<PrimitiveTypeAST>(p->primitiveKind);
        }

        case ASTKind::NamedType: {
            auto* n = static_cast<const NamedTypeAST*>(type);
            auto clone = std::make_unique<NamedTypeAST>(n->name);
            clone->isGenericParam = n->isGenericParam;
            for (auto& arg : n->genericArgs) {
                clone->genericArgs.push_back(cloneType(arg.get()));
            }
            return clone;
        }

        case ASTKind::NullableType: {
            auto* nl = static_cast<const NullableTypeAST*>(type);
            return std::make_unique<NullableTypeAST>(cloneType(nl->inner.get()));
        }

        case ASTKind::RefType: {
            auto* r = static_cast<const RefTypeAST*>(type);
            return std::make_unique<RefTypeAST>(cloneType(r->inner.get()));
        }

        case ASTKind::PtrType: {
            auto* p = static_cast<const PtrTypeAST*>(type);
            return std::make_unique<PtrTypeAST>(cloneType(p->inner.get()));
        }

        case ASTKind::FixedArrayType: {
            auto* a = static_cast<const FixedArrayTypeAST*>(type);
            return std::make_unique<FixedArrayTypeAST>(a->size, cloneType(a->element.get()));
        }

        case ASTKind::SliceType: {
            auto* s = static_cast<const SliceTypeAST*>(type);
            return std::make_unique<SliceTypeAST>(cloneType(s->element.get()));
        }

        case ASTKind::DynamicArrayType: {
            auto* d = static_cast<const DynamicArrayTypeAST*>(type);
            return std::make_unique<DynamicArrayTypeAST>(cloneType(d->element.get()));
        }

        case ASTKind::FuncType: {
            auto* f = static_cast<const FuncTypeAST*>(type);
            auto clone = std::make_unique<FuncTypeAST>(f->isNullable);
            clone->qualifiers = f->qualifiers;  // ADD THIS
            clone->returnType = cloneType(f->returnType.get());
            for (auto& p : f->params) {
                clone->params.push_back(cloneType(p.get()));
            }
            return clone;
        }

        default:
            LUC_LOG_SEMANTIC("cloneType: unhandled kind " << static_cast<int>(type->kind));
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Clone a TypeAST from a raw pointer (non-const overload)
// ─────────────────────────────────────────────────────────────────────────────
inline std::unique_ptr<TypeAST> cloneType(TypeAST* type) {
    return cloneType(static_cast<const TypeAST*>(type));
}

// ─────────────────────────────────────────────────────────────────────────────
// Build Function Signature — Creates a FuncTypeAST from a function declaration
// ─────────────────────────────────────────────────────────────────────────────
//
// This function builds the complete curried signature of a function by:
//   1. Starting with the innermost return type
//   2. Wrapping outward with each parameter group (from last group to first)
//
// For a non-curried function (single param group): (a int, b int) int
//   Returns: FuncTypeAST with params=[int, int], returnType=int
//
// For a curried function: (a int) (b int) int
//   Returns: FuncTypeAST with params=[int], returnType=FuncTypeAST([int], int)
//
// PRECONDITION: All parameter and return types have already been resolved
//   by the TypeResolver and have their resolvedType pointers set correctly.
//
// Ownership: Returns a unique_ptr that the caller should store in node.signature.
// ─────────────────────────────────────────────────────────────────────────────

inline std::unique_ptr<TypeAST> buildResolvedFunctionSignature(const FuncDeclAST& node, int32_t qualifiers = 0) {
    LUC_LOG_SEMANTIC_VERBOSE("buildResolvedFunctionSignature: " << node.name
                           << ", paramGroups=" << node.paramGroups.size());

    // Get the resolved return type
    TypeAST* returnType = nullptr;
    if (node.returnType) {
        returnType = node.returnType->resolvedType
                     ? static_cast<TypeAST*>(node.returnType->resolvedType)
                     : node.returnType.get();
    }

    // No parameter groups → signature is just the return type (or void)
    if (node.paramGroups.empty()) {
        if (returnType) {
            return cloneType(returnType);
        }
        return nullptr;
    }

    // Start with the innermost return type
    std::unique_ptr<TypeAST> curReturn = returnType ? cloneType(returnType) : nullptr;

    // Wrap from the LAST parameter group to the FIRST (builds curry chain)
    for (int i = static_cast<int>(node.paramGroups.size()) - 1; i >= 0; --i) {
        auto funcType = std::make_unique<FuncTypeAST>();
        funcType->isNullable = false;
        funcType->loc = node.loc;

        // Add all parameters from this group using their resolved types
        for (auto& param : node.paramGroups[i]) {
            TypeAST* paramType = param->type->resolvedType
                                 ? static_cast<TypeAST*>(param->type->resolvedType)
                                 : param->type.get();
            funcType->params.push_back(cloneType(paramType));
        }

        // Set the return type (previous layer of the curry chain)
        if (curReturn) {
            funcType->returnType = std::move(curReturn);
        }

        curReturn = std::move(funcType);
    }

    if (curReturn && curReturn->isa<FuncTypeAST>()) {
        curReturn->as<FuncTypeAST>()->qualifiers = qualifiers;
    }

    LUC_LOG_SEMANTIC_EXTREME("\tsignature built");
    return curReturn;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildResolvedMethodSignature  — Creates a FuncTypeAST from a resolved method
//
// Unlike buildResolvedFunctionSignature (which works on FuncDeclAST), this
// operates on MethodDeclAST nodes from impl blocks. The key difference is
// that methods do NOT have their own generic parameters — they inherit from
// the impl block's generic parameters (e.g., impl Scene<T> { drawAll() ... }).
//
// Signature building process:
//   1. Start with the innermost resolved return type
//   2. Wrap outward with each parameter group (from last group to first)
//   3. The resulting signature represents the method's type with ALL parameters,
//      NOT including the implicit 'self' parameter (which is handled separately
//      by the semantic pass when building mangled names).
//
// For a non-curried method: offset (dx float) (dy float) Point
//   Returns: FuncTypeAST { params=[float, float], returnType=NamedType("Point") }
//
// For a curried method: setTransform (x float) (y float) (z float) void
//   Returns: FuncTypeAST { params=[float], 
//              returnType=FuncTypeAST { params=[float], 
//                returnType=FuncTypeAST { params=[float], returnType=nullptr } } }
//
// PRECONDITION: All parameter and return types have already been resolved by
//   TypeResolver and have their resolvedType pointers set correctly.
//
// PRECONDITION: The caller (TypeResolver::visit(ImplDeclAST)) has already set
//   the genericParams_ context to the impl block's generic parameters, so any
//   generic type references (e.g., T in struct Scene<T>) resolve correctly.
//
// Ownership: Returns a unique_ptr that the caller should store in node.signature.
//   The caller takes ownership of the newly created FuncTypeAST tree.
// ─────────────────────────────────────────────────────────────────────────────
inline std::unique_ptr<TypeAST> buildResolvedMethodSignature(const MethodDeclAST& node, int32_t qualifiers = 0) {
    LUC_LOG_SEMANTIC_VERBOSE("buildResolvedMethodSignature: " << node.name
                           << ", paramGroups=" << node.paramGroups.size());

    // Get resolved return type
    TypeAST* returnType = nullptr;
    if (node.returnType) {
        returnType = node.returnType->resolvedType
                     ? static_cast<TypeAST*>(node.returnType->resolvedType)
                     : node.returnType.get();
    }

    if (node.paramGroups.empty()) {
        if (returnType) {
            return cloneType(returnType);
        }
        return nullptr;
    }

    // Start with return type as the innermost
    std::unique_ptr<TypeAST> curReturn = returnType ? cloneType(returnType) : nullptr;

    // Wrap from last parameter group to first
    for (int i = static_cast<int>(node.paramGroups.size()) - 1; i >= 0; --i) {
        auto funcType = std::make_unique<FuncTypeAST>();
        funcType->isNullable = false;
        funcType->loc = node.loc;

        for (auto& param : node.paramGroups[i]) {
            TypeAST* paramType = param->type->resolvedType
                                 ? static_cast<TypeAST*>(param->type->resolvedType)
                                 : param->type.get();
            funcType->params.push_back(cloneType(paramType));
        }

        if (curReturn) {
            funcType->returnType = std::move(curReturn);
        }

        curReturn = std::move(funcType);
    }

    if (curReturn && curReturn->isa<FuncTypeAST>()) {
        curReturn->as<FuncTypeAST>()->qualifiers = qualifiers;
    }

    return curReturn;
}

// ─────────────────────────────────────────────────────────────────────────────
// Resolve Expression Type Helper
// ─────────────────────────────────────────────────────────────────────────────
//
// Safely extracts the resolved type from an expression node, handling:
//   - Null pointers
//   - Missing resolvedType (returns nullptr)
//
// Usage:
//   TypeAST* t = SemanticHelpers::getExprType(exprNode);
//   if (!t) { /* error already reported or type inference needed */ }
// ─────────────────────────────────────────────────────────────────────────────

inline TypeAST* getExprType(const ExprAST* expr) {
    if (!expr) return nullptr;
    return static_cast<TypeAST*>(expr->resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// Check Type Compatibility and Report Error
// ─────────────────────────────────────────────────────────────────────────────
//
// Convenience wrapper around TypeChecker::isAssignable that also logs debug
// information and optionally reports a diagnostic error.
//
// Parameters:
//   from   - source type (being assigned from)
//   to     - target type (being assigned to)
//   loc    - source location for error reporting
//   dc     - diagnostic engine for error reporting
//   reportError - if true, emits a diagnostic on failure
//
// Returns: true if assignable, false otherwise
// ─────────────────────────────────────────────────────────────────────────────

inline bool checkAssignable(TypeAST* from, TypeAST* to,
                            const SourceLocation& loc,
                            DiagnosticEngine& dc,
                            bool reportError = true) {
    // VERBOSE level because this is called frequently during type checking
    LUC_LOG_SEMANTIC_VERBOSE("checkAssignable: from=" << (from ? LucDebug::kindToString(from->kind) : "null")
                           << ", to=" << (to ? LucDebug::kindToString(to->kind) : "null"));

    if (!from || !to) {
        if (reportError) {
            LUC_LOG_SEMANTIC("checkAssignable: null type");
            dc.error(DiagnosticCategory::Semantic, loc, DiagCode::E3002,
                     "type mismatch: cannot assign between incompatible types");
        }
        return false;
    }

    if (TypeChecker::isAssignable(from, to)) {
        return true;
    }

    if (reportError) {
        LUC_LOG_SEMANTIC("checkAssignable: type mismatch");
        dc.error(DiagnosticCategory::Semantic, loc, DiagCode::E3002,
                 "type mismatch: cannot assign from '" + 
                 LucDebug::kindToString(from->kind) + "' to '" +
                 LucDebug::kindToString(to->kind) + "'");
    }
    return false;
}

} // namespace SemanticHelpers
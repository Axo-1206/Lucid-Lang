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

// ─────────────────────────────────────────────────────────────────────────────
// Print utilities
// ─────────────────────────────────────────────────────────────────────────────

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

inline PrimitiveTypeAST* getPrimitiveType(PrimitiveKind k) {
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
// Type Cloning
// ─────────────────────────────────────────────────────────────────────────────

inline std::unique_ptr<TypeAST> cloneType(const TypeAST* type) {
    if (!type) return nullptr;

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
            clone->qualifiers = f->qualifiers;
            clone->returnType = cloneType(f->returnType.get());
            for (auto& group : f->paramGroups) {
                ParamGroup newGroup;
                for (auto& param : group) {
                    newGroup.emplace_back(param.name, cloneType(param.type.get()), 
                                          param.isVariadic, param.loc);
                }
                clone->paramGroups.push_back(std::move(newGroup));
            }
            return clone;
        }

        default:
            LUC_LOG_SEMANTIC("cloneType: unhandled kind " << static_cast<int>(type->kind));
            return nullptr;
    }
}

inline std::unique_ptr<TypeAST> cloneType(TypeAST* type) {
    return cloneType(static_cast<const TypeAST*>(type));
}

// ─────────────────────────────────────────────────────────────────────────────
// FUNCTION SIGNATURE HELPERS (Unified for all function-like nodes)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// resolveFunctionType
//
// Resolves all types inside a FuncTypeAST (parameter types and return type).
// Called during Phase 3 for function-like declarations.
// ─────────────────────────────────────────────────────────────────────────────
inline void resolveFunctionType(FuncTypeAST& type, TypeResolver& resolver) {
    // Resolve parameter types in all curry groups
    for (auto& group : type.paramGroups) {
        for (auto& param : group) {
            if (param.type) {
                resolver.resolveType(param.type.get());
            }
        }
    }
    
    // Resolve return type
    if (type.returnType) {
        resolver.resolveType(type.returnType.get());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// declareFunctionParameters
//
// Declares all parameters from a FuncTypeAST into the current symbol table scope.
// Used when entering a function body to make parameters available.
// ─────────────────────────────────────────────────────────────────────────────
inline void declareFunctionParameters(FuncTypeAST& type, SymbolTable& symbols,
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
// getFunctionReturnType
//
// Returns the resolved return type from a FuncTypeAST (nullptr = void).
// ─────────────────────────────────────────────────────────────────────────────
inline TypeAST* getFunctionReturnType(FuncTypeAST& type, TypeResolver& resolver) {
    if (type.returnType) {
        return resolver.resolveType(type.returnType.get());
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveFunctionTypeSignature (DEPRECATED - kept for backward compatibility)
// ─────────────────────────────────────────────────────────────────────────────

inline std::unique_ptr<TypeAST> buildResolvedFunctionSignature(const FuncDeclAST& node, uint32_t qualifiers = 0) {
    // DEPRECATED: Use resolveFunctionType on node.type instead
    // This is kept as a no-op wrapper for compatibility
    return nullptr;
}

inline std::unique_ptr<TypeAST> buildResolvedMethodSignature(const MethodDeclAST& node, uint32_t qualifiers = 0) {
    // DEPRECATED: Use resolveFunctionType on node.type instead
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Other Helpers
// ─────────────────────────────────────────────────────────────────────────────

inline TypeAST* getExprType(const ExprAST* expr) {
    if (!expr) return nullptr;
    return static_cast<TypeAST*>(expr->resolvedType);
}

inline bool checkAssignable(TypeAST* from, TypeAST* to,
                            const SourceLocation& loc,
                            DiagnosticEngine& dc,
                            bool reportError = true) {
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
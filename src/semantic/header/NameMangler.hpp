#pragma once

#include "ast/support/StringPool.hpp"
#include "ast/TypeAST.hpp"
#include <string>
#include <string_view>

namespace NameMangler {

    // Helper to get a stable string representation of a type for mangling
    inline std::string mangleType(TypeAST* type, StringPool& pool) {
        if (!type) return "unknown";
        switch (type->kind) {
            case ASTKind::PrimitiveType: {
                auto pt = static_cast<PrimitiveTypeAST*>(type);
                return "prim" + std::to_string(static_cast<int>(pt->primitiveKind));
            }
            case ASTKind::NamedType: {
                auto nt = static_cast<NamedTypeAST*>(type);
                std::string res = std::string(pool.lookup(nt->name));
                for (const auto& arg : nt->genericArgs) {
                    res += "_" + mangleType(arg.get(), pool);
                }
                return res;
            }
            case ASTKind::NullableType: {
                auto nt = static_cast<NullableTypeAST*>(type);
                return "opt_" + mangleType(nt->inner.get(), pool);
            }
            case ASTKind::FixedArrayType: {
                auto ft = static_cast<FixedArrayTypeAST*>(type);
                return "arr" + std::to_string(ft->size) + "_" + mangleType(ft->element.get(), pool);
            }
            case ASTKind::SliceType: {
                auto st = static_cast<SliceTypeAST*>(type);
                return "slice_" + mangleType(st->element.get(), pool);
            }
            case ASTKind::DynamicArrayType: {
                auto dt = static_cast<DynamicArrayTypeAST*>(type);
                return "dyn_" + mangleType(dt->element.get(), pool);
            }
            case ASTKind::RefType: {
                auto rt = static_cast<RefTypeAST*>(type);
                return "ref_" + mangleType(rt->inner.get(), pool);
            }
            case ASTKind::PtrType: {
                auto pt = static_cast<PtrTypeAST*>(type);
                return "ptr_" + mangleType(pt->inner.get(), pool);
            }
            case ASTKind::FuncType:
                return "func";
            default:
                return "type";
        }
    }

    // For methods: "StructName::methodName"
    inline std::string mangleMethod(std::string_view parent, std::string_view method) {
        return std::string(parent) + "::" + std::string(method);
    }

    // For enum variants: "EnumName::variantName"
    inline std::string mangleEnumVariant(std::string_view enumName, std::string_view variant) {
        return std::string(enumName) + "::" + std::string(variant);
    }

    // For from blocks: "TargetType::from::ParamType"
    inline std::string mangleFrom(std::string_view target, TypeAST* paramType, StringPool& pool) {
        return std::string(target) + "::from::" + mangleType(paramType, pool);
    }

    // Used by TypeChecker to prefix-search "TargetType::from::"
    inline std::string getFromPrefix(std::string_view target) {
        return std::string(target) + "::from::";
    }

} // namespace NameMangler

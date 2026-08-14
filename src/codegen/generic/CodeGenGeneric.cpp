/// @file CodeGenGeneric.cpp
/// @brief Implementation of generic instantiation.

#include "CodeGenGeneric.hpp"
#include "CodeGenType.hpp"
#include "CodeGenType.hpp"
#include "support/CodeGenAlloca.hpp"
#include "support/CodeGenPanic.hpp"
#include "support/MangledName.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/DeclAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// 1. Detection Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool isGenericFunction(FuncDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool isGenericStruct(StructDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool shouldSpecialize(DeclAST* decl) {
    if (!decl) return false;

    if (decl->isa<FuncDeclAST>()) {
        return decl->as<FuncDeclAST>()->shouldSpecialize;
    }
    if (decl->isa<StructDeclAST>()) {
        return decl->as<StructDeclAST>()->shouldSpecialize;
    }
    return false;
}

bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams) {
    for (GenericParamDeclAST* param : genericParams) {
        if (param->name == name) {
            return true;
        }
    }
    return false;
}

size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams) {
    for (size_t i = 0; i < genericParams.size(); ++i) {
        if (genericParams[i]->name == name) {
            return i;
        }
    }
    return SIZE_MAX;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Type Substitution
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* substituteGenericType(
    TypeAST* type,
    const ArenaSpan<GenericParamDeclAST*>& genericParams,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!type) return nullptr;
    (void)ctx;

    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        size_t index = findGenericParamIndex(named->name, genericParams);
        if (index != SIZE_MAX && index < typeArgs.size()) {
            return typeArgs[index];
        }
        return type;
    }

    if (type->isa<ArrayTypeAST>()) {
        ArrayTypeAST* arr = type->as<ArrayTypeAST>();
        TypeAST* substitutedElement = substituteGenericType(
            arr->element,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedElement != arr->element) {
            // Note: We return the original type. For full substitution,
            // use GenericSubstitution with getType() instead.
        }
        return type;
    }

    if (type->isa<NullableTypeAST>()) {
        NullableTypeAST* nullable = type->as<NullableTypeAST>();
        TypeAST* substitutedInner = substituteGenericType(
            nullable->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != nullable->inner) {
            // Note: We return the original type. For full substitution,
            // use GenericSubstitution with getType() instead.
        }
        return type;
    }

    if (type->isa<FallibleTypeAST>()) {
        FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
        TypeAST* substitutedInner = substituteGenericType(
            fallible->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != fallible->inner) {
            // Note: We return the original type. For full substitution,
            // use GenericSubstitution with getType() instead.
        }
        return type;
    }

    if (type->isa<CombinedTypeAST>()) {
        CombinedTypeAST* combined = type->as<CombinedTypeAST>();
        TypeAST* substitutedInner = substituteGenericType(
            combined->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != combined->inner) {
            // Note: We return the original type. For full substitution,
            // use GenericSubstitution with getType() instead.
        }
        return type;
    }

    if (type->isa<PtrTypeAST>()) {
        PtrTypeAST* ptr = type->as<PtrTypeAST>();
        TypeAST* substitutedInner = substituteGenericType(
            ptr->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != ptr->inner) {
            // Note: We return the original type. For full substitution,
            // use GenericSubstitution with getType() instead.
        }
        return type;
    }

    if (type->isa<RefTypeAST>()) {
        RefTypeAST* ref = type->as<RefTypeAST>();
        TypeAST* substitutedInner = substituteGenericType(
            ref->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != ref->inner) {
            // Note: We return the original type. For full substitution,
            // use GenericSubstitution with getType() instead.
        }
        return type;
    }

    if (type->isa<FuncTypeAST>()) {
        // Function types are handled by getType() with GenericSubstitution
        return type;
    }

    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Specialized Instantiation Creation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* createSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;

    // ─── Generate mangled name for this instantiation ──────────────────────
    InternedString mangledName = generateMangledNameForGeneric(
        funcDecl,
        typeArgs,
        ctx
    );
    
    if (!mangledName.isValid()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, funcDecl->loc,
                                "failed to generate mangled name for generic function '",
                                ctx.pool.lookup(funcDecl->name), "'");
        return nullptr;
    }
    
    std::string funcName = ctx.pool.lookup(mangledName);

    // ─── Build parameter types ──────────────────────────────────────────────
    GenericSubstitution subst{funcDecl->genericParams, typeArgs};
    std::vector<llvm::Type*> paramTypes;

    if (funcDecl->hasClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    FuncTypeAST* funcType = funcDecl->funcType;
    while (funcType) {
        for (ParamAST* param : funcType->params) {
            llvm::Type* paramType = getType(ctx, param->type, &subst);
            if (!paramType) {
                ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
                                        "parameter '", ctx.pool.lookup(param->name),
                                        "' has invalid type in specialization");
                return nullptr;
            }
            paramTypes.push_back(paramType);
        }
        funcType = funcType->getNext();
    }

    // ─── Build return type ──────────────────────────────────────────────────
    llvm::Type* returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    if (funcDecl->funcType->returnType) {
        returnType = getType(ctx, funcDecl->funcType->returnType, &subst);
        if (!returnType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidReturnType, funcDecl->loc,
                                    "invalid return type in specialization");
            return nullptr;
        }
    }

    llvm::FunctionType* llvmFuncType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    // ─── Check if already exists ──────────────────────────────────────────
    llvm::Function* existingFunc = ctx.module->getFunction(funcName);
    if (existingFunc) {
        return existingFunc;
    }

    // ─── Create the function with the mangled name ─────────────────────────
    llvm::Function* func = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::InternalLinkage,
        funcName,
        ctx.module
    );

    // ─── Set parameter names ──────────────────────────────────────────────
    size_t paramIndex = 0;
    if (funcDecl->hasClosure) {
        func->getArg(paramIndex++)->setName("env");
    }

    FuncTypeAST* paramTypeIter = funcDecl->funcType;
    while (paramTypeIter) {
        for (ParamAST* param : paramTypeIter->params) {
            if (paramIndex < func->arg_size()) {
                func->getArg(paramIndex)->setName(ctx.pool.lookup(param->name));
                paramIndex++;
            }
        }
        paramTypeIter = paramTypeIter->getNext();
    }

    LOG_CODEGEN("Created specialized function: ", funcName,
                " (", paramTypes.size(), " params)");

    return func;
}

llvm::Type* createSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;

    // ─── Generate mangled name for this instantiation ──────────────────────
    InternedString mangledName = generateMangledNameForGeneric(
        structDecl,
        typeArgs,
        ctx
    );
    
    if (!mangledName.isValid()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, structDecl->loc,
                                "failed to generate mangled name for generic struct '",
                                ctx.pool.lookup(structDecl->name), "'");
        return nullptr;
    }
    
    std::string structName = ctx.pool.lookup(mangledName);

    // ─── Build field types with substituted types ──────────────────────────
    GenericSubstitution subst{structDecl->genericParams, typeArgs};
    std::vector<llvm::Type*> fieldTypes;

    for (FieldDeclAST* field : structDecl->fields) {
        llvm::Type* fieldType = getType(ctx, field->type, &subst);
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, field->loc,
                                    "field '", ctx.pool.lookup(field->name),
                                    "' has invalid type in specialization");
            return nullptr;
        }
        fieldTypes.push_back(fieldType);
    }

    // ─── Check if already exists ────────────────────────────────────────────
    llvm::StructType* existingType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        structName
    );
    if (existingType && !existingType->isOpaque()) {
        return existingType;
    }

    // ─── Create the struct type with the mangled name ──────────────────────
    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        structName
    );

    LOG_CODEGEN("Created specialized struct: ", structName,
                " (", fieldTypes.size(), " fields)");

    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Type-Erased Generic Generation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;

    std::string funcName = ctx.pool.lookup(funcDecl->name);
    std::string mangledName = funcName + "__erased";

    std::vector<llvm::Type*> paramTypes;

    if (funcDecl->hasClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    FuncTypeAST* funcType = funcDecl->funcType;
    while (funcType) {
        for (size_t i = 0; i < funcType->params.size(); ++i) {
            paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
        }
        funcType = funcType->getNext();
    }

    llvm::Type* returnType = llvm::PointerType::get(ctx.llvmCtx, 0);

    llvm::FunctionType* llvmFuncType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    llvm::Function* existingFunc = ctx.module->getFunction(mangledName);
    if (existingFunc) {
        return existingFunc;
    }

    llvm::Function* func = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        mangledName,
        ctx.module
    );

    size_t paramIndex = 0;
    if (funcDecl->hasClosure) {
        func->getArg(paramIndex++)->setName("env");
    }

    FuncTypeAST* paramTypeIter = funcDecl->funcType;
    while (paramTypeIter) {
        for (ParamAST* param : paramTypeIter->params) {
            if (paramIndex < func->arg_size()) {
                std::string paramName = ctx.pool.lookup(param->name);
                func->getArg(paramIndex)->setName(paramName + "_tagged");
                paramIndex++;
            }
        }
        paramTypeIter = paramTypeIter->getNext();
    }

    LOG_CODEGEN("Created type-erased generic function: ", mangledName,
                " (", paramTypes.size(), " params)");

    return func;
}

llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;

    std::string structName = ctx.pool.lookup(structDecl->name);
    std::string mangledName = structName + "__erased";

    // ─── Get or create the canonical TaggedSlot type ──────────────────────
    // This is a SHARED type across all erased generic structs.
    // DO NOT recreate it per struct - that creates distinct types
    // that are structurally identical but nominally different.
    static const char* slotName = "TaggedSlot";
    llvm::StructType* slotType = llvm::StructType::getTypeByName(ctx.llvmCtx, slotName);
    if (!slotType) {
        std::vector<llvm::Type*> slotFields = {
            llvm::Type::getInt8Ty(ctx.llvmCtx),              // tag (0 = valid, 1 = nil, 2 = err)
            llvm::PointerType::get(ctx.llvmCtx, 0)          // value (opaque pointer)
        };
        slotType = llvm::StructType::create(ctx.llvmCtx, slotFields, slotName);
    }

    // ─── Check if this erased struct already exists ──────────────────────
    llvm::StructType* existingType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        mangledName
    );
    if (existingType && !existingType->isOpaque()) {
        return existingType;
    }

    // ─── Build field types using the shared TaggedSlot ───────────────────
    std::vector<llvm::Type*> fieldTypes;
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        fieldTypes.push_back(slotType);
    }

    // ─── Create the erased struct type ───────────────────────────────────
    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        mangledName
    );

    LOG_CODEGEN("Created type-erased generic struct: ", mangledName,
                " (", fieldTypes.size(), " fields)");

    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Public Registry API
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl || !isGenericFunction(funcDecl)) return nullptr;

    if (!shouldSpecialize(funcDecl)) {
        return generateErasedGenericFunction(funcDecl, ctx);
    }

    GenericInstantiationKey key{funcDecl, typeArgs};

    auto funcIt = ctx.genericRegistry.functionInstantiations.find(funcDecl);
    if (funcIt != ctx.genericRegistry.functionInstantiations.end()) {
        auto typeIt = funcIt->second.find(key);
        if (typeIt != funcIt->second.end()) {
            return typeIt->second;
        }
    }

    llvm::Function* specialized = createSpecializedFunction(funcDecl, typeArgs, ctx);
    if (!specialized) return nullptr;

    ctx.genericRegistry.functionInstantiations[funcDecl][key] = specialized;

    return specialized;
}

llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl || !isGenericStruct(structDecl)) return nullptr;

    if (!shouldSpecialize(structDecl)) {
        return generateErasedGenericStruct(structDecl, ctx);
    }

    GenericInstantiationKey key{structDecl, typeArgs};

    auto structIt = ctx.genericRegistry.structInstantiations.find(structDecl);
    if (structIt != ctx.genericRegistry.structInstantiations.end()) {
        auto typeIt = structIt->second.find(key);
        if (typeIt != structIt->second.end()) {
            return typeIt->second;
        }
    }

    llvm::Type* specialized = createSpecializedStruct(structDecl, typeArgs, ctx);
    if (!specialized) return nullptr;

    ctx.genericRegistry.structInstantiations[structDecl][key] = specialized;

    return specialized;
}

} // namespace codegen
/// @file CodeGenGeneric.cpp
/// @brief Implementation of generic instantiation.
///
/// This file implements the generic instantiation pipeline:
///   1. Detection: isGenericFunction, isGenericStruct, shouldSpecialize
///   2. Specialized creation: createSpecializedFunction, createSpecializedStruct
///   3. Type-erased generation: generateErasedGenericFunction, generateErasedGenericStruct
///   4. Registry access: getOrCreateSpecializedFunction, getOrCreateSpecializedStruct

#include "CodeGenGeneric.hpp"
#include "../types/CodeGenType.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
#include "GenericMangledName.hpp"
#include "core/trace/Trace.hpp"
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

bool isGenericParameterName(
    InternedString name,
    const ArenaSpan<GenericParamDeclAST*>& genericParams
) {
    for (const GenericParamDeclAST* param : genericParams) {
        if (param->name == name) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Helper: Build Type Argument Vector for LLVM Types
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Build a vector of LLVM types from type arguments with substitution.
/// @param ctx The code generation context.
/// @param typeArgs The type arguments (from ArenaSpan).
/// @param subst The substitution context.
/// @return A vector of LLVM types, or empty on error.
static std::vector<llvm::Type*> buildParamTypes(
    CodeGenContext& ctx,
    const ArenaSpan<TypeAST*>& typeArgs,
    const GenericSubstitution& subst
) {
    std::vector<llvm::Type*> paramTypes;
    paramTypes.reserve(typeArgs.size());

    for (size_t i = 0; i < typeArgs.size(); ++i) {
        llvm::Type* paramType = getType(ctx, typeArgs[i], &subst);
        if (!paramType) {
            return {};
        }
        paramTypes.push_back(paramType);
    }

    return paramTypes;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Specialized Instantiation Creation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* createSpecializedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;

    // ─── Validate arity ──────────────────────────────────────────────────
    if (typeArgs.size() != funcDecl->genericParams.size()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, funcDecl->loc,
            "generic function '", ctx.pool.lookup(funcDecl->name),
            "' expected ", funcDecl->genericParams.size(),
            " type arguments, got ", typeArgs.size());
        return nullptr;
    }

    // ─── Generate mangled name for this instantiation ──────────────────────
    InternedString mangledName = generateMangledNameForGeneric(funcDecl, typeArgs, ctx);
    
    if (!mangledName.isValid()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, funcDecl->loc,
            "failed to generate mangled name for generic function '",
            ctx.pool.lookup(funcDecl->name), "'");
        return nullptr;
    }
    
    std::string funcName = ctx.pool.lookup(mangledName);

    // ─── Build parameter types with substitution ──────────────────────────
    GenericSubstitution subst{funcDecl->genericParams, typeArgs};
    std::vector<llvm::Type*> paramTypes;

    // Add closure environment pointer if needed
    if (funcDecl->hasClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    // Build parameter types from function signature
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

    Trace::detail("Created specialized function: ", funcName,
                " (", paramTypes.size(), " params)");

    return func;
}

llvm::Type* createSpecializedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;

    // ─── Validate arity ──────────────────────────────────────────────────
    if (typeArgs.size() != structDecl->genericParams.size()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, structDecl->loc,
            "generic struct '", ctx.pool.lookup(structDecl->name),
            "' expected ", structDecl->genericParams.size(),
            " type arguments, got ", typeArgs.size());
        return nullptr;
    }

    // ─── Generate mangled name for this instantiation ──────────────────────
    InternedString mangledName = generateMangledNameForGeneric(structDecl, typeArgs, ctx);
    
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
    
    if (existingType) {
        if (!existingType->isOpaque()) {
            // ─── Already fully defined: cache and return ────────────────────
            ctx.cacheStruct(structDecl, existingType);
            structDecl->llvmType = existingType;
            structDecl->mangledName = mangledName;
            return existingType;
        }
        
        // ─── Forward-declared but not defined: complete it ────────────────
        // A struct with this exact mangled name was already forward-declared
        // somewhere (e.g. a recursive generic struct, or another module's
        // reference via getModuleTypeAccess()) but never given a body.
        // Completing the existing type in place keeps a single canonical type
        // for this name.
        existingType->setBody(fieldTypes);
        
        // ─── Cache the completed type ──────────────────────────────────────
        ctx.cacheStruct(structDecl, existingType);
        structDecl->llvmType = existingType;
        structDecl->mangledName = mangledName;
        
        Trace::detail("Completed forward-declared specialized struct: ", structName,
                    " (", fieldTypes.size(), " fields)");
        return existingType;
    }

    // ─── Create the struct type with the mangled name ──────────────────────
    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        structName
    );

    // ─── Cache the struct type ─────────────────────────────────────────────
    // This is CRITICAL: without this, ctx.lookupStruct() will fail
    ctx.cacheStruct(structDecl, structType);
    structDecl->llvmType = structType;
    structDecl->mangledName = mangledName;

    Trace::detail("Created specialized struct: ", structName,
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
    // Module-qualify to avoid collisions across modules
    std::string mangledName = getMangledModulePath(ctx) + "_" + funcName + "__erased";

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

    Trace::detail("Created type-erased generic function: ", mangledName,
                " (", paramTypes.size(), " params)");

    return func;
}

llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;

    std::string structName = ctx.pool.lookup(structDecl->name);
    std::string mangledName = getMangledModulePath(ctx) + "_" + structName + "__erased";

    // ─── Get or create the canonical TaggedSlot type ──────────────────────
    static const char* slotName = "TaggedSlot";
    llvm::StructType* slotType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        slotName
    );
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
    fieldTypes.reserve(structDecl->fields.size());
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        fieldTypes.push_back(slotType);
    }

    // ─── Create the erased struct type ───────────────────────────────────
    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        mangledName
    );

    Trace::detail("Created type-erased generic struct: ", mangledName,
                " (", fieldTypes.size(), " fields)");

    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Public Registry API
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl || !isGenericFunction(funcDecl)) {
        // Non-generic function - just return the regular function
        return ctx.lookupFunction(funcDecl);
    }

    // ─── Type-erased path (default) ──────────────────────────────────────
    if (!shouldSpecialize(funcDecl)) {
        return generateErasedGenericFunction(funcDecl, ctx);
    }

    // ─── Specialized path (@[specialize]) ──────────────────────────────
    GenericInstantiationKey key{funcDecl, typeArgs};

    // Check cache
    auto funcIt = ctx.genericRegistry.functionInstantiations.find(funcDecl);
    if (funcIt != ctx.genericRegistry.functionInstantiations.end()) {
        auto typeIt = funcIt->second.find(key);
        if (typeIt != funcIt->second.end()) {
            return typeIt->second;
        }
    }

    // Create new specialization
    llvm::Function* specialized = createSpecializedFunction(funcDecl, typeArgs, ctx);
    if (!specialized) return nullptr;

    // Cache it
    ctx.genericRegistry.functionInstantiations[funcDecl][key] = specialized;

    // Record for reflection
    recordGenericInstantiation(funcDecl, typeArgs, ctx);

    return specialized;
}

llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl || !isGenericStruct(structDecl)) {
        // Non-generic struct - just return the regular struct type
        return ctx.lookupStruct(structDecl);
    }

    // ─── Type-erased path (default) ──────────────────────────────────────
    if (!shouldSpecialize(structDecl)) {
        return generateErasedGenericStruct(structDecl, ctx);
    }

    // ─── Specialized path (@[specialize]) ──────────────────────────────
    GenericInstantiationKey key{structDecl, typeArgs};

    // Check cache
    auto structIt = ctx.genericRegistry.structInstantiations.find(structDecl);
    if (structIt != ctx.genericRegistry.structInstantiations.end()) {
        auto typeIt = structIt->second.find(key);
        if (typeIt != structIt->second.end()) {
            return typeIt->second;
        }
    }
    
    // ─── Also check ctx.structCache ──────────────────────────────────
    // The type might have been created but not stored in the registry yet
    // (e.g., through forward declaration completion).
    llvm::StructType* cached = ctx.lookupStruct(structDecl);
    if (cached && !cached->isOpaque()) {
        // Store in registry for future lookups
        ctx.genericRegistry.structInstantiations[structDecl][key] = cached;
        return cached;
    }

    // Create new specialization
    llvm::Type* specialized = createSpecializedStruct(structDecl, typeArgs, ctx);
    if (!specialized) return nullptr;

    // Cache it in registry
    ctx.genericRegistry.structInstantiations[structDecl][key] = specialized;

    return specialized;
}

llvm::Value* resolveGenericCall(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& genericArgs,
    CodeGenContext& ctx,
    const SourceLocation& loc
) {
    if (!funcDecl) return nullptr;

    // ─── Non-generic: return regular function ─────────────────────────────
    if (!isGenericFunction(funcDecl)) {
        if (!genericArgs.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, loc,
                "function '", ctx.pool.lookup(funcDecl->name), 
                "' is not generic but has generic arguments");
            return nullptr;
        }
        return ctx.lookupFunction(funcDecl);
    }

    // ─── Generic: validate arity ──────────────────────────────────────────
    if (genericArgs.size() != funcDecl->genericParams.size()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, loc,
            "generic function '", ctx.pool.lookup(funcDecl->name), 
            "' expected ", funcDecl->genericParams.size(), 
            " type arguments, got ", genericArgs.size());
        return nullptr;
    }

    // ─── Get or create specialized/erased function ──────────────────────
    return getOrCreateSpecializedFunction(funcDecl, genericArgs, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Reflection Support
// ─────────────────────────────────────────────────────────────────────────────

void recordGenericInstantiation(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    // This is where we'd store instantiations for reflection.
    // For now, this is a placeholder.
    // 
    // TODO: Store typeArgs in a list on FuncDeclAST for #sizeof/#alignof/#tostr.
    // 
    // Example implementation:
    // if (!funcDecl->instantiationList) {
    //     funcDecl->instantiationList = new std::vector<ArenaSpan<TypeAST*>>();
    // }
    // funcDecl->instantiationList->push_back(typeArgs);

    // For now, just trace it
    Trace::detail("Recorded generic instantiation: ",
                ctx.pool.lookup(funcDecl->name),
                " with ", typeArgs.size(), " type arguments");
}

std::vector<ArenaSpan<TypeAST*>> getRecordedInstantiations(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
) {
    // This would return the recorded instantiations.
    // For now, return empty.
    // 
    // TODO: Implement when FuncDeclAST has instantiationList.
    (void)funcDecl;
    (void)ctx;
    return {};
}

} // namespace codegen
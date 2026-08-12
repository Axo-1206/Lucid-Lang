/// @file CodeGenGeneric.cpp
/// @brief Implementation of generic instantiation.

#include "CodeGenGeneric.hpp"
#include "CodeGenType.hpp"
#include "support/CodeGenHelpers.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/DeclAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>

namespace codegen {

// ─── GenericInstantiationKey Implementation ──────────────────────────────

bool GenericInstantiationKey::operator==(const GenericInstantiationKey& other) const {
    if (decl != other.decl) return false;
    if (typeArgs.size() != other.typeArgs.size()) return false;
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (typeArgs[i] != other.typeArgs[i]) return false;
    }
    return true;
}

size_t GenericInstantiationKeyHash::operator()(const GenericInstantiationKey& key) const {
    size_t h = std::hash<const DeclAST*>{}(key.decl);
    for (const TypeAST* arg : key.typeArgs) {
        h = h ^ (std::hash<const TypeAST*>{}(arg) << 1);
    }
    return h;
}

// ─── Helper Functions ──────────────────────────────────────────────────────

bool isGenericFunction(const FuncDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool isGenericStruct(const StructDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool shouldSpecialize(const DeclAST* decl) {
    if (!decl) return false;
    
    if (decl->isa<FuncDeclAST>()) {
        return decl->as<FuncDeclAST>()->shouldSpecialize;
    }
    if (decl->isa<StructDeclAST>()) {
        return decl->as<StructDeclAST>()->shouldSpecialize;
    }
    return false;
}

bool isSpecializableType(const TypeAST* type, CodeGenContext& ctx) {
    if (!type) return false;
    
    // ─── Primitive types are always specializable ────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        return true;
    }
    
    // ─── Fixed arrays of primitives are specializable ────────────────────
    if (type->isa<ArrayTypeAST>()) {
        const ArrayTypeAST* arr = type->as<ArrayTypeAST>();
        if (arr->isFixed()) {
            return isSpecializableType(arr->element, ctx);
        }
        return false;
    }
    
    // ─── Small structs of primitives are specializable ──────────────────
    if (type->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = type->as<NamedTypeAST>();
        // Look up the struct declaration
        // Note: We need a way to look up type declarations from the context
        // For now, we'll check if it's a generic parameter (not specializable)
        // This will be properly handled when we have a full type resolver
        
        // If it's a generic parameter, it's not a concrete type to specialize
        // We'll handle this at instantiation time
        return false;
    }
    
    return false;
}

bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams) {
    for (const GenericParamDeclPtr param : genericParams) {
        if (param->name == name) {
            return true;
        }
    }
    return false;
}

size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams) {
    for (size_t i = 0; i < genericParams.size(); ++i) {
        if (genericParams[i]->name == name) {
            return i;
        }
    }
    return SIZE_MAX;
}

const TypeAST* substituteGenericType(
    const TypeAST* type,
    const ArenaSpan<GenericParamDeclPtr>& genericParams,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!type) return nullptr;
    (void)ctx;  // Used in future for more complex substitution
    
    // ─── Handle NamedType (generic parameter) ────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = type->as<NamedTypeAST>();
        
        // Check if this name matches any generic parameter
        size_t index = findGenericParamIndex(named->name, genericParams);
        if (index != SIZE_MAX && index < typeArgs.size()) {
            // Found a match - substitute with the concrete type
            return typeArgs[index];
        }
        
        // Not a generic parameter - return the original type
        return type;
    }
    
    // ─── Handle ArrayType (need to substitute element type) ──────────────
    if (type->isa<ArrayTypeAST>()) {
        const ArrayTypeAST* arr = type->as<ArrayTypeAST>();
        const TypeAST* substitutedElement = substituteGenericType(
            arr->element,
            genericParams,
            typeArgs,
            ctx
        );
        
        if (substitutedElement != arr->element) {
            // Element type changed - create new ArrayType
            // Note: This is a simplification - we'd need arena allocation
            // For now, we return the original type
            // TODO: Use arena to create new type nodes
        }
        return type;
    }
    
    // ─── Handle Nullable/Fallible types ──────────────────────────────────
    if (type->isa<NullableTypeAST>()) {
        const NullableTypeAST* nullable = type->as<NullableTypeAST>();
        const TypeAST* substitutedInner = substituteGenericType(
            nullable->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != nullable->inner) {
            // Need to create a new NullableTypeAST with substituted inner
            // TODO: Use arena to create new type nodes
        }
        return type;
    }
    
    if (type->isa<FallibleTypeAST>()) {
        const FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
        const TypeAST* substitutedInner = substituteGenericType(
            fallible->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != fallible->inner) {
            // Need to create a new FallibleTypeAST with substituted inner
            // TODO: Use arena to create new type nodes
        }
        return type;
    }
    
    // ─── For other types, return the original ────────────────────────────
    return type;
}

std::string mangleGenericName(
    const std::string& baseName,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    std::string mangled = baseName;
    mangled += "__specialized_";
    
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i > 0) mangled += "_";
        // Use the type's string representation
        // This is a placeholder - we need a proper type string function
        mangled += "T" + std::to_string(i);
    }
    
    return mangled;
}

// ─── Specialized Function Creation ────────────────────────────────────────

llvm::Function* createSpecializedFunction(
    const FuncDeclAST* funcDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;
    
    // ─── 1. Generate mangled name ────────────────────────────────────────
    std::string baseName = ctx.pool.lookup(funcDecl->name);
    std::string mangledName = mangleGenericName(baseName, typeArgs, ctx);
    
    // ─── 2. Build the specialized LLVM function type ────────────────────
    std::vector<llvm::Type*> paramTypes;
    
    // For closures, add environment pointer first
    if (funcDecl->hasClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }
    
    // Build parameter types from the substituted types
    const FuncTypeAST* funcType = funcDecl->funcType;
    while (funcType) {
        for (const ParamAST* param : funcType->params) {
            // Substitute generic parameters in the parameter type
            const TypeAST* substitutedType = substituteGenericType(
                param->type,
                funcDecl->genericParams,
                typeArgs,
                ctx
            );
            
            llvm::Type* paramType = getType(ctx, substitutedType);
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
    
    // Build return type
    llvm::Type* returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    if (funcDecl->funcType->returnType) {
        const TypeAST* substitutedReturn = substituteGenericType(
            funcDecl->funcType->returnType,
            funcDecl->genericParams,
            typeArgs,
            ctx
        );
        returnType = getType(ctx, substitutedReturn);
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
    
    // ─── 3. Check if function already exists ─────────────────────────────
    // First, check if there's already a function with this name
    llvm::Function* existingFunc = ctx.module->getFunction(mangledName);
    if (existingFunc) {
        return existingFunc;
    }
    
    // ─── 4. Create the LLVM function ────────────────────────────────────
    llvm::Function* func = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::InternalLinkage,
        mangledName,
        ctx.module
    );
    
    // ─── 5. Set parameter names ──────────────────────────────────────────
    size_t paramIndex = 0;
    if (funcDecl->hasClosure) {
        func->getArg(paramIndex++)->setName("env");
    }
    
    // Set names for regular parameters
    const FuncTypeAST* paramTypeIter = funcDecl->funcType;
    while (paramTypeIter) {
        for (const ParamAST* param : paramTypeIter->params) {
            if (paramIndex < func->arg_size()) {
                func->getArg(paramIndex)->setName(ctx.pool.lookup(param->name));
                paramIndex++;
            }
        }
        paramTypeIter = paramTypeIter->getNext();
    }
    
    LOG_CODEGEN("Created specialized function: ", mangledName,
                " (", paramTypes.size(), " params)");
    
    return func;
}

llvm::Type* createSpecializedStruct(
    const StructDeclAST* structDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;
    
    // ─── 1. Generate mangled name ────────────────────────────────────────
    std::string baseName = ctx.pool.lookup(structDecl->name);
    std::string mangledName = mangleGenericName(baseName, typeArgs, ctx);
    
    // ─── 2. Build field types with substitutions ────────────────────────
    std::vector<llvm::Type*> fieldTypes;
    
    for (const FieldDeclAST* field : structDecl->fields) {
        // Substitute generic parameters in the field type
        const TypeAST* substitutedType = substituteGenericType(
            field->type,
            structDecl->genericParams,
            typeArgs,
            ctx
        );
        
        llvm::Type* fieldType = getType(ctx, substitutedType);
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, field->loc,
                                    "field '", ctx.pool.lookup(field->name),
                                    "' has invalid type in specialization");
            return nullptr;
        }
        fieldTypes.push_back(fieldType);
    }
    
    // ─── 3. Check if struct type already exists ──────────────────────────
    llvm::StructType* existingType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        mangledName
    );
    if (existingType && !existingType->isOpaque()) {
        return existingType;
    }
    
    // ─── 4. Create the specialized struct type ──────────────────────────
    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        mangledName
    );
    
    LOG_CODEGEN("Created specialized struct: ", mangledName,
                " (", fieldTypes.size(), " fields)");
    
    return structType;
}

// ─── Type-Erased Generic Generation ──────────────────────────────────────

llvm::Function* generateErasedGenericFunction(
    const FuncDeclAST* funcDecl,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;
    
    // ─── 1. Generate function name ──────────────────────────────────────
    std::string funcName = ctx.pool.lookup(funcDecl->name);
    std::string mangledName = funcName + "__erased";
    
    // ─── 2. Build type-erased function type ─────────────────────────────
    // All parameters become opaque pointers (i8*)
    // Return type becomes opaque pointer (i8*)
    std::vector<llvm::Type*> paramTypes;
    
    // For closures, add environment pointer first
    if (funcDecl->hasClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }
    
    // Count parameters
    const FuncTypeAST* funcType = funcDecl->funcType;
    while (funcType) {
        for (size_t i = 0; i < funcType->params.size(); ++i) {
            // All parameters are opaque pointers (tagged slots)
            paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
        }
        funcType = funcType->getNext();
    }
    
    // Return type is also an opaque pointer
    llvm::Type* returnType = llvm::PointerType::get(ctx.llvmCtx, 0);
    
    llvm::FunctionType* llvmFuncType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );
    
    // ─── 3. Check if function already exists ─────────────────────────────
    llvm::Function* existingFunc = ctx.module->getFunction(mangledName);
    if (existingFunc) {
        return existingFunc;
    }
    
    // ─── 4. Create the type-erased function ─────────────────────────────
    llvm::Function* func = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        mangledName,
        ctx.module
    );
    
    // ─── 5. Set parameter names ──────────────────────────────────────────
    size_t paramIndex = 0;
    if (funcDecl->hasClosure) {
        func->getArg(paramIndex++)->setName("env");
    }
    
    // Set names for regular parameters
    const FuncTypeAST* paramTypeIter = funcDecl->funcType;
    while (paramTypeIter) {
        for (const ParamAST* param : paramTypeIter->params) {
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
    const StructDeclAST* structDecl,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;
    
    // ─── 1. Generate struct name ─────────────────────────────────────────
    std::string structName = ctx.pool.lookup(structDecl->name);
    std::string mangledName = structName + "__erased";
    
    // ─── 2. Build type-erased struct ─────────────────────────────────────
    // All fields become tagged slots (opaque pointers)
    // A tagged slot is { i8 tag, i8* value }
    std::vector<llvm::Type*> fieldTypes;
    
    // Create the tagged slot type
    std::vector<llvm::Type*> slotFields = {
        llvm::Type::getInt8Ty(ctx.llvmCtx),  // tag
        llvm::PointerType::get(ctx.llvmCtx, 0)  // value
    };
    llvm::StructType* slotType = llvm::StructType::create(
        ctx.llvmCtx,
        slotFields,
        "TaggedSlot"
    );
    
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        fieldTypes.push_back(slotType);
    }
    
    // ─── 3. Check if struct already exists ──────────────────────────────
    llvm::StructType* existingType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        mangledName
    );
    if (existingType && !existingType->isOpaque()) {
        return existingType;
    }
    
    // ─── 4. Create the type-erased struct ───────────────────────────────
    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        mangledName
    );
    
    LOG_CODEGEN("Created type-erased generic struct: ", mangledName,
                " (", fieldTypes.size(), " fields)");
    
    return structType;
}

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Function* getOrCreateSpecializedFunction(
    const FuncDeclAST* funcDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl || !isGenericFunction(funcDecl)) return nullptr;
    
    // ─── If not specialized, use type erasure ────────────────────────────
    if (!shouldSpecialize(funcDecl)) {
        return generateErasedGenericFunction(funcDecl, ctx);
    }
    
    // ─── Check if we already have this instantiation ─────────────────────
    GenericInstantiationKey key{funcDecl, typeArgs};
    
    auto funcIt = ctx.genericRegistry.functionInstantiations.find(funcDecl);
    if (funcIt != ctx.genericRegistry.functionInstantiations.end()) {
        auto typeIt = funcIt->second.find(key);
        if (typeIt != funcIt->second.end()) {
            return typeIt->second;
        }
    }
    
    // ─── Create the specialized function ─────────────────────────────────
    llvm::Function* specialized = createSpecializedFunction(funcDecl, typeArgs, ctx);
    if (!specialized) return nullptr;
    
    // ─── Store for future use ─────────────────────────────────────────────
    ctx.genericRegistry.functionInstantiations[funcDecl][key] = specialized;
    
    return specialized;
}

llvm::Type* getOrCreateSpecializedStruct(
    const StructDeclAST* structDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl || !isGenericStruct(structDecl)) return nullptr;
    
    // ─── If not specialized, use type erasure ────────────────────────────
    if (!shouldSpecialize(structDecl)) {
        return generateErasedGenericStruct(structDecl, ctx);
    }
    
    // ─── Check if we already have this instantiation ─────────────────────
    GenericInstantiationKey key{structDecl, typeArgs};
    
    auto structIt = ctx.genericRegistry.structInstantiations.find(structDecl);
    if (structIt != ctx.genericRegistry.structInstantiations.end()) {
        auto typeIt = structIt->second.find(key);
        if (typeIt != structIt->second.end()) {
            return typeIt->second;
        }
    }
    
    // ─── Create the specialized struct ───────────────────────────────────
    llvm::Type* specialized = createSpecializedStruct(structDecl, typeArgs, ctx);
    if (!specialized) return nullptr;
    
    // ─── Store for future use ─────────────────────────────────────────────
    ctx.genericRegistry.structInstantiations[structDecl][key] = specialized;
    
    return specialized;
}

} // namespace codegen
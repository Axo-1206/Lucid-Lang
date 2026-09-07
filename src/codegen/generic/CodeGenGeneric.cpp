/// @file CodeGenGeneric.cpp
/// @brief Implementation of generic instantiation.
///
/// Sema now handles ALL specialization and erased name generation.
/// CodeGen's job is now:
///   1. Detect if a decl is generic (has genericParams)
///   2. If genericParams is empty: Sema already specialized it → lookup by mangled name
///   3. If genericParams is non-empty: type-erased path → use cached erasedName
///
/// The createSpecializedFunction/createSpecializedStruct functions have been
/// REMOVED because Sema already does this work.

#include "CodeGenGeneric.hpp"
#include "../types/CodeGenType.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
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

bool containsGenericParameter(TypeAST* type, const GenericSubstitution* subst) {
    if (!type || !subst) return false;
    
    switch (type->kind) {
        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            if (subst->isGenericParam(named->name)) return true;
            for (TypeAST* arg : named->genericArgs) {
                if (containsGenericParameter(arg, subst)) return true;
            }
            return false;
        }
        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            return containsGenericParameter(arr->element, subst);
        }
        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return containsGenericParameter(nullable->inner, subst);
        }
        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return containsGenericParameter(fallible->inner, subst);
        }
        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return containsGenericParameter(combined->inner, subst);
        }
        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            return containsGenericParameter(ref->inner, subst);
        }
        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return containsGenericParameter(ptr->inner, subst);
        }
        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            for (ParamAST* param : func->params) {
                if (containsGenericParameter(param->type, subst)) return true;
            }
            if (func->returnType && containsGenericParameter(func->returnType, subst)) {
                return true;
            }
            return false;
        }
        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            return containsGenericParameter(future->inner, subst);
        }
        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return containsGenericParameter(thread->inner, subst);
        }
        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            return containsGenericParameter(simd->elementType, subst);
        }
        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Type-Erased Generic Generation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;

    // ✅ Read the cached erased name from the AST (set by Sema)
    std::string mangledName = ctx.pool.lookup(funcDecl->erasedName);
    if (mangledName.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, funcDecl->loc,
            "erased function '", ctx.pool.lookup(funcDecl->name),
            "' has no erased name (Sema should have set this)");
        return nullptr;
    }

    std::vector<llvm::Type*> paramTypes;

    // Add closure environment pointer if needed
    if (funcDecl->hasClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    // All parameters are TaggedSlot* (opaque pointers)
    FuncTypeAST* funcType = funcDecl->funcType;
    while (funcType) {
        for (size_t i = 0; i < funcType->params.size(); ++i) {
            paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
        }
        funcType = funcType->getNext();
    }

    // Return type is TaggedSlot* (opaque pointer) or void
    llvm::Type* returnType = llvm::PointerType::get(ctx.llvmCtx, 0);
    if (!funcDecl->funcType->returnType) {
        returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    }

    llvm::FunctionType* llvmFuncType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    // Check if already exists
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

    // ✅ Read the cached erased name from the AST (set by Sema)
    std::string mangledName = ctx.pool.lookup(structDecl->erasedName);
    if (mangledName.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, structDecl->loc,
            "erased struct '", ctx.pool.lookup(structDecl->name),
            "' has no erased name (Sema should have set this)");
        return nullptr;
    }

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
// 3. Public Registry API (Simplified)
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;

    // ─── Non-generic: just lookup the regular function ─────────────────────
    if (!isGenericFunction(funcDecl)) {
        return ctx.lookupFunction(funcDecl);
    }

    // ─── Check if Sema already specialized this ────────────────────────────
    // Sema's resolveGenericInstantiation() creates specialized decls with
    // genericParams = {} and mangledName already set.
    if (funcDecl->genericParams.empty()) {
        // Sema already specialized this - just lookup by mangled name
        std::string mangledName = ctx.pool.lookup(funcDecl->mangledName);
        if (mangledName.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, funcDecl->loc,
                "specialized function '", ctx.pool.lookup(funcDecl->name),
                "' has no mangled name");
            return nullptr;
        }
        
        llvm::Function* func = ctx.module->getFunction(mangledName);
        if (!func) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, funcDecl->loc,
                "specialized function '", ctx.pool.lookup(funcDecl->name),
                "' not found in module");
            return nullptr;
        }
        
        return func;
    }

    // ─── Type-erased path (default) ──────────────────────────────────────
    // The function is still generic (genericParams not empty), so we use
    // the type-erased version.
    return generateErasedGenericFunction(funcDecl, ctx);
}

llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;

    // ─── Non-generic: just lookup the regular struct ──────────────────────
    if (!isGenericStruct(structDecl)) {
        return ctx.lookupStruct(structDecl);
    }

    // ─── Check if Sema already specialized this ────────────────────────────
    // Sema's resolveGenericInstantiation() creates specialized decls with
    // genericParams = {} and mangledName already set.
    if (structDecl->genericParams.empty()) {
        // Sema already specialized this - just lookup by mangled name
        std::string mangledName = ctx.pool.lookup(structDecl->mangledName);
        if (mangledName.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, structDecl->loc,
                "specialized struct '", ctx.pool.lookup(structDecl->name),
                "' has no mangled name");
            return nullptr;
        }
        
        llvm::StructType* structType = llvm::StructType::getTypeByName(
            ctx.llvmCtx,
            mangledName
        );
        
        if (!structType || structType->isOpaque()) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, structDecl->loc,
                "specialized struct '", ctx.pool.lookup(structDecl->name),
                "' not found in module");
            return nullptr;
        }
        
        // Cache it if not already cached
        ctx.cacheStruct(structDecl, structType);
        structDecl->llvmType = structType;
        return structType;
    }

    // ─── Type-erased path (default) ──────────────────────────────────────
    // The struct is still generic (genericParams not empty), so we use
    // the type-erased version.
    return generateErasedGenericStruct(structDecl, ctx);
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

} // namespace codegen
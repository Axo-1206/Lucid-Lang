/**
 * @file IRLoweringDecl.cpp
 * @brief Lowers Lucid declarations to LLVM IR.
 * 
 * @responsibility Handles all declaration nodes: functions, variables, structs,
 *                 enums, and foreign declarations.
 * 
 * @related_files
 *   - src/codegen/IRLowering.hpp - Declarations
 *   - src/codegen/IRLoweringStmt.cpp - Statement lowering (called from functions)
 *   - src/codegen/IRLoweringExpr.cpp - Expression lowering (used in variable init)
 *   - src/codegen/IRLoweringBuilder.cpp - Helper builders
 */

#include "IRLowering.hpp"
#include "IRLoweringBuilder.hpp"  // If split out, or use IRBuilderHelper

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"

#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// IRDeclLowering - Main Dispatcher
// ─────────────────────────────────────────────────────────────────────────────

void IRDeclLowering::lowerDecl(IRLowering& lowerer, DeclAST* decl) {
    if (!decl) {
        return;
    }

    switch (decl->kind) {
        case ASTKind::FuncDecl:
            lowerFuncDecl(lowerer, decl->as<FuncDeclAST>());
            break;
        case ASTKind::VarDecl:
            lowerVarDecl(lowerer, decl->as<VarDeclAST>());
            break;
        case ASTKind::StructDecl:
            lowerStructDecl(lowerer, decl->as<StructDeclAST>());
            break;
        case ASTKind::EnumDecl:
            lowerEnumDecl(lowerer, decl->as<EnumDeclAST>());
            break;
        default:
            // Ignore other declaration types (imports, traits)
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRDeclLowering - Function Declarations
// ─────────────────────────────────────────────────────────────────────────────

void IRDeclLowering::lowerFuncDecl(IRLowering& lowerer, FuncDeclAST* funcDecl) {
    if (!funcDecl) {
        return;
    }

    // Check if it's a foreign function
    bool isForeign = false;
    for (AttributePtr attr : funcDecl->attributes) {
        std::string attrName = lowerer.internedToString(attr->name);
        if (attrName == "foreign") {
            isForeign = true;
            break;
        }
    }

    if (isForeign) {
        lowerForeignDecl(lowerer, funcDecl);
        return;
    }

    // Get the function type
    auto* funcType = funcDecl->funcType;
    if (!funcType) {
        std::string name = lowerer.internedToString(funcDecl->name);
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Function has no type: " + name);
    }

    // Convert to LLVM function type
    auto* llvmFuncType = lowerer.toLLVMFunctionType(funcType);
    if (!llvmFuncType) {
        std::string name = lowerer.internedToString(funcDecl->name);
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Failed to convert function type: " + name);
    }

    // Create the function
    std::string name = lowerer.internedToString(funcDecl->name);
    auto* function = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        name,
        lowerer.getModule()
    );

    if (!function) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Failed to create function: " + name);
    }

    // Create function context
    IRLowering::FunctionContext fnCtx;
    fnCtx.function = function;
    fnCtx.entryBlock = llvm::BasicBlock::Create(lowerer.getContext(), "entry", function);
    fnCtx.funcType = funcType;
    lowerer.m_functionStack.push_back(fnCtx);

    // Set insertion point to entry block
    lowerer.getBuilder().SetInsertPoint(fnCtx.entryBlock);

    // Enter a new scope for function locals
    lowerer.enterScope();

    // Allocate parameters
    size_t paramIndex = 0;
    for (ParamPtr param : funcType->params) {
        std::string paramName = lowerer.internedToString(param->name);
        llvm::Argument* arg = function->getArg(paramIndex++);
        auto* alloca = lowerer.allocateLocal(paramName, arg->getType());
        lowerer.getBuilder().CreateStore(arg, alloca);
        // Store parameter for lookup
        lowerer.m_functionStack.back().parameters[paramName] = alloca;
    }

    // Lower the function body
    if (funcDecl->body) {
        IRStmtLowering::lowerStmt(lowerer, funcDecl->body);
    }

    // If the function is void and has no return, add an implicit return
    if (funcType->isVoid() && !lowerer.m_functionStack.back().hasReturn) {
        lowerer.getBuilder().CreateRetVoid();
    }

    // Exit scope
    lowerer.exitScope();

    // Pop function context
    lowerer.m_functionStack.pop_back();
}

void IRDeclLowering::lowerForeignDecl(IRLowering& lowerer, FuncDeclAST* funcDecl) {
    // Foreign functions are declared as external functions
    auto* funcType = funcDecl->funcType;
    if (!funcType) {
        return;
    }

    auto* llvmFuncType = lowerer.toLLVMFunctionType(funcType);
    if (!llvmFuncType) {
        return;
    }

    std::string name = lowerer.internedToString(funcDecl->name);
    llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        name,
        lowerer.getModule()
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// IRDeclLowering - Variable Declarations
// ─────────────────────────────────────────────────────────────────────────────

void IRDeclLowering::lowerVarDecl(IRLowering& lowerer, VarDeclAST* varDecl) {
    if (!varDecl) {
        return;
    }

    // Only lower if we're inside a function (local variable)
    if (!lowerer.isInFunction()) {
        // Global variables are handled by the AOT backend
        // For interpreter, we ignore globals
        return;
    }

    std::string name = lowerer.internedToString(varDecl->name);
    auto* type = lowerer.toLLVMType(varDecl->type);
    if (!type) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Failed to convert type for variable: " + name);
    }

    // Allocate the variable
    auto* alloca = lowerer.allocateLocal(name, type);

    // Initialize if there's an initializer
    if (varDecl->init) {
        auto* initValue = IRExprLowering::lowerExpr(lowerer, varDecl->init);
        if (initValue) {
            lowerer.getBuilder().CreateStore(initValue, alloca);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRDeclLowering - Struct Declarations
// ─────────────────────────────────────────────────────────────────────────────

void IRDeclLowering::lowerStructDecl(IRLowering& lowerer, StructDeclAST* structDecl) {
    if (!structDecl) {
        return;
    }

    // Structs are represented as LLVM struct types
    // They are registered with the type mapper
    std::string name = lowerer.internedToString(structDecl->name);

    // Build the struct type
    std::vector<llvm::Type*> fieldTypes;
    for (FieldDeclPtr field : structDecl->fields) {
        auto* fieldType = lowerer.toLLVMType(field->type);
        if (fieldType) {
            fieldTypes.push_back(fieldType);
        }
    }

    // Create the struct type
    auto* structType = llvm::StructType::create(lowerer.getContext(), fieldTypes, name);
    
    // Register with the type mapper for later lookup
    lowerer.getTypeMapper().registerType(name, structType);

    // Also register with the LLVM module (for debugging)
    // The struct type is already created and named
}

// ─────────────────────────────────────────────────────────────────────────────
// IRDeclLowering - Enum Declarations
// ─────────────────────────────────────────────────────────────────────────────

void IRDeclLowering::lowerEnumDecl(IRLowering& lowerer, EnumDeclAST* enumDecl) {
    if (!enumDecl) {
        return;
    }

    // Enums are represented as integer types
    // The backing type is int32 by default
    auto* enumType = llvm::Type::getInt32Ty(lowerer.getContext());
    std::string name = lowerer.internedToString(enumDecl->name);
    
    // Register with the type mapper
    lowerer.getTypeMapper().registerType(name, enumType);

    // Register enum variants as constants
    // This makes them available as global constants in the IR
    for (EnumVariantPtr variant : enumDecl->variants) {
        // Create a constant integer for this variant's value
        auto* constValue = llvm::ConstantInt::get(enumType, variant->value);
        
        // Create a global variable for the variant
        // This allows references like Direction_North to be resolved
        std::string varName = name + "_" + lowerer.internedToString(variant->name);
        
        // Create the global variable
        auto* globalVar = new llvm::GlobalVariable(
            *lowerer.getModule(),
            enumType,
            true, // constant - enum variants are immutable
            llvm::GlobalValue::ExternalLinkage,
            constValue,
            varName
        );
        
        // Optional: store the global variable in a map for later lookup
        // The JIT will resolve these symbols by name
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRDeclLowering - Trait Declarations (Stub)
// ─────────────────────────────────────────────────────────────────────────────

// Traits are purely a compile-time concept and don't produce any IR directly.
// They are used for type checking and generic constraints.
// No IR generation is needed for trait declarations.
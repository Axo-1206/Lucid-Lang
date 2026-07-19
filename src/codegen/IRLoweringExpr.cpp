/**
 * @file IRLoweringExpr.cpp
 * @brief Lowers Lucid expressions to LLVM IR.
 * 
 * @responsibility Handles all expression nodes: literals, identifiers,
 *                 operations, calls, field access, indexing, and more.
 * 
 * @related_files
 *   - src/codegen/IRLowering.hpp - Declarations
 *   - src/codegen/IRLoweringStmt.cpp - Statement lowering
 *   - src/codegen/IRLoweringIntrinsic.cpp - Intrinsic lowering
 *   - src/codegen/IRLoweringBuilder.hpp - Helper builders
 */

#include "IRLowering.hpp"
#include "IRLoweringBuilder.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Main Dispatcher
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerExpr(IRLowering& lowerer, ExprAST* expr) {
    if (!expr) {
        return nullptr;
    }

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return lowerLiteral(lowerer, expr->as<LiteralExprAST>());
        case ASTKind::IdentifierExpr:
            return lowerIdentifier(lowerer, expr->as<IdentifierExprAST>());
        case ASTKind::BinaryExpr:
            return lowerBinary(lowerer, expr->as<BinaryExprAST>());
        case ASTKind::UnaryExpr:
            return lowerUnary(lowerer, expr->as<UnaryExprAST>());
        case ASTKind::CallExpr:
            return lowerCall(lowerer, expr->as<CallExprAST>());
        case ASTKind::FieldAccessExpr:
            return lowerFieldAccess(lowerer, expr->as<FieldAccessExprAST>());
        case ASTKind::ModuleAccessExpr:
            return lowerModuleAccess(lowerer, expr->as<ModuleAccessExprAST>());
        case ASTKind::IndexExpr:
            return lowerIndex(lowerer, expr->as<IndexExprAST>());
        case ASTKind::SliceExpr:
            return lowerSlice(lowerer, expr->as<SliceExprAST>());
        case ASTKind::IntrinsicCallExpr:
            return IRIntrinsicLowering::lowerIntrinsic(lowerer, expr->as<IntrinsicCallExprAST>());
        case ASTKind::NullCoalesceExpr:
            return lowerNullCoalesce(lowerer, expr->as<NullCoalesceExprAST>());
        case ASTKind::StructLiteralExpr:
            return lowerStructLiteral(lowerer, expr->as<StructLiteralExprAST>());
        case ASTKind::ArrayLiteralExpr:
            return lowerArrayLiteral(lowerer, expr->as<ArrayLiteralExprAST>());
        case ASTKind::PipelineExpr:
            return lowerPipeline(lowerer, expr->as<PipelineExprAST>());
        case ASTKind::ComposeExpr:
            return lowerCompose(lowerer, expr->as<ComposeExprAST>());
        case ASTKind::AnonFuncExpr:
            return lowerAnonFunc(lowerer, expr->as<AnonFuncExprAST>());
        case ASTKind::RangeExpr:
            return lowerRange(lowerer, expr->as<RangeExprAST>());
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported expression kind: " + std::to_string(static_cast<int>(expr->kind)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Literals
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerLiteral(IRLowering& lowerer, LiteralExprAST* literal) {
    if (!literal) {
        return nullptr;
    }

    switch (literal->kind) {
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string str = lowerer.internedToString(literal->value);
            int64_t value = std::stoll(str, nullptr, 0);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), value);
        }
        case LiteralKind::Float: {
            std::string str = lowerer.internedToString(literal->value);
            double value = std::stod(str);
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(lowerer.getContext()), value);
        }
        case LiteralKind::True:
            return llvm::ConstantInt::getTrue(lowerer.getContext());
        case LiteralKind::False:
            return llvm::ConstantInt::getFalse(lowerer.getContext());
        case LiteralKind::Nil:
            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(lowerer.getContext()));
        case LiteralKind::String: {
            std::string str = lowerer.internedToString(literal->value);
            return lowerer.getBuilder().CreateGlobalStringPtr(str, "str");
        }
        case LiteralKind::RawString: {
            std::string str = lowerer.internedToString(literal->value);
            return lowerer.getBuilder().CreateGlobalStringPtr(str, "rawstr");
        }
        case LiteralKind::Char: {
            std::string str = lowerer.internedToString(literal->value);
            if (str.length() == 1) {
                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(lowerer.getContext()), str[0]);
            }
            return llvm::ConstantInt::get(llvm::Type::getInt8Ty(lowerer.getContext()), 0);
        }
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported literal kind: " + std::to_string(static_cast<int>(literal->kind)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Identifiers
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerIdentifier(IRLowering& lowerer, IdentifierExprAST* identifier) {
    if (!identifier) {
        return nullptr;
    }

    std::string name = lowerer.internedToString(identifier->name);

    // Check if it's a local variable
    auto* local = lowerer.lookupLocal(name);
    if (local) {
        // Load the variable - handle pointer types
        if (local->getType()->isPointerTy()) {
            auto* elemType = local->getType()->getNonOpaquePointerElementType();
            if (elemType) {
                return lowerer.getBuilder().CreateLoad(elemType, local, name);
            }
        }
        return local;
    }

    // Check if it's a function parameter
    if (lowerer.isInFunction()) {
        auto it = lowerer.currentFunction().parameters.find(name);
        if (it != lowerer.currentFunction().parameters.end()) {
            return it->second;
        }
    }

    // Check if it's a global function
    auto* function = lowerer.getModule()->getFunction(name);
    if (function) {
        return function;
    }

    // Check if it's a global variable
    auto* global = lowerer.getModule()->getGlobalVariable(name);
    if (global) {
        return global;
    }

    // Check if it's an enum variant
    // Enum variants are stored as global constants
    // The name format is: EnumName_VariantName
    // If the identifier contains an underscore, it might be an enum variant
    // Actually, enum variants are accessed via FieldAccessExpr, not IdentifierExpr
    
    throw IRLoweringError(IRLoweringError::Kind::VariableNotFound,
                          "Variable not found: " + name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Binary Operations
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerBinary(IRLowering& lowerer, BinaryExprAST* binary) {
    if (!binary) {
        return nullptr;
    }

    auto* left = lowerExpr(lowerer, binary->left);
    auto* right = lowerExpr(lowerer, binary->right);
    if (!left || !right) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower binary expression operands");
    }

    switch (binary->op) {
        case BinaryOp::Add:
            return IRBuilderHelper::createAdd(lowerer, left, right, "add");
        case BinaryOp::Sub:
            return IRBuilderHelper::createSub(lowerer, left, right, "sub");
        case BinaryOp::Mul:
            return IRBuilderHelper::createMul(lowerer, left, right, "mul");
        case BinaryOp::Div:
            return IRBuilderHelper::createDiv(lowerer, left, right, "div");
        case BinaryOp::Mod:
            return IRBuilderHelper::createRem(lowerer, left, right, "mod");
        case BinaryOp::Pow: {
            // Power is not a simple LLVM instruction - use pow intrinsic or library call
            // For now, use the pow intrinsic if both operands are floating point
            if (left->getType()->isFloatingPointTy()) {
                auto* powFunc = llvm::Intrinsic::getDeclaration(
                    lowerer.getModule(),
                    llvm::Intrinsic::pow,
                    {left->getType()}
                );
                return lowerer.getBuilder().CreateCall(powFunc, {left, right}, "pow");
            }
            // Integer power would need a runtime library call
            // FIXME: Implement integer pow
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Integer power not yet implemented");
        }
        case BinaryOp::Eq:
            return IRBuilderHelper::createICmp(lowerer, llvm::CmpInst::ICMP_EQ, left, right, "eq");
        case BinaryOp::Ne:
            return IRBuilderHelper::createICmp(lowerer, llvm::CmpInst::ICMP_NE, left, right, "ne");
        case BinaryOp::Lt:
            return IRBuilderHelper::createICmp(lowerer, llvm::CmpInst::ICMP_SLT, left, right, "lt");
        case BinaryOp::Gt:
            return IRBuilderHelper::createICmp(lowerer, llvm::CmpInst::ICMP_SGT, left, right, "gt");
        case BinaryOp::Le:
            return IRBuilderHelper::createICmp(lowerer, llvm::CmpInst::ICMP_SLE, left, right, "le");
        case BinaryOp::Ge:
            return IRBuilderHelper::createICmp(lowerer, llvm::CmpInst::ICMP_SGE, left, right, "ge");
        case BinaryOp::And:
            return IRBuilderHelper::createAnd(lowerer, left, right, "and");
        case BinaryOp::Or:
            return IRBuilderHelper::createOr(lowerer, left, right, "or");
        case BinaryOp::BitAnd:
            return IRBuilderHelper::createAnd(lowerer, left, right, "bitand");
        case BinaryOp::BitOr:
            return IRBuilderHelper::createOr(lowerer, left, right, "bitor");
        case BinaryOp::BitXor:
            return IRBuilderHelper::createXor(lowerer, left, right, "bitxor");
        case BinaryOp::Shl:
            return IRBuilderHelper::createShl(lowerer, left, right, "shl");
        case BinaryOp::Shr:
            return IRBuilderHelper::createAShr(lowerer, left, right, "shr");
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported binary operator");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Unary Operations
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerUnary(IRLowering& lowerer, UnaryExprAST* unary) {
    if (!unary) {
        return nullptr;
    }

    auto* operand = lowerExpr(lowerer, unary->operand);
    if (!operand) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower unary expression operand");
    }

    switch (unary->op) {
        case UnaryOp::Neg:
            if (operand->getType()->isFloatingPointTy()) {
                return lowerer.getBuilder().CreateFNeg(operand, "neg");
            }
            return lowerer.getBuilder().CreateNeg(operand, "neg");
        case UnaryOp::Not:
            // Logical NOT - convert to boolean and negate
            if (!operand->getType()->isIntegerTy(1)) {
                operand = lowerer.getBuilder().CreateICmpNE(
                    operand,
                    llvm::ConstantInt::get(operand->getType(), 0),
                    "not.cond"
                );
            }
            return lowerer.getBuilder().CreateNot(operand, "not");
        case UnaryOp::BitNot:
            // Bitwise NOT - only valid for integer types
            if (!operand->getType()->isIntegerTy()) {
                throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                      "Bitwise NOT only valid for integer types");
            }
            return lowerer.getBuilder().CreateNot(operand, "bitnot");
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported unary operator");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Function Calls
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerCall(IRLowering& lowerer, CallExprAST* call) {
    if (!call) {
        return nullptr;
    }

    // Get the callee
    auto* callee = lowerExpr(lowerer, call->callee);
    if (!callee) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Failed to lower callee");
    }

    // Get the function
    llvm::Function* function = nullptr;
    if (auto* func = llvm::dyn_cast<llvm::Function>(callee)) {
        function = func;
    } else if (auto* ptr = llvm::dyn_cast<llvm::ConstantExpr>(callee)) {
        // Handle function pointer
        function = llvm::dyn_cast<llvm::Function>(ptr->getOperand(0));
    }

    if (!function) {
        // Try to get the function by name if callee is a global value
        if (auto* global = llvm::dyn_cast<llvm::GlobalValue>(callee)) {
            function = lowerer.getModule()->getFunction(global->getName());
        }
    }

    if (!function) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Callee is not a function");
    }

    // Lower arguments
    std::vector<llvm::Value*> args;
    for (ExprPtr arg : call->args) {
        auto* value = lowerExpr(lowerer, arg);
        if (value) {
            args.push_back(value);
        }
    }

    // Create the call
    if (call->hasArgPack) {
        // Argument pack - special handling for pipeline
        // The first argument will be filled by the pipeline
        // For now, just create the call with the arguments
        return lowerer.getBuilder().CreateCall(function, args, "call");
    }

    return lowerer.getBuilder().CreateCall(function, args, "call");
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Field Access
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerFieldAccess(IRLowering& lowerer, FieldAccessExprAST* field) {
    if (!field) {
        return nullptr;
    }

    auto* object = lowerExpr(lowerer, field->object);
    if (!object) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower field access object");
    }

    // Get the object type
    auto* objectType = object->getType();
    if (!objectType) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Object has no type");
    }

    std::string fieldName = lowerer.internedToString(field->fieldName);

    // Check if it's a struct type
    if (objectType->isPointerTy()) {
        auto* elemType = objectType->getNonOpaquePointerElementType();
        if (elemType && elemType->isStructTy()) {
            auto* structType = llvm::cast<llvm::StructType>(elemType);
            // Find the field index
            // FIXME: We need to maintain a mapping of field names to indices
            // For now, assume the field is the first one
            int fieldIndex = 0;
            // Get the field pointer
            auto* ptr = lowerer.getBuilder().CreateStructGEP(structType, object, fieldIndex, fieldName);
            auto* fieldPtrType = ptr->getType();
            auto* fieldElemType = fieldPtrType->getNonOpaquePointerElementType();
            if (fieldElemType) {
                return lowerer.getBuilder().CreateLoad(fieldElemType, ptr, fieldName);
            }
        }
    }

    // If it's a pointer, try to dereference
    if (objectType->isPointerTy()) {
        auto* elemType = objectType->getNonOpaquePointerElementType();
        if (elemType) {
            auto* ptr = lowerer.getBuilder().CreateLoad(elemType, object, "field.ptr");
            return ptr;
        }
    }

    throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                          "Field access on non-struct type");
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Module Access
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerModuleAccess(IRLowering& lowerer, ModuleAccessExprAST* moduleAccess) {
    if (!moduleAccess) {
        return nullptr;
    }

    // Module access is resolved during semantic analysis
    // At IR level, it's just a function call with module qualification
    std::string memberName = lowerer.internedToString(moduleAccess->memberName);
    
    // Look up the member in the module
    // For now, treat it as a function call
    auto* function = lowerer.getModule()->getFunction(memberName);
    if (function) {
        return function;
    }

    // Could also be a global variable
    auto* global = lowerer.getModule()->getGlobalVariable(memberName);
    if (global) {
        return global;
    }

    throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                          "Module member not found: " + memberName);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Index Access
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerIndex(IRLowering& lowerer, IndexExprAST* index) {
    if (!index) {
        return nullptr;
    }

    auto* target = lowerExpr(lowerer, index->target);
    auto* idx = lowerExpr(lowerer, index->index);
    if (!target || !idx) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower index expression");
    }

    // Ensure index is integer type
    if (!idx->getType()->isIntegerTy()) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Index must be an integer type");
    }

    // Convert index to i64 if needed
    if (idx->getType() != llvm::Type::getInt64Ty(lowerer.getContext())) {
        idx = lowerer.getBuilder().CreateSExtOrTrunc(
            idx,
            llvm::Type::getInt64Ty(lowerer.getContext()),
            "idx.cast"
        );
    }

    // FIXME: Add bounds checking - call runtime check function

    // Get the element pointer
    if (target->getType()->isPointerTy()) {
        auto* elemType = target->getType()->getNonOpaquePointerElementType();
        if (elemType) {
            return lowerer.getBuilder().CreateGEP(elemType, target, idx, "index");
        }
    }

    // Fallback: treat as array
    if (target->getType()->isArrayTy()) {
        auto* arrayType = llvm::cast<llvm::ArrayType>(target->getType());
        return lowerer.getBuilder().CreateGEP(arrayType, target, idx, "index");
    }

    throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                          "Cannot index into non-array type");
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Slice Expression
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerSlice(IRLowering& lowerer, SliceExprAST* slice) {
    if (!slice) {
        return nullptr;
    }

    // Slices are views into arrays
    // For now, just return the target
    // FIXME: Proper slice implementation
    return lowerExpr(lowerer, slice->target);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Null Coalesce
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerNullCoalesce(IRLowering& lowerer, NullCoalesceExprAST* coalesce) {
    if (!coalesce) {
        return nullptr;
    }

    // x ?? fallback
    // Lower the value
    auto* value = lowerExpr(lowerer, coalesce->value);
    if (!value) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower null coalesce value");
    }

    // Lower the fallback
    auto* fallback = lowerExpr(lowerer, coalesce->fallback);
    if (!fallback) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower null coalesce fallback");
    }

    // Check if value is null (or nil)
    auto* isNull = IRBuilderHelper::createIsNull(lowerer, value, "isnull");

    // Select between value and fallback
    return IRBuilderHelper::createSelect(lowerer, isNull, fallback, value, "coalesce");
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Struct Literal
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerStructLiteral(IRLowering& lowerer, StructLiteralExprAST* structLit) {
    if (!structLit) {
        return nullptr;
    }

    // Get the struct type
    std::string typeName = lowerer.internedToString(structLit->typeName);
    auto* structType = lowerer.getTypeMapper().getRegisteredType(typeName);
    if (!structType) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Struct type not found: " + typeName);
    }

    // Allocate the struct on the stack
    auto* alloca = lowerer.getBuilder().CreateAlloca(structType, nullptr, "struct");

    // Initialize fields
    for (FieldInitPtr init : structLit->inits) {
        auto* value = lowerExpr(lowerer, init->value);
        if (value) {
            // Find the field index
            // FIXME: Need to map field names to indices
            int fieldIndex = 0;
            std::string fieldName = lowerer.internedToString(init->name);
            auto* ptr = lowerer.getBuilder().CreateStructGEP(
                llvm::cast<llvm::StructType>(structType), 
                alloca, 
                fieldIndex, 
                fieldName
            );
            lowerer.getBuilder().CreateStore(value, ptr);
        }
    }

    // Load the struct
    return lowerer.getBuilder().CreateLoad(structType, alloca, "struct.literal");
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Array Literal
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerArrayLiteral(IRLowering& lowerer, ArrayLiteralExprAST* arrayLit) {
    if (!arrayLit) {
        return nullptr;
    }

    // For now, allocate a dynamic array
    // FIXME: Proper array allocation

    // Count elements
    size_t count = arrayLit->elements.size();
    auto* elementType = llvm::Type::getInt64Ty(lowerer.getContext()); // FIXME: Get element type from AST

    // Allocate array on the stack
    auto* arrayType = llvm::ArrayType::get(elementType, count);
    auto* alloca = lowerer.getBuilder().CreateAlloca(arrayType, nullptr, "array");

    // Initialize elements
    for (size_t i = 0; i < count; ++i) {
        auto* value = lowerExpr(lowerer, arrayLit->elements[i]);
        if (value) {
            std::vector<llvm::Value*> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 0),
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), i)
            };
            auto* ptr = lowerer.getBuilder().CreateGEP(
                elementType,
                alloca,
                indices,
                "array.elem"
            );
            lowerer.getBuilder().CreateStore(value, ptr);
        }
    }

    // Return the array pointer
    return alloca;
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Pipeline
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerPipeline(IRLowering& lowerer, PipelineExprAST* pipeline) {
    if (!pipeline) {
        return nullptr;
    }

    // Lower the seed
    llvm::Value* current = lowerExpr(lowerer, pipeline->seed);
    if (!current) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower pipeline seed");
    }

    // Apply each step
    for (PipelineStepPtr step : pipeline->steps) {
        if (!step) {
            continue;
        }

        // For each step, call the function with the current value
        auto* callable = lowerExpr(lowerer, step->callable);
        if (!callable) {
            throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                                  "Failed to lower pipeline step");
        }

        // Get the function
        llvm::Function* function = nullptr;
        if (auto* func = llvm::dyn_cast<llvm::Function>(callable)) {
            function = func;
        }

        if (!function) {
            throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                                  "Pipeline step is not a function");
        }

        // Build arguments
        std::vector<llvm::Value*> args;
        args.push_back(current); // The current value is the first argument

        // Add pack arguments if any
        for (ExprPtr arg : step->packArgs) {
            auto* value = lowerExpr(lowerer, arg);
            if (value) {
                args.push_back(value);
            }
        }

        // Call the function
        current = lowerer.getBuilder().CreateCall(function, args, "pipeline.step");
    }

    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Compose (Function Composition)
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerCompose(IRLowering& lowerer, ComposeExprAST* compose) {
    if (!compose) {
        return nullptr;
    }

    // Composition is compile-time - it should produce a new function
    // For now, just chain the calls
    llvm::Value* current = lowerExpr(lowerer, compose->left);
    if (!current) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower compose left operand");
    }

    for (ComposeOperandPtr operand : compose->operands) {
        auto* callable = lowerExpr(lowerer, operand->callable);
        if (!callable) {
            throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                                  "Failed to lower compose operand");
        }

        // Compose the functions
        // For now, treat as a call
        // FIXME: Proper function composition - this should create a new function
        if (auto* func = llvm::dyn_cast<llvm::Function>(callable)) {
            // Create a call to the function with the current value
            current = lowerer.getBuilder().CreateCall(func, {current}, "compose");
        }
    }

    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Anonymous Function
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerAnonFunc(IRLowering& lowerer, AnonFuncExprAST* anonFunc) {
    if (!anonFunc) {
        return nullptr;
    }

    // Anonymous functions are compiled as regular functions with generated names
    // For now, return the function pointer
    // FIXME: Proper anonymous function lowering

    // Create a function with a generated name
    auto* funcType = anonFunc->funcType;
    if (!funcType) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Anonymous function has no type");
    }

    static int anonCounter = 0;
    std::string name = "__anon_" + std::to_string(++anonCounter);

    auto* llvmFuncType = lowerer.toLLVMFunctionType(funcType);
    if (!llvmFuncType) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Failed to convert anonymous function type");
    }

    auto* function = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::InternalLinkage,
        name,
        lowerer.getModule()
    );

    // Lower the body
    if (anonFunc->body) {
        // Create function context
        IRLowering::FunctionContext fnCtx;
        fnCtx.function = function;
        fnCtx.entryBlock = llvm::BasicBlock::Create(lowerer.getContext(), "entry", function);
        fnCtx.funcType = funcType;
        lowerer.m_functionStack.push_back(fnCtx);

        lowerer.getBuilder().SetInsertPoint(fnCtx.entryBlock);

        lowerer.enterScope();
        IRStmtLowering::lowerStmt(lowerer, anonFunc->body);
        lowerer.exitScope();

        // If void and no return, add implicit return
        if (!lowerer.currentFunction().hasReturn && funcType->isVoid()) {
            lowerer.getBuilder().CreateRetVoid();
        }

        lowerer.m_functionStack.pop_back();
    }

    return function;
}

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Range Expression
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRExprLowering::lowerRange(IRLowering& lowerer, RangeExprAST* range) {
    if (!range) {
        return nullptr;
    }

    // Ranges are represented as a pair of values
    auto* lo = lowerExpr(lowerer, range->lo);
    auto* hi = lowerExpr(lowerer, range->hi);

    if (!lo || !hi) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower range bounds");
    }

    // Create a struct for the range
    auto* rangeType = llvm::StructType::get(
        lowerer.getContext(), 
        {lo->getType(), hi->getType()}
    );
    llvm::Value* rangeStruct = llvm::UndefValue::get(rangeType);
    rangeStruct = lowerer.getBuilder().CreateInsertValue(rangeStruct, lo, 0);
    rangeStruct = lowerer.getBuilder().CreateInsertValue(rangeStruct, hi, 1);

    return rangeStruct;
}
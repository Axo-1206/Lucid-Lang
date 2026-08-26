/// @file CodeGenExpr.cpp
/// @brief Implementation of expression lowering to LLVM IR.

#include "CodeGen.hpp"
#include "CodeGenType.hpp"
#include "core/ASTStrings.hpp"
#include "support/CodeGenAlloca.hpp"
#include "support/CodeGenHelpers.hpp"
#include "support/CodeGenPanic.hpp"
#include "support/LLVMHelpers.hpp"
#include "support/RuntimeError.hpp"
#include "intrinsic/IntrinsicEmitter.hpp"
#include "../sema/types/SemaCompare.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "closure/CodeGenClosure.hpp"
#include "generic/CodeGenGeneric.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include <cmath>
#include <cassert>
#include <unordered_map>

namespace codegen {

// ─── Helper: Convert Value* vector to Constant* vector ───────────────────

static std::vector<llvm::Constant*> toConstants(const std::vector<llvm::Value*>& values) {
    std::vector<llvm::Constant*> constants;
    constants.reserve(values.size());
    for (llvm::Value* v : values) {
        if (llvm::Constant* c = llvm::dyn_cast<llvm::Constant>(v)) {
            constants.push_back(c);
        } else {
            constants.push_back(nullptr);
        }
    }
    return constants;
}

// ─── Helper: Get the element type from an array type ─────────────────────

static llvm::Type* getArrayElementType(CodeGenContext& ctx, TypeAST* arrayType) {
    if (!arrayType) return nullptr;
    ArrayTypeAST* arr = arrayType->as<ArrayTypeAST>();
    if (!arr) return nullptr;
    return getType(ctx, arr->element);
}

// =============================================================================
// Expression Lowering - Dispatch
// =============================================================================

llvm::Value* lowerExpression(ExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return lowerLiteralExpr(expr->as<LiteralExprAST>(), ctx);
        case ASTKind::IdentifierExpr:
            return lowerIdentifierExpr(expr->as<IdentifierExprAST>(), ctx);
        case ASTKind::ArrayLiteralExpr:
            return lowerArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx);
        case ASTKind::StructLiteralExpr:
            return lowerStructLiteralExpr(expr->as<StructLiteralExprAST>(), ctx);
        case ASTKind::BinaryExpr:
            return lowerBinaryExpr(expr->as<BinaryExprAST>(), ctx);
        case ASTKind::UnaryExpr:
            return lowerUnaryExpr(expr->as<UnaryExprAST>(), ctx);
        case ASTKind::CallExpr:
            return lowerCallExpr(expr->as<CallExprAST>(), ctx);
        case ASTKind::IntrinsicCallExpr:
            return lowerIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), ctx);
        case ASTKind::IndexExpr:
            return lowerIndexExpr(expr->as<IndexExprAST>(), ctx);
        case ASTKind::SliceExpr:
            return lowerSliceExpr(expr->as<SliceExprAST>(), ctx);
        case ASTKind::FieldAccessExpr:
            return lowerFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx);
        case ASTKind::ModuleAccessExpr:
            return lowerModuleAccessExpr(expr->as<ModuleAccessExprAST>(), ctx);
        case ASTKind::NullCoalesceExpr:
            return lowerNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), ctx);
        case ASTKind::AssignExpr:
            return lowerAssignExpr(expr->as<AssignExprAST>(), ctx);
        case ASTKind::PipelineExpr:
            return lowerPipelineExpr(expr->as<PipelineExprAST>(), ctx);
        case ASTKind::ComposeExpr:
            return lowerComposeExpr(expr->as<ComposeExprAST>(), ctx);
        case ASTKind::AnonFuncExpr:
            return lowerAnonFuncExpr(expr->as<AnonFuncExprAST>(), ctx);
        case ASTKind::IfExpr:
            return lowerIfExpr(expr->as<IfExprAST>(), ctx);
        case ASTKind::RangeExpr:
            return lowerRangeExpr(expr->as<RangeExprAST>(), ctx);
        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, expr->loc,
                                    "unsupported expression kind: ",
                                    astKindToString(expr->kind));
            return nullptr;
    }
}

// =============================================================================
// Literal Expression
// =============================================================================

llvm::Value* lowerLiteralExpr(LiteralExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Type* type = getType(ctx, expr->resolvedType);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "literal has no type");
        return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->kind) {
        case LiteralKind::True:
            result = llvm::ConstantInt::get(type, 1);
            break;

        case LiteralKind::False:
            result = llvm::ConstantInt::get(type, 0);
            break;

        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string valStr = ctx.pool.lookup(expr->value);
            int64_t val = 0;
            try {
                if (expr->kind == LiteralKind::Hex) {
                    val = std::stoll(valStr, nullptr, 16);
                } else if (expr->kind == LiteralKind::Binary) {
                    val = std::stoll(valStr, nullptr, 2);
                } else {
                    val = std::stoll(valStr, nullptr, 10);
                }
            } catch (const std::exception& e) {
                ctx.diagnostics.errorAt(DiagCode::Lex_InvalidNumberLiteral, expr->loc,
                                        "invalid integer literal: ", valStr);
                return nullptr;
            }

            result = llvm::ConstantInt::get(type, val);
            break;
        }

        case LiteralKind::Float: {
            std::string valStr = ctx.pool.lookup(expr->value);
            double val = 0.0;
            try {
                val = std::stod(valStr);
            } catch (const std::exception& e) {
                ctx.diagnostics.errorAt(DiagCode::Lex_InvalidNumberLiteral, expr->loc,
                                        "invalid float literal: ", valStr);
                return nullptr;
            }

            if (type->isFloatTy()) {
                result = llvm::ConstantFP::get(type, static_cast<float>(val));
            } else if (type->isDoubleTy()) {
                result = llvm::ConstantFP::get(type, val);
            } else {
                result = llvm::ConstantFP::get(type, val);
            }
            break;
        }

        case LiteralKind::String:
        case LiteralKind::RawString: {
            std::string valStr = ctx.pool.lookup(expr->value);
            llvm::Constant* strConst = llvm::ConstantDataArray::getString(
                ctx.llvmCtx,
                valStr,
                true
            );

            llvm::GlobalVariable* global = new llvm::GlobalVariable(
                *ctx.module,
                strConst->getType(),
                true,
                llvm::GlobalValue::PrivateLinkage,
                strConst,
                ".str"
            );

            result = ctx.builder.CreatePointerCast(
                global,
                llvm::PointerType::get(ctx.llvmCtx, 0)
            );
            break;
        }

        case LiteralKind::Char: {
            std::string valStr = ctx.pool.lookup(expr->value);
            if (valStr.empty()) {
                result = llvm::ConstantInt::get(type, 0);
            } else {
                result = llvm::ConstantInt::get(type, valStr[0]);
            }
            break;
        }

        case LiteralKind::Nil:
        case LiteralKind::Err: {
            result = llvm::Constant::getNullValue(type);
            break;
        }

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, expr->loc,
                                    "unknown literal kind");
            return nullptr;
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Identifier Expression
// =============================================================================

llvm::Value* lowerIdentifierExpr(IdentifierExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    if (ctx.pool.lookupView(expr->name) == "_") {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "cannot use '_' as a value");
        return nullptr;
    }

    // ─── TODO: Implement proper identifier lookup ──────────────────────────
    // This should look up the variable/function from the context's symbol table.
    // For now, return a placeholder.
    llvm::Type* llvmType = getType(ctx, expr->resolvedType);
    if (!llvmType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "identifier '", ctx.pool.lookup(expr->name), "' has no type");
        return nullptr;
    }

    expr->llvmValue = llvm::Constant::getNullValue(llvmType);
    return expr->llvmValue;
}

// =============================================================================
// Array Literal Expression
// =============================================================================

llvm::Value* lowerArrayLiteralExpr(ArrayLiteralExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Type* arrayType = getType(ctx, expr->resolvedType);
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "array literal has no type");
        return nullptr;
    }

    if (expr->elements.empty()) {
        return llvm::Constant::getNullValue(arrayType);
    }

    std::vector<llvm::Value*> elements;
    for (ExprAST* elem : expr->elements) {
        llvm::Value* elemValue = lowerExpression(elem, ctx);
        if (!elemValue) {
            return nullptr;
        }
        elements.push_back(elemValue);
    }

    ArrayTypeAST* arrayTypeAST = expr->resolvedType->as<ArrayTypeAST>();

    if (arrayTypeAST->isFixed()) {
        std::vector<llvm::Constant*> constantElements;
        for (llvm::Value* elem : elements) {
            if (llvm::Constant* c = llvm::dyn_cast<llvm::Constant>(elem)) {
                constantElements.push_back(c);
            } else {
                constantElements.push_back(llvm::Constant::getNullValue(
                    llvm::cast<llvm::ArrayType>(arrayType)->getElementType()
                ));
            }
        }
        return llvm::ConstantArray::get(
            llvm::cast<llvm::ArrayType>(arrayType),
            llvm::ArrayRef<llvm::Constant*>(constantElements)
        );
    } else {
        // Dynamic array: allocate memory and store elements
        if (elements.empty()) {
            return llvm::Constant::getNullValue(arrayType);
        }
        return elements[0];
    }
}

// =============================================================================
// Struct Literal Expression
// =============================================================================

llvm::Value* lowerStructLiteralExpr(StructLiteralExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── 1. Get the struct type from the resolved type ─────────────────────
    TypeAST* structTypeAST = expr->resolvedType;
    if (!structTypeAST) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "struct literal has no resolved type");
        return nullptr;
    }

    // The resolved type should be a NamedTypeAST
    NamedTypeAST* namedType = structTypeAST->as<NamedTypeAST>();
    if (!namedType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "struct literal type is not a named type");
        return nullptr;
    }

    // ─── 2. Get the resolved declaration from the named type ────────────────
    // The resolvedDecl should be set by Sema on the NamedTypeAST
    TypeDeclAST* typeDecl = namedType->resolvedDecl;
    if (!typeDecl) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedType, expr->loc,
                                "struct type '", ctx.pool.lookup(expr->typeName), 
                                "' has no resolved declaration");
        return nullptr;
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "'", ctx.pool.lookup(expr->typeName), 
                                "' is not a struct type");
        return nullptr;
    }

    StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // ─── 3. Get the LLVM struct type ───────────────────────────────────────
    llvm::StructType* llvmStructType = ctx.lookupStruct(structDecl);
    if (!llvmStructType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "struct type '", ctx.pool.lookup(structDecl->name), 
                                "' has no LLVM type");
        return nullptr;
    }

    // ─── 4. Build the struct value from field initializers ─────────────────
    // Start with undef for the struct
    llvm::Value* result = llvm::UndefValue::get(llvmStructType);

    // ─── 5. Map field names to indices ─────────────────────────────────────
    std::unordered_map<InternedString, size_t> fieldIndexMap;
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        fieldIndexMap[structDecl->fields[i]->name] = i;
    }

    // Track which fields have been initialized
    std::vector<bool> initialized(structDecl->fields.size(), false);

    // ─── 6. Process each field initializer ─────────────────────────────────
    for (FieldInitAST* init : expr->inits) {
        if (!init) continue;

        // Find the field index
        auto it = fieldIndexMap.find(init->name);
        if (it == fieldIndexMap.end()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, init->loc,
                                    "unknown field '", ctx.pool.lookup(init->name), 
                                    "' in struct '", ctx.pool.lookup(structDecl->name), "'");
            return nullptr;
        }

        size_t fieldIndex = it->second;
        initialized[fieldIndex] = true;

        // ─── Lower the field value ─────────────────────────────────────────
        llvm::Value* fieldValue = lowerExpression(init->value, ctx);
        if (!fieldValue) {
            return nullptr;
        }

        // If the field value is an l-value, load it
        if (init->value->isLValue) {
            llvm::Type* elemType = getType(ctx, init->value->resolvedType);
            if (elemType) {
                fieldValue = loadIfNeeded(fieldValue, elemType, ctx);
            } else {
                fieldValue = loadIfNeeded(fieldValue, true, ctx);
            }
            if (!fieldValue) return nullptr;
        }

        // ─── Type compatibility check ──────────────────────────────────────
        llvm::Type* expectedType = llvmStructType->getElementType(fieldIndex);
        if (fieldValue->getType() != expectedType) {
            // Try to cast if compatible
            if (fieldValue->getType()->isIntegerTy() && expectedType->isIntegerTy()) {
                fieldValue = ctx.builder.CreateIntCast(
                    fieldValue, 
                    expectedType, 
                    true,  // signed
                    "field_cast"
                );
            } else if (fieldValue->getType()->isFloatingPointTy() && 
                       expectedType->isFloatingPointTy()) {
                if (fieldValue->getType()->isFloatTy() && expectedType->isDoubleTy()) {
                    fieldValue = ctx.builder.CreateFPExt(
                        fieldValue, 
                        expectedType, 
                        "field_fpext"
                    );
                } else if (fieldValue->getType()->isDoubleTy() && expectedType->isFloatTy()) {
                    fieldValue = ctx.builder.CreateFPTrunc(
                        fieldValue, 
                        expectedType, 
                        "field_fptrunc"
                    );
                }
            } else if (fieldValue->getType()->isPointerTy() && 
                       expectedType->isPointerTy()) {
                fieldValue = ctx.builder.CreatePointerCast(
                    fieldValue, 
                    expectedType, 
                    "field_ptr_cast"
                );
            } else {
                // Get type names for diagnostic using LLVM's own printing
                std::string expectedName;
                llvm::raw_string_ostream expectedOS(expectedName);
                expectedType->print(expectedOS);
                
                std::string actualName;
                llvm::raw_string_ostream actualOS(actualName);
                fieldValue->getType()->print(actualOS);
                
                ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, init->loc,
                                        "field '", ctx.pool.lookup(init->name), 
                                        "' type mismatch: expected ", expectedName,
                                        " but got ", actualName);
                return nullptr;
            }
        }

        // ─── Insert the value into the struct ─────────────────────────────
        result = ctx.builder.CreateInsertValue(
            result, 
            fieldValue, 
            static_cast<unsigned>(fieldIndex), 
            "field_" + ctx.pool.lookup(init->name)
        );
    }

    // ─── 7. Fill uninitialized fields with default values ──────────────────
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        if (initialized[i]) continue;

        FieldDeclAST* field = structDecl->fields[i];
        llvm::Type* fieldType = llvmStructType->getElementType(i);
        llvm::Constant* defaultValue = nullptr;
        
        // Check if the field has a default value expression
        if (field->defaultVal) {
            llvm::Value* defaultVal = lowerExpression(field->defaultVal, ctx);
            if (defaultVal && llvm::isa<llvm::Constant>(defaultVal)) {
                defaultValue = llvm::cast<llvm::Constant>(defaultVal);
            } else {
                // Default value is not a constant - use null
                defaultValue = llvm::Constant::getNullValue(fieldType);
            }
        } else {
            // Use null/default for uninitialized fields
            defaultValue = llvm::Constant::getNullValue(fieldType);
        }

        result = ctx.builder.CreateInsertValue(
            result, 
            defaultValue, 
            static_cast<unsigned>(i), 
            "default_field_" + ctx.pool.lookup(field->name)
        );
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Binary Expression
// =============================================================================

llvm::Value* lowerBinaryExpr(BinaryExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* left = lowerExpression(expr->left, ctx);
    llvm::Value* right = lowerExpression(expr->right, ctx);
    if (!left || !right) {
        return nullptr;
    }

    if (expr->left->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->left->resolvedType);
        if (elemType) {
            left = loadIfNeeded(left, elemType, ctx);
        } else {
            left = loadIfNeeded(left, true, ctx);
        }
    }
    if (expr->right->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->right->resolvedType);
        if (elemType) {
            right = loadIfNeeded(right, elemType, ctx);
        } else {
            right = loadIfNeeded(right, true, ctx);
        }
    }
    if (!left || !right) {
        return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->op) {
        // ─── Arithmetic Operators ────────────────────────────────────────
        case BinaryOp::Add:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateAdd(left, right, "add");
            } else {
                result = ctx.builder.CreateFAdd(left, right, "fadd");
            }
            break;

        case BinaryOp::Sub:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateSub(left, right, "sub");
            } else {
                result = ctx.builder.CreateFSub(left, right, "fsub");
            }
            break;

        case BinaryOp::Mul:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateMul(left, right, "mul");
            } else {
                result = ctx.builder.CreateFMul(left, right, "fmul");
            }
            break;

        case BinaryOp::Div: {
            if (isIntegerType(left->getType()) && isIntegerType(right->getType())) {
                // Check for division by zero
                RuntimeErrorKind kind = RuntimeErrorKind::DivisionByZero;
                llvm::Value* checkedRight = emitZeroCheck(right, kind, ctx);
                if (!checkedRight) return nullptr;
                result = ctx.builder.CreateSDiv(left, checkedRight, "sdiv");
            } else {
                result = ctx.builder.CreateFDiv(left, right, "fdiv");
            }
            break;
        }

        case BinaryOp::Mod: {
            if (isIntegerType(left->getType()) && isIntegerType(right->getType())) {
                RuntimeErrorKind kind = RuntimeErrorKind::ModuloByZero;
                llvm::Value* checkedRight = emitZeroCheck(right, kind, ctx);
                if (!checkedRight) return nullptr;
                result = ctx.builder.CreateSRem(left, checkedRight, "srem");
            } else {
                result = ctx.builder.CreateFRem(left, right, "frem");
            }
            break;
        }

        case BinaryOp::Pow: {
            llvm::Type* leftType = left->getType();
            llvm::Type* rightType = right->getType();

            if (isIntegerType(leftType) && isIntegerType(rightType)) {
                left = ctx.builder.CreateSIToFP(left, llvm::Type::getDoubleTy(ctx.llvmCtx));
                right = ctx.builder.CreateSIToFP(right, llvm::Type::getDoubleTy(ctx.llvmCtx));
            }

            result = emitIntrinsic(ctx.pool.intern("pow"), {left, right}, nullptr, ctx);
            break;
        }

        // ─── Comparison Operators ────────────────────────────────────────
        case BinaryOp::Eq:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpEQ(left, right, "eq");
            } else {
                result = ctx.builder.CreateFCmpUEQ(left, right, "feq");
            }
            break;

        case BinaryOp::Ne:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpNE(left, right, "ne");
            } else {
                result = ctx.builder.CreateFCmpUNE(left, right, "fne");
            }
            break;

        case BinaryOp::Lt:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSLT(left, right, "slt");
            } else {
                result = ctx.builder.CreateFCmpULT(left, right, "flt");
            }
            break;

        case BinaryOp::Gt:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSGT(left, right, "sgt");
            } else {
                result = ctx.builder.CreateFCmpUGT(left, right, "fgt");
            }
            break;

        case BinaryOp::Le:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSLE(left, right, "sle");
            } else {
                result = ctx.builder.CreateFCmpULE(left, right, "fle");
            }
            break;

        case BinaryOp::Ge:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSGE(left, right, "sge");
            } else {
                result = ctx.builder.CreateFCmpUGE(left, right, "fge");
            }
            break;

        // ─── Logical Operators ──────────────────────────────────────────
        case BinaryOp::And:
            if (!isBoolValue(left)) {
                left = ctx.builder.CreateICmpNE(left, llvm::Constant::getNullValue(left->getType()));
            }
            if (!isBoolValue(right)) {
                right = ctx.builder.CreateICmpNE(right, llvm::Constant::getNullValue(right->getType()));
            }
            result = ctx.builder.CreateAnd(left, right, "and");
            break;

        case BinaryOp::Or:
            if (!isBoolValue(left)) {
                left = ctx.builder.CreateICmpNE(left, llvm::Constant::getNullValue(left->getType()));
            }
            if (!isBoolValue(right)) {
                right = ctx.builder.CreateICmpNE(right, llvm::Constant::getNullValue(right->getType()));
            }
            result = ctx.builder.CreateOr(left, right, "or");
            break;

        // ─── Bitwise Operators ──────────────────────────────────────────
        case BinaryOp::BitAnd:
            result = ctx.builder.CreateAnd(left, right, "band");
            break;

        case BinaryOp::BitOr:
            result = ctx.builder.CreateOr(left, right, "bor");
            break;

        case BinaryOp::BitXor:
            result = ctx.builder.CreateXor(left, right, "bxor");
            break;

        case BinaryOp::Shl:
            result = ctx.builder.CreateShl(left, right, "shl");
            break;

        case BinaryOp::Shr:
            result = ctx.builder.CreateAShr(left, right, "ashr");
            break;

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidBinary, expr->loc,
                                    "unknown binary operator");
            return nullptr;
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Unary Expression
// =============================================================================

llvm::Value* lowerUnaryExpr(UnaryExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* operand = lowerExpression(expr->operand, ctx);
    if (!operand) {
        return nullptr;
    }

    if (expr->operand->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->operand->resolvedType);
        if (elemType) {
            operand = loadIfNeeded(operand, elemType, ctx);
        } else {
            operand = loadIfNeeded(operand, true, ctx);
        }
        if (!operand) return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->op) {
        case UnaryOp::Neg:
            if (isIntegerType(operand->getType())) {
                result = ctx.builder.CreateNeg(operand, "neg");
            } else {
                result = ctx.builder.CreateFNeg(operand, "fneg");
            }
            break;

        case UnaryOp::Not:
            if (!isBoolValue(operand)) {
                operand = ctx.builder.CreateICmpNE(operand,
                    llvm::Constant::getNullValue(operand->getType()));
            }
            result = ctx.builder.CreateNot(operand, "not");
            break;

        case UnaryOp::BitNot:
            result = ctx.builder.CreateNot(operand, "bnot");
            break;

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, expr->loc,
                                    "unknown unary operator");
            return nullptr;
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Call Expression
// =============================================================================

llvm::Value* lowerCallExpr(CallExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* calleeVal = lowerExpression(expr->callee, ctx);
    if (!calleeVal) {
        return nullptr;
    }

    // ─── Lower arguments ───────────────────────────────────────────────────
    std::vector<llvm::Value*> args;
    for (ExprAST* arg : expr->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return nullptr;
        }
        if (arg->isLValue) {
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            if (elemType) {
                argVal = loadIfNeeded(argVal, elemType, ctx);
            } else {
                argVal = loadIfNeeded(argVal, true, ctx);
            }
            if (!argVal) return nullptr;
        }
        args.push_back(argVal);
    }

    // ─── Build the callable's real signature ───────────────────────────────
    // Sema (resolveCallExpr) already guarantees expr->callee resolves to a
    // FuncTypeAST - a value that's neither the closure struct shape nor a
    // callable pointer means CodeGen and Sema have gone out of sync, not
    // that the user wrote something uncallable.
    FuncTypeAST* calleeFuncType = expr->callee->resolvedType
        ? expr->callee->resolvedType->as<FuncTypeAST>()
        : nullptr;
    assert(calleeFuncType && "call callee does not resolve to a FuncTypeAST - "
                              "Sema should have caught this");
    if (!calleeFuncType) {
        return nullptr;
    }

    llvm::FunctionType* fnType = getFunctionType(ctx, calleeFuncType);
    if (!fnType) {
        return nullptr;
    }

    // emitCallableCall (closure/CodeGenClosure.hpp) discriminates closure
    // values, plain llvm::Function references, and indirect function
    // pointers - the same shared dispatch lowerPipelineStep and
    // createCompositionWrapper use, instead of a fourth independent copy
    // of that three-way check here.
    llvm::Value* result = emitCallableCall(calleeVal, args, fnType, ctx, "call");
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Intrinsic Call Expression
// =============================================================================

llvm::Value* lowerIntrinsicCallExpr(IntrinsicCallExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    return emitIntrinsicFromAST(expr, ctx);
}

// =============================================================================
// Index Expression
// =============================================================================

llvm::Value* lowerIndexExpr(IndexExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* target = lowerExpression(expr->target, ctx);
    if (!target) {
        return nullptr;
    }

    llvm::Value* index = lowerExpression(expr->index, ctx);
    if (!index) {
        return nullptr;
    }

    if (expr->index->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->index->resolvedType);
        if (elemType) {
            index = loadIfNeeded(index, elemType, ctx);
        } else {
            index = loadIfNeeded(index, true, ctx);
        }
        if (!index) return nullptr;
    }

    ArrayTypeAST* arrayType = expr->target->resolvedType->as<ArrayTypeAST>();
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "target is not an array type");
        return nullptr;
    }

    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (!elemType) {
        return nullptr;
    }

    // ─── Get pointer to array data ──────────────────────────────────────
    llvm::Value* ptr = target;
    if (arrayType->isFixed()) {
        ptr = ctx.builder.CreateConstGEP2_32(elemType, target, 0, 0);
    }

    // ─── Get array length ──────────────────────────────────────────────
    llvm::Value* len = getArrayLength(target, arrayType, ctx);
    if (!len) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "could not determine array length");
        return nullptr;
    }

    // ─── Bounds check ──────────────────────────────────────────────────
    llvm::Value* checkedIndex = emitBoundsCheck(index, len, ctx);
    if (!checkedIndex) return nullptr;

    // ─── GEP and load ──────────────────────────────────────────────────
    llvm::Value* gep = ctx.builder.CreateGEP(elemType, ptr, checkedIndex, "array_idx");

    if (expr->isLValue) {
        expr->llvmValue = gep;
        return gep;
    }

    llvm::Value* result = ctx.builder.CreateLoad(elemType, gep, "array_load");
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Slice Expression
// =============================================================================

llvm::Value* lowerSliceExpr(SliceExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* target = lowerExpression(expr->target, ctx);
    if (!target) {
        return nullptr;
    }

    ArrayTypeAST* arrayType = expr->target->resolvedType->as<ArrayTypeAST>();
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "target is not an array type");
        return nullptr;
    }

    // ─── Get array length ──────────────────────────────────────────────
    llvm::Value* len = getArrayLength(target, arrayType, ctx);
    if (!len) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "could not determine array length");
        return nullptr;
    }

    // ─── Resolve start and end bounds ──────────────────────────────────
    llvm::Value* start = nullptr;
    llvm::Value* end = nullptr;

    if (expr->start) {
        start = lowerExpression(expr->start, ctx);
        if (!start) return nullptr;
        if (expr->start->isLValue) {
            llvm::Type* elemType = getType(ctx, expr->start->resolvedType);
            if (elemType) {
                start = loadIfNeeded(start, elemType, ctx);
            } else {
                start = loadIfNeeded(start, true, ctx);
            }
            if (!start) return nullptr;
        }
        if (start->getType() != len->getType()) {
            start = ctx.builder.CreateIntCast(start, len->getType(), true, "start_cast");
        }
    } else {
        start = llvm::ConstantInt::get(len->getType(), 0);
    }

    if (expr->end) {
        end = lowerExpression(expr->end, ctx);
        if (!end) return nullptr;
        if (expr->end->isLValue) {
            llvm::Type* elemType = getType(ctx, expr->end->resolvedType);
            if (elemType) {
                end = loadIfNeeded(end, elemType, ctx);
            } else {
                end = loadIfNeeded(end, true, ctx);
            }
            if (!end) return nullptr;
        }
        if (end->getType() != len->getType()) {
            end = ctx.builder.CreateIntCast(end, len->getType(), true, "end_cast");
        }
    } else {
        end = len;
    }

    // ─── Slice bounds check ──────────────────────────────────────────────
    auto [checkedStart, checkedEnd] = emitSliceBoundsCheck(start, end, len, ctx);
    if (!checkedStart || !checkedEnd) return nullptr;

    // ─── Get data pointer ──────────────────────────────────────────────────
    llvm::Value* dataPtr = target;
    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (arrayType->isFixed()) {
        dataPtr = ctx.builder.CreateConstGEP2_32(elemType, target, 0, 0);
    }

    // ─── Offset data pointer by start ──────────────────────────────────────
    llvm::Value* slicePtr = ctx.builder.CreateGEP(elemType, dataPtr, checkedStart, "slice_ptr");

    // ─── Calculate length: end - start ──────────────────────────────────────
    llvm::Value* sliceLen = ctx.builder.CreateSub(checkedEnd, checkedStart, "slice_len");

    // ─── Calculate capacity: len - start ────────────────────────────────────
    llvm::Value* sliceCap = ctx.builder.CreateSub(len, checkedStart, "slice_cap");

    // ─── Build the slice struct ─────────────────────────────────────────────
    llvm::StructType* sliceType = llvm::cast<llvm::StructType>(getType(ctx, expr->resolvedType));
    if (!sliceType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->loc,
                                "could not create slice type");
        return nullptr;
    }

    llvm::Value* slice = llvm::UndefValue::get(sliceType);
    slice = ctx.builder.CreateInsertValue(slice, slicePtr, 0);
    slice = ctx.builder.CreateInsertValue(slice, sliceLen, 1);
    slice = ctx.builder.CreateInsertValue(slice, sliceCap, 2);

    expr->llvmValue = slice;
    return slice;
}

// =============================================================================
// Field Access Expression
// =============================================================================

llvm::Value* lowerFieldAccessExpr(FieldAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── 1. Lower the object expression ─────────────────────────────────────
    llvm::Value* object = lowerExpression(expr->object, ctx);
    if (!object) {
        return nullptr;
    }

    // ─── 2. Get the type of the object ─────────────────────────────────────
    TypeAST* objectType = expr->object->resolvedType;
    if (!objectType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->object->loc,
                                "object has no resolved type");
        return nullptr;
    }

    // ─── 3. Determine what kind of access this is ──────────────────────────
    // Case 1: Enum variant access (e.g., Direction.North)
    // Case 2: Struct field access (e.g., point.x)
    // Case 3: Module access (handled by ModuleAccessExprAST)
    
    // Check if the object is a type name (enum access)
    bool isEnumAccess = false;
    TypeDeclAST* typeDecl = nullptr;
    
    if (objectType->isa<NamedTypeAST>()) {
        NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
        typeDecl = namedType->resolvedDecl;
        if (typeDecl && typeDecl->isa<EnumDeclAST>()) {
            isEnumAccess = true;
        }
    }

    // ─── 4a. Enum variant access ────────────────────────────────────────────
    if (isEnumAccess) {
        EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();
        
        // Find the variant by name
        EnumVariantAST* variant = nullptr;
        for (EnumVariantAST* v : enumDecl->variants) {
            if (v->name == expr->fieldName) {
                variant = v;
                break;
            }
        }
        
        if (!variant) {
            ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, expr->loc,
                                    "enum '", ctx.pool.lookup(enumDecl->name), 
                                    "' has no variant '", ctx.pool.lookup(expr->fieldName), "'");
            return nullptr;
        }
        
        // Get the LLVM constant for the variant
        llvm::ConstantInt* variantConst = enumDecl->constantForVariant(expr->fieldName);
        if (!variantConst) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                    "enum variant '", ctx.pool.lookup(expr->fieldName), 
                                    "' has no LLVM constant");
            return nullptr;
        }
        
        expr->llvmValue = variantConst;
        return variantConst;
    }

    // ─── 4b. Struct field access ────────────────────────────────────────────
    // The object should be a struct type
    StructDeclAST* structDecl = nullptr;
    
    // Unwrap nullable/fallible if present
    TypeAST* innerType = objectType;
    if (objectType->isa<NullableTypeAST>()) {
        innerType = objectType->as<NullableTypeAST>()->inner;
    } else if (objectType->isa<FallibleTypeAST>()) {
        innerType = objectType->as<FallibleTypeAST>()->inner;
    } else if (objectType->isa<CombinedTypeAST>()) {
        innerType = objectType->as<CombinedTypeAST>()->inner;
    }
    
    if (innerType->isa<NamedTypeAST>()) {
        NamedTypeAST* namedType = innerType->as<NamedTypeAST>();
        TypeDeclAST* decl = namedType->resolvedDecl;
        if (decl && decl->isa<StructDeclAST>()) {
            structDecl = decl->as<StructDeclAST>();
        }
    }
    
    if (!structDecl) {
        ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, expr->loc,
                                "cannot access field on non-struct type");
        return nullptr;
    }

    // ─── 5. Find the field in the struct ────────────────────────────────────
    FieldDeclAST* field = nullptr;
    size_t fieldIndex = 0;
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        if (structDecl->fields[i]->name == expr->fieldName) {
            field = structDecl->fields[i];
            fieldIndex = i;
            break;
        }
    }
    
    if (!field) {
        ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, expr->loc,
                                "struct '", ctx.pool.lookup(structDecl->name), 
                                "' has no field '", ctx.pool.lookup(expr->fieldName), "'");
        return nullptr;
    }

    // ─── 6. Get the LLVM struct type ──────────────────────────────────────
    llvm::StructType* llvmStructType = ctx.lookupStruct(structDecl);
    if (!llvmStructType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "struct '", ctx.pool.lookup(structDecl->name), 
                                "' has no LLVM type");
        return nullptr;
    }

    // ─── 7. If the object is an l-value, we need the pointer ──────────────
    // If the object is already a pointer (from an l-value), we can GEP directly.
    // If it's a value (struct value), we need to take its address first.
    
    bool objectIsLValue = expr->object->isLValue;
    llvm::Value* structPtr = nullptr;
    llvm::Value* fieldPtr = nullptr;
    llvm::Type* fieldType = llvmStructType->getElementType(fieldIndex);

    if (objectIsLValue) {
        // Object is an l-value - we have a pointer to the struct
        structPtr = object;
        
        // GEP to the field
        std::vector<llvm::Value*> indices = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 0),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), static_cast<uint32_t>(fieldIndex))
        };
        fieldPtr = ctx.builder.CreateInBoundsGEP(
            llvmStructType,
            structPtr,
            indices,
            "field_ptr_" + ctx.pool.lookup(field->name)
        );
    } else {
        // Object is a value - we need to extract the field directly
        // If the object is a struct value (not a pointer), we can extract
        if (object->getType()->isStructTy()) {
            // Extract the field value directly
            llvm::Value* fieldVal = ctx.builder.CreateExtractValue(
                object,
                static_cast<unsigned>(fieldIndex),
                "field_val_" + ctx.pool.lookup(field->name)
            );
            expr->llvmValue = fieldVal;
            return fieldVal;
        } else {
            // Object is a pointer to a struct - GEP and load
            structPtr = object;
            
            std::vector<llvm::Value*> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 0),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), static_cast<uint32_t>(fieldIndex))
            };
            fieldPtr = ctx.builder.CreateInBoundsGEP(
                llvmStructType,
                structPtr,
                indices,
                "field_ptr_" + ctx.pool.lookup(field->name)
            );
            
            // Load the field value
            llvm::Value* fieldVal = ctx.builder.CreateLoad(
                fieldType,
                fieldPtr,
                "field_load_" + ctx.pool.lookup(field->name)
            );
            
            // If this is an l-value (for assignment), return the pointer
            if (expr->isLValue) {
                expr->llvmValue = fieldPtr;
                return fieldPtr;
            }
            
            expr->llvmValue = fieldVal;
            return fieldVal;
        }
    }

    // ─── 8. Load the field value if not an l-value ─────────────────────────
    if (expr->isLValue) {
        expr->llvmValue = fieldPtr;
        return fieldPtr;
    }

    llvm::Value* fieldVal = ctx.builder.CreateLoad(
        fieldType,
        fieldPtr,
        "field_load_" + ctx.pool.lookup(field->name)
    );
    
    expr->llvmValue = fieldVal;
    return fieldVal;
}

// =============================================================================
// Module Access Expression
// =============================================================================

llvm::Value* lowerModuleAccessExpr(ModuleAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── 1. Get the resolved declaration ───────────────────────────────────
    // Sema should have set resolvedDecl during semantic analysis
    ValueDeclAST* resolvedDecl = expr->resolvedDecl;
    if (!resolvedDecl) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "module member '", ctx.pool.lookup(expr->moduleName),
                                ":", ctx.pool.lookup(expr->memberName), 
                                "' was not resolved");
        return nullptr;
    }

    // ─── 2. Handle function access ──────────────────────────────────────────
    if (resolvedDecl->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = resolvedDecl->as<FuncDeclAST>();
        
        // Check if this is a generic function that needs specialization
        if (!funcDecl->genericParams.empty() && !expr->genericArgs.empty()) {
            // Build type arguments from the expression's generic args
            std::vector<TypeAST*> typeArgs;
            typeArgs.reserve(expr->genericArgs.size());
            for (TypeAST* arg : expr->genericArgs) {
                typeArgs.push_back(arg);
            }
            
            // Use the existing generic helper
            llvm::Function* specializedFunc = getOrCreateSpecializedFunction(
                funcDecl, 
                typeArgs, 
                ctx
            );
            
            if (!specializedFunc) {
                ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, expr->loc,
                                        "failed to instantiate generic function '",
                                        ctx.pool.lookup(funcDecl->name), "'");
                return nullptr;
            }
            
            expr->llvmValue = specializedFunc;
            return specializedFunc;
        }
        
        // Regular function - look it up in the function map
        llvm::Function* func = ctx.lookupFunction(funcDecl);
        if (!func) {
            ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, expr->loc,
                                    "function '", ctx.pool.lookup(funcDecl->name), 
                                    "' not found in codegen cache");
            return nullptr;
        }
        
        expr->llvmValue = func;
        return func;
    }

    // ─── 3. Handle variable/constant access ─────────────────────────────────
    if (resolvedDecl->isa<VarDeclAST>()) {
        VarDeclAST* varDecl = resolvedDecl->as<VarDeclAST>();
        
        // Look up the global variable in the context
        llvm::Value* global = ctx.lookupValue(varDecl);
        if (!global) {
            ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, expr->loc,
                                    "global variable '", ctx.pool.lookup(varDecl->name), 
                                    "' not found in codegen cache");
            return nullptr;
        }
        
        // If this is an l-value (for assignment), return the pointer
        if (expr->isLValue) {
            expr->llvmValue = global;
            return global;
        }
        
        // Otherwise, load the value
        llvm::Type* varType = getType(ctx, varDecl->type);
        if (!varType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                    "variable '", ctx.pool.lookup(varDecl->name), 
                                    "' has no type");
            return nullptr;
        }
        
        llvm::Value* loaded = ctx.builder.CreateLoad(varType, global, 
                                                      "module_load_" + ctx.pool.lookup(varDecl->name));
        expr->llvmValue = loaded;
        return loaded;
    }

    // ─── 4. Handle enum variant access ──────────────────────────────────────
    if (resolvedDecl->isa<EnumVariantAST>()) {
        EnumVariantAST* variant = resolvedDecl->as<EnumVariantAST>();
        
        // Use the variant's llvmValue if already set
        if (variant->llvmValue) {
            expr->llvmValue = variant->llvmValue;
            return variant->llvmValue;
        }
        
        // Otherwise, create a constant from the variant's value
        llvm::Type* enumType = getType(ctx, expr->resolvedType);
        if (!enumType || !enumType->isIntegerTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                    "enum variant '", ctx.pool.lookup(variant->name), 
                                    "' has invalid type");
            return nullptr;
        }
        
        // Create the constant - explicitly cast to ConstantInt*
        llvm::Constant* constVal = llvm::ConstantInt::get(
            enumType, 
            static_cast<uint64_t>(variant->value), 
            true  // signed
        );
        
        // Store as ConstantInt* (safe cast since ConstantInt::get returns ConstantInt*)
        variant->llvmValue = llvm::cast<llvm::ConstantInt>(constVal);
        expr->llvmValue = variant->llvmValue;
        return variant->llvmValue;
    }

    // ─── 5. Unknown declaration type ───────────────────────────────────────
    ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                            "module member '", ctx.pool.lookup(expr->moduleName),
                            ":", ctx.pool.lookup(expr->memberName), 
                            "' has unknown declaration type");
    return nullptr;
}

// =============================================================================
// Null Coalesce Expression
// =============================================================================

llvm::Value* lowerNullCoalesceExpr(NullCoalesceExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* lhs = lowerExpression(expr->value, ctx);
    if (!lhs) {
        return nullptr;
    }

    llvm::Value* rhs = lowerExpression(expr->fallback, ctx);
    if (!rhs) {
        return nullptr;
    }

    TypeAST* lhsType = expr->value->resolvedType;
    if (!lhsType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->value->loc,
                                "LHS has no type");
        return nullptr;
    }

    if (!sema::isNullableType(lhsType) && !sema::isFallibleType(lhsType)) {
        expr->llvmValue = lhs;
        return lhs;
    }

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* lhsBlock = ctx.builder.GetInsertBlock();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "then", func);
    llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "else", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "merge", func);

    // ─── Check the tag ────────────────────────────────────────────────────
    llvm::Value* tag = ctx.builder.CreateExtractValue(lhs, 0);
    llvm::Value* isNil = ctx.builder.CreateICmpEQ(
        tag,
        llvm::ConstantInt::get(tag->getType(), 0),
        "is_nil"
    );

    ctx.builder.CreateCondBr(isNil, elseBlock, thenBlock);

    // ─── Then block (LHS is valid) ──────────────────────────────────────
    ctx.builder.SetInsertPoint(thenBlock);
    llvm::Value* lhsValid = ctx.builder.CreateExtractValue(lhs, 1);
    ctx.builder.CreateBr(mergeBlock);

    // ─── Else block (LHS is nil/err) ────────────────────────────────────
    ctx.builder.SetInsertPoint(elseBlock);
    llvm::Value* rhsValue = rhs;
    if (expr->fallback->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->fallback->resolvedType);
        if (elemType) {
            rhsValue = loadIfNeeded(rhsValue, elemType, ctx);
        } else {
            rhsValue = loadIfNeeded(rhsValue, true, ctx);
        }
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(
        lhsValid->getType(),
        2,
        "coalesce"
    );
    phi->addIncoming(lhsValid, thenBlock);
    phi->addIncoming(rhsValue, elseBlock);

    expr->llvmValue = phi;
    return phi;
}

// =============================================================================
// Assignment Expression
// =============================================================================

llvm::Value* lowerAssignExpr(AssignExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* lhs = lowerExpression(expr->lhs, ctx);
    if (!lhs) {
        return nullptr;
    }

    if (!expr->lhs->isLValue) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, expr->lhs->loc,
                                "assignment target is not an l-value");
        return nullptr;
    }

    llvm::Value* rhs = lowerExpression(expr->rhs, ctx);
    if (!rhs) {
        return nullptr;
    }

    if (expr->rhs->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->rhs->resolvedType);
        if (elemType) {
            rhs = loadIfNeeded(rhs, elemType, ctx);
        } else {
            rhs = loadIfNeeded(rhs, true, ctx);
        }
        if (!rhs) return nullptr;
    }

    // ─── Handle compound assignment ──────────────────────────────────────
    if (expr->op != AssignOp::Assign) {
        llvm::Type* elemType = getType(ctx, expr->lhs->resolvedType);
        llvm::Value* lhsValue = loadIfNeeded(lhs, elemType, ctx);
        if (!lhsValue) {
            return nullptr;
        }

        switch (expr->op) {
            case AssignOp::AddAssign:
                if (isIntegerType(lhsValue->getType())) {
                    rhs = ctx.builder.CreateAdd(lhsValue, rhs, "add");
                } else {
                    rhs = ctx.builder.CreateFAdd(lhsValue, rhs, "fadd");
                }
                break;
            case AssignOp::SubAssign:
                if (isIntegerType(lhsValue->getType())) {
                    rhs = ctx.builder.CreateSub(lhsValue, rhs, "sub");
                } else {
                    rhs = ctx.builder.CreateFSub(lhsValue, rhs, "fsub");
                }
                break;
            case AssignOp::MulAssign:
                if (isIntegerType(lhsValue->getType())) {
                    rhs = ctx.builder.CreateMul(lhsValue, rhs, "mul");
                } else {
                    rhs = ctx.builder.CreateFMul(lhsValue, rhs, "fmul");
                }
                break;
            case AssignOp::DivAssign:
                if (isIntegerType(lhsValue->getType())) {
                    // Check for division by zero
                    RuntimeErrorKind kind = RuntimeErrorKind::DivisionByZero;
                    llvm::Value* checkedDivisor = emitZeroCheck(rhs, kind, ctx);
                    if (!checkedDivisor) return nullptr;
                    rhs = ctx.builder.CreateSDiv(lhsValue, checkedDivisor, "sdiv");
                } else {
                    rhs = ctx.builder.CreateFDiv(lhsValue, rhs, "fdiv");
                }
                break;
            default:
                ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, expr->loc,
                                        "unsupported compound assignment");
                return nullptr;
        }
    }

    ctx.builder.CreateStore(rhs, lhs);
    expr->llvmValue = rhs;
    return rhs;
}

// =============================================================================
// Pipeline Expression
// =============================================================================

llvm::Value* lowerPipelineExpr(PipelineExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* currentValue = lowerExpression(expr->seed, ctx);
    if (!currentValue) {
        return nullptr;
    }

    if (expr->seed->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->seed->resolvedType);
        if (elemType) {
            currentValue = loadIfNeeded(currentValue, elemType, ctx);
        } else {
            currentValue = loadIfNeeded(currentValue, true, ctx);
        }
    }

    for (PipelineStepAST* step : expr->steps) {
        // ─── FIX: Pass currentValue to lowerPipelineStep ─────────────────
        currentValue = lowerPipelineStep(step, currentValue, ctx);
        if (!currentValue) {
            return nullptr;
        }
    }

    expr->llvmValue = currentValue;
    return currentValue;
}

llvm::Value* lowerPipelineStep(PipelineStepAST* step, llvm::Value* upstreamValue, CodeGenContext& ctx) {
    if (!step) return nullptr;

    // Get the function type from Sema, NOT guessed from args
    TypeAST* callableType = step->callable->resolvedType;
    if (!callableType || !callableType->isa<FuncTypeAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_NotCallable, step->callable->loc,
                                "pipeline step callable is not a function type");
        return nullptr;
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    // ─── Get the LLVM function type (same as lowerCallExpr uses) ──────────
    llvm::FunctionType* fnType = getFunctionType(ctx, funcType, /* isClosure */ false);
    if (!fnType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, step->callable->loc,
                                "could not get function type for pipeline step");
        return nullptr;
    }

    // Build argument list: upstream + packArgs (in order)
    std::vector<llvm::Value*> args;

    // ─── Upstream value is passed FIRST (if the function takes any params) ──
    // If the function takes parameters, upstream is injected as the first arg.
    // If the function takes no params, upstream is discarded (valid).
    bool hasUpstream = (upstreamValue != nullptr);
    bool hasParameters = !funcType->params.empty();

    if (hasUpstream && hasParameters) {
        args.push_back(upstreamValue);
    } else if (hasUpstream && !hasParameters) {
        // Upstream is discarded - no warning needed (Sema already warned)
    }

    // ─── Lower pack arguments ──────────────────────────────────────────────
    for (ExprAST* arg : step->packArgs) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return nullptr;
        }
        if (arg->isLValue) {
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            if (elemType) {
                argVal = loadIfNeeded(argVal, elemType, ctx);
            } else {
                argVal = loadIfNeeded(argVal, true, ctx);
            }
            if (!argVal) return nullptr;
        }
        args.push_back(argVal);
    }

    // Truncate excess non-variadic arguments
    size_t paramCount = funcType->params.size();
    bool hasVariadic = !funcType->params.empty() && funcType->params.back()->isVariadic;

    if (!hasVariadic && args.size() > paramCount) {
        // Sema already validated this is safe - just truncate
        args.resize(paramCount);
    }

    // =========================================================================
    // Variadic parameter handling - collect tail into slice
    // =========================================================================
    // If the function has a variadic parameter, we need to collect all
    // remaining arguments (after the fixed parameters) into a slice value.
    // 
    // The variadic parameter's type is [*]T (dynamic array) - we need to:
    //   1. Allocate memory for the slice
    //   2. Store each remaining argument into the slice
    //   3. Pass the slice as the variadic parameter
    //
    // For simplicity in this fix, we assume the runtime provides a helper
    // function to create a slice from a list of values.
    // 
    // TODO: Implement proper slice construction for variadic parameters.
    // This requires:
    //   - Determining the element type from the variadic parameter
    //   - Allocating a dynamic array
    //   - Storing each argument into the array
    //   - Returning a {ptr, len, cap} slice struct

    if (hasVariadic) {
        // ─── Get the variadic parameter ─────────────────────────────────────
        ParamAST* variadicParam = funcType->params.back();
        llvm::Type* variadicLLVMType = getType(ctx, variadicParam->type);
        if (!variadicLLVMType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, step->callable->loc,
                                    "variadic parameter has no LLVM type");
            return nullptr;
        }

        // ─── The variadic parameter is [*]T - we need to build a slice ────
        // For now, we need to handle this properly. The simple approach:
        //   1. Determine how many variadic arguments there are
        //   2. Allocate a dynamic array of the element type
        //   3. Store each argument into the array
        //   4. Pass the slice as the variadic parameter

        // ─── Get element type from the variadic parameter type ────────────
        llvm::Type* elemType = nullptr;
        if (variadicLLVMType->isPointerTy()) {
            // For [*]T, we need the element type T
            // With opaque pointers, we need to get it from the AST
            if (variadicParam->type->isa<ArrayTypeAST>()) {
                TypeAST* elementTypeAST = variadicParam->type->as<ArrayTypeAST>()->element;
                elemType = getType(ctx, elementTypeAST);
            }
        }

        if (!elemType) {
            // Fallback: use i8
            elemType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }

        // ─── Determine which arguments go to the variadic parameter ──────
        // Fixed parameters are the first (paramCount - 1) arguments
        size_t fixedParamCount = paramCount - 1;
        size_t variadicArgCount = args.size() - fixedParamCount;

        // ─── Build a slice from the variadic arguments ────────────────────
        // For now, we use a runtime helper. In a full implementation, this
        // would be inlined.
        //
        // We need to:
        //   1. Allocate memory for the slice
        //   2. Store each variadic argument into the slice
        //   3. Return a {ptr, len, cap} struct
        //
        // The runtime helper: __lucid_slice_from_values(ptr, len, cap)
        // 
        // TODO: Proper slice construction

        // ─── For now, create an empty slice (placeholder) ─────────────────
        // This is a placeholder - real implementation would build the slice.
        // We emit a warning that variadic pipeline support is limited.
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, step->callable->loc,
                                  "variadic parameters in pipeline steps have limited support");

        // ─── Build a slice from the variadic arguments ─────────────────────
        // We need to collect the variadic arguments into an array and create
        // a slice from them. For now, we use the runtime helper.
        if (variadicArgCount > 0) {
            // Allocate an array to store the variadic arguments
            llvm::Type* arrayType = llvm::ArrayType::get(elemType, variadicArgCount);
            llvm::Value* array = llvm::UndefValue::get(arrayType);

            // Store each variadic argument into the array
            for (size_t i = 0; i < variadicArgCount; ++i) {
                llvm::Value* val = args[fixedParamCount + i];
                // FIXME: Need to cast val to elemType if needed
                array = ctx.builder.CreateInsertValue(array, val, i, "variadic_elem");
            }

            // Allocate the array on the heap
            llvm::Value* arraySize = llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx.llvmCtx),
                variadicArgCount
            );
            llvm::Type* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);
            llvm::Function* allocFn = ctx.getRuntimeFn(RuntimeFn::Alloc);
            llvm::Value* arrayPtr = ctx.builder.CreateCall(allocFn, {arraySize});

            // Store the array elements into the allocated memory
            // For each element, store it into the allocated array
            for (size_t i = 0; i < variadicArgCount; ++i) {
                llvm::Value* idx = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx.llvmCtx),
                    i
                );
                llvm::Value* gep = ctx.builder.CreateGEP(
                    elemType,
                    arrayPtr,
                    idx,
                    "variadic_ptr"
                );
                llvm::Value* val = args[fixedParamCount + i];
                if (val->getType() != elemType) {
                    val = ctx.builder.CreateBitCast(val, elemType);
                }
                ctx.builder.CreateStore(val, gep);
            }

            // Build the slice struct { ptr, len, cap }
            llvm::StructType* sliceType = ctx.getStringType();  // Reuse string type {ptr, len, cap}
            llvm::Value* slice = llvm::UndefValue::get(sliceType);
            slice = ctx.builder.CreateInsertValue(slice, arrayPtr, 0);
            slice = ctx.builder.CreateInsertValue(slice, arraySize, 1);
            slice = ctx.builder.CreateInsertValue(slice, arraySize, 2);

            // Replace the variadic arguments with the slice in the args list
            args.resize(fixedParamCount + 1);
            args[fixedParamCount] = slice;
        } else {
            // No variadic arguments - pass null slice
            llvm::StructType* sliceType = ctx.getStringType();
            args.resize(fixedParamCount + 1);
            args[fixedParamCount] = llvm::Constant::getNullValue(sliceType);
        }
    }

    llvm::Value* calleeVal = lowerExpression(step->callable, ctx);
    if (!calleeVal) {
        return nullptr;
    }

    if (step->callable->isLValue) {
        llvm::Type* elemType = getType(ctx, step->callable->resolvedType);
        if (elemType) {
            calleeVal = loadIfNeeded(calleeVal, elemType, ctx);
        } else {
            calleeVal = loadIfNeeded(calleeVal, true, ctx);
        }
        if (!calleeVal) return nullptr;
    }

    // ─── Verify argument count matches function signature ────────────────
    if (args.size() != fnType->getNumParams()) {
        // Sema should have validated this, but we truncate just in case
        // This can happen with variadic parameters that we've already handled
        if (args.size() > fnType->getNumParams()) {
            args.resize(fnType->getNumParams());
        } else {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, step->callable->loc,
                                    "pipeline step argument count mismatch: expected ",
                                    fnType->getNumParams(), ", got ", args.size());
            return nullptr;
        }
    }

    // emitCallableCall (closure/CodeGenClosure.hpp) discriminates closure
    // values, plain llvm::Function references, and indirect function
    // pointers - the same shared dispatch lowerCallExpr and
    // createCompositionWrapper use.
    return emitCallableCall(calleeVal, args, fnType, ctx, "pipeline_call");
}

// =============================================================================
// Compose Operand
// =============================================================================

llvm::Value* lowerComposeOperand(ComposeOperandAST* operand, CodeGenContext& ctx) {
    if (!operand) return nullptr;

    // ─── 1. Lower the callable expression ──────────────────────────────────
    llvm::Value* callable = lowerExpression(operand->callable, ctx);
    if (!callable) {
        return nullptr;
    }

    // ─── 2. If it's an l-value, load it ────────────────────────────────────
    if (operand->callable->isLValue) {
        llvm::Type* elemType = getType(ctx, operand->callable->resolvedType);
        if (elemType) {
            callable = loadIfNeeded(callable, elemType, ctx);
        } else {
            callable = loadIfNeeded(callable, true, ctx);
        }
        if (!callable) return nullptr;
    }

    // ─── 3. Validate the shape ──────────────────────────────────────────────
    // A composition operand is either a plain callable (a bare
    // llvm::Function* or an indirect function pointer) or a closure value
    // (the { funcPtr, envPtr } fat-pointer struct from lowerClosure) -
    // both are legal per resolveComposeOperand (single parameter, no
    // variadics; nothing there requires a bare function). Leave the value
    // exactly as lowered - emitCallableCall (used by
    // createCompositionWrapper) discriminates the shapes itself, the same
    // way lowerCallExpr/lowerPipelineStep already do. The old
    // isFunctionTy() special-case is gone: nothing in this codebase
    // actually represents a callable as a bare LLVM function *value*
    // needing a cast - named functions are llvm::Function* globals,
    // closures are structs, and neither takes that path.
    if (!callable->getType()->isPointerTy() && !callable->getType()->isStructTy()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_NotCallable, operand->loc,
                                "composition operand is not callable");
        return nullptr;
    }

    return callable;  // Return the value, don't store on operand
}

// =============================================================================
// Compose Expression
// =============================================================================

/// @brief Create a wrapper function that composes two functions: f +> g
/// @param f The first function (returns R) - a closure value or plain
///        callable, dispatched via emitCallableCall.
/// @param fType The function type of f (T -> R)
/// @param g The second function (takes R) - a closure value or plain
///        callable, dispatched via emitCallableCall.
/// @param gType The function type of g (R -> U)
/// @param ctx The code generation context
/// @return A function pointer to the composed function (T -> U)
static llvm::Function* createCompositionWrapper(
    llvm::Value* f,
    FuncTypeAST* fType,
    llvm::Value* g,
    FuncTypeAST* gType,
    CodeGenContext& ctx
) {
    if (!f || !fType || !g || !gType) return nullptr;

    // ─── 1. Build f's and g's real LLVM function types ─────────────────────
    // Reuses CodeGenType.cpp's canonical FuncTypeAST -> llvm::FunctionType
    // mapping (getFunctionType) instead of hand-rebuilding param/return
    // types here - same fact, one source, and it's also what
    // emitCallableCall needs to call each of f/g correctly regardless of
    // whether they're closures or plain functions.
    llvm::FunctionType* fLLVMType = getFunctionType(ctx, fType);
    if (!fLLVMType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, fType->loc,
                                "invalid function type in composition (left operand)");
        return nullptr;
    }
    llvm::FunctionType* gLLVMType = getFunctionType(ctx, gType);
    if (!gLLVMType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, gType->loc,
                                "invalid function type in composition (right operand)");
        return nullptr;
    }

    llvm::Type* returnType = gLLVMType->getReturnType();

    // ─── 2. Build the wrapper's own LLVM function type ─────────────────────
    // The wrapper takes f's parameters and returns g's return type.
    llvm::FunctionType* composedFuncType = llvm::FunctionType::get(
        returnType,
        fLLVMType->params(),
        false
    );

    // ─── 3. Generate a unique name for the wrapper ─────────────────────────
    static int wrapperCounter = 0;
    std::string wrapperName = "_compose_wrapper_" + std::to_string(wrapperCounter++);

    // ─── 4. Create the LLVM function ────────────────────────────────────────
    llvm::Function* wrapper = llvm::Function::Create(
        composedFuncType,
        llvm::Function::InternalLinkage,
        wrapperName,
        ctx.module
    );

    // ─── 5. Set parameter names ─────────────────────────────────────────────
    size_t paramIndex = 0;
    for (ParamAST* param : fType->params) {
        if (paramIndex < wrapper->arg_size()) {
            wrapper->getArg(paramIndex)->setName(ctx.pool.lookup(param->name));
            paramIndex++;
        }
    }

    // ─── 6. Create entry block ──────────────────────────────────────────────
    // Building the wrapper's body means moving the builder's insertion
    // point into a different function - save the caller's insertion point
    // and restore it before returning, or every statement CodeGen emits
    // after this composition expression would land inside this wrapper
    // instead of the caller's actual function. (The previous version of
    // this function never restored it at all.)
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "entry",
        wrapper
    );
    llvm::IRBuilderBase::InsertPoint savedIP = ctx.builder.saveIP();
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── 7. Call f with the wrapper's own parameters ───────────────────────
    std::vector<llvm::Value*> fArgs;
    for (size_t i = 0; i < wrapper->arg_size(); ++i) {
        fArgs.push_back(wrapper->getArg(i));
    }

    llvm::Value* intermediate = emitCallableCall(f, fArgs, fLLVMType, ctx, "f_result");
    if (!intermediate) {
        ctx.builder.restoreIP(savedIP);
        wrapper->eraseFromParent();
        return nullptr;
    }

    // ─── 8. Call g with f's result ──────────────────────────────────────────
    // g takes exactly one parameter (the result of f) - already guaranteed
    // by resolveComposeOperand (composition operands have exactly one
    // parameter).
    llvm::Value* result = emitCallableCall(g, {intermediate}, gLLVMType, ctx, "g_result");
    if (!result) {
        ctx.builder.restoreIP(savedIP);
        wrapper->eraseFromParent();
        return nullptr;
    }

    // ─── 9. Return the result ────────────────────────────────────────────────
    ctx.builder.CreateRet(result);

    // ─── 10. Verify the wrapper function ────────────────────────────────────
    llvm::verifyFunction(*wrapper);

    // ─── 11. Restore the caller's insertion point ──────────────────────────
    ctx.builder.restoreIP(savedIP);

    return wrapper;
}

llvm::Value* lowerComposeExpr(ComposeExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Step 1: Lower left operand ──────────────────────────────────────
    if (!expr->left || !expr->left->isa<ComposeOperandAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_CompositionMismatch, expr->loc,
                                "invalid left operand in composition");
        return nullptr;
    }

    ComposeOperandAST* leftOperand = expr->left->as<ComposeOperandAST>();
    llvm::Value* leftFunc = lowerComposeOperand(leftOperand, ctx);
    if (!leftFunc) {
        return nullptr;
    }

    // ─── Step 2: Get the function type of the left operand ────────────────
    TypeAST* leftType = leftOperand->callable->resolvedType;
    if (!leftType || !leftType->isa<FuncTypeAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_CompositionMismatch, expr->left->loc,
                                "left operand is not a function type");
        return nullptr;
    }

    FuncTypeAST* leftFuncType = leftType->as<FuncTypeAST>();
    if (leftFuncType->isCurried()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_CompositionMismatch, expr->left->loc,
                                "left operand must have exactly one parameter group "
                                "(curried functions are not allowed in composition)");
        return nullptr;
    }

    // ─── Step 3: Compose each right operand sequentially ──────────────────
    // Sema already validated type compatibility, so CodeGen just needs to
    // create wrapper functions for each composition step.
    
    llvm::Value* currentFunc = leftFunc;
    FuncTypeAST* currentFuncType = leftFuncType;

    for (ComposeOperandAST* operand : expr->operands) {
        // ─── 3a. Lower the operand ──────────────────────────────────────
        llvm::Value* nextFunc = lowerComposeOperand(operand, ctx);
        if (!nextFunc) {
            return nullptr;
        }

        // ─── 3b. Get the function type of the operand ──────────────────
        TypeAST* nextType = operand->callable->resolvedType;
        if (!nextType || !nextType->isa<FuncTypeAST>()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_CompositionMismatch, operand->loc,
                                    "operand is not a function type");
            return nullptr;
        }

        FuncTypeAST* nextFuncType = nextType->as<FuncTypeAST>();
        if (nextFuncType->isCurried()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_CompositionMismatch, operand->loc,
                                    "operand must have exactly one parameter group "
                                    "(curried functions are not allowed in composition)");
            return nullptr;
        }

        // ─── 3c. Create a wrapper function that composes current +> next ──
        currentFunc = createCompositionWrapper(
            currentFunc,
            currentFuncType,
            nextFunc,
            nextFuncType,
            ctx
        );

        if (!currentFunc) {
            return nullptr;
        }

        // ─── 3d. Update current function type for the next iteration ────
        // The result type of the composition is the next function's return type
        // The parameter type is the current function's parameter type
        // We build this from the AST types for the next iteration.
        // Since we can't allocate a new FuncTypeAST in CodeGen (no arena),
        // we create a temporary one on the stack and use it only for the
        // next iteration's type information. This is safe because we only
        // need the type info during lowering, and we don't store it anywhere.
        FuncTypeAST composedType;
        composedType.params = currentFuncType->params;
        composedType.hasArrow = true;
        composedType.returnType = nextFuncType->returnType;

        // Update for next iteration - we need to keep the type info alive
        // since we reference it in the next iteration. We use a small vector
        // to store the composed types we create.
        // Alternatively, we could just use the nextFuncType's return type
        // directly and keep currentFuncType->params.
        // 
        // Actually, for the next iteration we need:
        //   params = currentFuncType->params (f's original params)
        //   returnType = nextFuncType->returnType (the latest return type)
        // 
        // We don't need to allocate a new FuncTypeAST because we can just
        // keep track of the parameter list separately.
        currentFuncType->returnType = nextFuncType->returnType;
        // currentFuncType->params stays the same
    }

    // ─── Step 4: Store the result on the expression ──────────────────────
    expr->llvmValue = currentFunc;
    return currentFunc;
}

// =============================================================================
// Anonymous Function Expression
// =============================================================================

llvm::Value* lowerAnonFuncExpr(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // lowerClosure now uniformly returns a { funcPtr, envPtr } fat pointer
    // for every anonymous function, capturing or not (a non-capturing one
    // just gets a null env pointer and skips the heap allocation). This
    // used to special-case the no-captures path here and return a bare
    // null pointer constant instead of actually compiling the function -
    // that made every non-capturing closure used as a value silently
    // uncallable. See CodeGenClosure.cpp for the uniform-shape rationale.
    return lowerClosure(expr, ctx);
}

// =============================================================================
// If Expression
// =============================================================================

llvm::Value* lowerIfExpr(IfExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* cond = lowerExpression(expr->condition, ctx);
    if (!cond) {
        return nullptr;
    }

    if (!isBoolValue(cond)) {
        cond = ctx.builder.CreateICmpNE(cond,
            llvm::Constant::getNullValue(cond->getType()));
    }

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_then", func);
    llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_else", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_merge", func);

    ctx.builder.CreateCondBr(cond, thenBlock, elseBlock);

    // ─── Then branch ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(thenBlock);
    llvm::Value* thenVal = lowerExpression(expr->thenBranch, ctx);
    if (!thenVal) {
        return nullptr;
    }
    if (expr->thenBranch->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->thenBranch->resolvedType);
        if (elemType) {
            thenVal = loadIfNeeded(thenVal, elemType, ctx);
        } else {
            thenVal = loadIfNeeded(thenVal, true, ctx);
        }
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Else branch ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(elseBlock);
    llvm::Value* elseVal = lowerExpression(expr->elseBranch, ctx);
    if (!elseVal) {
        return nullptr;
    }
    if (expr->elseBranch->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->elseBranch->resolvedType);
        if (elemType) {
            elseVal = loadIfNeeded(elseVal, elemType, ctx);
        } else {
            elseVal = loadIfNeeded(elseVal, true, ctx);
        }
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(thenVal->getType(), 2, "if");
    phi->addIncoming(thenVal, thenBlock);
    phi->addIncoming(elseVal, elseBlock);

    expr->llvmValue = phi;
    return phi;
}

// =============================================================================
// Range Expression
// =============================================================================

llvm::Value* lowerRangeExpr(RangeExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "range expression should not be used as a value");
    return nullptr;
}

} // namespace codegen
/// @file CodeGenExpr.cpp
/// @brief Implementation of expression lowering to LLVM IR.

#include "CodeGen.hpp"
#include "core/ASTStrings.hpp"
#include "support/CodeGenAlloca.hpp"
#include "support/CodeGenHelpers.hpp"
#include "support/CodeGenPanic.hpp"
#include "support/ArenaHelpers.hpp"
#include "types/LLVMTypeHelpers.hpp"
#include "codegen/runtime/RuntimeError.hpp"
#include "codegen/runtime/closure/CodeGenClosure.hpp"
#include "intrinsic/IntrinsicEmitter.hpp"
#include "sema/types/SemaType.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
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
        case ASTKind::LiteralExpr:       return lowerLiteralExpr(expr->as<LiteralExprAST>(), ctx);
        case ASTKind::IdentifierExpr:    return lowerIdentifierExpr(expr->as<IdentifierExprAST>(), ctx);
        case ASTKind::ArrayLiteralExpr:  return lowerArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx);
        case ASTKind::StructLiteralExpr: return lowerStructLiteralExpr(expr->as<StructLiteralExprAST>(), ctx);
        case ASTKind::BinaryExpr:        return lowerBinaryExpr(expr->as<BinaryExprAST>(), ctx);
        case ASTKind::UnaryExpr:         return lowerUnaryExpr(expr->as<UnaryExprAST>(), ctx);
        case ASTKind::CallExpr:          return lowerCallExpr(expr->as<CallExprAST>(), ctx);
        case ASTKind::IntrinsicCallExpr: return lowerIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), ctx);
        case ASTKind::IndexExpr:         return lowerIndexExpr(expr->as<IndexExprAST>(), ctx);
        case ASTKind::SliceExpr:         return lowerSliceExpr(expr->as<SliceExprAST>(), ctx);
        case ASTKind::FieldAccessExpr:   return lowerFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx);
        case ASTKind::ModuleAccessExpr:  return lowerModuleAccessExpr(expr->as<ModuleAccessExprAST>(), ctx);
        case ASTKind::ArenaAccessExpr:   return lowerArenaAccessExpr(expr->as<ArenaAccessExprAST>(), ctx);
        case ASTKind::NullCoalesceExpr:  return lowerNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), ctx);
        case ASTKind::AssignExpr:        return lowerAssignExpr(expr->as<AssignExprAST>(), ctx);
        case ASTKind::PipelineExpr:      return lowerPipelineExpr(expr->as<PipelineExprAST>(), ctx);
        case ASTKind::ComposeExpr:       return lowerComposeExpr(expr->as<ComposeExprAST>(), ctx);
        case ASTKind::AnonFuncExpr:      return lowerAnonFuncExpr(expr->as<AnonFuncExprAST>(), ctx);
        case ASTKind::IfExpr:            return lowerIfExpr(expr->as<IfExprAST>(), ctx);
        case ASTKind::RangeExpr:         return lowerRangeExpr(expr->as<RangeExprAST>(), ctx);
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

    // ─── Special case: `_` is the discard placeholder ──────────────────────
    if (ctx.pool.lookupView(expr->name) == "_") {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "cannot use '_' as a value");
        return nullptr;
    }

    ValueDeclAST* decl = expr->resolvedDecl;
    if (!decl) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "undefined identifier '", ctx.pool.lookup(expr->name), "'");
        return nullptr;
    }

    // ─── Handle implicit field access through self ─────────────────────
    // Sema sets isImplicitFieldAccess when an identifier resolves to a field
    // and 'self' is in scope. CodeGen transforms this to self.field.
    if (expr->isImplicitFieldAccess) {
        // ─── Guard: selfObject must be set ──────────────────────────────────
        // This should never happen if Sema is correct. If it does, it's a
        // bug in Sema that needs to be fixed, not silently ignored.
        if (!expr->selfObject) {
            ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, expr->loc,
                                    "implicit field access for '", ctx.pool.lookup(expr->name),
                                    "' has no self object - Sema bug");
            return nullptr;
        }

        // ─── Guard: decl must be a FieldDeclAST ─────────────────────────────
        if (!decl->isa<FieldDeclAST>()) {
            ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, expr->loc,
                                    "implicit field access resolved to non-field declaration '",
                                    ctx.pool.lookup(decl->name), "' - Sema bug");
            return nullptr;
        }

        // ─── Lower the self object ──────────────────────────────────────────
        llvm::Value* selfVal = lowerExpression(expr->selfObject, ctx);
        if (!selfVal) return nullptr;

        // ─── Get the field declaration ──────────────────────────────────────
        FieldDeclAST* fieldDecl = decl->as<FieldDeclAST>();

        // ─── Find the parent struct from self's type ───────────────────────
        StructDeclAST* structDecl = nullptr;
        TypeAST* selfType = expr->selfObject->resolvedType;
        if (selfType && selfType->isa<RefTypeAST>()) {
            RefTypeAST* refType = selfType->as<RefTypeAST>();
            if (refType->inner && refType->inner->isa<NamedTypeAST>()) {
                NamedTypeAST* namedType = refType->inner->as<NamedTypeAST>();
                if (namedType->resolvedDecl && namedType->resolvedDecl->isa<StructDeclAST>()) {
                    structDecl = namedType->resolvedDecl->as<StructDeclAST>();
                }
            }
        }

        // ─── Guard: structDecl must be found ────────────────────────────────
        if (!structDecl) {
            ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, expr->loc,
                                    "could not determine struct for implicit field access '",
                                    ctx.pool.lookup(expr->name), "' - Sema bug");
            return nullptr;
        }

        // ─── Get the LLVM struct type ───────────────────────────────────────
        llvm::StructType* llvmStructType = ctx.lookupStruct(structDecl);
        if (!llvmStructType) {
            ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, expr->loc,
                                    "struct '", ctx.pool.lookup(structDecl->name),
                                    "' has no LLVM type - Sema bug");
            return nullptr;
        }

        // ─── Get the field index ─────────────────────────────────────────────
        size_t fieldIndex = expr->fieldIndex;
        if (fieldIndex == SIZE_MAX) {
            fieldIndex = structDecl->indexOfField(fieldDecl->name);
            if (fieldIndex == SIZE_MAX) {
                ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, expr->loc,
                                        "field '", ctx.pool.lookup(fieldDecl->name),
                                        "' not found in struct '",
                                        ctx.pool.lookup(structDecl->name), "'");
                return nullptr;
            }
            // Cache the field index for future use
            expr->fieldIndex = fieldIndex;
        }

        // ─── GEP to the field ────────────────────────────────────────────────
        // selfVal is a pointer to the struct (from &self)
        std::vector<llvm::Value*> indices = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 0),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx),
                                    static_cast<uint32_t>(fieldIndex))
        };

        llvm::Value* fieldPtr = ctx.builder.CreateInBoundsGEP(
            llvmStructType,
            selfVal,
            indices,
            "implicit_field_ptr_" + ctx.pool.lookup(fieldDecl->name)
        );

        // ─── If this is an l-value, return the pointer ──────────────────────
        if (expr->isLValue) {
            expr->llvmValue = fieldPtr;
            return fieldPtr;
        }

        // ─── Otherwise, load the field value ────────────────────────────────
        llvm::Type* fieldType = llvmStructType->getElementType(fieldIndex);
        llvm::Value* fieldVal = ctx.builder.CreateLoad(
            fieldType,
            fieldPtr,
            "implicit_field_load_" + ctx.pool.lookup(fieldDecl->name)
        );

        expr->llvmValue = fieldVal;
        return fieldVal;
    }

    // ─── Normal identifier handling ──────────────────────────────────────────
    if (decl->isa<VarDeclAST>() || decl->isa<ParamAST>()) {
        llvm::Value* binding = ctx.lookupValue(decl);
        if (!binding) {
            ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, expr->loc,
                                    "identifier '", ctx.pool.lookup(expr->name),
                                    "' has no LLVM binding");
            return nullptr;
        }

        if (expr->isLValue) {
            expr->llvmValue = binding;
            return binding;
        }

        llvm::Type* type = getType(ctx, expr->resolvedType);
        if (!type) return nullptr;
        expr->llvmValue = ctx.builder.CreateLoad(
            type,
            binding,
            "load_" + ctx.pool.lookup(expr->name)
        );
        return expr->llvmValue;
    }

    if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        llvm::Function* func = ctx.lookupFunction(funcDecl);
        if (!func) {
            llvm::FunctionType* funcType = getFunctionType(
                ctx,
                funcDecl->funcType,
                funcDecl->hasClosure
            );
            if (!funcType) return nullptr;

            std::string funcName = funcDecl->isForeignFunction
                ? ctx.pool.lookup(funcDecl->name)
                : ctx.pool.lookup(funcDecl->mangledName);
            func = llvm::Function::Create(
                funcType,
                llvm::GlobalValue::ExternalLinkage,
                funcName,
                ctx.module
            );
            ctx.storeFunction(funcDecl, func);
        }

        expr->llvmValue = func;
        return func;
    }

    if (decl->isa<EnumVariantAST>()) {
        EnumVariantAST* variant = decl->as<EnumVariantAST>();
        if (variant->llvmValue) {
            expr->llvmValue = variant->llvmValue;
            return variant->llvmValue;
        }

        llvm::Type* enumType = getType(ctx, expr->resolvedType);
        if (!enumType || !enumType->isIntegerTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                    "enum variant '", ctx.pool.lookup(expr->name),
                                    "' has invalid type");
            return nullptr;
        }

        variant->llvmValue = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(
            enumType,
            static_cast<uint64_t>(variant->value),
            true
        ));
        expr->llvmValue = variant->llvmValue;
        return variant->llvmValue;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                            "identifier '", ctx.pool.lookup(expr->name),
                            "' has unsupported declaration type");
    return nullptr;
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
        }
    }
    if (expr->right->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->right->resolvedType);
        if (elemType) {
            right = loadIfNeeded(right, elemType, ctx);
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

        case BinaryOp::Div:
        case BinaryOp::Mod: {
            if (isIntegerType(left->getType()) && isIntegerType(right->getType())) {
                // ─── Check for division/modulo by zero ─────────────────────────────
                llvm::Value* isZero = ctx.builder.CreateICmpEQ(
                    right,
                    llvm::ConstantInt::get(right->getType(), 0),
                    "div_mod_by_zero_check"
                );
                
                llvm::Function* func = ctx.getCurrentFunction();
                llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                    ctx.llvmCtx, "div_mod_continue", func);
                
                RuntimeErrorKind kind = (expr->op == BinaryOp::Div) 
                    ? RuntimeErrorKind::DivisionByZero 
                    : RuntimeErrorKind::ModuloByZero;
                
                if (ctx.isInsideNullCoalesce()) {
                    // ─── In ?? context: branch to fallback on zero ──────────────────
                    llvm::BasicBlock* fallbackBlock = ctx.getNullCoalesceFallbackBlock();
                    ctx.builder.CreateCondBr(isZero, fallbackBlock, continueBlock);
                } else {
                    // ─── Normal context: panic on zero ──────────────────────────────
                    llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
                        ctx.llvmCtx, "div_mod_panic", func);
                    ctx.builder.CreateCondBr(isZero, panicBlock, continueBlock);
                    
                    ctx.builder.SetInsertPoint(panicBlock);
                    emitPanic(kind, ctx, getRuntimeErrorMessage(kind), expr);
                    ctx.builder.CreateUnreachable();
                }
                
                ctx.builder.SetInsertPoint(continueBlock);
                
                // ─── Perform the operation ────────────────────────────────────────────
                if (expr->op == BinaryOp::Div) {
                    result = ctx.builder.CreateSDiv(left, right, "sdiv");
                } else { // Mod
                    result = ctx.builder.CreateSRem(left, right, "srem");
                }
            } else {
                // Floating point division/modulo - no zero check needed
                if (expr->op == BinaryOp::Div) {
                    result = ctx.builder.CreateFDiv(left, right, "fdiv");
                } else {
                    result = ctx.builder.CreateFRem(left, right, "frem");
                }
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
    if (!target) return nullptr;

    llvm::Value* index = lowerExpression(expr->index, ctx);
    if (!index) return nullptr;

    if (expr->index->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->index->resolvedType);
        if (elemType) {
            index = loadIfNeeded(index, elemType, ctx);
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
    if (!elemType) return nullptr;

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
    // Check: 0 <= index < len
    llvm::Value* cond1 = ctx.builder.CreateICmpULT(index, len, "bounds_check_lt");
    llvm::Value* cond2 = ctx.builder.CreateICmpSGE(index, llvm::ConstantInt::get(index->getType(), 0), "bounds_check_ge");
    llvm::Value* inBounds = ctx.builder.CreateAnd(cond1, cond2, "in_bounds");

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, "idx_continue", func);

    if (ctx.isInsideNullCoalesce()) {
        // ─── In ?? context: branch to fallback on out-of-bounds ─────────────
        llvm::BasicBlock* fallbackBlock = ctx.getNullCoalesceFallbackBlock();
        ctx.builder.CreateCondBr(inBounds, continueBlock, fallbackBlock);
    } else {
        // ─── Normal context: panic on out-of-bounds ─────────────────────────
        llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "idx_panic", func);
        ctx.builder.CreateCondBr(inBounds, continueBlock, panicBlock);
        
        ctx.builder.SetInsertPoint(panicBlock);
        emitPanic(RuntimeErrorKind::ArrayIndexOutOfBounds, ctx, 
                  "array index out of bounds", expr);
        ctx.builder.CreateUnreachable();
    }

    ctx.builder.SetInsertPoint(continueBlock);

    // ─── GEP and load ──────────────────────────────────────────────────
    llvm::Value* gep = ctx.builder.CreateGEP(elemType, ptr, index, "array_idx");

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
    if (!target) return nullptr;

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
            }
            if (!end) return nullptr;
        }
        if (end->getType() != len->getType()) {
            end = ctx.builder.CreateIntCast(end, len->getType(), true, "end_cast");
        }
    } else {
        end = len;
    }

    // ─── Slice bounds check ──────────────────────────────────────────
    // Check: 0 <= start <= end <= len
    llvm::Value* cond1 = ctx.builder.CreateICmpSGE(start, llvm::ConstantInt::get(start->getType(), 0), "slice_check_start_ge_0");
    llvm::Value* cond2 = ctx.builder.CreateICmpULE(end, len, "slice_check_end_le_len");
    llvm::Value* cond3 = ctx.builder.CreateICmpULE(start, end, "slice_check_start_le_end");
    llvm::Value* inBounds1 = ctx.builder.CreateAnd(cond1, cond2, "slice_in_bounds_1");
    llvm::Value* inBounds = ctx.builder.CreateAnd(inBounds1, cond3, "slice_in_bounds");

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, "slice_continue", func);

    if (ctx.isInsideNullCoalesce()) {
        // ─── In ?? context: branch to fallback on out-of-bounds ─────────────
        llvm::BasicBlock* fallbackBlock = ctx.getNullCoalesceFallbackBlock();
        ctx.builder.CreateCondBr(inBounds, continueBlock, fallbackBlock);
    } else {
        // ─── Normal context: panic on out-of-bounds ─────────────────────────
        llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "slice_panic", func);
        ctx.builder.CreateCondBr(inBounds, continueBlock, panicBlock);
        
        ctx.builder.SetInsertPoint(panicBlock);
        emitPanic(RuntimeErrorKind::SliceBoundsOutOfRange, ctx, 
                  "slice bounds out of range", expr);
        ctx.builder.CreateUnreachable();
    }

    ctx.builder.SetInsertPoint(continueBlock);

    // ─── Get data pointer ──────────────────────────────────────────────────
    llvm::Value* dataPtr = target;
    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (arrayType->isFixed()) {
        dataPtr = ctx.builder.CreateConstGEP2_32(elemType, target, 0, 0);
    }

    // ─── Offset data pointer by start ──────────────────────────────────────
    llvm::Value* slicePtr = ctx.builder.CreateGEP(elemType, dataPtr, start, "slice_ptr");

    // ─── Calculate length: end - start ──────────────────────────────────────
    llvm::Value* sliceLen = ctx.builder.CreateSub(end, start, "slice_len");

    // ─── Calculate capacity: len - start ────────────────────────────────────
    llvm::Value* sliceCap = ctx.builder.CreateSub(len, start, "slice_cap");

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

    // ─── 5. Get the field index ─────────────────────────────────────────────
    // Use cached field index from Sema if available
    size_t fieldIndex = expr->fieldIndex;
    FieldDeclAST* field = nullptr;
    
    if (fieldIndex != SIZE_MAX && fieldIndex < structDecl->fields.size()) {
        field = structDecl->fields[fieldIndex];
        // Verify the field name matches (defensive)
        if (field->name != expr->fieldName) {
            // Fall back to name lookup if cache is stale
            fieldIndex = SIZE_MAX;
            field = nullptr;
        }
    }
    
    // If cache wasn't available or was stale, look up by name
    if (fieldIndex == SIZE_MAX) {
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
        
        // Cache the field index for future use
        expr->fieldIndex = fieldIndex;
    }

    // ─── 6. Get the LLVM struct type ──────────────────────────────────────
    llvm::StructType* llvmStructType = ctx.lookupStruct(structDecl);
    if (!llvmStructType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "struct '", ctx.pool.lookup(structDecl->name), 
                                "' has no LLVM type");
        return nullptr;
    }

    // ─── 7. Get the field pointer or value ─────────────────────────────────
    llvm::Type* fieldType = llvmStructType->getElementType(fieldIndex);
    bool objectIsLValue = expr->object->isLValue;
    
    // For l-value object: we have a pointer → GEP
    if (objectIsLValue) {
        // Object is an l-value - we have a pointer to the struct
        llvm::Value* structPtr = object;
        
        // GEP to the field
        std::vector<llvm::Value*> indices = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 0),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 
                                    static_cast<uint32_t>(fieldIndex))
        };
        llvm::Value* fieldPtr = ctx.builder.CreateInBoundsGEP(
            llvmStructType,
            structPtr,
            indices,
            "field_ptr_" + ctx.pool.lookup(field->name)
        );
        
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
    
    // For value object: we have the struct value → ExtractValue
    if (object->getType()->isStructTy()) {
        llvm::Value* fieldVal = ctx.builder.CreateExtractValue(
            object,
            static_cast<unsigned>(fieldIndex),
            "field_val_" + ctx.pool.lookup(field->name)
        );
        expr->llvmValue = fieldVal;
        return fieldVal;
    }
    
    // Object is a pointer but not an l-value (e.g., pointer from #toRef)
    // GEP and load
    llvm::Value* structPtr = object;
    std::vector<llvm::Value*> indices = {
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 0),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 
                                static_cast<uint32_t>(fieldIndex))
    };
    llvm::Value* fieldPtr = ctx.builder.CreateInBoundsGEP(
        llvmStructType,
        structPtr,
        indices,
        "field_ptr_" + ctx.pool.lookup(field->name)
    );
    
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

llvm::Value* lowerArenaAccessExpr(ArenaAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);
    llvm::StructType* arenaType = ctx.getArenaType();
    llvm::Function* func = ctx.getCurrentFunction();
    
    // ──────────────────────────────────────────────────────────────────────────
    // STATIC FORMS: Arena::create(size) or Arena::empty()
    // ──────────────────────────────────────────────────────────────────────────
    
    if (expr->isStatic) {
        // ─── Arena::create(size) -> Arena! ──────────────────────────────────
        if (expr->methodName == ctx.pool.intern("create")) {
            if (expr->args.empty()) {
                ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, expr->loc,
                                        "Arena::create requires a size argument");
                return nullptr;
            }
            
            llvm::Value* size = lowerExpression(expr->args[0], ctx);
            if (!size) return nullptr;
            
            if (expr->args[0]->isLValue) {
                llvm::Type* elemType = getType(ctx, expr->args[0]->resolvedType);
                if (elemType) {
                    size = loadIfNeeded(size, elemType, ctx);
                }
                if (!size) return nullptr;
            }
            
            if (size->getType() != i64) {
                size = ctx.builder.CreateIntCast(size, i64, false, "arena_size_cast");
            }
            
            // ─── Call __lucid_arena_create(size) ──────────────────────────
            llvm::Function* createFn = ctx.getRuntimeFn(RuntimeFn::ArenaCreate);
            llvm::Value* desc = ctx.builder.CreateCall(createFn, {size}, "arena_create_result");
            
            // ─── Check for allocation failure ──────────────────────────────
            llvm::Value* base = ctx.builder.CreateExtractValue(desc, 0, "arena_base");
            llvm::Value* isNull = ctx.builder.CreateIsNull(base, "arena_create_failed");
            
            // ─── Convert ArenaDescriptor to Arena ──────────────────────────
            // Arena { base: i8*, size: i64, cursor: i64 }
            llvm::Value* arena = llvm::UndefValue::get(arenaType);
            llvm::Value* descSize = ctx.builder.CreateExtractValue(desc, 1, "arena_size");
            arena = ctx.builder.CreateInsertValue(arena, base, 0);
            arena = ctx.builder.CreateInsertValue(arena, descSize, 1);
            arena = ctx.builder.CreateInsertValue(arena, llvm::ConstantInt::get(i64, 0), 2);
            
            // ─── Wrap in fallible type ─────────────────────────────────────
            llvm::StructType* fallibleType = llvm::cast<llvm::StructType>(getType(ctx, expr->resolvedType));
            
            llvm::Value* result = llvm::UndefValue::get(fallibleType);
            
            // Tag: 1 = valid, 2 = err
            llvm::Value* tag = ctx.builder.CreateSelect(
                isNull,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx.llvmCtx), 2),
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx.llvmCtx), 1),
                "arena_create_tag"
            );
            
            result = ctx.builder.CreateInsertValue(result, tag, 0);
            result = ctx.builder.CreateInsertValue(result, arena, 1);
            
            return result;
        }
        
        // ─── Arena::empty() -> Arena ──────────────────────────────────────
        if (expr->methodName == ctx.pool.intern("empty")) {
            return llvm::Constant::getNullValue(arenaType);
        }
        
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownMethod, expr->loc,
                                "unknown Arena static method '", 
                                ctx.pool.lookup(expr->methodName), 
                                "' - expected 'create' or 'empty'");
        return nullptr;
    }
    
    // ──────────────────────────────────────────────────────────────────────────
    // INSTANCE FORMS: arena::method(...)
    // ──────────────────────────────────────────────────────────────────────────
    
    // ─── Get pointer to the Arena struct (must be &Arena) ──────────────────
    llvm::Value* arenaPtr = getArenaPointer(expr, ctx);
    if (!arenaPtr) return nullptr;
    
    // ─── arena::alloc<T>(count) -> [_]T ─────────────────────────────────────
    if (expr->methodName == ctx.pool.intern("alloc")) {
        if (expr->genericArgs.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, expr->loc,
                                    "arena::alloc requires a type argument (e.g., arena::alloc<Node>(128))");
            return nullptr;
        }
        
        auto [elemSize, elemAlign] = getElementSizeAndAlignment(expr->genericArgs[0], ctx);
        
        llvm::Value* count = llvm::ConstantInt::get(i64, 1);
        if (!expr->args.empty()) {
            count = lowerExpression(expr->args[0], ctx);
            if (!count) return nullptr;
            
            if (expr->args[0]->isLValue) {
                llvm::Type* argType = getType(ctx, expr->args[0]->resolvedType);
                if (argType) {
                    count = loadIfNeeded(count, argType, ctx);
                }
                if (!count) return nullptr;
            }
            
            if (count->getType() != i64) {
                count = ctx.builder.CreateIntCast(count, i64, false, "alloc_count_cast");
            }
        }
        
        llvm::Value* totalSize = ctx.builder.CreateMul(
            count,
            llvm::ConstantInt::get(i64, elemSize),
            "alloc_total_size"
        );
        
        llvm::Function* allocFn = ctx.getRuntimeFn(RuntimeFn::ArenaAlloc);
        llvm::Value* data = ctx.builder.CreateCall(
            allocFn,
            {arenaPtr, totalSize, llvm::ConstantInt::get(i64, elemAlign)},
            "arena_alloc_data"
        );
        
        // ─── Check for allocation failure with fallback support ──────────────
        llvm::Value* isNull = ctx.builder.CreateIsNull(data, "arena_alloc_failed");
        
        llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "arena_alloc_continue", func);
        
        if (ctx.isInsideNullCoalesce()) {
            llvm::BasicBlock* fallbackBlock = ctx.getNullCoalesceFallbackBlock();
            ctx.builder.CreateCondBr(isNull, fallbackBlock, continueBlock);
        } else {
            llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
                ctx.llvmCtx, "arena_alloc_panic", func);
            ctx.builder.CreateCondBr(isNull, panicBlock, continueBlock);
            
            ctx.builder.SetInsertPoint(panicBlock);
            emitPanic(RuntimeErrorKind::ArenaOutOfCapacity, ctx, 
                      "arena out of capacity", expr);
            ctx.builder.CreateUnreachable();
        }
        
        ctx.builder.SetInsertPoint(continueBlock);
        
        llvm::Type* elemPtrType = llvm::PointerType::get(ctx.llvmCtx, 0);
        llvm::Value* typedData = ctx.builder.CreatePointerCast(
            data,
            elemPtrType,
            "arena_alloc_typed"
        );
        
        llvm::StructType* sliceType = llvm::cast<llvm::StructType>(getType(ctx, expr->resolvedType));
        llvm::Value* slice = llvm::UndefValue::get(sliceType);
        slice = ctx.builder.CreateInsertValue(slice, typedData, 0);
        slice = ctx.builder.CreateInsertValue(slice, count, 1);
        slice = ctx.builder.CreateInsertValue(slice, count, 2);
        
        return slice;
    }
    
    // ─── arena::reset() -> void ────────────────────────────────────────────
    if (expr->methodName == ctx.pool.intern("reset")) {
        llvm::Function* resetFn = ctx.getRuntimeFn(RuntimeFn::ArenaReset);
        ctx.builder.CreateCall(resetFn, {arenaPtr}, "arena_reset");
        return nullptr;
    }
    
    // ─── arena::descriptor() -> ArenaDescriptor ──────────────────────────
    if (expr->methodName == ctx.pool.intern("descriptor")) {
        return buildArenaDescriptor(arenaPtr, ctx);
    }
    
    // ─── arena::capacity() -> uint64 ──────────────────────────────────────
    if (expr->methodName == ctx.pool.intern("capacity")) {
        llvm::Function* capFn = ctx.getRuntimeFn(RuntimeFn::ArenaCapacity);
        return ctx.builder.CreateCall(capFn, {arenaPtr}, "arena_capacity");
    }
    
    // ─── arena::remaining() -> uint64 ──────────────────────────────────────
    if (expr->methodName == ctx.pool.intern("remaining")) {
        llvm::Function* remainingFn = ctx.getRuntimeFn(RuntimeFn::ArenaRemaining);
        return ctx.builder.CreateCall(remainingFn, {arenaPtr}, "arena_remaining");
    }
    
    // ─── arena::isEmpty() -> bool ──────────────────────────────────────────
    if (expr->methodName == ctx.pool.intern("isEmpty")) {
        llvm::Function* isEmptyFn = ctx.getRuntimeFn(RuntimeFn::ArenaIsEmpty);
        return ctx.builder.CreateCall(isEmptyFn, {arenaPtr}, "arena_is_empty");
    }
    
    // ─── arena::space<T>() -> uint64 ──────────────────────────────────────
    if (expr->methodName == ctx.pool.intern("space")) {
        if (expr->genericArgs.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, expr->loc,
                                    "arena::space requires a type argument (e.g., arena::space<Node>())");
            return nullptr;
        }
        
        auto [elemSize, _] = getElementSizeAndAlignment(expr->genericArgs[0], ctx);
        
        llvm::Function* spaceFn = ctx.getRuntimeFn(RuntimeFn::ArenaSpace);
        return ctx.builder.CreateCall(
            spaceFn,
            {arenaPtr, llvm::ConstantInt::get(i64, elemSize)},
            "arena_space"
        );
    }
    
    // ─── arena::canFit<T>(count) -> bool ──────────────────────────────────
    if (expr->methodName == ctx.pool.intern("canFit")) {
        if (expr->genericArgs.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_GenericInstantiate, expr->loc,
                                    "arena::canFit requires a type argument (e.g., arena::canFit<Node>(128))");
            return nullptr;
        }
        
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, expr->loc,
                                    "arena::canFit requires a count argument");
            return nullptr;
        }
        
        auto [elemSize, _] = getElementSizeAndAlignment(expr->genericArgs[0], ctx);
        
        llvm::Value* count = lowerExpression(expr->args[0], ctx);
        if (!count) return nullptr;
        
        if (expr->args[0]->isLValue) {
            llvm::Type* argType = getType(ctx, expr->args[0]->resolvedType);
            if (argType) {
                count = loadIfNeeded(count, argType, ctx);
            }
            if (!count) return nullptr;
        }
        
        if (count->getType() != i64) {
            count = ctx.builder.CreateIntCast(count, i64, false, "canfit_count_cast");
        }
        
        llvm::Function* canFitFn = ctx.getRuntimeFn(RuntimeFn::ArenaCanFit);
        return ctx.builder.CreateCall(
            canFitFn,
            {arenaPtr, llvm::ConstantInt::get(i64, elemSize), count},
            "arena_can_fit"
        );
    }
    
    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownMethod, expr->loc,
                            "unknown Arena method '", 
                            ctx.pool.lookup(expr->methodName),
                            "' - expected 'alloc', 'reset', 'descriptor', "
                            "'capacity', 'remaining', 'isEmpty', 'space', or 'canFit'");
    return nullptr;
}

// =============================================================================
// Null Coalesce Expression
// =============================================================================

llvm::Value* lowerNullCoalesceExpr(NullCoalesceExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* lhsType = expr->value->resolvedType;
    if (!lhsType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->value->loc,
                                "LHS has no type");
        return nullptr;
    }

    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "Null coalesce expression outside of function");

    // ──────────────────────────────────────────────────────────────────────────
    // CASE 1: LHS is already nullable/fallible (tagged type)
    // ──────────────────────────────────────────────────────────────────────────

    if (sema::isNullableType(lhsType) || sema::isFallibleType(lhsType)) {
        // ─── Lower the LHS ──────────────────────────────────────────────────
        llvm::Value* lhs = lowerExpression(expr->value, ctx);
        if (!lhs) return nullptr;

        // ─── LHS should be a tagged struct { i8 tag, T value } ─────────────
        if (!lhs->getType()->isStructTy() || 
            lhs->getType()->getStructNumElements() != 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->value->loc,
                                    "LHS must be a tagged type (T?, T!, or T?!)");
            return nullptr;
        }

        // ─── Create basic blocks ─────────────────────────────────────────────
        llvm::BasicBlock* successBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "coalesce_success", func);
        llvm::BasicBlock* fallbackBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "coalesce_fallback", func);
        llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "coalesce_merge", func);

        // ─── Extract and check the tag ──────────────────────────────────────
        llvm::Value* tag = ctx.builder.CreateExtractValue(lhs, 0, "coalesce_tag");
        llvm::Value* isNil = ctx.builder.CreateICmpEQ(
            tag,
            llvm::ConstantInt::get(tag->getType(), 0),
            "is_nil"
        );

        // ─── For fallible types, check both nil (0) and err (2) ────────────
        llvm::Value* isFailure = isNil;
        if (sema::isFallibleType(lhsType)) {
            llvm::Value* isErr = ctx.builder.CreateICmpEQ(
                tag,
                llvm::ConstantInt::get(tag->getType(), 2),
                "is_err"
            );
            isFailure = ctx.builder.CreateOr(isNil, isErr, "is_failure");
        }

        ctx.builder.CreateCondBr(isFailure, fallbackBlock, successBlock);

        // ─── Success block: extract the value ──────────────────────────────
        ctx.builder.SetInsertPoint(successBlock);
        llvm::Value* lhsValid = ctx.builder.CreateExtractValue(lhs, 1, "coalesce_value");
        ctx.builder.CreateBr(mergeBlock);
        llvm::BasicBlock* actualSuccessBlock = ctx.builder.GetInsertBlock();

        // ─── Fallback block: lower the RHS ──────────────────────────────────
        ctx.builder.SetInsertPoint(fallbackBlock);
        llvm::Value* rhsValue = lowerExpression(expr->fallback, ctx);
        if (!rhsValue) return nullptr;
        if (expr->fallback->isLValue) {
            llvm::Type* elemType = getType(ctx, expr->fallback->resolvedType);
            if (elemType) {
                rhsValue = loadIfNeeded(rhsValue, elemType, ctx);
            }
        }
        ctx.builder.CreateBr(mergeBlock);
        llvm::BasicBlock* actualFallbackBlock = ctx.builder.GetInsertBlock();

        // ─── Merge block: PHI selects the correct value ─────────────────────
        ctx.builder.SetInsertPoint(mergeBlock);
        llvm::PHINode* phi = ctx.builder.CreatePHI(
            lhsValid->getType(),
            2,
            "coalesce_result"
        );
        phi->addIncoming(lhsValid, actualSuccessBlock);
        phi->addIncoming(rhsValue, actualFallbackBlock);

        expr->llvmValue = phi;
        return phi;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // CASE 2: LHS is a plain risky operation
    // ──────────────────────────────────────────────────────────────────────────

    // ─── Create basic blocks ─────────────────────────────────────────────────
    llvm::BasicBlock* fallbackBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, "coalesce_risky_fallback", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, "coalesce_risky_merge", func);

    // ─── Push the ?? context ─────────────────────────────────────────────────
    // Risky operations (division, index, arena::alloc) will check
    // ctx.isInsideNullCoalesce() and branch to fallbackBlock on failure.
    ctx.pushNullCoalesce(fallbackBlock);

    // ─── Lower the LHS in ?? context ──────────────────────────────────────
    // If a risky operation fails, it branches to fallbackBlock and leaves
    // the builder in its own "continue" block (the success path).
    // If no risky operation participates, the builder is just wherever it was.
    // Either way, the builder's current position IS the success continuation.
    llvm::Value* lhs = lowerExpression(expr->value, ctx);

    // ─── Pop the ?? context ─────────────────────────────────────────────────
    // We popped it early so the fallback expression itself is NOT in the
    // ?? context. If the fallback contains another ??, it starts its own
    // fresh context.
    ctx.popNullCoalesce();

    if (!lhs) return nullptr;

    // ─── Load if the LHS is an l-value ─────────────────────────────────────
    llvm::Value* lhsValid = lhs;
    if (expr->value->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->value->resolvedType);
        if (elemType) {
            lhsValid = loadIfNeeded(lhsValid, elemType, ctx);
        }
        if (!lhsValid) return nullptr;
    }

    // ─── Success path ──────────────────────────────────────────────────────
    // Wherever the builder is right now IS the success continuation.
    // The risky operation (if any) already left the builder in its own
    // "continue" block after branching failures away.
    ctx.builder.CreateBr(mergeBlock);
    llvm::BasicBlock* successBlock = ctx.builder.GetInsertBlock();

    // ─── Fallback path ─────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(fallbackBlock);
    llvm::Value* rhsValue = lowerExpression(expr->fallback, ctx);
    if (!rhsValue) return nullptr;
    if (expr->fallback->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->fallback->resolvedType);
        if (elemType) {
            rhsValue = loadIfNeeded(rhsValue, elemType, ctx);
        }
        if (!rhsValue) return nullptr;
    }
    ctx.builder.CreateBr(mergeBlock);
    llvm::BasicBlock* actualFallbackBlock = ctx.builder.GetInsertBlock();

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(
        lhsValid->getType(),
        2,
        "coalesce_risky_result"
    );
    phi->addIncoming(lhsValid, successBlock);
    phi->addIncoming(rhsValue, actualFallbackBlock);

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

    // ─── Handle reassignment of mutable functions ──────────────────────────
    // If the LHS is a `let` function that holds a closure, we need to:
    //   1. Release the old closure's environment
    //   2. Store the new value
    //   3. Retain the new closure's environment if it's a closure
    bool isLetFunctionReassignment = false;
    FuncDeclAST* targetFuncDecl = nullptr;
    
    if (expr->lhs->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = expr->lhs->as<IdentifierExprAST>();
        if (id->resolvedDecl && id->resolvedDecl->isa<FuncDeclAST>()) {
            FuncDeclAST* funcDecl = id->resolvedDecl->as<FuncDeclAST>();
            if (funcDecl->keyword == DeclKeyword::Let && funcDecl->hasClosure) {
                isLetFunctionReassignment = true;
                targetFuncDecl = funcDecl;
            }
        }
    }

    // If reassigning a mutable closure function, release the old environment
    if (isLetFunctionReassignment && targetFuncDecl) {
        llvm::Value* oldValue = ctx.lookupValue(targetFuncDecl);
        if (oldValue) {
            // Load the current closure value from the alloca
            llvm::Value* loadedOld = ctx.builder.CreateLoad(
                ctx.getClosureType(),
                oldValue,
                "old_closure_load"
            );
            
            // Check if it has a non-null environment
            llvm::Value* oldEnvPtr = ctx.builder.CreateExtractValue(loadedOld, 1);
            llvm::Value* isNull = ctx.builder.CreateIsNull(oldEnvPtr, "old_env_is_null");
            
            llvm::Function* func = ctx.getCurrentFunction();
            llvm::BasicBlock* releaseBlock = llvm::BasicBlock::Create(
                ctx.llvmCtx, "release_old_env", func);
            llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                ctx.llvmCtx, "release_continue", func);
            
            ctx.builder.CreateCondBr(isNull, continueBlock, releaseBlock);
            
            ctx.builder.SetInsertPoint(releaseBlock);
            llvm::Function* releaseFn = ctx.getRuntimeFn(RuntimeFn::ReleaseEnv);
            ctx.builder.CreateCall(releaseFn, {oldEnvPtr});
            ctx.builder.CreateBr(continueBlock);
            
            ctx.builder.SetInsertPoint(continueBlock);
        }
    }

    llvm::Value* rhs = lowerExpression(expr->rhs, ctx);
    if (!rhs) {
        return nullptr;
    }

    if (expr->rhs->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->rhs->resolvedType);
        if (elemType) {
            rhs = loadIfNeeded(rhs, elemType, ctx);
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

    // ─── If assigning a closure to a let function, retain the environment ──
    if (isLetFunctionReassignment && targetFuncDecl) {
        if (expr->rhs->resolvedType && expr->rhs->resolvedType->isa<FuncTypeAST>()) {
            if (rhs->getType()->isStructTy() && rhs->getType()->getStructNumElements() == 2) {
                llvm::Value* newEnvPtr = ctx.builder.CreateExtractValue(rhs, 1);
                llvm::Value* isNull = ctx.builder.CreateIsNull(newEnvPtr, "new_env_is_null");
                
                llvm::Function* func = ctx.getCurrentFunction();
                llvm::BasicBlock* retainBlock = llvm::BasicBlock::Create(
                    ctx.llvmCtx, "retain_new_env", func);
                llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                    ctx.llvmCtx, "retain_continue", func);
                
                ctx.builder.CreateCondBr(isNull, continueBlock, retainBlock);
                
                ctx.builder.SetInsertPoint(retainBlock);
                llvm::Function* retainFn = ctx.getRuntimeFn(RuntimeFn::RetainEnv);
                ctx.builder.CreateCall(retainFn, {newEnvPtr});
                ctx.builder.CreateBr(continueBlock);
                
                ctx.builder.SetInsertPoint(continueBlock);
            }
        }
    }

    expr->llvmValue = rhs;
    return rhs;
}

// =============================================================================
// Pipeline Expression
// =============================================================================

llvm::Value* lowerPipelineExpr(PipelineExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // 1. Lower the seed expression (initial value)
    llvm::Value* currentValue = lowerExpression(expr->seed, ctx);
    if (!currentValue) {
        return nullptr;
    }

    // 2. If seed is an l-value, load it (get the value, not the address)
    if (expr->seed->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->seed->resolvedType);
        if (elemType) {
            currentValue = loadIfNeeded(currentValue, elemType, ctx);
        }
    }

    // 3. Process each step sequentially
    for (PipelineStepAST* step : expr->steps) {
        // Pass the current value to the step, get the new value
        currentValue = lowerPipelineStep(step, currentValue, ctx);
        if (!currentValue) {
            return nullptr;
        }
    }

    // 4. Store the final result
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

    if (hasVariadic) {
        ParamAST* variadicParam = funcType->params.back();
        ArrayTypeAST* variadicArray = variadicParam->type->as<ArrayTypeAST>();
        if (!variadicArray) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, step->callable->loc,
                                    "variadic parameter must be an array type");
            return nullptr;
        }

        llvm::Type* elemType = getType(ctx, variadicArray->element);
        if (!elemType) return nullptr;

        size_t fixedParamCount = paramCount - 1;
        if (args.size() < fixedParamCount) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, step->callable->loc,
                                    "pipeline step is missing fixed arguments");
            return nullptr;
        }
        size_t variadicArgCount = args.size() - fixedParamCount;

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx);
        llvm::StructType* sliceType = ctx.getSliceType();
        llvm::Value* count = llvm::ConstantInt::get(i64Ty, variadicArgCount);

        if (variadicArgCount > 0) {
            uint64_t elementSize = ctx.module->getDataLayout().getTypeAllocSize(elemType);
            llvm::Value* allocationSize = ctx.builder.CreateMul(
                count,
                llvm::ConstantInt::get(i64Ty, elementSize),
                "variadic_bytes"
            );
            llvm::Value* arrayPtr = ctx.builder.CreateCall(
                ctx.getRuntimeFn(RuntimeFn::Alloc),
                {allocationSize},
                "variadic_data"
            );

            for (size_t i = 0; i < variadicArgCount; ++i) {
                llvm::Value* val = args[fixedParamCount + i];
                if (val->getType() != elemType) {
                    if (val->getType()->isIntegerTy() && elemType->isIntegerTy()) {
                        bool isSigned = true;
                        if (variadicArray->element->isa<PrimitiveTypeAST>()) {
                            isSigned = isSignedIntegerKind(
                                variadicArray->element->as<PrimitiveTypeAST>()->primitiveKind
                            );
                        }
                        val = ctx.builder.CreateIntCast(
                            val,
                            elemType,
                            isSigned,
                            "variadic_int_cast"
                        );
                    } else if (val->getType()->isFloatingPointTy() && elemType->isFloatingPointTy()) {
                        val = ctx.builder.CreateFPCast(val, elemType, "variadic_float_cast");
                    } else if (val->getType()->isPointerTy() && elemType->isPointerTy()) {
                        val = ctx.builder.CreatePointerCast(val, elemType, "variadic_ptr_cast");
                    } else {
                        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, step->callable->loc,
                                                "variadic argument type is incompatible with element type");
                        return nullptr;
                    }
                }
                llvm::Value* gep = ctx.builder.CreateGEP(
                    elemType,
                    arrayPtr,
                    llvm::ConstantInt::get(i64Ty, i),
                    "variadic_ptr"
                );
                ctx.builder.CreateStore(val, gep);
            }

            llvm::Value* slice = llvm::UndefValue::get(sliceType);
            slice = ctx.builder.CreateInsertValue(slice, arrayPtr, 0);
            slice = ctx.builder.CreateInsertValue(slice, count, 1);
            slice = ctx.builder.CreateInsertValue(slice, count, 2);

            // Replace the variadic arguments with the slice in the args list
            args.resize(fixedParamCount + 1);
            args[fixedParamCount] = slice;
        } else {
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
        }
        if (!callable) return nullptr;
    }

    return callable;
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
    llvm::FunctionType* fLLVMType,
    FuncTypeAST* fParamSource,
    llvm::Value* g,
    FuncTypeAST* gType,
    CodeGenContext& ctx
) {
    if (!f || !fLLVMType || !fParamSource || !g || !gType) return nullptr;

    // ─── 1. Build g's canonical LLVM function type ─────────────────────────
    if (!fLLVMType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, fParamSource->loc,
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
    for (ParamAST* param : fParamSource->params) {
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
    llvm::FunctionType* currentLLVMType = getFunctionType(ctx, leftFuncType);
    if (!currentLLVMType) {
        return nullptr;
    }

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
            currentLLVMType,
            leftFuncType,
            nextFunc,
            nextFuncType,
            ctx
        );

        if (!currentFunc) {
            return nullptr;
        }

        llvm::Type* nextReturnType = getType(ctx, nextFuncType->returnType);
        if (!nextReturnType) {
            return nullptr;
        }
        currentLLVMType = llvm::FunctionType::get(
            nextReturnType,
            currentLLVMType->params(),
            false
        );
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
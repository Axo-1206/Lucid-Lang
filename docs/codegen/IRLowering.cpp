/**
 * @file IRLowering.cpp
 * @brief Implementation of the AST to LLVM IR lowerer.
 */

#include "IRLowering.hpp"

#include "llvm/IR/Verifier.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include <iostream>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Construction
// ─────────────────────────────────────────────────────────────────────────────

IRLowering::IRLowering(llvm::LLVMContext& context, TypeMapping& typeMapper, StringPool& stringPool)
    : m_context(context)
    , m_typeMapper(typeMapper)
    , m_stringPool(stringPool)
    , m_builder(context) {
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Helper Methods
// ─────────────────────────────────────────────────────────────────────────────

std::string IRLowering::internedToString(InternedString name) const {
    std::string_view view = m_stringPool.lookup(name);
    return std::string(view);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Main Entry Point
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<llvm::Module> IRLowering::lower(ModuleAST* module,
                                                const std::string& moduleName) {
    if (!module) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Cannot lower null module");
    }

    m_moduleName = moduleName;

    // Create the LLVM module
    m_module = std::make_unique<llvm::Module>(moduleName, m_context);

    // Set target triple
    m_module->setTargetTriple(llvm::sys::getDefaultTargetTriple());

    // Set data layout (use the target's data layout)
    // m_module->setDataLayout(...); // Will be set by the backend

    // Create runtime functions
    createPanicFunction(m_module.get());
    createCheckBoundsFunction(m_module.get());

    // Reset state
    m_scopeStack.clear();
    m_functionStack.clear();
    m_loopStack.clear();

    // Lower each top-level declaration
    for (DeclPtr decl : module->decls) {
        try {
            lowerDecl(decl);
        } catch (const IRLoweringError& e) {
            // Log and continue with other declarations
            std::cerr << "Warning: Failed to lower declaration: " << e.what() << "\n";
        }
    }

    // Verify the module
    std::string errorMsg;
    llvm::raw_string_ostream errorStream(errorMsg);
    if (llvm::verifyModule(*m_module, &errorStream)) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Invalid IR module: " + errorMsg);
    }

    return std::move(m_module);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Scope Management
// ─────────────────────────────────────────────────────────────────────────────

void IRLowering::enterScope() {
    m_scopeStack.emplace_back();
}

void IRLowering::exitScope() {
    if (!m_scopeStack.empty()) {
        m_scopeStack.pop_back();
    }
}

IRLowering::Scope& IRLowering::currentScope() {
    if (m_scopeStack.empty()) {
        // Should never happen - create a scope
        enterScope();
    }
    return m_scopeStack.back();
}

llvm::AllocaInst* IRLowering::allocateLocal(const std::string& name, llvm::Type* type) {
    if (!currentFunction().function) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Cannot allocate local outside a function");
    }

    // Insert at the start of the function (alloca at entry)
    llvm::IRBuilder<> tmpBuilder(&currentFunction().function->getEntryBlock(),
                                 currentFunction().function->getEntryBlock().begin());

    auto* alloca = tmpBuilder.CreateAlloca(type, nullptr, name);
    currentScope().locals[name] = alloca;
    return alloca;
}

llvm::Value* IRLowering::lookupLocal(const std::string& name) {
    // Search from innermost to outermost
    for (auto it = m_scopeStack.rbegin(); it != m_scopeStack.rend(); ++it) {
        auto found = it->locals.find(name);
        if (found != it->locals.end()) {
            return found->second;
        }
    }
    return nullptr;
}

void IRLowering::storeLocal(const std::string& name, llvm::Value* value) {
    auto* ptr = lookupLocal(name);
    if (!ptr) {
        // Allocate if not found
        ptr = allocateLocal(name, value->getType());
    }
    m_builder.CreateStore(value, ptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Declarations
// ─────────────────────────────────────────────────────────────────────────────

void IRLowering::lowerDecl(DeclAST* decl) {
    if (!decl) {
        return;
    }

    switch (decl->kind) {
        case ASTKind::FuncDecl:
            lowerFuncDecl(decl->as<FuncDeclAST>());
            break;
        case ASTKind::VarDecl:
            lowerVarDecl(decl->as<VarDeclAST>());
            break;
        case ASTKind::StructDecl:
            lowerStructDecl(decl->as<StructDeclAST>());
            break;
        case ASTKind::EnumDecl:
            lowerEnumDecl(decl->as<EnumDeclAST>());
            break;
        default:
            // Ignore other declaration types (imports, traits)
            break;
    }
}

void IRLowering::lowerFuncDecl(FuncDeclAST* funcDecl) {
    if (!funcDecl) {
        return;
    }

    // Check if it's a foreign function
    bool isForeign = false;
    for (AttributePtr attr : funcDecl->attributes) {
        std::string attrName = internedToString(attr->name);
        if (attrName == "foreign") {
            isForeign = true;
            break;
        }
    }

    if (isForeign) {
        lowerForeignDecl(funcDecl);
        return;
    }

    // Get the function type
    auto* funcType = funcDecl->funcType;
    if (!funcType) {
        std::string name = internedToString(funcDecl->name);
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Function has no type: " + name);
    }

    // Convert to LLVM function type
    auto* llvmFuncType = toLLVMFunctionType(funcType);
    if (!llvmFuncType) {
        std::string name = internedToString(funcDecl->name);
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Failed to convert function type: " + name);
    }

    // Create the function
    std::string name = internedToString(funcDecl->name);
    auto* function = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        name,
        m_module.get()
    );

    if (!function) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Failed to create function: " + name);
    }

    // Create function context
    FunctionContext fnCtx;
    fnCtx.function = function;
    fnCtx.entryBlock = llvm::BasicBlock::Create(m_context, "entry", function);
    fnCtx.funcType = funcType;
    m_functionStack.push_back(fnCtx);

    // Set insertion point to entry block
    m_builder.SetInsertPoint(fnCtx.entryBlock);

    // Enter a new scope for function locals
    enterScope();

    // Allocate parameters
    size_t paramIndex = 0;
    for (ParamPtr param : funcType->params) {
        std::string paramName = internedToString(param->name);
        llvm::Argument* arg = function->getArg(paramIndex++);
        auto* alloca = allocateLocal(paramName, arg->getType());
        m_builder.CreateStore(arg, alloca);
        // Store parameter for lookup
        currentFunction().parameters[paramName] = alloca;
    }

    // Lower the function body
    if (funcDecl->body) {
        lowerStmt(funcDecl->body);
    }

    // If the function is void and has no return, add an implicit return
    if (funcType->isVoid() && !currentFunction().hasReturn) {
        m_builder.CreateRetVoid();
    }

    // Exit scope
    exitScope();

    // Pop function context
    m_functionStack.pop_back();
}

void IRLowering::lowerForeignDecl(FuncDeclAST* funcDecl) {
    // Foreign functions are declared as external functions
    auto* funcType = funcDecl->funcType;
    if (!funcType) {
        return;
    }

    auto* llvmFuncType = toLLVMFunctionType(funcType);
    if (!llvmFuncType) {
        return;
    }

    std::string name = internedToString(funcDecl->name);
    llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        name,
        m_module.get()
    );
}

void IRLowering::lowerVarDecl(VarDeclAST* varDecl) {
    if (!varDecl) {
        return;
    }

    // Only lower if we're inside a function (local variable)
    if (m_functionStack.empty()) {
        // Global variables are handled differently
        // For now, ignore globals (they will be lowered by AOT)
        return;
    }

    std::string name = internedToString(varDecl->name);
    auto* type = toLLVMType(varDecl->type);
    if (!type) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Failed to convert type for variable: " + name);
    }

    // Allocate the variable
    auto* alloca = allocateLocal(name, type);

    // Initialize if there's an initializer
    if (varDecl->init) {
        auto* initValue = lowerExpr(varDecl->init);
        if (initValue) {
            m_builder.CreateStore(initValue, alloca);
        }
    }
}

void IRLowering::lowerStructDecl(StructDeclAST* structDecl) {
    // Structs are represented as LLVM struct types
    // They are registered with the type mapper
    std::string name = internedToString(structDecl->name);

    // Build the struct type
    std::vector<llvm::Type*> fieldTypes;
    for (FieldDeclPtr field : structDecl->fields) {
        auto* fieldType = toLLVMType(field->type);
        if (fieldType) {
            fieldTypes.push_back(fieldType);
        }
    }

    auto* structType = llvm::StructType::create(m_context, fieldTypes, name);
    m_typeMapper.registerType(name, structType);
}

void IRLowering::lowerEnumDecl(EnumDeclAST* enumDecl) {
    // Enums are represented as integer types
    // The backing type is int32 by default
    auto* enumType = llvm::Type::getInt32Ty(m_context);
    std::string name = internedToString(enumDecl->name);
    m_typeMapper.registerType(name, enumType);

    // Register enum variants as constants
    for (EnumVariantPtr variant : enumDecl->variants) {
        // Variants are compiled as constants
        // They can be referenced by name via the module's symbol table
        auto* constValue = llvm::ConstantInt::get(enumType, variant->value);
        // Store the constant in a global for later reference
        // This could be optimized to use the value directly
        std::string varName = name + "_" + internedToString(variant->name);
        new llvm::GlobalVariable(
            *m_module,
            enumType,
            true, // constant
            llvm::GlobalValue::ExternalLinkage,
            constValue,
            varName
        );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRLowering::lowerStmt(StmtAST* stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            lowerBlockStmt(stmt->as<BlockStmtAST>());
            break;
        case ASTKind::IfStmt:
            lowerIfStmt(stmt->as<IfStmtAST>());
            break;
        case ASTKind::SwitchStmt:
            lowerSwitchStmt(stmt->as<SwitchStmtAST>());
            break;
        case ASTKind::ForStmt:
            lowerForStmt(stmt->as<ForStmtAST>());
            break;
        case ASTKind::WhileStmt:
            lowerWhileStmt(stmt->as<WhileStmtAST>());
            break;
        case ASTKind::ReturnStmt:
            lowerReturnStmt(stmt->as<ReturnStmtAST>());
            break;
        case ASTKind::ExprStmt:
            lowerExprStmt(stmt->as<ExprStmtAST>());
            break;
        case ASTKind::AssignExpr:
            lowerAssignStmt(stmt->as<AssignExprAST>());
            break;
        default:
            // Ignore other statement types
            break;
    }
}

void IRLowering::lowerBlockStmt(BlockStmtAST* block) {
    if (!block) {
        return;
    }

    enterScope();
    for (StmtPtr stmt : block->stmts) {
        lowerStmt(stmt);
        // Check if the block was terminated (return/break/continue)
        if (m_builder.GetInsertBlock() && 
            m_builder.GetInsertBlock()->getTerminator()) {
            break;
        }
    }
    exitScope();
}

void IRLowering::lowerIfStmt(IfStmtAST* ifStmt) {
    if (!ifStmt) {
        return;
    }

    // Lower the condition
    auto* condition = lowerExpr(ifStmt->condition);
    if (!condition) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower if condition");
    }

    // Convert to boolean if needed
    if (condition->getType()->isIntegerTy(1)) {
        // Already boolean
    } else if (condition->getType()->isIntegerTy()) {
        condition = m_builder.CreateICmpNE(
            condition,
            llvm::ConstantInt::get(condition->getType(), 0),
            "ifcond"
        );
    }

    // Create blocks
    auto* thenBlock = llvm::BasicBlock::Create(m_context, "then", currentFunction().function);
    auto* elseBlock = llvm::BasicBlock::Create(m_context, "else", currentFunction().function);
    auto* mergeBlock = llvm::BasicBlock::Create(m_context, "ifcont", currentFunction().function);

    // Branch based on condition
    if (ifStmt->elseBranch) {
        m_builder.CreateCondBr(condition, thenBlock, elseBlock);
    } else {
        m_builder.CreateCondBr(condition, thenBlock, mergeBlock);
    }

    // Lower then branch
    m_builder.SetInsertPoint(thenBlock);
    lowerStmt(ifStmt->thenBranch);
    if (!m_builder.GetInsertBlock()->getTerminator()) {
        m_builder.CreateBr(mergeBlock);
    }

    // Lower else branch (if present)
    if (ifStmt->elseBranch) {
        m_builder.SetInsertPoint(elseBlock);
        lowerStmt(ifStmt->elseBranch);
        if (!m_builder.GetInsertBlock()->getTerminator()) {
            m_builder.CreateBr(mergeBlock);
        }
    }

    // Set insertion point to merge block
    m_builder.SetInsertPoint(mergeBlock);
}

void IRLowering::lowerSwitchStmt(SwitchStmtAST* switchStmt) {
    if (!switchStmt) {
        return;
    }

    // Lower the subject
    auto* subject = lowerExpr(switchStmt->subject);
    if (!subject) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower switch subject");
    }

    // Create blocks
    auto* defaultBlock = llvm::BasicBlock::Create(m_context, "switch.default", currentFunction().function);
    auto* mergeBlock = llvm::BasicBlock::Create(m_context, "switch.end", currentFunction().function);

    // Create switch instruction
    auto* switchInst = m_builder.CreateSwitch(subject, defaultBlock, switchStmt->cases.size());

    // Lower each case
    for (SwitchCasePtr caseNode : switchStmt->cases) {
        auto* caseBlock = llvm::BasicBlock::Create(m_context, "switch.case", currentFunction().function);

        // Add case values to switch
        for (ExprPtr value : caseNode->values) {
            auto* caseValue = lowerExpr(value);
            if (caseValue) {
                // Convert to constant if possible
                if (auto* constValue = llvm::dyn_cast<llvm::ConstantInt>(caseValue)) {
                    switchInst->addCase(constValue, caseBlock);
                }
            }
        }

        // Lower case body
        m_builder.SetInsertPoint(caseBlock);
        if (caseNode->body) {
            lowerStmt(caseNode->body);
        }
        if (!m_builder.GetInsertBlock()->getTerminator()) {
            m_builder.CreateBr(mergeBlock);
        }
    }

    // Lower default body
    m_builder.SetInsertPoint(defaultBlock);
    if (switchStmt->defaultBody) {
        lowerStmt(switchStmt->defaultBody);
    }
    if (!m_builder.GetInsertBlock()->getTerminator()) {
        m_builder.CreateBr(mergeBlock);
    }

    m_builder.SetInsertPoint(mergeBlock);
}

void IRLowering::lowerForStmt(ForStmtAST* forStmt) {
    if (!forStmt) {
        return;
    }

    // For now, only support range loops
    // Collection loops would require iterators

    // Lower the iterable
    auto* iterable = lowerExpr(forStmt->iterable);
    if (!iterable) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower for iterable");
    }

    // Create blocks
    auto* conditionBlock = llvm::BasicBlock::Create(m_context, "for.cond", currentFunction().function);
    auto* bodyBlock = llvm::BasicBlock::Create(m_context, "for.body", currentFunction().function);
    auto* incrementBlock = llvm::BasicBlock::Create(m_context, "for.inc", currentFunction().function);
    auto* exitBlock = llvm::BasicBlock::Create(m_context, "for.end", currentFunction().function);

    // Allocate index variable
    llvm::Value* indexVar = nullptr;
    if (forStmt->indexVar) {
        std::string name = internedToString(forStmt->indexVar->name);
        indexVar = allocateLocal(name, llvm::Type::getInt64Ty(m_context));
        m_builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), 0), indexVar);
    }

    // Save loop context for break/continue
    LoopContext loopCtx;
    loopCtx.conditionBlock = conditionBlock;
    loopCtx.bodyBlock = bodyBlock;
    loopCtx.incrementBlock = incrementBlock;
    loopCtx.exitBlock = exitBlock;
    m_loopStack.push_back(loopCtx);

    // Jump to condition
    m_builder.CreateBr(conditionBlock);

    // Condition block
    m_builder.SetInsertPoint(conditionBlock);
    if (indexVar) {
        auto* index = m_builder.CreateLoad(llvm::Type::getInt64Ty(m_context), indexVar, "index");
        auto* end = llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), 10); // FIXME: Get from iterable
        auto* cond = m_builder.CreateICmpSLT(index, end, "for.cond");
        m_builder.CreateCondBr(cond, bodyBlock, exitBlock);
    } else {
        // Infinite loop if no index
        m_builder.CreateBr(bodyBlock);
    }

    // Body block
    m_builder.SetInsertPoint(bodyBlock);
    enterScope();

    // Lower value variable
    if (forStmt->valueVar) {
        // Load current value from iterable
        // FIXME: Implement proper iteration
        auto* zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), 0);
        auto* value = m_builder.CreateLoad(llvm::Type::getInt64Ty(m_context), zero, "value");
        std::string name = internedToString(forStmt->valueVar->name);
        storeLocal(name, value);
    }

    // Lower body
    lowerStmt(forStmt->body);

    exitScope();

    // Jump to increment if no terminator
    if (!m_builder.GetInsertBlock()->getTerminator()) {
        m_builder.CreateBr(incrementBlock);
    }

    // Increment block
    m_builder.SetInsertPoint(incrementBlock);
    if (indexVar) {
        auto* index = m_builder.CreateLoad(llvm::Type::getInt64Ty(m_context), indexVar, "index");
        auto* inc = m_builder.CreateAdd(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), 1), "index.inc");
        m_builder.CreateStore(inc, indexVar);
    }
    m_builder.CreateBr(conditionBlock);

    // Exit block
    m_builder.SetInsertPoint(exitBlock);

    // Pop loop context
    m_loopStack.pop_back();
}

void IRLowering::lowerWhileStmt(WhileStmtAST* whileStmt) {
    if (!whileStmt) {
        return;
    }

    // Create blocks
    auto* conditionBlock = llvm::BasicBlock::Create(m_context, "while.cond", currentFunction().function);
    auto* bodyBlock = llvm::BasicBlock::Create(m_context, "while.body", currentFunction().function);
    auto* exitBlock = llvm::BasicBlock::Create(m_context, "while.end", currentFunction().function);

    // Save loop context
    LoopContext loopCtx;
    loopCtx.conditionBlock = conditionBlock;
    loopCtx.bodyBlock = bodyBlock;
    loopCtx.exitBlock = exitBlock;
    m_loopStack.push_back(loopCtx);

    // Jump to condition
    m_builder.CreateBr(conditionBlock);

    // Condition block
    m_builder.SetInsertPoint(conditionBlock);
    auto* condition = lowerExpr(whileStmt->condition);
    if (!condition) {
        m_loopStack.pop_back();
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower while condition");
    }
    m_builder.CreateCondBr(condition, bodyBlock, exitBlock);

    // Body block
    m_builder.SetInsertPoint(bodyBlock);
    enterScope();
    lowerStmt(whileStmt->body);
    exitScope();

    if (!m_builder.GetInsertBlock()->getTerminator()) {
        m_builder.CreateBr(conditionBlock);
    }

    // Exit block
    m_builder.SetInsertPoint(exitBlock);

    m_loopStack.pop_back();
}

void IRLowering::lowerReturnStmt(ReturnStmtAST* returnStmt) {
    if (!returnStmt) {
        return;
    }

    if (returnStmt->values.empty()) {
        // Void return
        m_builder.CreateRetVoid();
        currentFunction().hasReturn = true;
    } else if (returnStmt->values.size() == 1) {
        // Single return value
        auto* value = lowerExpr(returnStmt->values[0]);
        if (value) {
            m_builder.CreateRet(value);
            currentFunction().hasReturn = true;
        }
    } else {
        // Multiple return values - pack into a struct
        std::vector<llvm::Value*> values;
        for (ExprPtr expr : returnStmt->values) {
            auto* value = lowerExpr(expr);
            if (value) {
                values.push_back(value);
            }
        }
        // Pack into a struct
        auto* structType = llvm::StructType::get(m_context, 
            [&]() {
                std::vector<llvm::Type*> types;
                for (auto* v : values) {
                    types.push_back(v->getType());
                }
                return types;
            }()
        );
        llvm::Value* packed = llvm::UndefValue::get(structType);
        for (size_t i = 0; i < values.size(); ++i) {
            packed = m_builder.CreateInsertValue(packed, values[i], i);
        }
        m_builder.CreateRet(packed);
        currentFunction().hasReturn = true;
    }
}

void IRLowering::lowerExprStmt(ExprStmtAST* exprStmt) {
    if (!exprStmt) {
        return;
    }
    // Lower the expression and discard the result
    lowerExpr(exprStmt->expr);
    // If the expression has side effects, it was already lowered
}

void IRLowering::lowerAssignStmt(AssignExprAST* assign) {
    if (!assign) {
        return;
    }

    auto* lhs = lowerExpr(assign->lhs);
    auto* rhs = lowerExpr(assign->rhs);

    if (lhs && rhs) {
        // Store rhs into lhs
        m_builder.CreateStore(rhs, lhs);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Expressions
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRLowering::lowerExpr(ExprAST* expr) {
    if (!expr) {
        return nullptr;
    }

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return lowerLiteral(expr->as<LiteralExprAST>());
        case ASTKind::IdentifierExpr:
            return lowerIdentifier(expr->as<IdentifierExprAST>());
        case ASTKind::BinaryExpr:
            return lowerBinary(expr->as<BinaryExprAST>());
        case ASTKind::UnaryExpr:
            return lowerUnary(expr->as<UnaryExprAST>());
        case ASTKind::CallExpr:
            return lowerCall(expr->as<CallExprAST>());
        case ASTKind::FieldAccessExpr:
            return lowerFieldAccess(expr->as<FieldAccessExprAST>());
        case ASTKind::ModuleAccessExpr:
            return lowerModuleAccess(expr->as<ModuleAccessExprAST>());
        case ASTKind::IndexExpr:
            return lowerIndex(expr->as<IndexExprAST>());
        case ASTKind::SliceExpr:
            return lowerSlice(expr->as<SliceExprAST>());
        case ASTKind::IntrinsicCallExpr:
            return lowerIntrinsic(expr->as<IntrinsicCallExprAST>());
        case ASTKind::NullCoalesceExpr:
            return lowerNullCoalesce(expr->as<NullCoalesceExprAST>());
        case ASTKind::StructLiteralExpr:
            return lowerStructLiteral(expr->as<StructLiteralExprAST>());
        case ASTKind::ArrayLiteralExpr:
            return lowerArrayLiteral(expr->as<ArrayLiteralExprAST>());
        case ASTKind::PipelineExpr:
            return lowerPipeline(expr->as<PipelineExprAST>());
        case ASTKind::ComposeExpr:
            return lowerCompose(expr->as<ComposeExprAST>());
        case ASTKind::AnonFuncExpr:
            return lowerAnonFunc(expr->as<AnonFuncExprAST>());
        case ASTKind::RangeExpr:
            return lowerRange(expr->as<RangeExprAST>());
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported expression kind: " + std::to_string(static_cast<int>(expr->kind)));
    }
}

llvm::Value* IRLowering::lowerLiteral(LiteralExprAST* literal) {
    if (!literal) {
        return nullptr;
    }

    switch (literal->kind) {
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string str = internedToString(literal->value);
            int64_t value = std::stoll(str, nullptr, 0);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), value);
        }
        case LiteralKind::Float: {
            std::string str = internedToString(literal->value);
            double value = std::stod(str);
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(m_context), value);
        }
        case LiteralKind::True:
            return llvm::ConstantInt::getTrue(m_context);
        case LiteralKind::False:
            return llvm::ConstantInt::getFalse(m_context);
        case LiteralKind::Nil:
            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(m_context));
        case LiteralKind::String: {
            std::string str = internedToString(literal->value);
            return m_builder.CreateGlobalStringPtr(str, "str");
        }
        case LiteralKind::Char: {
            std::string str = internedToString(literal->value);
            if (str.length() == 1) {
                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(m_context), str[0]);
            }
            return llvm::ConstantInt::get(llvm::Type::getInt8Ty(m_context), 0);
        }
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported literal kind");
    }
}

llvm::Value* IRLowering::lowerIdentifier(IdentifierExprAST* identifier) {
    if (!identifier) {
        return nullptr;
    }

    std::string name = internedToString(identifier->name);

    // Check if it's a local variable
    auto* local = lookupLocal(name);
    if (local) {
        // Load the variable - handle pointer types
        if (local->getType()->isPointerTy()) {
            auto* elemType = local->getType()->getNonOpaquePointerElementType();
            if (elemType) {
                return m_builder.CreateLoad(elemType, local, name);
            }
        }
        return local;
    }

    // Check if it's a function parameter
    if (!m_functionStack.empty()) {
        auto it = currentFunction().parameters.find(name);
        if (it != currentFunction().parameters.end()) {
            return it->second;
        }
    }

    // Check if it's a global function
    auto* function = m_module->getFunction(name);
    if (function) {
        return function;
    }

    // Check if it's a global variable
    auto* global = m_module->getGlobalVariable(name);
    if (global) {
        return global;
    }

    throw IRLoweringError(IRLoweringError::Kind::VariableNotFound,
                          "Variable not found: " + name);
}

llvm::Value* IRLowering::lowerBinary(BinaryExprAST* binary) {
    if (!binary) {
        return nullptr;
    }

    auto* left = lowerExpr(binary->left);
    auto* right = lowerExpr(binary->right);
    if (!left || !right) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower binary expression operands");
    }

    switch (binary->op) {
        case BinaryOp::Add:
            return m_builder.CreateAdd(left, right, "add");
        case BinaryOp::Sub:
            return m_builder.CreateSub(left, right, "sub");
        case BinaryOp::Mul:
            return m_builder.CreateMul(left, right, "mul");
        case BinaryOp::Div:
            return m_builder.CreateSDiv(left, right, "div");
        case BinaryOp::Mod:
            return m_builder.CreateSRem(left, right, "mod");
        case BinaryOp::Eq:
            return m_builder.CreateICmpEQ(left, right, "eq");
        case BinaryOp::Ne:
            return m_builder.CreateICmpNE(left, right, "ne");
        case BinaryOp::Lt:
            return m_builder.CreateICmpSLT(left, right, "lt");
        case BinaryOp::Gt:
            return m_builder.CreateICmpSGT(left, right, "gt");
        case BinaryOp::Le:
            return m_builder.CreateICmpSLE(left, right, "le");
        case BinaryOp::Ge:
            return m_builder.CreateICmpSGE(left, right, "ge");
        case BinaryOp::And:
            return m_builder.CreateAnd(left, right, "and");
        case BinaryOp::Or:
            return m_builder.CreateOr(left, right, "or");
        case BinaryOp::BitAnd:
            return m_builder.CreateAnd(left, right, "bitand");
        case BinaryOp::BitOr:
            return m_builder.CreateOr(left, right, "bitor");
        case BinaryOp::BitXor:
            return m_builder.CreateXor(left, right, "bitxor");
        case BinaryOp::Shl:
            return m_builder.CreateShl(left, right, "shl");
        case BinaryOp::Shr:
            return m_builder.CreateLShr(left, right, "shr");
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported binary operator");
    }
}

llvm::Value* IRLowering::lowerUnary(UnaryExprAST* unary) {
    if (!unary) {
        return nullptr;
    }

    auto* operand = lowerExpr(unary->operand);
    if (!operand) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower unary expression operand");
    }

    switch (unary->op) {
        case UnaryOp::Neg:
            return m_builder.CreateNeg(operand, "neg");
        case UnaryOp::Not:
            return m_builder.CreateNot(operand, "not");
        case UnaryOp::BitNot:
            return m_builder.CreateNot(operand, "bitnot");
        default:
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Unsupported unary operator");
    }
}

llvm::Value* IRLowering::lowerCall(CallExprAST* call) {
    if (!call) {
        return nullptr;
    }

    // Get the callee
    auto* callee = lowerExpr(call->callee);
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
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Callee is not a function");
    }

    // Lower arguments
    std::vector<llvm::Value*> args;
    for (ExprPtr arg : call->args) {
        auto* value = lowerExpr(arg);
        if (value) {
            args.push_back(value);
        }
    }

    // Create the call
    if (call->hasArgPack) {
        // Argument pack - special handling for pipeline
        // The first argument will be filled by the pipeline
        return m_builder.CreateCall(function, args, "call");
    }

    return m_builder.CreateCall(function, args, "call");
}

llvm::Value* IRLowering::lowerFieldAccess(FieldAccessExprAST* field) {
    if (!field) {
        return nullptr;
    }

    auto* object = lowerExpr(field->object);
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

    // Check if it's a struct type
    if (objectType->isPointerTy()) {
        auto* elemType = objectType->getNonOpaquePointerElementType();
        if (elemType && elemType->isStructTy()) {
            auto* structType = llvm::cast<llvm::StructType>(elemType);
            // Find the field index
            std::string fieldName = internedToString(field->fieldName);
            // FIXME: We need to maintain a mapping of field names to indices
            // For now, assume the field is the first one
            int fieldIndex = 0;
            // Get the field pointer
            auto* ptr = m_builder.CreateStructGEP(structType, object, fieldIndex, fieldName);
            auto* fieldPtrType = ptr->getType();
            auto* fieldElemType = fieldPtrType->getNonOpaquePointerElementType();
            if (fieldElemType) {
                return m_builder.CreateLoad(fieldElemType, ptr, fieldName);
            }
        }
    }

    // If it's a pointer, try to dereference
    if (objectType->isPointerTy()) {
        auto* elemType = objectType->getNonOpaquePointerElementType();
        if (elemType) {
            auto* ptr = m_builder.CreateLoad(elemType, object, "field.ptr");
            return ptr;
        }
    }

    throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                          "Field access on non-struct type");
}

llvm::Value* IRLowering::lowerModuleAccess(ModuleAccessExprAST* moduleAccess) {
    if (!moduleAccess) {
        return nullptr;
    }

    // Module access is resolved during semantic analysis
    // At IR level, it's just a function call with module qualification
    std::string memberName = internedToString(moduleAccess->memberName);
    // Look up the member in the module
    // For now, treat it as a function call
    auto* function = m_module->getFunction(memberName);
    if (function) {
        return function;
    }

    throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                          "Module member not found: " + memberName);
}

llvm::Value* IRLowering::lowerIndex(IndexExprAST* index) {
    if (!index) {
        return nullptr;
    }

    auto* target = lowerExpr(index->target);
    auto* idx = lowerExpr(index->index);
    if (!target || !idx) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower index expression");
    }

    // Bounds check
    // Call runtime check function
    // FIXME: Implement bounds checking

    // Get the element pointer
    return m_builder.CreateGEP(target->getType(), target, idx, "index");
}

llvm::Value* IRLowering::lowerSlice(SliceExprAST* slice) {
    if (!slice) {
        return nullptr;
    }

    // Slices are views into arrays
    // For now, just return the target
    return lowerExpr(slice->target);
}

llvm::Value* IRLowering::lowerIntrinsic(IntrinsicCallExprAST* intrinsic) {
    if (!intrinsic) {
        return nullptr;
    }

    std::string name = internedToString(intrinsic->intrinsicName);

    if (name == "sizeof") {
        return lowerIntrinsicSizeof(intrinsic);
    } else if (name == "typeof") {
        return lowerIntrinsicTypeof(intrinsic);
    } else if (name == "tostr") {
        return lowerIntrinsicTostr(intrinsic);
    } else if (name == "sqrt") {
        return lowerIntrinsicSqrt(intrinsic);
    } else if (name == "memcpy") {
        return lowerIntrinsicMemcpy(intrinsic);
    } else if (name == "addrof") {
        return lowerIntrinsicAddrof(intrinsic);
    } else if (name == "ptrOffset") {
        return lowerIntrinsicPtrOffset(intrinsic);
    }

    throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                          "Unknown intrinsic: " + name);
}

llvm::Value* IRLowering::lowerIntrinsicSizeof(IntrinsicCallExprAST* intrinsic) {
    // #sizeof(T) - returns the size of a type
    // The type is passed as a type argument, not a value
    // FIXME: Need to get the type from the intrinsic arguments
    // For now, return 8 (pointer size)
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), 8);
}

llvm::Value* IRLowering::lowerIntrinsicTypeof(IntrinsicCallExprAST* intrinsic) {
    // #typeof(x) - returns the type name as a string
    // For now, return a string literal
    std::string typeName = "unknown";
    if (!intrinsic->args.empty()) {
        auto* arg = intrinsic->args[0];
        if (arg->kind == ASTKind::IdentifierExpr) {
            auto* ident = arg->as<IdentifierExprAST>();
            typeName = internedToString(ident->name);
        }
    }
    return m_builder.CreateGlobalStringPtr(typeName, "typeof");
}

llvm::Value* IRLowering::lowerIntrinsicTostr(IntrinsicCallExprAST* intrinsic) {
    // #tostr(x) - converts a value to a string
    // For now, return a string representation
    std::string str = "value";
    if (!intrinsic->args.empty()) {
        auto* arg = intrinsic->args[0];
        if (arg->kind == ASTKind::LiteralExpr) {
            auto* literal = arg->as<LiteralExprAST>();
            str = internedToString(literal->value);
        }
    }
    return m_builder.CreateGlobalStringPtr(str, "tostr");
}

llvm::Value* IRLowering::lowerIntrinsicSqrt(IntrinsicCallExprAST* intrinsic) {
    // #sqrt(x) - call llvm.sqrt intrinsic
    if (intrinsic->args.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "sqrt requires one argument");
    }

    auto* value = lowerExpr(intrinsic->args[0]);
    if (!value) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower sqrt argument");
    }

    // Get the llvm.sqrt intrinsic
    auto* sqrtFunc = llvm::Intrinsic::getDeclaration(
        m_module.get(),
        llvm::Intrinsic::sqrt,
        {value->getType()}
    );

    return m_builder.CreateCall(sqrtFunc, {value}, "sqrt");
}

llvm::Value* IRLowering::lowerIntrinsicMemcpy(IntrinsicCallExprAST* intrinsic) {
    // #memcpy(dst, src, len) - call llvm.memcpy intrinsic
    if (intrinsic->args.size() < 3) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "memcpy requires three arguments");
    }

    auto* dst = lowerExpr(intrinsic->args[0]);
    auto* src = lowerExpr(intrinsic->args[1]);
    auto* len = lowerExpr(intrinsic->args[2]);

    if (!dst || !src || !len) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower memcpy arguments");
    }

    // Get the llvm.memcpy intrinsic - use the specific ID
    auto* memcpyFunc = llvm::Intrinsic::getDeclaration(
        m_module.get(),
        llvm::Intrinsic::memcpy,
        {dst->getType(), src->getType()}
    );

    // Create the call
    std::vector<llvm::Value*> args = {
        dst,
        src,
        len,
        llvm::ConstantInt::get(llvm::Type::getInt1Ty(m_context), 0) // isVolatile
    };

    return m_builder.CreateCall(memcpyFunc, args, "memcpy");
}

llvm::Value* IRLowering::lowerIntrinsicAddrof(IntrinsicCallExprAST* intrinsic) {
    // #addrof(x) - returns the address of a value
    if (intrinsic->args.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "addrof requires one argument");
    }

    auto* value = lowerExpr(intrinsic->args[0]);
    if (!value) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower addrof argument");
    }

    // If value is a pointer, return it directly
    if (value->getType()->isPointerTy()) {
        return value;
    }

    // Otherwise, it's an lvalue - return the address
    // FIXME: Handle lvalue properly
    return value;
}

llvm::Value* IRLowering::lowerIntrinsicPtrOffset(IntrinsicCallExprAST* intrinsic) {
    // #ptrOffset(ptr, n) - pointer arithmetic
    if (intrinsic->args.size() < 2) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "ptrOffset requires two arguments");
    }

    auto* ptr = lowerExpr(intrinsic->args[0]);
    auto* offset = lowerExpr(intrinsic->args[1]);

    if (!ptr || !offset) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower ptrOffset arguments");
    }

    // Convert offset to i64 if needed
    if (offset->getType() != llvm::Type::getInt64Ty(m_context)) {
        offset = m_builder.CreateSExtOrTrunc(offset, llvm::Type::getInt64Ty(m_context));
    }

    // Use GEP for pointer arithmetic
    return m_builder.CreateGEP(ptr->getType(), ptr, offset, "ptroffset");
}

llvm::Value* IRLowering::lowerNullCoalesce(NullCoalesceExprAST* coalesce) {
    if (!coalesce) {
        return nullptr;
    }

    // x ?? fallback
    // Lower the value
    auto* value = lowerExpr(coalesce->value);
    if (!value) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower null coalesce value");
    }

    // Lower the fallback
    auto* fallback = lowerExpr(coalesce->fallback);
    if (!fallback) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower null coalesce fallback");
    }

    // Check if value is null
    auto* isNull = m_builder.CreateIsNull(value, "isnull");

    // Select between value and fallback
    return m_builder.CreateSelect(isNull, fallback, value, "coalesce");
}

llvm::Value* IRLowering::lowerStructLiteral(StructLiteralExprAST* structLit) {
    if (!structLit) {
        return nullptr;
    }

    // Get the struct type
    std::string typeName = internedToString(structLit->typeName);
    auto* structType = m_typeMapper.getRegisteredType(typeName);
    if (!structType) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Struct type not found: " + typeName);
    }

    // Allocate the struct
    auto* alloca = m_builder.CreateAlloca(structType, nullptr, "struct");

    // Initialize fields
    for (FieldInitPtr init : structLit->inits) {
        auto* value = lowerExpr(init->value);
        if (value) {
            // Find the field index
            // FIXME: Need to map field names to indices
            int fieldIndex = 0;
            std::string fieldName = internedToString(init->name);
            auto* ptr = m_builder.CreateStructGEP(structType, alloca, fieldIndex, fieldName);
            m_builder.CreateStore(value, ptr);
        }
    }

    // Load the struct
    return m_builder.CreateLoad(structType, alloca, "struct.literal");
}

llvm::Value* IRLowering::lowerArrayLiteral(ArrayLiteralExprAST* arrayLit) {
    if (!arrayLit) {
        return nullptr;
    }

    // For now, allocate a dynamic array
    // FIXME: Proper array allocation

    // Count elements
    size_t count = arrayLit->elements.size();
    auto* elementType = llvm::Type::getInt64Ty(m_context); // FIXME: Get element type

    // Allocate array
    auto* arrayType = llvm::ArrayType::get(elementType, count);
    auto* alloca = m_builder.CreateAlloca(arrayType, nullptr, "array");

    // Initialize elements
    for (size_t i = 0; i < count; ++i) {
        auto* value = lowerExpr(arrayLit->elements[i]);
        if (value) {
            auto* ptr = m_builder.CreateGEP(elementType, alloca, 
                {llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), 0),
                 llvm::ConstantInt::get(llvm::Type::getInt64Ty(m_context), i)},
                "array.elem");
            m_builder.CreateStore(value, ptr);
        }
    }

    // Return the array pointer
    return alloca;
}

llvm::Value* IRLowering::lowerPipeline(PipelineExprAST* pipeline) {
    if (!pipeline) {
        return nullptr;
    }

    // Lower the seed
    llvm::Value* current = lowerExpr(pipeline->seed);
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
        auto* callable = lowerExpr(step->callable);
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
            auto* value = lowerExpr(arg);
            if (value) {
                args.push_back(value);
            }
        }

        // Call the function
        current = m_builder.CreateCall(function, args, "pipeline.step");
    }

    return current;
}

llvm::Value* IRLowering::lowerCompose(ComposeExprAST* compose) {
    if (!compose) {
        return nullptr;
    }

    // Composition is compile-time - it should produce a new function
    // For now, just chain the calls
    llvm::Value* current = lowerExpr(compose->left);
    if (!current) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower compose left operand");
    }

    for (ComposeOperandPtr operand : compose->operands) {
        auto* callable = lowerExpr(operand->callable);
        if (!callable) {
            throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                                  "Failed to lower compose operand");
        }

        // Compose the functions
        // For now, treat as a call
        // FIXME: Proper function composition
        if (auto* func = llvm::dyn_cast<llvm::Function>(callable)) {
            // Create a call to the function with the current value
            current = m_builder.CreateCall(func, {current}, "compose");
        }
    }

    return current;
}

llvm::Value* IRLowering::lowerAnonFunc(AnonFuncExprAST* anonFunc) {
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

    auto* llvmFuncType = toLLVMFunctionType(funcType);
    if (!llvmFuncType) {
        throw IRLoweringError(IRLoweringError::Kind::TypeConversionFailed,
                              "Failed to convert anonymous function type");
    }

    auto* function = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::InternalLinkage,
        name,
        m_module.get()
    );

    // Lower the body
    if (anonFunc->body) {
        // Create function context
        FunctionContext fnCtx;
        fnCtx.function = function;
        fnCtx.entryBlock = llvm::BasicBlock::Create(m_context, "entry", function);
        fnCtx.funcType = funcType;
        m_functionStack.push_back(fnCtx);

        m_builder.SetInsertPoint(fnCtx.entryBlock);

        enterScope();
        lowerStmt(anonFunc->body);
        exitScope();

        if (!currentFunction().hasReturn && funcType->isVoid()) {
            m_builder.CreateRetVoid();
        }

        m_functionStack.pop_back();
    }

    return function;
}

llvm::Value* IRLowering::lowerRange(RangeExprAST* range) {
    if (!range) {
        return nullptr;
    }

    // Ranges are represented as a pair of values
    auto* lo = lowerExpr(range->lo);
    auto* hi = lowerExpr(range->hi);

    if (!lo || !hi) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower range bounds");
    }

    // Create a struct for the range
    auto* rangeType = llvm::StructType::get(m_context, {lo->getType(), hi->getType()});
    llvm::Value* rangeStruct = llvm::UndefValue::get(rangeType);
    rangeStruct = m_builder.CreateInsertValue(rangeStruct, lo, 0);
    rangeStruct = m_builder.CreateInsertValue(rangeStruct, hi, 1);

    return rangeStruct;
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Type Helpers
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* IRLowering::toLLVMType(TypeAST* type) {
    if (!type) {
        return nullptr;
    }

    return m_typeMapper.toLLVMType(type);
}

llvm::FunctionType* IRLowering::toLLVMFunctionType(FuncTypeAST* funcType) {
    if (!funcType) {
        return nullptr;
    }

    // Get return type
    llvm::Type* returnType = llvm::Type::getVoidTy(m_context);
    if (!funcType->returnTypes.empty()) {
        if (funcType->isCurried()) {
            // Curried function - return type is another function
            auto* next = funcType->getNext();
            if (next) {
                returnType = toLLVMFunctionType(next);
            }
        } else if (funcType->returnTypes.size() == 1) {
            returnType = toLLVMType(funcType->returnTypes[0]);
        } else {
            // Multiple return values - pack into a struct
            std::vector<llvm::Type*> types;
            for (TypePtr t : funcType->returnTypes) {
                auto* llvmType = toLLVMType(t);
                if (llvmType) {
                    types.push_back(llvmType);
                }
            }
            if (!types.empty()) {
                returnType = llvm::StructType::get(m_context, types);
            }
        }
    }

    // Get parameter types
    std::vector<llvm::Type*> paramTypes;
    for (ParamPtr param : funcType->params) {
        auto* paramType = toLLVMType(param->type);
        if (paramType) {
            paramTypes.push_back(paramType);
        }
    }

    // Check if the return type is a function type (currying)
    if (funcType->isCurried()) {
        // For curried functions, the return type is a function type
        // This is already handled above
    }

    return llvm::FunctionType::get(returnType, paramTypes, false);
}

llvm::Type* IRLowering::getPrimitiveType(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Bool:
            return llvm::Type::getInt1Ty(m_context);
        case PrimitiveKind::Int:
        case PrimitiveKind::Int32:
            return llvm::Type::getInt32Ty(m_context);
        case PrimitiveKind::Int64:
        case PrimitiveKind::Long:
            return llvm::Type::getInt64Ty(m_context);
        case PrimitiveKind::Int8:
        case PrimitiveKind::Byte:
            return llvm::Type::getInt8Ty(m_context);
        case PrimitiveKind::Int16:
        case PrimitiveKind::Short:
            return llvm::Type::getInt16Ty(m_context);
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint:
            return llvm::Type::getInt32Ty(m_context);
        case PrimitiveKind::Uint64:
        case PrimitiveKind::Ulong:
            return llvm::Type::getInt64Ty(m_context);
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Ubyte:
            return llvm::Type::getInt8Ty(m_context);
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Ushort:
            return llvm::Type::getInt16Ty(m_context);
        case PrimitiveKind::Float:
            return llvm::Type::getFloatTy(m_context);
        case PrimitiveKind::Double:
            return llvm::Type::getDoubleTy(m_context);
        case PrimitiveKind::String:
            return llvm::PointerType::getUnqual(m_context);
        case PrimitiveKind::Char:
            return llvm::Type::getInt8Ty(m_context);
        default:
            return llvm::Type::getInt32Ty(m_context); // Fallback
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Function Helpers
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* IRLowering::getOrCreateFunction(const std::string& name,
                                                FuncTypeAST* funcType,
                                                llvm::Module* module) {
    // Check if the function already exists
    if (auto* existing = module->getFunction(name)) {
        return existing;
    }

    // Create the function
    auto* llvmFuncType = toLLVMFunctionType(funcType);
    if (!llvmFuncType) {
        return nullptr;
    }

    return llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        name,
        module
    );
}

llvm::Function* IRLowering::getOrCreateForeignFunction(const std::string& name,
                                                       FuncTypeAST* funcType,
                                                       llvm::Module* module) {
    return getOrCreateFunction(name, funcType, module);
}

llvm::CallInst* IRLowering::createIntrinsicCall(const std::string& intrinsicName,
                                                const std::vector<llvm::Value*>& args,
                                                llvm::Type* returnType) {
    // Get the intrinsic declaration
    auto* intrinsicFunc = llvm::Intrinsic::getDeclaration(
        m_module.get(),
        llvm::StringRef(intrinsicName),
        {}
    );

    if (!intrinsicFunc) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Intrinsic not found: " + intrinsicName);
    }

    return m_builder.CreateCall(intrinsicFunc, args);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Runtime Functions
// ─────────────────────────────────────────────────────────────────────────────

void IRLowering::createPanicFunction(llvm::Module* module) {
    // void panic(const char* message)
    auto* panicType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(m_context),
        {llvm::PointerType::getUnqual(m_context)},  // i8*
        false
    );

    m_panicFunction = llvm::Function::Create(
        panicType,
        llvm::Function::ExternalLinkage,
        "_lucid_panic",
        module
    );
}

void IRLowering::createCheckBoundsFunction(llvm::Module* module) {
    // void check_bounds(void* ptr, uint64_t index, uint64_t size)
    auto* boundsType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(m_context),
        {
            llvm::PointerType::getUnqual(m_context),  // void* → i8*
            llvm::Type::getInt64Ty(m_context),
            llvm::Type::getInt64Ty(m_context)
        },
        false
    );

    m_checkBoundsFunction = llvm::Function::Create(
        boundsType,
        llvm::Function::ExternalLinkage,
        "_lucid_check_bounds",
        module
    );
}

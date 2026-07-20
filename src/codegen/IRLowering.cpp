/**
 * @file IRLowering.cpp
 * @brief Main entry point for IR lowering.
 * 
 * @responsibility Provides the main orchestration for lowering AST(s) to LLVM IR.
 *                 Delegates specific lowering tasks to specialized modules.
 *                 Supports both single-module and multi-module compilation.
 * 
 * @related_files
 *   - IRLoweringDecl.cpp   - Declaration lowering
 *   - IRLoweringStmt.cpp   - Statement lowering
 *   - IRLoweringExpr.cpp   - Expression lowering
 *   - IRLoweringIntrinsic.cpp - Intrinsic lowering
 *   - IRLoweringBuilder.cpp   - Helper builders
 */

#include "IRLowering.hpp"

#include "llvm/IR/Verifier.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Construction
// ─────────────────────────────────────────────────────────────────────────────

IRLowering::IRLowering(llvm::LLVMContext& context, TypeMapping& typeMapper, StringPool& stringPool)
    : m_context(context)
    , m_typeMapper(typeMapper)
    , m_stringPool(stringPool)
    , m_builder(context) {
}

std::string IRLowering::internedToString(InternedString name) const {
    std::string_view view = m_stringPool.lookup(name);
    return std::string(view);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Main Entry Points
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<llvm::Module> IRLowering::lower(ModuleAST* module,
                                                const std::string& moduleName) {
    if (!module) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Cannot lower null module");
    }

    // Delegate to the vector version
    return lower(std::vector<ModuleAST*>{module}, moduleName);
}

std::unique_ptr<llvm::Module> IRLowering::lower(const std::vector<ModuleAST*>& modules,
                                                const std::string& moduleName) {
    if (modules.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Cannot lower empty module list");
    }

    // Validate all modules
    for (ModuleAST* module : modules) {
        if (!module) {
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Cannot lower null module in list");
        }
    }

    return lowerImpl(modules, moduleName);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Internal Implementation
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<llvm::Module> IRLowering::lowerImpl(const std::vector<ModuleAST*>& modules,
                                                    const std::string& moduleName) {
    m_moduleName = moduleName;

    // Create the LLVM module
    m_module = std::make_unique<llvm::Module>(moduleName, m_context);

    // Set target triple
    m_module->setTargetTriple(llvm::sys::getDefaultTargetTriple());

    // Create runtime functions
    createPanicFunction(m_module.get());
    createCheckBoundsFunction(m_module.get());

    // Reset state
    m_scopeStack.clear();
    m_functionStack.clear();
    m_loopStack.clear();

    // Lower each module's declarations
    // Modules are processed in dependency order (imported first)
    for (ModuleAST* module : modules) {
        m_currentModule = module;

        // Lower each top-level declaration
        for (DeclPtr decl : module->decls) {
            try {
                IRDeclLowering::lowerDecl(*this, decl);
            } catch (const IRLoweringError& e) {
                // Log and continue with other declarations
                std::cerr << "Warning: Failed to lower declaration in module "
                          << internedToString(module->filePath) << ": " << e.what() << "\n";
            }
        }
    }

    m_currentModule = nullptr;

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
        enterScope();
    }
    return m_scopeStack.back();
}

llvm::AllocaInst* IRLowering::allocateLocal(const std::string& name, llvm::Type* type) {
    if (!isInFunction()) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Cannot allocate local outside a function");
    }

    auto& entryBlock = currentFunction().function->getEntryBlock();
    llvm::IRBuilder<> tmpBuilder(&entryBlock, entryBlock.begin());

    auto* alloca = tmpBuilder.CreateAlloca(type, nullptr, name);
    currentScope().locals[name] = alloca;
    return alloca;
}

llvm::Value* IRLowering::lookupLocal(const std::string& name) {
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
        ptr = allocateLocal(name, value->getType());
    }
    m_builder.CreateStore(value, ptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Function Context
// ─────────────────────────────────────────────────────────────────────────────

IRLowering::FunctionContext& IRLowering::currentFunction() {
    if (m_functionStack.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "No active function context");
    }
    return m_functionStack.back();
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Loop Context
// ─────────────────────────────────────────────────────────────────────────────

void IRLowering::pushLoop(const LoopContext& ctx) {
    m_loopStack.push_back(ctx);
}

void IRLowering::popLoop() {
    if (!m_loopStack.empty()) {
        m_loopStack.pop_back();
    }
}

bool IRLowering::hasLoop() const {
    return !m_loopStack.empty();
}

const IRLowering::LoopContext& IRLowering::currentLoop() const {
    if (m_loopStack.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "No active loop context");
    }
    return m_loopStack.back();
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
    return m_typeMapper.toLLVMFunctionType(funcType);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Runtime Functions
// ─────────────────────────────────────────────────────────────────────────────

void IRLowering::createPanicFunction(llvm::Module* module) {
    auto* panicType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(m_context),
        {llvm::PointerType::getUnqual(m_context)},
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
    auto* boundsType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(m_context),
        {
            llvm::PointerType::getUnqual(m_context),
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
/// @file codegen/passes/DeclarePass.cpp
/// @brief Declaration emission: types and function prototypes.

#include "Passes.hpp"

#include "codegen/LLVMTypeHelpers.hpp"
#include "codegen/emit/Emitter.hpp"
#include "codegen/Types.hpp"
#include "codegen/Abi.hpp"
#include "codegen/Program.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/trace/Trace.hpp"

namespace codegen {

namespace {

/// Emit the LLVM type for a struct declaration.
///
/// Idempotent — `Types::structType` caches.
void declareStruct(StructDeclAST* decl, ProgramState& program) {
    if (!decl || decl->hasSyntaxError) return;

    // Skip generic templates; specializations are emitted from the
    // module's `specializations` list.
    if (decl->isGeneric()) return;

    program.types().structType(decl);
}

/// Emit the LLVM type for an enum declaration.
void declareEnum(EnumDeclAST* decl, ProgramState& program) {
    if (!decl || decl->hasSyntaxError) return;
    program.types().enumType(decl);
}

/// Emit the LLVM function prototype for a function declaration.
void declareFunction(FuncDeclAST* decl, ProgramState& program) {
    if (!decl || decl->hasSyntaxError) return;

    // Skip generic templates; specializations are in `specializations`.
    if (decl->isGeneric()) return;

    // Foreign functions: emit `declare` with the raw name.
    if (decl->isForeignFunction) {
        std::string name = program.pool.lookup(decl->mangledName.isValid()
                                                ? decl->mangledName
                                                : decl->name);
        llvm::FunctionType* fnTy = program.types().functionType(
            decl->funcType, /*isClosure=*/false);
        if (!fnTy) return;

        llvm::Function* fn = program.module().getFunction(name);
        if (!fn) {
            fn = llvm::Function::Create(
                fnTy, llvm::GlobalValue::ExternalLinkage, name,
                program.module());
        }
        program.storeFunction(decl, fn);
        return;
    }

    // `cls`-shaped functions are values, not LLVM functions. Their
    // prototypes are the closure values, emitted during DefinePass.
    FuncShape shape = decl->funcType ? decl->funcType->shape : FuncShape::Fn;
    if (shape == FuncShape::Cls) return;

    // Non-foreign, non-generic: create the LLVM function prototype.
    llvm::Function* fn = program.lookupFunction(decl);
    if (fn) return;  // already declared

    std::string name = program.pool.lookup(decl->mangledName.isValid()
                                            ? decl->mangledName
                                            : decl->name);
    llvm::FunctionType* fnTy = program.types().functionType(
        decl->funcType, /*isClosure=*/false);
    if (!fnTy) return;

    llvm::GlobalValue::LinkageTypes linkage =
        decl->isExported ? llvm::GlobalValue::ExternalLinkage
                         : llvm::GlobalValue::InternalLinkage;

    fn = llvm::Function::Create(fnTy, linkage, name, program.module());

    // Name parameters for readable IR.
    size_t argIdx = 0;
    for (FuncTypeAST* stage = decl->funcType; stage; stage = stage->getNext()) {
        for (ParamAST* param : stage->params) {
            if (argIdx < fn->arg_size()) {
                fn->getArg(argIdx++)->setName(program.pool.lookup(param->name));
            }
        }
    }

    program.storeFunction(decl, fn);
}

/// Emit a declaration. Dispatch by kind.
void declare(DeclAST* decl, ProgramState& program) {
    if (!decl || decl->hasSyntaxError) return;

    switch (decl->kind) {
        case ASTKind::StructDecl: declareStruct(decl->as<StructDeclAST>(), program); break;
        case ASTKind::EnumDecl:   declareEnum(decl->as<EnumDeclAST>(), program); break;
        case ASTKind::FuncDecl:   declareFunction(decl->as<FuncDeclAST>(), program); break;
        case ASTKind::VarDecl:    /* module-level vars are fields, no decl */ break;
        case ASTKind::TraitDecl:  /* no runtime representation */ break;
        case ASTKind::ImportDecl: /* handled by module resolver */ break;
        default: break;
    }
}

} // anonymous namespace

void runDeclarePass(const std::vector<ModuleAST*>& modules,
                    ProgramState& program,
                    Manifest& manifest) {
    Trace::info("DeclarePass: ", modules.size(), " modules");

    for (ModuleAST* module : modules) {
        if (!module) continue;

        // Track which module we're in for cross-module symbol naming.
        program.currentModule = module;

        // ─── 1. Declarations from the module's parser output ──────────────
        for (DeclAST* decl : module->decls) {
            declare(decl, program);
        }

        // ─── 2. Specializations Sema built ────────────────────────────────
        // Concrete forms of generics that were referenced. Each is a
        // non-generic StructDeclAST or FuncDeclAST.
        for (DeclAST* spec : module->specializations) {
            declare(spec, program);
        }

        // ─── 3. Build the module instance struct type ─────────────────────
        // This populates the module layout cache. The global itself is
        // emitted by ModulePass.
        program.types().moduleInstanceType(module,
                                            program.moduleLayouts());

        // ─── 4. Manifest: record the module's symbol names ────────────────
        //
        // These symbol strings MUST match the ones ModulePass emits. Both
        // passes derive them the same way (`sanitizeForSymbol(filePath)` plus a
        // fixed prefix), so they stay in sync as long as that derivation is
        // the same in both places. If you change the sanitization or the
        // prefix in one pass, change it in the other.
        std::string sanitized = sanitizeForLLVMSymbol(
            program.pool.lookup(module->filePath));

        ManifestModule entry;
        entry.sourcePath = program.pool.lookup(module->filePath);
        entry.initSymbol = "__init_module_" + sanitized;
        entry.freeSymbol = "__free_module_" + sanitized;
        entry.stateSymbol = "__module_state_" + sanitized;
        manifest.modules.push_back(std::move(entry));
    }

    program.currentModule = nullptr;

    Trace::detail("DeclarePass complete");
}

} // namespace codegen
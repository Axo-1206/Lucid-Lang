/// @file codegen/CodeGen.cpp
/// @brief Implementation of the public entry point.

#include "CodeGen.hpp"

#include "Program.hpp"
#include "passes/Passes.hpp"

#include "core/ast/ModuleAST.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace codegen {

CodegenResult generate(const std::vector<ModuleAST*>& modules,
                       StringPool& pool,
                       DiagnosticEngine& diagnostics,
                       const CodeGenOptions& /*options*/) {
    CodegenResult result;

    // ─── 1. Create the program state ──────────────────────────────────────
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    std::string moduleName = "__lucid_program__";
    ProgramState program(pool, diagnostics, std::move(llvmCtx), moduleName);

    Trace::info("Codegen: ", modules.size(), " modules");

    // ─── 2. Run the three passes ──────────────────────────────────────────
    runDeclarePass(modules, program, result.manifest);
    runDefinePass(modules, program, result.manifest);
    runModulePass(modules, program, result.manifest);

    // ─── 3. Verify the module ─────────────────────────────────────────────
    std::string verifyError;
    llvm::raw_string_ostream verifyStream(verifyError);
    bool verified = !llvm::verifyModule(program.module(), &verifyStream);

    if (!verified) {
        diagnostics.error(
            DiagCode::Backend_InvalidIR, nullptr,
            "generated module failed verification: ", verifyError);
        result.success = false;
        // Still move the module out so the caller can inspect it for
        // debugging if desired.
        result.llvmCtx = program.releaseContext();
        result.module = program.releaseModule();
        return result;
    }

    // ─── 4. Move the module and context out ──────────────────────────────
    result.llvmCtx = program.releaseContext();
    result.module = program.releaseModule();
    result.success = true;

    Trace::detail("Codegen complete: module '", moduleName, "' verified");
    return result;
}

} // namespace codegen
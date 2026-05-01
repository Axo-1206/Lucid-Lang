/**
 * @file CodeGen.cpp
 *
 * @responsibility Orchestration driver for the codegen phase.
 *   Implements the three-pass pipeline, target machine initialisation,
 *   IR verification, optimization, JIT execution, and AOT object emission.
 *
 * @related CodeGen.hpp for the full architecture description.
 */

#include "CodeGen.hpp"
#include "ValueEnv.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// ── LLVM core ────────────────────────────────────────────────────────────────
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"

// ── LLVM target ──────────────────────────────────────────────────────────────
#include "llvm/Support/TargetSelect.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

// ── LLVM optimization passes ─────────────────────────────────────────────────
#include "llvm/Transforms/Scalar/Reg2Mem.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Utils.h"
// #include "llvm/Transforms/IPO/PassManagerBuilder.h" // Removed in LLVM 18
// #include "llvm/Transforms/Vectorize.h"             // Removed in LLVM 18

// ── LLVM output ──────────────────────────────────────────────────────────────
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/TargetPassConfig.h"

// ── LLVM ORC JIT ─────────────────────────────────────────────────────────────
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"

// ── Standard library ─────────────────────────────────────────────────────────
#include <cstdlib>    // std::system
#include <cassert>
#include <iostream>   // temporary debug output — remove when driver has proper logging
#include <optional>
#include "llvm/Support/CodeGen.h"

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations of the pass entry points defined in CodeGenDecl.cpp.
// These are free functions rather than methods so CodeGenDecl.cpp does not
// need to include CodeGen.hpp — breaking potential circular includes.
// ─────────────────────────────────────────────────────────────────────────────
class ValueEnv;
void codegenPass0(std::vector<ProgramAST*>& files, ValueEnv& env);
void codegenPass1A(std::vector<ProgramAST*>& files,
                   llvm::Module& mod, llvm::IRBuilder<>& builder,
                   llvm::LLVMContext& ctx, ValueEnv& env,
                   DiagnosticEngine& dc);
void codegenPass1B(llvm::Module& mod, llvm::IRBuilder<>& builder,
                   llvm::LLVMContext& ctx, ValueEnv& env,
                   DiagnosticEngine& dc);
void codegenPass2(std::vector<ProgramAST*>& files,
                  llvm::Module& mod, llvm::IRBuilder<>& builder,
                  llvm::LLVMContext& ctx, ValueEnv& env,
                  DiagnosticEngine& dc);

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations of runtime symbol registration (defined in this file).
// These register luc_runtime.c symbols with the JIT dylib so that JIT-compiled
// Luc code can call luc_alloc, luc_free, luc_printl, etc.
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {
    // Declared in luc_runtime.c — resolved via process symbol table in JIT,
    // or via object file linking in AOT.
    void* luc_alloc(uint64_t bytes);
    void  luc_free(void* ptr);
    void  luc_printl(const void* str);
    void  luc_release_dynarray(void* arr_ptr);
    void  luc_release_string(void* str_ptr);
    void  luc_release_closure(void* closure_ptr);
    void* luc_int_to_string(int64_t n);
    void* luc_float_to_string(float f);
    void* luc_bool_to_string(int8_t b);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

CodeGen::CodeGen(CompilationMode mode,
                 const AOTOptions& aotOptions,
                 DiagnosticEngine& dc)
    : mode_(mode)
    , aotOptions_(aotOptions)
    , dc_(dc)
    , ctx_(std::make_unique<llvm::LLVMContext>())
    , module_(std::make_unique<llvm::Module>("luc_module", *ctx_))
    , builder_(std::make_unique<llvm::IRBuilder<>>(*ctx_))
{
    // Target machine MUST be initialised before any pass runs.
    // @sizeof/@alignof use DataLayout during IR generation (Pass 1A/1B/2).
    initTargetMachine();
}

CodeGen::~CodeGen() = default;

// ─────────────────────────────────────────────────────────────────────────────
// initTargetMachine
//
// Initializes LLVM's target infrastructure and creates the TargetMachine for
// the host (or the cross-compilation triple in aotOptions_).
//
// Sets the DataLayout and target triple on the module so:
//   - @sizeof(T) / @alignof(T) produce correct values
//   - The backend emits correct code for the target ABI
//
// Called from the constructor. On failure emits E4001 and leaves
// targetMachine_ as nullptr — callers check dc_.hasErrors() before proceeding.
// ─────────────────────────────────────────────────────────────────────────────

void CodeGen::initTargetMachine() {
    // Initialise all built-in LLVM targets.
    // For the native target only: use InitializeNativeTarget*() variants.
    // We initialise all targets so cross-compilation works without rebuild.
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    // Determine the target triple.
    std::string triple = aotOptions_.targetTriple.empty()
        ? llvm::sys::getDefaultTargetTriple()
        : aotOptions_.targetTriple;

    // Look up the LLVM Target for this triple.
    std::string errorMsg;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, errorMsg);
    if (!target) {
        SourceLocation loc; // no source location for target init failure
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4001,
                  "target machine initialisation failed for triple '" + triple +
                  "': " + errorMsg);
        return;
    }

    // Create the TargetMachine.
    // CPU and features: "generic" / "" works for most targets.
    // Relocation model: PIC is required for shared libraries and position-independent
    // executables. Also required for JIT because the JIT allocates memory at
    // arbitrary addresses.
    llvm::TargetOptions targetOpts;
    auto relocModel = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
    targetMachine_.reset(
        target->createTargetMachine(triple, "generic", "", targetOpts, relocModel));

    if (!targetMachine_) {
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4001,
                  "failed to create target machine for triple '" + triple + "'");
        return;
    }

    // Stamp the DataLayout and triple onto the module.
    // This must happen before Pass 1A runs — any @sizeof/@alignof call during
    // IR generation queries module_->getDataLayout().
    module_->setDataLayout(targetMachine_->createDataLayout());
    module_->setTargetTriple(triple);
}

// ─────────────────────────────────────────────────────────────────────────────
// run — top-level entry point
// ─────────────────────────────────────────────────────────────────────────────

int CodeGen::run(std::vector<ProgramAST*>& files) {
    // Abort immediately if target machine failed to initialise.
    if (!targetMachine_) return -1;

    // ── Three-pass pipeline ───────────────────────────────────────────────────

    std::cout << "\n--- Codegen Pass 0: Generic instantiation collection ---\n";
    runPass0(files);

    std::cout << "\n--- Codegen Pass 1A: Non-generic forward declarations ---\n";
    runPass1A(files);
    if (dc_.hasErrors()) return -1;

    std::cout << "\n--- Codegen Pass 1B: Generic instantiation declarations ---\n";
    runPass1B();
    if (dc_.hasErrors()) return -1;

    std::cout << "\n--- Codegen Pass 2: Function bodies ---\n";
    runPass2(files);
    if (dc_.hasErrors()) return -1;

    // ── IR verification ───────────────────────────────────────────────────────
    if (!verifyModule()) return -1;

    // ── Mode divergence ───────────────────────────────────────────────────────
    if (mode_ == CompilationMode::JIT) {
        return runJIT();
    } else {
        return runAOT();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass delegation — actual implementation in CodeGenDecl.cpp
// ─────────────────────────────────────────────────────────────────────────────

void CodeGen::runPass0(std::vector<ProgramAST*>& files) {
    codegenPass0(files, env_);
}

void CodeGen::runPass1A(std::vector<ProgramAST*>& files) {
    codegenPass1A(files, *module_, *builder_, *ctx_, env_, dc_);
}

void CodeGen::runPass1B() {
    codegenPass1B(*module_, *builder_, *ctx_, env_, dc_);
}

void CodeGen::runPass2(std::vector<ProgramAST*>& files) {
    codegenPass2(files, *module_, *builder_, *ctx_, env_, dc_);
}

// ─────────────────────────────────────────────────────────────────────────────
// verifyModule
// ─────────────────────────────────────────────────────────────────────────────

bool CodeGen::verifyModule() {
    std::string verifyErrors;
    llvm::raw_string_ostream os(verifyErrors);

    if (llvm::verifyModule(*module_, &os)) {
        os.flush();
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4006,
                  "LLVM module verification failed:\n" + verifyErrors);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// runOptimizations
//
// O0: mem2reg only — always required. Promotes alloca/load/store patterns
//   into SSA φ-nodes. Required for correct IR regardless of opt level because
//   the codegen emits alloca for every local variable (simple, correct, and
//   mem2reg makes it efficient).
//
// O1: O0 + instcombine + simplifycfg + DCE.
//   Used by JIT to keep compile time low.
//
// O2: O1 + function inlining + loop unrolling + SLP vectorization.
//   Used by AOT for release builds. Heavier but run once at build time.
// ─────────────────────────────────────────────────────────────────────────────

void CodeGen::runOptimizations(OptLevel level) {
    // Function-level passes — applied to every function in the module.
    llvm::legacy::FunctionPassManager fpm(module_.get());

    // O0+: mem2reg — always required.
    // mem2reg only promotes allocas that are in the entry block of their function.
    // The codegen emits all allocas in the entry block by design (see CodeGenStmt.cpp).
    fpm.add(llvm::createPromoteMemoryToRegisterPass());

    if (level >= OptLevel::O1) {
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createCFGSimplificationPass());
        fpm.add(llvm::createDeadCodeEliminationPass());
        // fpm.add(llvm::createAggressiveDCEPass()); // Missing in LLVM 18 legacy PM
    }

    fpm.doInitialization();
    for (auto& fn : *module_) {
        fpm.run(fn);
    }

    if (level < OptLevel::O2) return;

    // Module-level passes — applied to the whole module.
    // Only run at O2 (AOT release builds).
    llvm::legacy::PassManager pm;

    // Function inlining — inline small functions at call sites.
    // pm.add(llvm::createFunctionInliningPass(225)); // Missing in LLVM 18 legacy PM

    // Loop unrolling — unroll small constant-bound loops.
    pm.add(llvm::createLoopUnrollPass());

    // SLP vectorization
    // pm.add(llvm::createSLPVectorizerPass()); // Missing in LLVM 18 legacy PM

    // Loop vectorization
    // pm.add(llvm::createLoopVectorizePass()); // Missing in LLVM 18 legacy PM

    // Global dead code elimination
    // pm.add(llvm::createGlobalDCEPass()); // Missing in LLVM 18 legacy PM

    pm.run(*module_);
}

// ─────────────────────────────────────────────────────────────────────────────
// runJIT
//
// Compiles the module via LLVM ORC JIT and calls the Luc main() function.
//
// Symbol resolution for @extern functions:
//   ORC's DynamicLibrarySearchGenerator looks up unresolved symbols in the
//   current process's dynamic symbol table. This means any @extern function
//   that is linked into the compiler process (e.g. malloc, printf, Vulkan
//   entry points loaded via dlopen) will be found automatically.
//
//   For user C libraries that are NOT loaded into the compiler process,
//   the caller must use jit->loadLibraryPermanently("libmylib.so") or
//   equivalent before calling run(). This is currently a driver responsibility
//   — CodeGen does not know about user library paths.
//
// main() ABI:
//   JIT calls main() with no arguments for the zero-param Luc form.
//   For the (args []string) form, the JIT wrapper constructs the slice
//   from argc/argv of the compiler process itself. The Luc program sees
//   the compiler's command-line arguments, which is correct behaviour for
//   a JIT-executed script-like workflow.
// ─────────────────────────────────────────────────────────────────────────────

int CodeGen::runJIT() {
    runOptimizations(OptLevel::O1);

    // Create the ORC LLJIT engine.
    auto jitExpected = llvm::orc::LLJITBuilder().create();
    if (!jitExpected) {
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4005,
                  "JIT engine creation failed: " +
                  llvm::toString(jitExpected.takeError()));
        return -1;
    }
    auto& jit = *jitExpected;

    // Register the current process's exported symbols so @extern functions
    // that are linked into the compiler (malloc, free, printf, etc.) resolve.
    auto& dylib = jit->getMainJITDylib();
    dylib.addGenerator(
        llvm::cantFail(
            llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                jit->getDataLayout().getGlobalPrefix())));

    // Explicitly register luc_runtime symbols.
    // These are defined in luc_runtime.c and compiled into the compiler binary.
    // ORC's DynamicLibrarySearchGenerator finds them through the process symbol
    // table as long as the linker exports them (which it does for extern "C" symbols).
    // No manual registration needed — the generator above handles them.
    // The explicit extern "C" declarations at the top of this file are sufficient.

    // Add the compiled module to the JIT.
    llvm::orc::ThreadSafeContext tsCtx(std::move(ctx_));
    llvm::orc::ThreadSafeModule tsModule(std::move(module_), tsCtx);

    if (auto err = jit->addIRModule(std::move(tsModule))) {
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4005,
                  "JIT module addition failed: " + llvm::toString(std::move(err)));
        return -1;
    }

    // Look up the main symbol.
    auto mainSymbol = jit->lookup("main");
    if (!mainSymbol) {
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4005,
                  "JIT symbol lookup failed: 'main' not found after compilation");
        return -1;
    }

    // Cast to the correct function pointer type and call.
    // Both main() forms emit  define i32 @main(...)  — the zero-param form
    // still uses the C ABI signature with argc/argv ignored.
    using MainFn = int(*)(int, char**);
    auto* mainFn = reinterpret_cast<MainFn>(mainSymbol->getValue());

    // Pass the compiler process's argc/argv so JIT-executed programs that use
    // (args []string) see the command-line arguments of the luc driver.
    // The JIT wrapper in Pass 2 converts these to a []string slice.
    // For zero-param main, these are ignored.
    return mainFn(0, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// runAOT
//
// Optimizes the module and emits it to an object file. Optionally emits debug
// listings (.ll, .s) and invokes the system linker to produce an executable.
//
// main() ABI for AOT:
//   The Luc main() is emitted as  define i32 @main(i32 %argc, i8** %argv)
//   regardless of which signature the programmer used (zero-param or []string).
//   The zero-param form emits main with argc/argv in the LLVM signature but
//   ignores them in the body. This is required because the C runtime (crt0)
//   always calls main with those arguments — the function must accept them
//   even if they are unused.
//   The []string form emits the argc/argv → []string conversion in the entry
//   block before any Luc code runs.
// ─────────────────────────────────────────────────────────────────────────────

int CodeGen::runAOT() {
    runOptimizations(OptLevel::O2);

    // Optionally dump LLVM IR for debugging.
    if (aotOptions_.emitLLVMAssembly) {
        printIR(aotOptions_.outputPath + ".ll");
    }

    // Emit the object file — always produced in AOT mode.
    const std::string objPath = aotOptions_.outputPath + ".o";
    if (!emitObjectFile(objPath)) {
        return -1; // E4002 or E4003 already emitted inside emitObjectFile
    }

    // Optionally emit native assembly for debugging.
    if (aotOptions_.emitNativeAssembly) {
        emitAssemblyFile(aotOptions_.outputPath + ".s");
    }

    // If an executable path is specified, invoke the linker.
    if (!aotOptions_.executablePath.empty()) {
        if (!linkExecutable(objPath,
                            aotOptions_.executablePath,
                            aotOptions_.extraLinkFlags)) {
            return -1; // E4004 already emitted inside linkExecutable
        }
    }

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// printIR — emit LLVM assembly text for debugging
// ─────────────────────────────────────────────────────────────────────────────

void CodeGen::printIR(const std::string& path) {
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
    if (ec) {
        // Not a fatal error — IR printing is a debug aid, not required output.
        std::cerr << "warning: could not write LLVM IR to '" << path
                  << "': " << ec.message() << "\n";
        return;
    }
    module_->print(out, nullptr);
    out.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// emitObjectFile
//
// Emits the module as a native object file to the given path.
// The object file is in the platform-native format:
//   Linux/macOS: ELF or Mach-O
//   Windows:     COFF
// The format is determined by the TargetMachine's target triple.
// ─────────────────────────────────────────────────────────────────────────────

bool CodeGen::emitObjectFile(const std::string& path) {
    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);

    if (ec) {
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4002,
                  "failed to open output file '" + path + "': " + ec.message());
        return false;
    }

    llvm::legacy::PassManager emitPM;
    if (targetMachine_->addPassesToEmitFile(
            emitPM, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4003,
                  "AOT object file emission failed: target does not support "
                  "object file generation");
        return false;
    }

    emitPM.run(*module_);
    dest.flush();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitAssemblyFile
//
// Emits native assembly text to the given path. Same flow as emitObjectFile
// but uses CGFT_AssemblyFile. Failure is non-fatal (debug aid only).
// ─────────────────────────────────────────────────────────────────────────────

bool CodeGen::emitAssemblyFile(const std::string& path) {
    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_Text);

    if (ec) {
        std::cerr << "warning: could not write assembly to '"
                  << path << "': " << ec.message() << "\n";
        return false;
    }

    llvm::legacy::PassManager emitPM;
    if (targetMachine_->addPassesToEmitFile(
            emitPM, dest, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        std::cerr << "warning: assembly emission not supported for this target\n";
        return false;
    }

    emitPM.run(*module_);
    dest.flush();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// linkExecutable
//
// Invokes the system linker (via clang) to produce a final executable.
//
// Why clang as the linker driver:
//   Linking a C-ABI binary requires more than just running ld. The linker needs:
//     - C runtime startup objects (crt0.o, crtbegin.o, crtend.o)
//     - Standard library linkage (-lc or equivalent)
//     - Platform-specific flags (--eh-frame-hdr, -dynamic-linker, etc.)
//   Manually constructing this command is platform-specific and fragile.
//   clang knows the correct flags for the host platform and handles all of this
//   automatically. Using clang as the driver is the standard approach used by
//   most LLVM-based compilers (Rust, Swift, Clang itself for C/C++).
//
//   Future milestone: replace std::system("clang ...") with direct lld C++ API
//   integration (lld::elf::link or lld::coff::link) to eliminate the subprocess,
//   improve error reporting, and remove the dependency on clang being in PATH.
// ─────────────────────────────────────────────────────────────────────────────

bool CodeGen::linkExecutable(const std::string& objectPath,
                              const std::string& executablePath,
                              const std::string& extraFlags) {
    // luc_runtime.o must be in the same directory as the compiler binary,
    // or on the library search path. The driver is responsible for ensuring this.
    std::string cmd = "clang "
                    + objectPath
                    + " luc_runtime.o "
                    + extraFlags
                    + " -o "
                    + executablePath;

    std::cout << "Linking: " << cmd << "\n";

    int result = std::system(cmd.c_str());
    if (result != 0) {
        SourceLocation loc;
        dc_.error(DiagnosticCategory::Backend, loc, DiagCode::E4004,
                  "linker invocation failed with exit code " +
                  std::to_string(result) +
                  "\nCommand was: " + cmd);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// defaultOptLevel
// ─────────────────────────────────────────────────────────────────────────────

OptLevel CodeGen::defaultOptLevel() const {
    // JIT: O1 — fast compilation, light optimization.
    //   The compiler and the user program share the same process.
    //   Compilation happens at runtime so keeping it fast matters.
    if (mode_ == CompilationMode::JIT) return OptLevel::O1;

    // AOT: O2 — full optimization.
    //   Compilation happens once at build time; the binary runs many times.
    //   Heavier optimization is worth the longer compile time.
    return OptLevel::O2;
}
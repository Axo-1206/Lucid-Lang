/**
 * @file CodeGen.hpp
 *
 * @responsibility Driver for the LLVM IR generation and execution/emission phase.
 *   Owns the LLVMContext, Module, IRBuilder, target machine, and either the ORC
 *   JIT engine (JIT mode) or the object-file emission pipeline (AOT mode).
 *
 * @architecture
 *   The codegen phase runs in three passes over every ProgramAST in the package:
 *
 *     Pass 0 — Generic instantiation collection
 *       Walks the fully-annotated AST without emitting IR. Discovers every unique
 *       (baseName, [concreteTypeArgs]) combination and populates the InstRegistry.
 *       No LLVM objects are created here.
 *
 *     Pass 1A — Non-generic forward declarations
 *       Emits llvm::StructType, llvm::Function signatures, enum constants, and
 *       @extern declarations for all non-generic top-level declarations.
 *       Must complete before Pass 1B so the type registry is fully populated.
 *
 *     Pass 1B — Generic instantiation forward declarations
 *       For each InstKey collected in Pass 0, emits the concrete llvm::StructType
 *       and llvm::Function signature using the TypeSubst built from the key.
 *
 *     Pass 2 — Function bodies
 *       Lowers every function body (regular and generic instantiation) into LLVM IR.
 *       Skips @extern functions — they have no body.
 *
 *   After the three passes the module is complete and the driver diverges:
 *
 *     JIT mode  — verify → light optimizations (mem2reg, instcombine, simplifycfg)
 *                 → ORC JIT compile → lookup "main" → call → return exit code
 *
 *     AOT mode  — verify → full optimizations (O2: inlining, unrolling, vectorization)
 *                 → emit object file to disk → optionally invoke linker
 *
 * @compilation_mode
 *   The mode is determined by the @aot / @jit attribute on the main entry point,
 *   read from SemanticAnalyzer::getCompilationMode() before CodeGen is constructed.
 *   Defaults to AOT when neither attribute is present.
 *
 * @data_layout_timing
 *   The DataLayout and target triple MUST be set on the module before Pass 1A
 *   runs, because @sizeof(T) and @alignof(T) intrinsic calls use
 *   DataLayout::getTypeAllocSize() during IR generation. Both JIT and AOT paths
 *   call initTargetMachine() in the constructor to ensure this.
 *
 * @related
 *   CodeGenDecl.cpp   — Pass 0/1/2 declaration lowering
 *   CodeGenExpr.cpp   — expression lowering (lowerExpr)
 *   CodeGenStmt.cpp   — statement lowering (lowerStmt)
 *   CodeGenType.cpp   — TypeAST* → llvm::Type* mapping
 *   ValueEnv.hpp      — scope stack, type/function/from registries, TypeSubst stack
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ValueEnv.hpp"
#include "ast/BaseAST.hpp"
#include "semantic/SemanticAnalyzer.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "codegen/ValueEnv.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Target/TargetMachine.h>

// Forward declarations — avoids pulling LLVM ORC headers into every TU.
namespace llvm { namespace orc { class LLJIT; } }

// ─────────────────────────────────────────────────────────────────────────────
// OptLevel — optimization intensity for the IR pass pipeline.
//
// JIT uses O1 (fast compile, light optimizations — compilation and execution
//   share the same process so compile time matters).
// AOT uses O2 (slower compile, heavier optimizations — binary is run many
//   times so runtime performance matters more).
// ─────────────────────────────────────────────────────────────────────────────
enum class OptLevel {
    O0,   // No optimization — debug builds, fastest compile
    O1,   // Light: mem2reg, instcombine, simplifycfg, DCE
    O2,   // Standard: O1 + function inlining, loop unrolling, vectorization
};

// ─────────────────────────────────────────────────────────────────────────────
// AOTOptions — configuration for the AOT emission path.
// ─────────────────────────────────────────────────────────────────────────────
struct AOTOptions {
    // Path prefix for the output object file.
    // CodeGen emits <outputPath>.o  (object file always produced).
    std::string outputPath = "out";

    // If non-empty, CodeGen invokes the system linker after emitting the object
    // file to produce a final executable at this path.
    // If empty, only the .o file is produced — the caller links manually.
    std::string executablePath;

    // Extra flags forwarded verbatim to the linker command line.
    // e.g. "-lm -lpthread" for math and POSIX thread libraries.
    // luc_runtime.o is always linked automatically — do not include it here.
    std::string extraLinkFlags;

    // Target triple override. Empty = use the host machine's default triple.
    // Set this for cross-compilation: "aarch64-linux-gnu", "x86_64-w64-mingw32", etc.
    std::string targetTriple;

    // Emit LLVM assembly (.ll) alongside the object file for debugging.
    // File is written to <outputPath>.ll
    bool emitLLVMAssembly = false;

    // Emit native assembly (.s) alongside the object file for debugging.
    // File is written to <outputPath>.s
    bool emitNativeAssembly = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// CodeGen — the codegen driver
// ─────────────────────────────────────────────────────────────────────────────
class CodeGen {
public:
    // ── Construction ──────────────────────────────────────────────────────────

    // mode        — from SemanticAnalyzer::getCompilationMode()
    // aotOptions  — ignored in JIT mode; describes output paths in AOT mode
    // dc          — shared diagnostic engine for E4xxx errors
    //
    // The constructor calls initTargetMachine() immediately so the DataLayout
    // is available to all three passes. Failure to initialize the target is
    // a fatal E4001 — the caller should check dc.hasErrors() before proceeding.
    explicit CodeGen(CompilationMode mode,
                     const AOTOptions& aotOptions,
                     DiagnosticEngine& dc);

    ~CodeGen();

    // ── Entry point ───────────────────────────────────────────────────────────

    // Run all three passes over the package, then either execute (JIT) or
    // emit an object file and optionally link (AOT).
    //
    // Returns the process exit code:
    //   JIT mode  — the int value returned by the Luc main() function
    //   AOT mode  — 0 on success, non-zero if any E4xxx error was emitted
    //
    // The caller should check dc.hasErrors() after run() regardless of the
    // return value — some E4xxx errors (e.g. module verification failure) are
    // reported via the DiagnosticEngine rather than through the return code.
    int run(std::vector<ProgramAST*>& files);

private:
    // ── LLVM core objects ─────────────────────────────────────────────────────

    std::unique_ptr<llvm::LLVMContext>    ctx_;
    std::unique_ptr<llvm::Module>         module_;
    std::unique_ptr<llvm::IRBuilder<>>    builder_;
    std::unique_ptr<llvm::TargetMachine>  targetMachine_;

    // ValueEnv owns all codegen registries — scope stack, type registry,
    // function registry, from-entry registry, InstRegistry, TypeSubst stack,
    // and loop control stack. Constructed at the start of run() and lives
    // for the duration of all three passes.
    ValueEnv env_;

    // ── Configuration ─────────────────────────────────────────────────────────

    CompilationMode  mode_;
    AOTOptions       aotOptions_;
    DiagnosticEngine& dc_;

    // ── Target machine initialisation ─────────────────────────────────────────

    // Called from the constructor. Initializes LLVM targets, selects the
    // target machine for the host (or aotOptions_.targetTriple if set), and
    // sets the DataLayout and target triple on the module.
    //
    // Must complete before Pass 1A — @sizeof/@alignof depend on the layout.
    // Emits E4001 into dc_ on failure.
    void initTargetMachine();

    // ── Three-pass pipeline ───────────────────────────────────────────────────

    // Pass 0: collect all generic instantiations into the InstRegistry.
    // No IR emitted. Walks the entire AST looking for:
    //   - StructLiteralExprAST::genericArgs (explicit struct type args)
    //   - CallExprAST::genericArgs (explicit function type args)
    //   - BehaviorAccessExprAST::concreteTypeArgs (implicit method receiver)
    // Each concrete (non-abstract) instantiation is recorded as an InstKey.
    void runPass0(std::vector<ProgramAST*>& files);

    // Pass 1A: forward-declare all non-generic types and function signatures.
    // Order within 1A: struct types first, then enums, then functions.
    // @extern functions are declared with ExternalLinkage — no body in Pass 2.
    void runPass1A(std::vector<ProgramAST*>& files);

    // Pass 1B: forward-declare all generic instantiations from the InstRegistry.
    // Depends on Pass 1A having populated the type registry (non-generic structs
    // must exist before generic structs that contain them as field types).
    void runPass1B();

    // Pass 2: lower all function bodies.
    // Skips @extern functions (isExtern = true on their Symbol).
    // For generic instantiation bodies, pushes the TypeSubst onto ValueEnv
    // before lowering and pops it after.
    void runPass2(std::vector<ProgramAST*>& files);

    // ── Post-pass: IR verification ────────────────────────────────────────────

    // Runs llvm::verifyModule. On failure emits E4006 and returns false.
    // Called after Pass 2 and before the JIT/AOT divergence point.
    bool verifyModule();

    // ── Optimization ─────────────────────────────────────────────────────────

    // Run the optimization pass pipeline at the given level.
    // O0: mem2reg only (always required for SSA correctness — never truly skipped)
    // O1: O0 + instcombine + simplifycfg + DCE
    // O2: O1 + function inlining + loop unrolling + SLP vectorization
    //
    // Both JIT and AOT call this — JIT with O1, AOT with O2 (or O0 for debug).
    void runOptimizations(OptLevel level);

    // ── JIT execution path ────────────────────────────────────────────────────

    // Runs the JIT path:
    //   1. runOptimizations(O1)
    //   2. Create ORC LLJIT engine
    //   3. Register runtime symbols (luc_alloc, luc_free, luc_printl, etc.)
    //   4. Add module to JIT dylib
    //   5. Lookup "main" symbol
    //   6. Call main(), return exit code
    //
    // User C libraries specified via @extern are resolved through the process's
    // dynamic symbol table automatically by ORC. No explicit registration needed
    // for symbols visible in the current process.
    //
    // Emits E4005 into dc_ if the "main" symbol cannot be found after compilation.
    // Returns the Luc main() return value (int) on success, -1 on JIT failure.
    int runJIT();

    // ── AOT emission path ─────────────────────────────────────────────────────

    // Runs the AOT path:
    //   1. runOptimizations(O2)  [or O0 if debug mode is requested]
    //   2. Optionally emit LLVM assembly (.ll) via printIR()
    //   3. Emit object file (<outputPath>.o) via emitObjectFile()
    //   4. Optionally emit native assembly (.s) via emitAssemblyFile()
    //   5. If executablePath is non-empty, invoke the system linker via linkExecutable()
    //
    // Returns 0 on success, non-zero if any E4xxx error was emitted.
    int runAOT();

    // Emits LLVM IR text to <outputPath>.ll for debugging.
    void printIR(const std::string& path);

    // Emits a native object file to <outputPath>.o.
    // Uses targetMachine_->addPassesToEmitFile with CGFT_ObjectFile.
    // Emits E4002 if the file cannot be opened.
    // Emits E4003 if the LLVM pass pipeline fails.
    bool emitObjectFile(const std::string& path);

    // Emits native assembly to <outputPath>.s.
    // Uses targetMachine_->addPassesToEmitFile with CGFT_AssemblyFile.
    bool emitAssemblyFile(const std::string& path);

    // Invokes the system linker to produce a final executable.
    //
    // Command constructed:
    //   clang <objectPath> luc_runtime.o <extraLinkFlags> -o <executablePath>
    //
    // "clang" is used as the linker driver because it correctly handles:
    //   - C runtime initialization (crt0/crtbegin/crtend)
    //   - Standard library linkage (-lc, libc++/libstdc++)
    //   - Platform-specific link flags automatically
    //
    // A future milestone may replace this with direct lld C++ API integration
    // to eliminate the subprocess and improve error reporting.
    //
    // Emits E4004 into dc_ if clang returns a non-zero exit code.
    // Returns true on success.
    bool linkExecutable(const std::string& objectPath,
                        const std::string& executablePath,
                        const std::string& extraFlags);

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Returns the OptLevel appropriate for the current mode and build type.
    // JIT → O1, AOT → O2, debug → O0.
    OptLevel defaultOptLevel() const;
};
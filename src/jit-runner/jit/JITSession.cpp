/// @file jit-runner/jit/JITSession.cpp
/// @brief ORC JIT session management implementation.

#include "JITSession.hpp"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace jit_runner {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

JITSession::JITSession(StringPool& pool,
                       DiagnosticEngine& diag,
                       const JITRunnerOptions& options)
    : m_pool(pool)
    , m_diag(diag)
    , m_options(options)
    , m_context(std::make_unique<llvm::LLVMContext>())
{
    // The JIT engine (m_jit) is not created here. It is created in
    // initialize(), after LLVM target initialization. Creating it in the
    // constructor would make the constructor fallible, which obscures
    // the object's lifecycle. The explicit initialize() call is the
    // single failure point.
    //
    // m_context is created here because it has no dependencies and its
    // destruction order must be after m_jit's (m_context is declared
    // before m_jit in the header, so it is destroyed after — correct).
}

JITSession::~JITSession() {
    // LLJIT's destructor cleans up its resources, including every
    // module that was added via addIRModule and not yet quarantined.
    //
    // Foreign libraries loaded via loadLibrary are *not* unloaded. The
    // process-wide dlopen / LoadLibrary registration is intentional,
    // and unloading a library whose symbols were resolved by JIT'd code
    // would leave those resolutions dangling.
    //
    // Destruction order is guaranteed by member declaration order: m_jit
    // is declared after m_context, so m_jit is destroyed first. This is
    // the correct order — the JIT holds references into the context that
    // must be valid until the JIT is destroyed.
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void JITSession::initialize() {
    if (m_initialized) return;

    try {
        // ─── 1. Initialize LLVM native target ─────────────────────────────
        //
        // These are idempotent across calls (LLVM's own target registry
        // deduplicates), so calling initialize() twice does not
        // double-register anything.
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        // ─── 2. Set up the JIT and platform symbols ───────────────────────
        setupTarget();
        setupPlatformSymbols();

        m_initialized = true;
    } catch (const JITError&) {
        // setupTarget and setupPlatformSymbols throw JITError with the
        // correct Kind. Propagate unchanged.
        throw;
    } catch (const std::exception& e) {
        throw JITError(
            JITError::Kind::InitFailed,
            std::string("JIT initialization failed: ") + e.what());
    }
}

void JITSession::setupTarget() {
    // ─── Detect the host target ───────────────────────────────────────────
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(jtmb.takeError(), os,
                                    "JITTargetMachineBuilder::detectHost: ");
        throw JITError(JITError::Kind::InitFailed,
                       "failed to detect host target: " + os.str());
    }

    // ─── Configure the target machine ─────────────────────────────────────
    //
    // PIC and Small code model are what a JIT wants by default on
    // 64-bit hosts. The optimization level is set from the runner's
    // options; the codegen result's module gets whatever codegen
    // produced, and the JIT's codegen follows the level set here.
    jtmb->setRelocationModel(llvm::Reloc::PIC_);
    jtmb->setCodeModel(llvm::CodeModel::Small);

    // ─── Create the LLJIT ─────────────────────────────────────────────────
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();

    if (!jit) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(jit.takeError(), os,
                                    "LLJITBuilder::create: ");
        throw JITError(JITError::Kind::InitFailed,
                       "failed to create LLJIT: " + os.str());
    }

    m_jit = std::move(*jit);
}

void JITSession::setupPlatformSymbols() {
    // ─── Install the process-wide symbol generator ────────────────────────
    //
    // `DynamicLibrarySearchGenerator::GetForCurrentProcess` installs a
    // resolver on the main JITDylib that looks up symbols in the current
    // process's symbol table. This is what makes:
    //
    //   - The Lucid runtime (`__lucid_*` symbols), statically linked
    //     into the `lucid` binary.
    //   - Foreign libraries loaded via `loadLibrary`, which `dlopen`s
    //     with `RTLD_GLOBAL` / `LoadLibraryA`.
    //
    // resolvable to JIT-compiled code.
    //
    // Without this generator, every foreign call and every runtime call
    // would fail to resolve at JIT time.
    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        m_jit->getDataLayout().getGlobalPrefix());

    if (!gen) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(gen.takeError(), os,
                                    "DynamicLibrarySearchGenerator: ");
        throw JITError(
            JITError::Kind::InitFailed,
            "failed to install platform symbol generator: " + os.str());
    }

    m_jit->getMainJITDylib().addGenerator(std::move(*gen));
}

// ─────────────────────────────────────────────────────────────────────────────
// Module Management
// ─────────────────────────────────────────────────────────────────────────────

llvm::orc::ResourceTrackerSP JITSession::addModule(
    llvm::orc::ThreadSafeModule tsm,
    const std::string& name)
{
    if (!m_initialized) {
        throw JITError(JITError::Kind::InitFailed,
                       "JIT session is not initialized");
    }

    // ─── Access the module for verification and setup ─────────────────────
    //
    // `tsm.getModule()` returns a non-owning pointer, valid while the
    // `ThreadSafeModule` is alive. The pointer is used below for
    // verification and target-configuration; after `tsm` is moved into
    // `addIRModule`, the pointer is stale and must not be reused.
    llvm::Module* module = tsm.getModule();
    if (!module) {
        throw JITError(JITError::Kind::ModuleAddFailed,
                       "cannot add a null module");
    }

    // ─── Match the host's target triple and data layout ───────────────────
    //
    // Codegen does not set these — it does not know the host target. The
    // JIT does. Setting them here (before verification) ensures the
    // module is verified against the host's data layout; verifying
    // against an unset layout would miss size and alignment mismatches.
    module->setTargetTriple(m_jit->getTargetTriple().str());
    module->setDataLayout(m_jit->getDataLayout());

    // ─── Verify the module ────────────────────────────────────────────────
    //
    // `verifyModule` checks structural invariants: types are consistent,
    // SSA form is valid, terminators are correct, and — with the data
    // layout now set — sizes and alignments match the host.
    //
    // A module that fails verification would produce undefined behavior
    // in the JIT; rejecting it here gives a clear diagnostic instead.
    std::string verifyErr;
    llvm::raw_string_ostream verifyStream(verifyErr);
    if (llvm::verifyModule(*module, &verifyStream)) {
        std::string msg = "module '" + name +
                          "' failed verification: " + verifyStream.str();
        m_diag.error(DiagCode::Backend_InvalidIR, nullptr, msg);
        throw JITError(JITError::Kind::ModuleAddFailed, msg);
    }

    // ─── Add to the JIT under a fresh tracker ─────────────────────────────
    //
    // `addIRModule` consumes the `ThreadSafeModule`: the module and its
    // `LLVMContext` are now owned by the JIT, and will be released when
    // the tracker is removed (via `quarantine`) or when the JIT is
    // destroyed.
    //
    // The tracker identifies the module for later removal. It is
    // returned to the caller, which stores it and passes it back to
    // `quarantine` when the program is replaced.
    auto tracker = m_jit->getMainJITDylib().createResourceTracker();

    if (auto err = m_jit->addIRModule(tracker, std::move(tsm))) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(std::move(err), os,
                                    "LLJIT::addIRModule: ");
        m_diag.error(DiagCode::Backend_CodegenError, nullptr,
                     "failed to add module '", name, "' to JIT: ", os.str());
        throw JITError(JITError::Kind::ModuleAddFailed,
                       "failed to add module '" + name + "': " + os.str());
    }

    return tracker;
}

void JITSession::quarantine(llvm::orc::ResourceTrackerSP tracker) {
    if (!tracker) return;

    // ─── Remove the tracker ───────────────────────────────────────────────
    //
    // `ResourceTracker::remove` releases every symbol the tracker owns
    // from the JITDylib, and releases the module and its context. In
    // single-threaded mode this is immediate. In multi-threaded mode,
    // this is unsafe while a thread could be executing in the removed
    // module; the current design assumes the runner is single-threaded
    // at this point.
    //
    // Do NOT propagate failures as exceptions. `remove` can fail with
    // `ResourceTrackerDefunct` (the tracker is already gone, which means
    // the JIT's resources are released — the state we wanted) or
    // `SymbolsCouldNotBeRemoved` (a symbol is mid-materialization, which
    // is unreachable from the runner's perspective because the program's
    // state was already freed before quarantine was called). Both are
    // non-fatal from the runner's point of view. Log and continue.
    if (auto err = tracker->remove()) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(std::move(err), os,
                                    "JITSession::quarantine: ");
        if (m_options.verbose) {
            std::cerr << "Warning: " << os.str() << "\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Symbol Lookup
// ─────────────────────────────────────────────────────────────────────────────

void* JITSession::lookupSymbol(const std::string& name) {
    if (!m_initialized) {
        throw JITError(JITError::Kind::InitFailed,
                       "JIT session is not initialized");
    }

    // ─── Look up the symbol ───────────────────────────────────────────────
    //
    // `LLJIT::lookup` returns an `Expected<JITEvaluatedSymbol>`. Two
    // outcomes are possible:
    //
    //   - Error: the resolver failed (not "symbol not found," which is
    //     its own case). Throw as LookupFailed.
    //
    //   - Success with a null address: LLVM reports "symbol not found"
    //     via a successful lookup with a null `ExecutorAddr`. This is
    //     the common "symbol doesn't exist" case. Return nullptr.
    //
    //   - Success with a non-null address: return it.
    //
    // The distinction matters: "not found" is a normal outcome the
    // runner handles by diagnosing a compiler bug; "resolver failed" is
    // an LLVM-level error.
    auto sym = m_jit->lookup(name);

    if (!sym) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        llvm::logAllUnhandledErrors(sym.takeError(), os,
                                    "LLJIT::lookup: ");
        throw JITError(JITError::Kind::LookupFailed,
                       "symbol lookup failed for '" + name + "': " + os.str());
    }

    return reinterpret_cast<void*>(sym->getValue());
}

// ─────────────────────────────────────────────────────────────────────────────
// Foreign Libraries
// ─────────────────────────────────────────────────────────────────────────────

void JITSession::loadLibrary(const std::string& path) {
    if (!m_initialized) {
        throw JITError(JITError::Kind::InitFailed,
                       "JIT session is not initialized");
    }

    // ─── Idempotence ──────────────────────────────────────────────────────
    //
    // A library already loaded is skipped. The runner may load the same
    // library across multiple programs; loading it twice is unnecessary
    // and (on some platforms) has side effects (incremented reference
    // counts, duplicate initializers).
    if (m_loadedLibraries.count(path)) {
        return;
    }

    // ─── Load ─────────────────────────────────────────────────────────────
    //
    // On Linux/macOS, use `RTLD_NOW` (resolve all symbols eagerly, catch
    // missing symbols at load time rather than at first call) and
    // `RTLD_GLOBAL` (make the loaded symbols visible to subsequent
    // `dlopen` calls, which is what LLVM's process-wide symbol
    // generator needs to find them).
    //
    // On Windows, `LoadLibraryA` makes the DLL's exports available
    // process-wide by default.
    //
    // Errors are logged and thrown as LibraryLoadFailed. The runner
    // catches and converts to its own error type.
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle) {
        std::string msg = "LoadLibraryA failed for '" + path +
                          "' (error " + std::to_string(GetLastError()) + ")";
        m_diag.error(DiagCode::Ffi_SymbolNotFound, nullptr, msg);
        throw JITError(JITError::Kind::LibraryLoadFailed, msg);
    }
    // The library stays loaded for the process's lifetime. The handle is
    // not stored: LoadLibraryA maintains the reference count, and the
    // process's exit handles cleanup. Explicitly unloading would risk
    // dangling symbol resolutions in JIT'd code.
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char* dlErr = dlerror();
        std::string msg = "dlopen failed for '" + path + "': " +
                          (dlErr ? dlErr : "unknown error");
        m_diag.error(DiagCode::Ffi_SymbolNotFound, nullptr, msg);
        throw JITError(JITError::Kind::LibraryLoadFailed, msg);
    }
    // Same note as Windows: the library stays loaded. dlclose is not
    // called; the process's exit handles cleanup.
#endif

    m_loadedLibraries.insert(path);
}

} // namespace jit_runner
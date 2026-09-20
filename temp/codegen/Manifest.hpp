/// @file codegen/Manifest.hpp
/// @brief The plain-data contract between CodeGen and its consumers.
///
/// ─── Why This File Exists ──────────────────────────────────────────────────
/// Before this file, the only thing CodeGen returned was a
/// `std::vector<std::unique_ptr<llvm::Module>>`. Everything the interpreter
/// or AOT needed to know about the program — the entry symbol's name, the
/// per-module init/free symbol names, the runtime symbols the module
/// imports, the foreign libraries to link — had to be re-derived by the
/// consumer by walking the LLVM modules, or by re-walking the AST, or by
/// convention.
///
/// The manifest is that information, expressed as plain data. It contains
/// no LLVM types and no AST pointers, so it can be consumed by code that
/// does not link LLVM, and it can be serialized for debugging.
///
/// ─── What The Manifest Is NOT ──────────────────────────────────────────────
/// It is NOT a symbol table. The LLVM module is the symbol table. The
/// manifest lists the symbols the *host* has to know about by name —
/// the ones it binds to, or the ones it calls without having a reference
/// to the llvm::Function. Everything else stays in the module.
///
/// It is NOT a description of the program's types. Types live in the AST
/// and in the LLVM module's type table. The manifest describes the
/// *runtime handshake*, not the program.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codegen {

/// @brief The entry point's symbol name and calling convention.
///
/// The host reads this to know what to call to start the program. For an
/// executable, the entry is `main` with C linkage. For a library, it is
/// whatever `@[export]` marked; the manifest records it so the host does
/// not have to guess.
struct ManifestEntry {
    /// The linker-level symbol name of the entry function. For a standard
    /// executable this is "main"; for an `@[export]`-marked function it is
    /// the function's mangled name.
    std::string symbol;

    /// True if the entry is `main` with C linkage, so the host can call it
    /// as `int(*)(int, char**)`. False for a Lucid-ABI entry, which the host
    /// must reach through the generated `__lucid_program_init` /
    /// `__lucid_program_free` pair.
    bool isCMain = false;
};

/// @brief One module's contribution to the program.
///
/// A module here is a `ModuleAST` — one source file. With the module-state
/// change (one `@__module_state_<id>` global per module instead of the old
/// per-variable globals + `@__lucid_module_instances` table), the host
/// needs to know each module's state symbol and the per-module helper
/// symbols, but it no longer needs a module id or a table capacity.
struct ManifestModule {
    /// The module's source path, as written by the user. For diagnostics
    /// and for the file watcher's hot-reload mapping.
    std::string sourcePath;

    /// The synthesized initializer's symbol name. Always emitted, even when
    /// the module has no state, so the host has a uniform contract.
    std::string initSymbol;

    /// The synthesized finalizer's symbol name. Always emitted. Runs the
    /// module's drops in reverse declaration order.
    std::string freeSymbol;

    /// The name of the module's state global, or empty if the module has no
    /// state. With the global-state model this is the single symbol the
    /// host (and the debugger) can use to find a module's storage.
    std::string stateSymbol;
};

/// @brief A runtime symbol the generated module imports.
///
/// The interpreter uses this to register the runtime library's symbols with
/// the JIT before executing. The AOT linker uses it to decide which runtime
/// object files to link. The list is derived from functions.def: it is the
/// set of rows CodeGen actually emitted a reference to, not the whole
/// surface.
struct ManifestRuntimeSymbol {
    /// The linker-level symbol name (the second column of functions.def).
    std::string symbol;

    /// True if the generated module *defines* the symbol (only possible for
    /// the synthesized program init/free and per-module helpers, which are
    /// emitted by CodeGen, not imported from the runtime).
    bool definedByModule = false;
};

/// @brief A foreign library the program must be linked against or dlopen'd.
///
/// Collected from `@[link("name")]` attributes on the module. The
/// interpreter passes each to DynLink; the AOT linker passes each as a
/// `-l` flag.
struct ManifestForeignLibrary {
    /// The library name as written in the attribute, e.g. "opengl".
    std::string name;
};

/// @brief The complete manifest for one `generate()` call.
///
/// Produced by CodeGen, consumed by the interpreter's JIT session and by
/// the AOT linker. See the file header for what the manifest is and is not.
struct Manifest {
    /// The program's entry point. Empty for a library build with no
    /// `@[export]` entry.
    ManifestEntry entry;

    /// One entry per input module, in the order CodeGen lowered them.
    /// For `run`, this is the order the JIT adds them to the session.
    /// For `build`, this is the order `aot/ModuleMerge` links them.
    ///
    /// The order is the dependency order produced by ModuleResolver. CodeGen
    /// does not re-derive it; it trusts the input order and records it here.
    std::vector<ManifestModule> modules;

    /// The runtime symbols the generated code references. Used by the
    /// interpreter to register the runtime with the JIT, and by the AOT
    /// linker to pick the runtime object files to link.
    std::vector<ManifestRuntimeSymbol> runtimeSymbols;

    /// The foreign libraries to link against or dlopen. Collected from
    /// `@[link(...)]` attributes.
    std::vector<ManifestForeignLibrary> foreignLibraries;

    /// The synthesized program-level initializer. Calls each module's init
    /// in dependency order. Always emitted.
    std::string programInitSymbol;

    /// The synthesized program-level finalizer. Calls each module's free in
    /// reverse dependency order. Always emitted.
    std::string programFreeSymbol;

    /// True if the program was generated with the module-state model (one
    /// `@__module_state_<id>` global per module). This is a transitional
    /// flag: it lets the interpreter support both the old table-based
    /// handshake and the new global-based one during the rewrite. It will
    /// be removed once the old path is deleted.
    bool usesGlobalModuleState = true;
};

} // namespace codegen
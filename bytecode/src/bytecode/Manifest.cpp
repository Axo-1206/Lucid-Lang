/// @file bytecode/Manifest.cpp
/// @brief Invariant checking for the module manifest.

#include "bytecode/Manifest.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

#include <unordered_set>

namespace lucid::bytecode {

void Manifest::checkInvariants() const {
    std::unordered_set<std::string> seenModules;
    std::unordered_set<std::string> seenMangledNames;

    for (size_t mi = 0; mi < modules.size(); ++mi) {
        const Module& mod = modules[mi];

        AST_ASSERT_MSG(!mod.modulePath.empty(),
            "Manifest: a module has an empty module path");

        auto [it, inserted] = seenModules.insert(mod.modulePath);
        AST_ASSERT_MSG(inserted,
            "Manifest: two modules have the same module path — "
            "the compiler should have registered each path once");

        // Import aliases must be unique within a module.
        std::unordered_set<std::string> seenAliases;
        for (const auto& imp : mod.imports) {
            AST_ASSERT_MSG(!imp.alias.empty(),
                "Manifest: an import has an empty alias");
            AST_ASSERT_MSG(!imp.targetPath.empty(),
                "Manifest: an import has an empty target path");

            auto [aliasIt, aliasInserted] = seenAliases.insert(imp.alias);
            AST_ASSERT_MSG(aliasInserted,
                "Manifest: a module has two imports with the same alias — "
                "Sema should have rejected the second one");
        }

        // Mangled names are globally unique. A collision is a compiler
        // bug: mangled names are the artifact's primary key.
        auto checkUnique = [&](const std::string& name, const char* kind) {
            AST_ASSERT_MSG(!name.empty(),
                "Manifest: a mangled name is empty");
            auto [nameIt, nameInserted] = seenMangledNames.insert(name);
            AST_ASSERT_MSG(nameInserted,
                "Manifest: two declarations have the same mangled name — "
                "the mangling scheme is not producing unique names");
            (void)nameIt;
            (void)kind;
        };

        for (const auto& fn : mod.functions) checkUnique(fn, "function");
        for (const auto& b  : mod.bindings)  checkUnique(b,  "binding");
        for (const auto& t  : mod.tables)    checkUnique(t,  "table");
    }

    // The entry module, if specified, must be one of the modules in the
    // list.
    if (!entryModulePath.empty()) {
        AST_ASSERT_MSG(seenModules.count(entryModulePath) > 0,
            "Manifest: the entry module path names a module that is not "
            "in the manifest's module list");
    }
}

} // namespace lucid::bytecode
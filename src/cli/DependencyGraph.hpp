/// @file cli/DependencyGraph.hpp
/// @brief Bi‑directional dependency graph for hot‑reload.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"

#include <unordered_map>
#include <vector>
#include <set>
#include <queue>
#include <string>

namespace cli {

/**
 * @brief Bi‑directional dependency graph built from ModuleAST::imports.
 *
 * This graph is used by the file watcher to determine which files
 * must be re‑parsed when a file changes.
 *
 * ─── Usage ──────────────────────────────────────────────────────────────
 * ```cpp
 * DependencyGraph graph;
 * graph.build(modules);
 *
 * // When "io/math.luc" changes:
 * auto affected = graph.getAffected("io/math.luc");
 * // → ["io/math.luc", "main.luc"] (if main imports math)
 * ```
 *
 * @note Keys are resolved file paths (e.g., "io/math.luc"),
 *       NOT user‑written import paths (e.g., "io.math").
 */
class DependencyGraph {
public:
    /// @brief Build graph from a list of modules.
    void build(const std::vector<ModuleAST*>& modules) {
        // Clear existing state
        dependencies_.clear();
        dependents_.clear();
        allModules_.clear();

        for (ModuleAST* module : modules) {
            InternedString name = module->filePath;
            allModules_.insert(name);

            // Mark that this module exists
            dependencies_[name] = {};

            // For each import, add forward and reverse edges
            for (InternedString imp : module->imports) {
                dependencies_[name].push_back(imp);
                dependents_[imp].insert(name);
            }
        }
    }

    /// @brief Get all modules that depend on a given module (transitive).
    /// @param changedModule The changed module's resolved file path.
    /// @return List of affected module paths (includes the changed module).
    std::vector<InternedString> getAffected(InternedString changedModule) const {
        std::vector<InternedString> result;
        std::set<uint32_t> visited;

        // BFS starting from changedModule
        std::queue<InternedString> queue;
        queue.push(changedModule);
        visited.insert(changedModule.id);

        while (!queue.empty()) {
            InternedString current = queue.front();
            queue.pop();

            // Add to results
            result.push_back(current);

            // Add all dependents (modules that import this one)
            auto it = dependents_.find(current);
            if (it != dependents_.end()) {
                for (InternedString dep : it->second) {
                    if (visited.find(dep.id) == visited.end()) {
                        visited.insert(dep.id);
                        queue.push(dep);
                    }
                }
            }
        }

        return result;
    }

    /// @brief Get direct imports of a module.
    const std::vector<InternedString>& getImports(InternedString module) const {
        auto it = dependencies_.find(module);
        if (it == dependencies_.end()) {
            static const std::vector<InternedString> empty;
            return empty;
        }
        return it->second;
    }

    /// @brief Check if a module exists in the graph.
    bool hasModule(InternedString module) const {
        return dependencies_.find(module) != dependencies_.end();
    }

    /// @brief Get all modules in the graph.
    const std::set<InternedString>& getAllModules() const {
        return allModules_;
    }

    /// @brief Get the number of modules.
    size_t size() const { return allModules_.size(); }

private:
    // Forward: module → list of imported modules (resolved file paths)
    std::unordered_map<InternedString, std::vector<InternedString>> dependencies_;

    // Reverse: module → set of modules that import it
    std::unordered_map<InternedString, std::set<InternedString>> dependents_;

    // All modules in the graph
    std::set<InternedString> allModules_;
};

} // namespace cli
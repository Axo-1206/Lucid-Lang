/**
 * @file ModuleResolver.cpp
 * @brief Implementation of module resolution and caching.
 */

#include "parser/ModuleResolver.hpp"
#include "core/ast/DeclAST.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>

namespace parser {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

ModuleResolver::ModuleResolver(const std::filesystem::path& packageRoot, StringPool& pool)
    : packageRoot_(packageRoot)
    , pool_(pool) {
    // Ensure package root exists
    if (!std::filesystem::exists(packageRoot_)) {
        std::filesystem::create_directories(packageRoot_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Path Resolution
// ─────────────────────────────────────────────────────────────────────────────

InternedString ModuleResolver::resolveImportPath(InternedString importPath) {
    // 1. Check cache first
    auto cacheIt = importPathToFile_.find(importPath);
    if (cacheIt != importPathToFile_.end()) {
        return cacheIt->second;
    }
    
    // 2. Convert import path to relative file path
    std::string relativePath = importPathToRelativePath(importPath);
    if (relativePath.empty()) {
        return InternedString();
    }
    
    // 3. Try to resolve the relative path
    std::filesystem::path foundPath = resolveRelativePath(relativePath);
    if (!foundPath.empty()) {
        InternedString result = pool_.intern(relativePath);
        importPathToFile_[importPath] = result;
        resolvedPathCache_[result] = foundPath;
        return result;
    }
    
    // 4. Try without .luc extension (directory module)
    if (relativePath.size() > 6 && relativePath.substr(relativePath.size() - 6) == ".luc") {
        std::string withoutExt = relativePath.substr(0, relativePath.size() - 6);
        foundPath = resolveRelativePath(withoutExt);
        if (!foundPath.empty()) {
            InternedString result = pool_.intern(withoutExt);
            importPathToFile_[importPath] = result;
            resolvedPathCache_[result] = foundPath;
            return result;
        }
    }
    
    // Not found
    return InternedString();
}

std::filesystem::path ModuleResolver::getModuleFilePath(InternedString modulePath) const {
    // Check cache first
    auto cacheIt = resolvedPathCache_.find(modulePath);
    if (cacheIt != resolvedPathCache_.end()) {
        return cacheIt->second;
    }
    
    // Build path from package root
    std::string pathStr = pool_.lookup(modulePath);
    std::filesystem::path result = packageRoot_;
    
    // Split path by '/' and append each component
    std::string path = std::string(pathStr);
    size_t start = 0;
    size_t end = path.find('/');
    while (end != std::string::npos) {
        result /= path.substr(start, end - start);
        start = end + 1;
        end = path.find('/', start);
    }
    if (start < path.size()) {
        result /= path.substr(start);
    }
    
    // Cache the result
    resolvedPathCache_[modulePath] = result;
    return result;
}

bool ModuleResolver::isValidImportPath(InternedString importPath) const {
    // Try to resolve without caching
    std::string relativePath = importPathToRelativePath(importPath);
    if (relativePath.empty()) {
        return false;
    }
    return !resolveRelativePath(relativePath).empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Module Caching
// ─────────────────────────────────────────────────────────────────────────────

bool ModuleResolver::isModuleParsed(InternedString modulePath) const {
    return parsedModules_.find(modulePath) != parsedModules_.end();
}

ModuleAST* ModuleResolver::getParsedModule(InternedString modulePath) const {
    auto it = parsedModules_.find(modulePath);
    return it != parsedModules_.end() ? it->second : nullptr;
}

void ModuleResolver::cacheModule(InternedString modulePath, ModuleAST* ast) {
    if (parsedModules_.find(modulePath) != parsedModules_.end()) {
        return;  // Already cached
    }
    
    parsedModules_[modulePath] = ast;
    
    // ─── Resolve and store imports on the module ──────────────────────────
    resolveAndStoreImports(ast);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dependency Order
// ─────────────────────────────────────────────────────────────────────────────

int ModuleResolver::getModuleOrder(InternedString modulePath) const {
    auto it = moduleOrderMap_.find(modulePath);
    return it != moduleOrderMap_.end() ? it->second : -1;
}

const std::vector<ModuleAST*>& ModuleResolver::getModulesInOrder() const {
    // If module order hasn't been computed yet, compute it now
    if (moduleOrderList_.empty() && !parsedModules_.empty()) {
        const_cast<ModuleResolver*>(this)->computeTopologicalOrder();
    }
    return moduleOrderList_;
}

ModuleAST* ModuleResolver::findModuleByAlias(ModuleAST* fromModule, InternedString alias) const {
    return findModuleByAliasInternal(fromModule, alias);
}

// ─────────────────────────────────────────────────────────────────────────────
// Circular Import Detection
// ─────────────────────────────────────────────────────────────────────────────

bool ModuleResolver::isParsing(InternedString modulePath) const {
    for (InternedString path : parsingStack_) {
        if (path == modulePath) {
            return true;
        }
    }
    return false;
}

void ModuleResolver::pushParsing(InternedString modulePath) {
    parsingStack_.push_back(modulePath);
}

void ModuleResolver::popParsing() {
    if (!parsingStack_.empty()) {
        parsingStack_.pop_back();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// File Operations
// ─────────────────────────────────────────────────────────────────────────────

std::string ModuleResolver::readModuleSource(InternedString filePath) const {
    std::filesystem::path fullPath = getModuleFilePath(filePath);
    
    if (!fileExists(fullPath)) {
        return "";
    }
    
    std::ifstream file(fullPath);
    if (!file.is_open()) {
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool ModuleResolver::moduleFileExists(InternedString filePath) const {
    std::filesystem::path fullPath = getModuleFilePath(filePath);
    return fileExists(fullPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// Module Enumeration
// ─────────────────────────────────────────────────────────────────────────────

 std::vector<ModuleAST*> ModuleResolver::getAllModules() const {
    std::vector<ModuleAST*> result;
    result.reserve(parsedModules_.size());
    for (const auto& [path, module] : parsedModules_) {
        result.push_back(module);
    }
    return result;
} 
    
size_t ModuleResolver::getModuleCount() const {
    return parsedModules_.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// Private Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string ModuleResolver::importPathToRelativePath(InternedString importPath) const {
    std::string useStr = pool_.lookup(importPath);
    if (useStr.empty()) {
        return "";
    }
    
    std::string relativePath;
    relativePath.reserve(useStr.size() + 7); // +7 for ".luc"
    
    // Replace '.' with '/' for path separators
    for (char c : useStr) {
        relativePath += (c == '.') ? '/' : c;
    }
    
    relativePath += ".luc";
    return relativePath;
}

std::filesystem::path ModuleResolver::resolveRelativePath(const std::string& relativePath) const {
    std::filesystem::path rootPath = packageRoot_ / relativePath;
    if (fileExists(rootPath)) {
        return rootPath;
    }
    return {};
}

bool ModuleResolver::fileExists(const std::filesystem::path& fullPath) const {
    return std::filesystem::exists(fullPath) && std::filesystem::is_regular_file(fullPath);
}

void ModuleResolver::resolveAndStoreImports(ModuleAST* module) {
    if (!module) return;
    
    // Clear existing resolved imports
    module->resolvedImports.clear();
    
    // For each import declaration in the module
    for (DeclAST* decl : module->decls) {
        if (decl->isa<ImportDeclAST>()) {
            ImportDeclAST* import = decl->as<ImportDeclAST>();
            
            // Resolve the import path
            InternedString resolvedPath = resolveImportPath(import->path);
            if (!resolvedPath.isValid()) {
                // Error will be reported by Sema
                continue;
            }
            
            // Get the parsed module
            ModuleAST* targetModule = getParsedModule(resolvedPath);
            if (!targetModule) {
                // Module not parsed yet - this is a forward reference
                // The module will be parsed later, so we skip for now
                // The import will be resolved when all modules are parsed
                continue;
            }
            
            // Store the resolved import mapping alias → module
            module->resolvedImports[import->alias] = targetModule;
        }
    }
    
    // Update the legacy imports vector for backward compatibility
    module->imports.clear();
    for (const auto& [alias, mod] : module->resolvedImports) {
        module->imports.push_back(mod->filePath);
    }
}

void ModuleResolver::buildDependencyGraph(
    std::unordered_map<InternedString, std::vector<InternedString>>& graph,
    std::unordered_map<InternedString, int>& indegree
) const {
    // Initialize graph and indegree for all modules
    for (const auto& [path, module] : parsedModules_) {
        graph[path] = {};
        indegree[path] = 0;
    }
    
    // Build edges: dependency → dependent
    for (const auto& [path, module] : parsedModules_) {
        for (const auto& [alias, targetModule] : module->resolvedImports) {
            InternedString targetPath = targetModule->filePath;
            if (parsedModules_.find(targetPath) != parsedModules_.end()) {
                graph[targetPath].push_back(path);  // dependency → dependent
                indegree[path]++;
            }
        }
    }
}

void ModuleResolver::computeTopologicalOrder() {
    if (parsedModules_.empty()) return;
    
    // ─── Build dependency graph ────────────────────────────────────────────
    std::unordered_map<InternedString, std::vector<InternedString>> graph;
    std::unordered_map<InternedString, int> indegree;
    buildDependencyGraph(graph, indegree);
    
    // ─── Kahn's algorithm ──────────────────────────────────────────────────
    std::queue<InternedString> queue;
    for (const auto& [path, count] : indegree) {
        if (count == 0) queue.push(path);
    }
    
    moduleOrderList_.clear();
    moduleOrderMap_.clear();
    
    int order = 0;
    while (!queue.empty()) {
        InternedString path = queue.front();
        queue.pop();
        
        moduleOrderMap_[path] = order++;
        
        // Get the module AST and add to ordered list
        auto it = parsedModules_.find(path);
        if (it != parsedModules_.end()) {
            moduleOrderList_.push_back(it->second);
        }
        
        for (InternedString dependent : graph[path]) {
            if (--indegree[dependent] == 0) {
                queue.push(dependent);
            }
        }
    }
    
    // ─── Check for cycles ──────────────────────────────────────────────────
    if (moduleOrderList_.size() != parsedModules_.size()) {
        // Some modules weren't processed - there's a cycle
        // We'll let Sema report the error
        // For now, assign remaining modules a high order
        for (const auto& [path, module] : parsedModules_) {
            if (moduleOrderMap_.find(path) == moduleOrderMap_.end()) {
                moduleOrderMap_[path] = order++;
                moduleOrderList_.push_back(module);
            }
        }
    }
    
    // ─── Update ModuleAST with order ──────────────────────────────────────
    for (const auto& [path, module] : parsedModules_) {
        auto it = moduleOrderMap_.find(path);
        module->dependencyOrder = (it != moduleOrderMap_.end()) ? it->second : -1;
    }
}

ModuleAST* ModuleResolver::findModuleByAliasInternal(ModuleAST* fromModule, InternedString alias) const {
    if (!fromModule) return nullptr;
    
    auto it = fromModule->resolvedImports.find(alias);
    if (it != fromModule->resolvedImports.end()) {
        return it->second;
    }
    
    return nullptr;
}

} // namespace parser
/**
 * @file ModuleResolver.cpp
 * @brief Implementation of module resolution and caching.
 */

#include "parser/ModuleResolver.hpp"
#include "core/ast/BaseAST.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

namespace parser {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

ModuleResolver::ModuleResolver(const std::filesystem::path& packageRoot, StringPool& pool)
    : packageRoot_(packageRoot)
    , pool_(pool) {
    // Ensure package root exists
    if (!std::filesystem::exists(packageRoot_)) {
        // Create if it doesn't exist (for tests)
        std::filesystem::create_directories(packageRoot_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Path Resolution
// ─────────────────────────────────────────────────────────────────────────────

InternedString ModuleResolver::resolveUsePath(InternedString usePath) {
    // 1. Check custom mappings first (from build manifest)
    auto it = customMappings_.find(usePath);
    if (it != customMappings_.end()) {
        return it->second;
    }
    
    // 2. Check cache
    auto cacheIt = usePathToFile_.find(usePath);
    if (cacheIt != usePathToFile_.end()) {
        return cacheIt->second;
    }
    
    // 3. Convert import path to relative file path
    std::string relativePath = usePathToRelativePath(usePath);
    if (relativePath.empty()) {
        return InternedString();
    }
    
    // 4. Try to resolve the relative path
    std::filesystem::path foundPath = resolveRelativePath(relativePath);
    if (!foundPath.empty()) {
        // Store in cache
        InternedString result = pool_.intern(relativePath);
        usePathToFile_[usePath] = result;
        resolvedPathCache_[result] = foundPath;
        return result;
    }
    
    // 5. Try without .lucid extension
    if (relativePath.size() > 6 && relativePath.substr(relativePath.size() - 6) == ".lucid") {
        std::string withoutExt = relativePath.substr(0, relativePath.size() - 6);
        foundPath = resolveRelativePath(withoutExt);
        if (!foundPath.empty()) {
            InternedString result = pool_.intern(withoutExt);
            usePathToFile_[usePath] = result;
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
    std::string_view pathStr = pool_.lookup(modulePath);
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

bool ModuleResolver::isValidUsePath(InternedString usePath) const {
    // Try to resolve without caching
    std::string relativePath = usePathToRelativePath(usePath);
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
    if (it != parsedModules_.end()) {
        return it->second;
    }
    return nullptr;
}

void ModuleResolver::cacheModule(InternedString modulePath, ModuleAST* ast) {
    if (parsedModules_.find(modulePath) == parsedModules_.end()) {
        parsedModules_[modulePath] = ast;
        moduleOrder_.push_back(modulePath);
    }
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
    
    // Check if file exists
    if (!std::filesystem::exists(fullPath)) {
        return "";
    }
    
    // Read file
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
    return std::filesystem::exists(fullPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// Private Helpers
// ─────────────────────────────────────────────────────────────────────────────

InternedString ModuleResolver::normalizePath(std::string_view path) const {
    // Convert Windows backslashes to forward slashes
    std::string normalized;
    normalized.reserve(path.size());
    for (char c : path) {
        if (c == '\\') {
            normalized += '/';
        } else {
            normalized += c;
        }
    }
    return pool_.intern(normalized);
}

std::string ModuleResolver::usePathToRelativePath(InternedString usePath) const {
    std::string_view useStr = pool_.lookup(usePath);
    if (useStr.empty()) {
        return "";
    }
    
    std::string relativePath;
    relativePath.reserve(useStr.size() + 7); // +7 for ".lucid" + slashes
    
    // Replace '.' with '/' for path separators
    for (char c : useStr) {
        if (c == '.') {
            relativePath += '/';
        } else {
            relativePath += c;
        }
    }
    
    // Add .lucid extension
    relativePath += ".lucid";
    
    return relativePath;
}

std::filesystem::path ModuleResolver::resolveRelativePath(const std::string& relativePath) const {
    // Check package root first
    std::filesystem::path rootPath = packageRoot_ / relativePath;
    if (std::filesystem::exists(rootPath)) {
        return rootPath;
    }
    
    // Check additional search paths
    for (const auto& searchPath : searchPaths_) {
        std::filesystem::path fullPath = searchPath / relativePath;
        if (std::filesystem::exists(fullPath)) {
            return fullPath;
        }
    }
    
    // Not found
    return {};
}

} // namespace parser
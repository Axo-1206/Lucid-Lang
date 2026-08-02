/**
 * @file ModuleResolver.cpp
 * @brief Implementation of module resolution and caching.
 */

#include "parser/ModuleResolver.hpp"

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

InternedString ModuleResolver::resolveUsePath(InternedString usePath) {
    // 1. Check cache first
    auto cacheIt = usePathToFile_.find(usePath);
    if (cacheIt != usePathToFile_.end()) {
        return cacheIt->second;
    }
    
    // 2. Convert import path to relative file path
    std::string relativePath = usePathToRelativePath(usePath);
    if (relativePath.empty()) {
        return InternedString();
    }
    
    // 3. Try to resolve the relative path
    std::filesystem::path foundPath = resolveRelativePath(relativePath);
    if (!foundPath.empty()) {
        InternedString result = pool_.intern(relativePath);
        usePathToFile_[usePath] = result;
        resolvedPathCache_[result] = foundPath;
        return result;
    }
    
    // 4. Try without .lucid extension (directory module)
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
    return it != parsedModules_.end() ? it->second : nullptr;
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
// Private Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string ModuleResolver::usePathToRelativePath(InternedString usePath) const {
    std::string useStr = pool_.lookup(usePath);
    if (useStr.empty()) {
        return "";
    }
    
    std::string relativePath;
    relativePath.reserve(useStr.size() + 7); // +7 for ".lucid"
    
    // Replace '.' with '/' for path separators
    for (char c : useStr) {
        relativePath += (c == '.') ? '/' : c;
    }
    
    relativePath += ".lucid";
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

} // namespace parser
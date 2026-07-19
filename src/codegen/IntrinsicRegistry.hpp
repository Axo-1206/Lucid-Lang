/**
 * @file IntrinsicRegistry.hpp
 * @brief Maps Lucid intrinsic names to LLVM intrinsic IDs and provides validation.
 * 
 * @responsibility Provides compile-time and runtime mapping between Lucid
 *                 intrinsic names and their corresponding LLVM intrinsic IDs.
 *                 Also handles validation of arguments for each intrinsic.
 * 
 * @related_files
 *   - src/codegen/IRLoweringIntrinsic.cpp - uses IntrinsicRegistry for lowering
 *   - src/sema/IntrinsicValidator.cpp - uses IntrinsicRegistry for validation
 *   - src/ast/ExprAST.hpp - IntrinsicCallExprAST node
 */

#pragma once

#include "llvm/IR/Intrinsics.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>
#include <cstddef>

/**
 * @brief Information about a registered intrinsic.
 */
struct IntrinsicInfo {
    llvm::Intrinsic::ID id;              // LLVM intrinsic ID
    std::string name;                     // Full intrinsic name
    size_t minArgs;                       // Minimum number of arguments
    size_t maxArgs;                       // Maximum number of arguments (-1 = unlimited)
    bool isVarArg;                        // Whether the intrinsic is variadic
    std::vector<llvm::Type*> typeParams;  // Expected type parameters (if any)
    
    IntrinsicInfo()
        : id(llvm::Intrinsic::not_intrinsic)
        , minArgs(0)
        , maxArgs(0)
        , isVarArg(false) {}
    
    IntrinsicInfo(llvm::Intrinsic::ID id, const std::string& name, 
                  size_t minArgs, size_t maxArgs = 0, bool isVarArg = false)
        : id(id), name(name), minArgs(minArgs), maxArgs(maxArgs), isVarArg(isVarArg) {}
    
    bool isValid() const { return id != llvm::Intrinsic::not_intrinsic; }
    bool hasFixedArgs() const { return !isVarArg && maxArgs > 0; }
};

/**
 * @brief Registry for intrinsic validation and lookup.
 * 
 * Provides:
 *   - Mapping from Lucid intrinsic names to LLVM intrinsic IDs
 *   - Argument count validation
 *   - Type parameter inference for overloaded intrinsics
 *   - Detection of compiler-handled intrinsics (no LLVM enum)
 */
class IntrinsicRegistry {
public:
    /**
     * @brief Get the singleton instance.
     * 
     * @return IntrinsicRegistry& The singleton instance
     */
    static IntrinsicRegistry& getInstance();

    /**
     * @brief Get the LLVM intrinsic ID for a Lucid intrinsic name.
     * 
     * @param name The Lucid intrinsic name (e.g., "sqrt", "memcpy")
     * @return std::optional<llvm::Intrinsic::ID> The LLVM ID, or nullopt if not found
     */
    std::optional<llvm::Intrinsic::ID> getLLVMIntrinsicID(const std::string& name) const;

    /**
     * @brief Get information about an intrinsic.
     * 
     * @param name The Lucid intrinsic name
     * @return const IntrinsicInfo* Pointer to info, or nullptr if not found
     */
    const IntrinsicInfo* getIntrinsicInfo(const std::string& name) const;

    /**
     * @brief Check if an intrinsic is compiler-handled (no LLVM enum).
     * 
     * @param name The Lucid intrinsic name
     * @return true if the intrinsic is handled by the compiler directly
     */
    bool isCompilerIntrinsic(const std::string& name) const;

    /**
     * @brief Check if an intrinsic maps to an LLVM intrinsic.
     * 
     * @param name The Lucid intrinsic name
     * @return true if the intrinsic maps to an LLVM intrinsic
     */
    bool isLLVMIntrinsic(const std::string& name) const;

    /**
     * @brief Validate the number of arguments for an intrinsic.
     * 
     * @param name The Lucid intrinsic name
     * @param argCount The number of arguments provided
     * @return true if the argument count is valid
     */
    bool validateArgCount(const std::string& name, size_t argCount) const;

    /**
     * @brief Get the expected argument count for an intrinsic.
     * 
     * @param name The Lucid intrinsic name
     * @return std::optional<size_t> The expected count, or nullopt if variable
     */
    std::optional<size_t> getExpectedArgCount(const std::string& name) const;

    /**
     * @brief Get all registered intrinsic names.
     * 
     * @return std::vector<std::string> List of all intrinsic names
     */
    std::vector<std::string> getAllIntrinsicNames() const;

    /**
     * @brief Get all LLVM intrinsic names.
     * 
     * @return std::vector<std::string> List of LLVM intrinsic names
     */
    std::vector<std::string> getLLVMIntrinsicNames() const;

    /**
     * @brief Get all compiler-handled intrinsic names.
     * 
     * @return std::vector<std::string> List of compiler-handled intrinsic names
     */
    std::vector<std::string> getCompilerIntrinsicNames() const;

private:
    IntrinsicRegistry();
    ~IntrinsicRegistry() = default;

    // Disable copy/move
    IntrinsicRegistry(const IntrinsicRegistry&) = delete;
    IntrinsicRegistry& operator=(const IntrinsicRegistry&) = delete;

    /**
     * @brief Register all intrinsics.
     */
    void registerIntrinsics();

    /**
     * @brief Register an LLVM intrinsic.
     * 
     * @param lucidName The Lucid intrinsic name
     * @param llvmID The LLVM intrinsic ID
     * @param minArgs Minimum number of arguments
     * @param maxArgs Maximum number of arguments (0 means same as min)
     * @param isVarArg Whether the intrinsic is variadic
     */
    void registerLLVMIntrinsic(const std::string& lucidName, 
                               llvm::Intrinsic::ID llvmID,
                               size_t minArgs, 
                               size_t maxArgs = 0,
                               bool isVarArg = false);

    /**
     * @brief Register a compiler-handled intrinsic.
     * 
     * @param name The intrinsic name
     * @param minArgs Minimum number of arguments
     * @param maxArgs Maximum number of arguments (0 means same as min)
     * @param isVarArg Whether the intrinsic is variadic
     */
    void registerCompilerIntrinsic(const std::string& name,
                                   size_t minArgs,
                                   size_t maxArgs = 0,
                                   bool isVarArg = false);

    // ─── Members ─────────────────────────────────────────────────────────────

    std::unordered_map<std::string, IntrinsicInfo> m_intrinsicMap;
    std::unordered_set<std::string> m_compilerIntrinsics;
    std::unordered_set<std::string> m_llvmIntrinsics;
    bool m_initialized = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience Function
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Convenience function to get the intrinsic ID.
 * 
 * @param name The Lucid intrinsic name
 * @return std::optional<llvm::Intrinsic::ID> The LLVM ID, or nullopt
 */
inline std::optional<llvm::Intrinsic::ID> getIntrinsicID(const std::string& name) {
    return IntrinsicRegistry::getInstance().getLLVMIntrinsicID(name);
}

/**
 * @brief Convenience function to check if an intrinsic is compiler-handled.
 * 
 * @param name The Lucid intrinsic name
 * @return true if compiler-handled
 */
inline bool isCompilerIntrinsic(const std::string& name) {
    return IntrinsicRegistry::getInstance().isCompilerIntrinsic(name);
}

/**
 * @brief Convenience function to check if an intrinsic is an LLVM intrinsic.
 * 
 * @param name The Lucid intrinsic name
 * @return true if LLVM intrinsic
 */
inline bool isLLVMIntrinsic(const std::string& name) {
    return IntrinsicRegistry::getInstance().isLLVMIntrinsic(name);
}

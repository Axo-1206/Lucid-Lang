// #pragma once

// #include "ast/TypeAST.hpp"
// #include "diagnostics/DiagnosticEngine.hpp"
// #include <string>
// #include <vector>
// #include <functional>
// #include <unordered_map>
// #include <memory>

// /**
//  * @file BuiltinMethodRegistry.hpp
//  * 
//  * @responsibility Central registry for all built-in methods on array types
//  *   and other language-provided types.
//  * 
//  * @design_pattern Registry pattern with pluggable checkers.
//  * 
//  * @usage:
//  *   // In checkCallExpr:
//  *   if (auto* method = BuiltinMethodRegistry::instance().lookup(objType, methodName)) {
//  *       return method->check(callNode, resolver, dc, ...);
//  *   }
//  * 
//  * @extensibility:
//  *   To add a new built-in method, call `registerMethod()` during initialization
//  *   or add a static entry to the `initialize()` method.
//  */

// // Forward declarations
// struct CallExprAST;
// struct TypeResolver;
// struct DiagnosticEngine;
// struct SymbolTable;  // Add this forward declaration

// // Result of a built-in method check
// struct BuiltinMethodResult {
//     TypeAST* returnType = nullptr;  // nullptr for void
//     bool isHandled = false;          // true if this method handled the call
//     std::string errorMessage;        // populated if isHandled=false with error
// };

// // Function signature for a built-in method checker
// // Now includes SymbolTable& parameter
// using BuiltinMethodChecker = std::function<BuiltinMethodResult(
//     CallExprAST& node,
//     TypeAST* receiverType,
//     SymbolTable& symbols,      // Added
//     TypeResolver& resolver,
//     DiagnosticEngine& dc,
//     int& loopDepth,
//     int& parallelDepth,
//     bool insideExtern
// )>;

// // Represents a registered built-in method
// struct BuiltinMethodInfo {
//     std::string name;
//     BuiltinMethodChecker checker;
//     std::string description;  // For debugging
// };

// class BuiltinMethodRegistry {
// public:
//     static BuiltinMethodRegistry& instance() {
//         static BuiltinMethodRegistry registry;
//         return registry;
//     }
    
//     void registerMethod(const std::string& typeKey, const BuiltinMethodInfo& method);
//     const BuiltinMethodInfo* lookup(const std::string& typeKey, const std::string& methodName) const;
//     bool hasMethods(const std::string& typeKey) const;
//     std::vector<BuiltinMethodInfo> getMethodsForType(const std::string& typeKey) const;
    
// private:
//     BuiltinMethodRegistry() { initialize(); }
//     ~BuiltinMethodRegistry() = default;
    
//     void initialize();
    
//     std::unordered_map<std::string, std::unordered_map<std::string, BuiltinMethodInfo>> registry_;
// };

// std::string getBuiltinTypeKey(TypeAST* type);
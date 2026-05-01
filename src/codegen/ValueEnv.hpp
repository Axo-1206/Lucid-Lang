/**
 * @file ValueEnv.hpp
 *
 * @responsibility Holds the code generation environment, including the scope stack,
 *   type and function registries, generic instantiation registry, and loop control stack.
 *
 * @architecture
 *   The ValueEnv mirrors the Semantic Phase's SymbolTable but stores LLVM objects
 *   (llvm::Value*, llvm::Type*, etc.) instead of Semantic Symbols. It is used
 *   across all three codegen passes to resolve names and manage state.
 *
 * @related
 *   CodeGen.hpp       - Driver that owns and manages ValueEnv
 *   docs/phases/CODEGEN.md - Architectural overview of the codegen phase
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <map>

#include <llvm/IR/Value.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>

// ─────────────────────────────────────────────────────────────────────────────
// InstKey — key for identifying a unique generic instantiation.
// ─────────────────────────────────────────────────────────────────────────────
struct InstKey {
    std::string              baseName;
    std::vector<llvm::Type*> typeArgs;

    bool operator==(const InstKey& other) const {
        if (baseName != other.baseName) return false;
        if (typeArgs.size() != other.typeArgs.size()) return false;
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (typeArgs[i] != other.typeArgs[i]) return false;
        }
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeSubst — mapping of generic parameter names to concrete LLVM types.
// ─────────────────────────────────────────────────────────────────────────────
using TypeSubst = std::unordered_map<std::string, llvm::Type*>;

// ─────────────────────────────────────────────────────────────────────────────
// ValueEnv — the codegen environment
// ─────────────────────────────────────────────────────────────────────────────
class ValueEnv {
public:
    // ── Scope stack ───────────────────────────────────────────────────────────

    void         pushScope();
    void         popScope();
    void         define(const std::string& name, llvm::Value* val);
    llvm::Value* lookup(const std::string& name) const;
    size_t       depth() const { return scopes_.size(); }

    // ── Struct type registry ──────────────────────────────────────────────────

    void              defineType(const std::string& name, llvm::StructType* ty);
    llvm::StructType* lookupType(const std::string& name) const;

    // ── Function registry (regular + extern + methods + from entries) ─────────

    void            defineFunc(const std::string& name, llvm::Function* fn);
    llvm::Function* lookupFunc(const std::string& name) const;

    // ── From-entry registry — keyed by "TargetType.from.SourceType" ───────────

    // Separate from the function registry for clarity; both point to the same Function*.
    void            defineFromEntry(const std::string& mangledName, llvm::Function* fn);
    llvm::Function* lookupFromEntry(const std::string& mangledName) const;

    // ── Generic instantiation registry ────────────────────────────────────────

    void                         recordInst(const InstKey& key);
    const std::vector<InstKey>&  instsFor(const std::string& baseName) const;
    bool                         hasInst(const InstKey& key) const;

    // ── Type substitution stack (for generic bodies) ──────────────────────────

    void            pushSubst(const TypeSubst& subst);
    void            popSubst();
    llvm::Type*     resolveSubst(const std::string& paramName) const;
    bool            inGenericBody() const;

    // ── Loop control ──────────────────────────────────────────────────────────

    void              push_loop(llvm::BasicBlock* exitBB, llvm::BasicBlock* continueBB);
    void              pop_loop();
    llvm::BasicBlock* current_loop_exit()     const;
    llvm::BasicBlock* current_loop_continue() const;

private:
    // Each scope is a map from name to LLVM value (usually an AllocaInst* for locals).
    std::vector<std::unordered_map<std::string, llvm::Value*>>   scopes_;

    // Global registries for types and functions.
    std::unordered_map<std::string, llvm::StructType*>           types_;
    std::unordered_map<std::string, llvm::Function*>             funcs_;
    std::unordered_map<std::string, llvm::Function*>             fromEntries_;

    // Tracks all concrete instantiations discovered in Pass 0.
    std::map<std::string, std::vector<InstKey>>                  insts_;

    // Stack of type substitutions for lowering generic function/struct bodies.
    std::vector<TypeSubst>                                       substStack_;

    // Stack of exit/continue blocks for nested loops.
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loopStack_;
};

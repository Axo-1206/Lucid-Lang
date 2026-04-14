/**
 * @file SemanticCollector.cpp
 *
 * @nutshell Implements the logic to scoop all top-level symbols directly into the scope manager.
 *
 * @reason Acts as the concrete implementation of the AST traversal to capture definitions without touching complex nested bodies or loops.
 *
 * @responsibility Implementation of the Phase 1 semantic pass (top-level symbol collection).
 *
 * @logic Traverses AST nodes for top-level declarations and populates the SymbolTable for cross-referencing.
 *
 * @related SemanticCollector.hpp, SemanticAnalyzer.cpp
 */

#include "SemanticCollector.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "diagnostics/Diagnostic.hpp"

SemanticCollector::SemanticCollector(SymbolTable& symbols, DiagnosticEngine& dc)
    : symbols_(symbols), dc_(dc) {}

// ─────────────────────────────────────────────────────────────────────────────
// collectProgram  — Entry point to index a parsed file
//
// Passes each top-level statement through the AST visitor mechanics to process
// and register its definitions.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::collectProgram(ProgramAST& program) {
    for (auto& decl : program.decls) {
        decl->accept(*this);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// declareSymbol  — Safely registers the structured semantic tracking info
//
// Tries to push the Symbol into the SymbolTable. Failure designates a pre-existing 
// identifier in this exact scope, raising `DiagCode::E3005` safely without crashing.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::declareSymbol(const Symbol& sym) {
    if (!symbols_.declare(sym)) {
        // Find existing to report properly, though dc_.error is enough here
        dc_.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                  "symbol '" + sym.name + "' is already declared in this scope");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)  — Simple top-level global variable/constant registration
//
// Inserts the top-level let or const definition name into the global map.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(VarDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Var,
        node.keyword,
        node.visibility,
        node.type.get(),
        &node,
        false,
        node.loc
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncDeclAST)  — Collects top-level functions and checks parameter clashes
//
// Registers the function identifier in the main scope. Temporarily pushes
// an inner scope to quickly process arguments, confirming no two parameters 
// share the same name locally, then instantly discards the parameter bindings.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(FuncDeclAST& node) {
    // Build signature
    TypePtr sig = nullptr;
    // Iterate groups in REVERSE to build the curry chain
    for (int i = (int)node.paramGroups.size() - 1; i >= 0; --i) {
        auto ft = std::make_unique<FuncTypeAST>();
        ft->loc = node.loc;
        for (auto& p : node.paramGroups[i]) {
            // Proxy type: just enough for the type checker. 
            // We use the same name/kind for now. 
            // In a better design, TypeAST would have a clone().
            if (p->type->kind == ASTKind::PrimitiveType) {
                ft->params.push_back(std::make_unique<PrimitiveTypeAST>(
                    static_cast<PrimitiveTypeAST*>(p->type.get())->primitiveKind));
            } else if (p->type->kind == ASTKind::NamedType) {
                ft->params.push_back(std::make_unique<NamedTypeAST>(
                    static_cast<NamedTypeAST*>(p->type.get())->name));
            } else {
                // Fallback: just use a dummy any for complex types during Phase 1
                ft->params.push_back(std::make_unique<PrimitiveTypeAST>(PrimitiveKind::Any));
            }
        }
        if (sig) {
            ft->returnType = std::move(sig);
        } else if (node.returnType) {
            if (node.returnType->kind == ASTKind::PrimitiveType) {
                ft->returnType = std::make_unique<PrimitiveTypeAST>(
                    static_cast<PrimitiveTypeAST*>(node.returnType.get())->primitiveKind);
            } else if (node.returnType->kind == ASTKind::NamedType) {
                ft->returnType = std::make_unique<NamedTypeAST>(
                    static_cast<NamedTypeAST*>(node.returnType.get())->name);
            } else {
                ft->returnType = std::make_unique<PrimitiveTypeAST>(PrimitiveKind::Any);
            }
        }
        sig = std::move(ft);
    }
    node.signature = std::move(sig);

    declareSymbol({
        node.name,
        SymbolKind::Func,
        node.keyword,
        node.visibility,
        node.signature.get(),
        &node,
        node.isAsync,
        node.loc
    });

    // Register params to check for duplicates
    symbols_.pushScope();
    for (const auto& group : node.paramGroups) {
        for (const auto& param : group) {
            declareSymbol({
                param->name,
                SymbolKind::Param,
                DeclKeyword::Let,
                Visibility::Private,
                param->type.get(),
                param.get(),
                false,
                param->loc
            });
        }
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(StructDeclAST)  — Maps structures and cross-checks internal field shapes
//
// Like functions, maps the struct globally. Pushes a mock localized scope to
// iterate through the struct's definition, asserting no duplicate field aliases
// are used before popping the ephemeral scope.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(StructDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Struct,
        DeclKeyword::Let, // N/A
        node.visibility,
        nullptr,
        &node,
        false,
        node.loc
    });

    // Register fields to check for duplicates
    symbols_.pushScope();
    for (const auto& field : node.fields) {
        declareSymbol({
            field->name,
            SymbolKind::Field,
            DeclKeyword::Let,
            Visibility::Private,
            field->type.get(),
            field.get(),
            false,
            field->loc
        });
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(EnumDeclAST)  — Registers enumerations and uniqueness of their choices
//
// Submits the enum label to the main table, pushing a localized scope to enforce 
// uniquely labelled variant flags.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(EnumDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Enum,
        DeclKeyword::Let,
        node.visibility,
        nullptr,
        &node,
        false,
        node.loc
    });

    symbols_.pushScope();
    for (const auto& variant : node.variants) {
        declareSymbol({
            variant->name,
            SymbolKind::EnumVariant,
            DeclKeyword::Let,
            Visibility::Private,
            nullptr,
            variant.get(),
            false,
            variant->loc
        });
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitDeclAST)  — Connects method contract names into semantic awareness
//
// Adds the trait name itself, validating inside an ephemeral scope that no
// internal method signatures possess exactly duplicate naming.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TraitDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Trait,
        DeclKeyword::Let,
        node.visibility,
        nullptr,
        &node,
        false,
        node.loc
    });

    symbols_.pushScope();
    for (const auto& method : node.methods) {
        declareSymbol({
            method->name,
            SymbolKind::Method,
            DeclKeyword::Let,
            Visibility::Private,
            method->returnType.get(),
            method.get(),
            false,
            method->loc
        });
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ImplDeclAST)  — Validates Implementation blocks structure bindings
//
// Struct instance actions aren't directly available without instance traversal.
// To map them, we synthesize artificial `StructName.methodName` tags on the
// global scope index. It catches multi-impl blocks conflicting via same names.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(ImplDeclAST& node) {
    // Merge methods onto the struct's namespace by mangling their names.
    // E.g., StructName.methodName in the global scope.
    for (const auto& method : node.methods) {
        // Build signature for method
        TypePtr sig = nullptr;
        
        // 1. Create the implicit 'self' group for methods (allows p:offset or Point:offset(p))
        auto selfGroup = std::make_unique<FuncTypeAST>();
        selfGroup->loc = method->loc;
        selfGroup->params.push_back(std::make_unique<NamedTypeAST>(node.structName));
        
        // 2. Build the rest of the signature from paramGroups
        for (int i = (int)method->paramGroups.size() - 1; i >= 0; --i) {
            auto ft = std::make_unique<FuncTypeAST>();
            ft->loc = method->loc;
            for (auto& p : method->paramGroups[i]) {
                if (p->type->kind == ASTKind::PrimitiveType) {
                    ft->params.push_back(std::make_unique<PrimitiveTypeAST>(static_cast<PrimitiveTypeAST*>(p->type.get())->primitiveKind));
                } else if (p->type->kind == ASTKind::NamedType) {
                    ft->params.push_back(std::make_unique<NamedTypeAST>(static_cast<NamedTypeAST*>(p->type.get())->name));
                } else {
                    ft->params.push_back(std::make_unique<PrimitiveTypeAST>(PrimitiveKind::Any));
                }
            }
            if (sig) {
                ft->returnType = std::move(sig);
            } else if (method->returnType) {
                if (method->returnType->kind == ASTKind::PrimitiveType) {
                    ft->returnType = std::make_unique<PrimitiveTypeAST>(static_cast<PrimitiveTypeAST*>(method->returnType.get())->primitiveKind);
                } else if (method->returnType->kind == ASTKind::NamedType) {
                    ft->returnType = std::make_unique<NamedTypeAST>(static_cast<NamedTypeAST*>(method->returnType.get())->name);
                } else {
                    ft->returnType = std::make_unique<PrimitiveTypeAST>(PrimitiveKind::Any);
                }
            }
            sig = std::move(ft);
        }
        
        // Connect the 'self' group to the front
        if (sig) {
            selfGroup->returnType = std::move(sig);
        } else if (method->returnType) {
            // Case for magSquared() where There are no param groups, only self.
             if (method->returnType->kind == ASTKind::PrimitiveType) {
                selfGroup->returnType = std::make_unique<PrimitiveTypeAST>(static_cast<PrimitiveTypeAST*>(method->returnType.get())->primitiveKind);
            } else if (method->returnType->kind == ASTKind::NamedType) {
                selfGroup->returnType = std::make_unique<NamedTypeAST>(static_cast<NamedTypeAST*>(method->returnType.get())->name);
            } else {
                selfGroup->returnType = std::make_unique<PrimitiveTypeAST>(PrimitiveKind::Any);
            }
        }
        method->signature = std::move(selfGroup);

        std::string mangledName = node.structName + "." + method->name;
        declareSymbol({
            mangledName,
            SymbolKind::Method,
            DeclKeyword::Let,
            node.visibility, // Inherit impl visibility conceptually
            method->signature.get(),
            method.get(),
            method->isAsync,
            method->loc
        });
    }
}
 
// ─────────────────────────────────────────────────────────────────────────────
// visit(FromDeclAST)  — Registers custom type conversions for Type(source) calls
//
// Like methods, conversions are indexed on the target type's namespace.
// Because the language supports curried conversion overloads, and Phase 1 runs
// before type resolution, we assign them a unique address-based mangled name here.
// True duplicate signature checking is deferred to Phase 3 (SemanticDecl).
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(FromDeclAST& node) {
    // Collect each conversion entry from the block.
    // Mangled name: TargetType.from.[unique_id]
    for (auto& entry : node.entries) {
        if (!entry) continue;

        // Use pointer address as a phase 1 unique identifier to avert false clashes.
        std::string mangledName = node.targetTypeName + ".from." + 
            std::to_string(reinterpret_cast<std::uintptr_t>(entry.get()));

        declareSymbol({
            mangledName,
            SymbolKind::Conversion,
            DeclKeyword::Let,
            node.visibility,
            nullptr, // resolved in Phase 2
            entry.get(),
            false,
            entry->loc
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TypeAliasDeclAST)  — Assigns proxy labels to underlying complex shapes
//
// Stores the 'type XYZ = int' alias safely on the central scope.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TypeAliasDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::TypeAlias,
        DeclKeyword::Let,
        node.visibility,
        node.aliasedType.get(),
        &node,
        false,
        node.loc
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ExternDeclAST)  — Collects external bindings for LLVM linkers
//
// Treat identically to standard `FuncDeclAST` nodes, indexing the base extern
// function and temporarily asserting parameter label uniqueness.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(ExternDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Func, // Extern funcs are treated similarly
        DeclKeyword::Let,
        Visibility::Private,
        node.returnType.get(),
        &node,
        false,
        node.loc
    });

    symbols_.pushScope();
    for (const auto& param : node.params) {
        declareSymbol({
            param->name,
            SymbolKind::Param,
            DeclKeyword::Let,
            Visibility::Private,
            param->type.get(),
            param.get(),
            false,
            param->loc
        });
    }
    symbols_.popScope();
}
#include "ast/ExprAST.hpp"
#include "ast/support/ArenaSpan.hpp"
#include "ast/support/StringPool.hpp"
#include "debug/DebugUtils.hpp"
#include "registry/AttributeRegistry.hpp"
#include "semantic/SymbolTable.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "semantic/checkers/declCheckers/DeclHelpers.hpp"
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// checkFromDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local from blocks are private.
//   - Target struct must exist in the symbol table (can be from any visible scope).
//   - Each entry's return type must match the target struct type.
//   - Parameter types in each entry must resolve.
//   - No duplicate entry signatures within the same from block.
//   - Each entry body is checked to return the target type.
//   - From entries cannot have qualifiers (~async, ~nullable, ~parallel).
//   - Generic parameters are pushed so entries can refer to T.
//   - From blocks can appear in any scope and are visible in that scope and
//     any nested scope (standard lexical scoping).
// ─────────────────────────────────────────────────────────────────────────────
void checkFromDecl(FromDeclAST& node, SemanticContext& ctx, bool isLocal) {
    // Extract target type name from targetType (must be NamedTypeAST)
    InternedString targetTypeName;
    if (node.targetType && node.targetType->isa<NamedTypeAST>()) {
        targetTypeName = node.targetType->as<NamedTypeAST>()->name;
    } else {
        ctx.error(node.loc, DiagCode::E2016, "from block target must be a named type (struct, enum, or alias)");
        return;
    }

    LUC_LOG_SEMANTIC("checkFromDecl: target=" << ctx.pool.lookup(targetTypeName)
                     << ", entries=" << node.entries.size()
                     << ", isLocal=" << isLocal);

    // ── Local from blocks cannot have visibility modifiers ───────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E2005, "local from block cannot have visibility modifier (pub/export)");
    }

    // ── Validate attributes ─────────────────────────────────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, static_cast<uint32_t>(AttributeContext::From),
                    std::string(ctx.pool.lookup(targetTypeName)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Verify target struct exists (lookup in symbol table) ─────────────────
    Symbol* targetSym = ctx.symbols->lookup(targetTypeName);
    if (!targetSym || targetSym->kind != SymbolKind::Struct) {
        ctx.error(node.loc, DiagCode::E2001,
                  "from block target '", ctx.pool.lookup(targetTypeName),
                  "' is not a declared struct in the current scope");
        return;
    }
    auto* structDecl = targetSym->decl->as<StructDeclAST>();
    TypeAST* targetType = structDecl->selfType.get();
    if (!targetType) {
        ctx.error(node.loc, DiagCode::E2001, "from block target type not resolved");
        return;
    }

    // ── Push generic parameters (from block can be generic: from Wrapper<T>) ──
    if (!node.genericParams.empty() && ctx.dispatcher) {
        ctx.dispatcher->pushGenericParams(&node.genericParams);
    }

    // ── Check each from entry ────────────────────────────────────────────────
    std::vector<FromEntryAST*> verifiedEntries;

    for (auto& entry : node.entries) {
        if (!entry) continue;

        LUC_LOG_SEMANTIC_EXTREME("\tchecking from entry");

        // Resolve return type (should be target struct type)
        TypeAST* entryReturnType = entry->returnType.get();
        if (!entryReturnType) {
            if (ctx.dispatcher) {
                entryReturnType = ctx.dispatcher->resolveType(entry->returnType.get());
            }
            if (!entryReturnType) {
                ctx.error(entry->loc, DiagCode::E2001, "from entry: cannot resolve return type");
                continue;
            }
        }

        // Verify return type matches target struct
        if (!TypeChecker::isEqual(entryReturnType, targetType, ctx)) {
            ctx.error(entry->loc, DiagCode::E2002,
                      "from entry return type must be '", ctx.pool.lookup(targetTypeName),
                      "', got '", LucDebug::kindToString(entryReturnType->kind), "'");
            continue;
        }

        // Push a new scope for the entry's parameters
        ctx.symbols->pushScope();

        // Resolve and declare parameters (flattened allParams)
        bool paramError = false;
        for (const auto& param : entry->sig.allParams) {
            if (!param) continue;

            TypeAST* paramType = param->type.get();
            if (!paramType) {
                if (ctx.dispatcher) {
                    paramType = ctx.dispatcher->resolveType(param->type.get());
                }
                if (!paramType) {
                    ctx.error(param->loc, DiagCode::E2001,
                              "cannot resolve parameter type for '", ctx.pool.lookup(param->name), "'");
                    paramError = true;
                    continue;
                }
            }

            Symbol ps;
            ps.name = param->name;
            ps.kind = SymbolKind::Param;
            ps.declKw = DeclKeyword::Let;
            ps.visibility = Visibility::Private;
            ps.type = paramType;
            ps.decl = param.get();
            ps.loc = param->loc;
            if (!ctx.symbols->declare(ps)) {
                ctx.error(param->loc, DiagCode::E2005,
                          "duplicate parameter name '", ctx.pool.lookup(param->name),
                          "' in from entry");
                paramError = true;
            }
        }

        if (paramError) {
            ctx.symbols->popScope();
            continue;
        }

        // Check entry body
        if (entry->body) {
            checkStmt(entry->body.get(), ctx, targetType);
        } else {
            ctx.error(entry->loc, DiagCode::E2003, "from entry must have a body");
        }

        // Pop parameter scope
        ctx.symbols->popScope();

        // Check for duplicate signature within this from block only
        bool isDuplicate = false;
        for (auto* seen : verifiedEntries) {
            if (entry->sig.groupCount() != seen->sig.groupCount())
                continue;

            bool match = true;
            for (size_t g = 0; g < entry->sig.groupCount(); ++g) {
                auto group1 = entry->sig.getGroup(g);
                auto group2 = seen->sig.getGroup(g);
                if (group1.size() != group2.size()) {
                    match = false;
                    break;
                }
                for (size_t p = 0; p < group1.size(); ++p) {
                    if (!TypeChecker::isEqual(group1[p]->type.get(), group2[p]->type.get(), ctx)) {
                        match = false;
                        break;
                    }
                }
                if (!match) break;
            }
            if (match) {
                isDuplicate = true;
                break;
            }
        }

        if (isDuplicate) {
            ctx.error(entry->loc, DiagCode::E2005,
                      "duplicate from entry signature (same parameter types) within the same from block");
        } else {
            verifiedEntries.push_back(entry.get());
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty() && ctx.dispatcher) {
        ctx.dispatcher->popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkFromDecl: complete for "
                             << ctx.pool.lookup(targetTypeName)
                             << " with " << verifiedEntries.size() << " valid entries");
}
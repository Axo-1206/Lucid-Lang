/**
 * @file GenericParamHandler.cpp
 * @brief Implementation of generic parameter stack management.
 */

#include "GenericParamHandler.hpp"
#include "debug/DebugMacros.hpp"

void GenericParamHandler::pushParams(const ArenaSpan<GenericParamPtr>* params) {
    paramsStack_.push_back(params);
    LUC_LOG_SEMANTIC_EXTREME("GenericParamHandler::pushParams: depth=" << paramsStack_.size());
}

void GenericParamHandler::popParams() {
    if (!paramsStack_.empty()) {
        paramsStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("GenericParamHandler::popParams: depth=" << paramsStack_.size());
    } else {
        LUC_LOG_SEMANTIC("GenericParamHandler::popParams: ERROR - stack empty");
    }
}

bool GenericParamHandler::isParam(InternedString name) const {
    // Search from innermost to outermost
    for (auto it = paramsStack_.rbegin(); it != paramsStack_.rend(); ++it) {
        const auto* params = *it;
        if (!params) continue;
        
        for (const auto& gp : *params) {
            if (gp && gp->name == name) {
                LUC_LOG_SEMANTIC_EXTREME("GenericParamHandler::isParam: found " 
                                         << static_cast<uint32_t>(name.id));
                return true;
            }
        }
    }
    return false;
}

const ArenaSpan<GenericParamPtr>* GenericParamHandler::currentParams() const {
    if (paramsStack_.empty()) {
        return nullptr;
    }
    return paramsStack_.back();
}

void GenericParamHandler::pushSubstMap(const std::unordered_map<InternedString, TypeAST*>* map) {
    substStack_.push_back(map);
    LUC_LOG_SEMANTIC_EXTREME("GenericParamHandler::pushSubstMap: depth=" << substStack_.size());
}

void GenericParamHandler::popSubstMap() {
    if (!substStack_.empty()) {
        substStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("GenericParamHandler::popSubstMap: depth=" << substStack_.size());
    } else {
        LUC_LOG_SEMANTIC("GenericParamHandler::popSubstMap: ERROR - stack empty");
    }
}

TypeAST* GenericParamHandler::lookupSubst(InternedString name) const {
    // Search from innermost to outermost
    for (auto it = substStack_.rbegin(); it != substStack_.rend(); ++it) {
        const auto* map = *it;
        if (!map) continue;
        
        auto found = map->find(name);
        if (found != map->end()) {
            LUC_LOG_SEMANTIC_EXTREME("GenericParamHandler::lookupSubst: found substitution for "
                                     << static_cast<uint32_t>(name.id));
            return found->second;
        }
    }
    return nullptr;
}

void GenericParamHandler::clear() {
    paramsStack_.clear();
    substStack_.clear();
    LUC_LOG_SEMANTIC("GenericParamHandler::clear: all stacks cleared");
}
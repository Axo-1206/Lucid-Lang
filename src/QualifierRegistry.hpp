#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>

struct QualifierInfo {
    std::string name;
    uint32_t bit;
    bool affectsTypeEquality;  // true = part of type comparison
    bool validOnFunction;      // can appear on function types
    bool validOnVariable;      // can appear on variable types (future)
    bool validOnStruct;        // can appear on struct types (future)
};

class QualifierRegistry {
public:
    static const QualifierRegistry& instance() {
        static QualifierRegistry inst;
        return inst;
    }
    
    // Get bitmask for a qualifier name, returns 0 if unknown
    uint32_t getBit(const std::string& name) const {
        auto it = qualifiers.find(name);
        return it != qualifiers.end() ? it->second.bit : 0;
    }
    
    // Check if qualifier name is valid
    bool isValid(const std::string& name) const {
        return qualifiers.find(name) != qualifiers.end();
    }
    
    // Get full info for a qualifier
    const QualifierInfo* getInfo(const std::string& name) const {
        auto it = qualifiers.find(name);
        return it != qualifiers.end() ? &it->second : nullptr;
    }
    
    // Get all valid qualifier names (for error messages)
    std::string allNames() const {
        std::string result;
        for (const auto& [name, info] : qualifiers) {
            if (!result.empty()) result += ", ";
            result += "~" + name;
        }
        return result;
    }

    uint32_t equalityMask() const {
        uint32_t mask = 0;
        for (const auto& [name, info] : qualifiers) {
            if (info.affectsTypeEquality) {
                mask |= info.bit;
            }
        }
        return mask;
    }

    const std::unordered_map<std::string, QualifierInfo>& getAll() const {
        return qualifiers;
    }
    
    // Bitmask for async (convenience for codegen)
    uint32_t asyncBit() const { return getBit("async"); }
    uint32_t noinlineBit() const { return getBit("noinline"); }
    uint32_t cdeclBit() const { return getBit("cdecl"); }
    uint32_t stdcallBit() const { return getBit("stdcall"); }
    uint32_t fastcallBit() const { return getBit("fastcall"); }
    uint32_t heapBit() const { return getBit("heap"); }
    uint32_t coldBit() const { return getBit("cold"); }
    
private:
    QualifierRegistry() {
        // Single source of truth - add new qualifiers here only
        add("async",    1 << 0,  true,  true,  false, false);
        add("noinline", 1 << 1,  false, true,  false, false);
        add("cdecl",    1 << 2,  true,  true,  false, false);
        add("stdcall",  1 << 3,  true,  true,  false, false);
        add("fastcall", 1 << 4,  true,  true,  false, false);
        add("heap",     1 << 5,  false, true,  false, false);
        add("cold",     1 << 6,  false, true,  false, false);
    }
    
    void add(const std::string& name, uint32_t bit, bool affectsType,
             bool onFunc, bool onVar, bool onStruct) {
        qualifiers[name] = {name, bit, affectsType, onFunc, onVar, onStruct};
        nextBit = bit << 1;  // Not strictly needed, but useful for debugging
    }
    
    std::unordered_map<std::string, QualifierInfo> qualifiers;
    uint32_t nextBit = 1 << 7;  // Next available bit (for future expansion)
};
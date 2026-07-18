/**
 * @file TypeMapping.cpp
 * @brief Implementation of the type mapper.
 */

#include "TypeMapping.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// TypeMapping - Construction
// ─────────────────────────────────────────────────────────────────────────────

TypeMapping::TypeMapping(llvm::LLVMContext& context, StringPool& stringPool)
    : m_context(context)
    , m_stringPool(stringPool)
    , m_dataLayout("")  // Will be set later if needed
    , m_tagType(llvm::Type::getInt8Ty(context)) {
    // Register primitive types with themselves (no-op, they're built-in)
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeMapping - Public API
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* TypeMapping::toLLVMType(TypeAST* type) {
    if (!type) {
        return nullptr;
    }

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return convertPrimitive(type->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return convertNamed(type->as<NamedTypeAST>());
        case ASTKind::ArrayType:
            return convertArray(type->as<ArrayTypeAST>());
        case ASTKind::NullableType:
            return convertNullable(type->as<NullableTypeAST>());
        case ASTKind::FallibleType:
            return convertFallible(type->as<FallibleTypeAST>());
        case ASTKind::CombinedType:
            return convertCombined(type->as<CombinedTypeAST>());
        case ASTKind::RefType:
            return convertRef(type->as<RefTypeAST>());
        case ASTKind::PtrType:
            return convertPtr(type->as<PtrTypeAST>());
        case ASTKind::FuncType:
            return convertFunc(type->as<FuncTypeAST>());
        default:
            throw TypeMappingError(TypeMappingError::Kind::UnknownType,
                                   "Unknown type kind: " + std::to_string(static_cast<int>(type->kind)));
    }
}

llvm::FunctionType* TypeMapping::toLLVMFunctionType(FuncTypeAST* funcType) {
    if (!funcType) {
        return nullptr;
    }

    // Get return type
    llvm::Type* returnType = llvm::Type::getVoidTy(m_context);
    
    if (!funcType->returnTypes.empty()) {
        if (funcType->isCurried()) {
            // Curried function: return type is another function type
            auto* next = funcType->getNext();
            if (next) {
                returnType = toLLVMFunctionType(next);
            }
        } else if (funcType->returnTypes.size() == 1) {
            // Single return value
            returnType = toLLVMType(funcType->returnTypes[0]);
        } else {
            // Multiple return values - pack into a struct
            std::vector<llvm::Type*> types;
            for (TypePtr t : funcType->returnTypes) {
                auto* llvmType = toLLVMType(t);
                if (llvmType) {
                    types.push_back(llvmType);
                }
            }
            if (!types.empty()) {
                returnType = llvm::StructType::get(m_context, types);
            }
        }
    }

    // Get parameter types
    std::vector<llvm::Type*> paramTypes;
    for (ParamPtr param : funcType->params) {
        auto* paramType = toLLVMType(param->type);
        if (paramType) {
            paramTypes.push_back(paramType);
        }
    }

    return llvm::FunctionType::get(returnType, paramTypes, false);
}

void TypeMapping::registerType(InternedString name, llvm::Type* llvmType) {
    if (!llvmType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Cannot register null type");
    }
    
    std::string nameStr = internedToString(name);
    if (nameStr.empty()) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Cannot register type with empty name");
    }
    
    m_typeRegistry[nameStr] = llvmType;
}

void TypeMapping::registerType(const std::string& name, llvm::Type* llvmType) {
    if (!llvmType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Cannot register null type: " + name);
    }
    if (name.empty()) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Cannot register type with empty name");
    }
    m_typeRegistry[name] = llvmType;
}

llvm::Type* TypeMapping::getRegisteredType(InternedString name) const {
    std::string nameStr = internedToString(name);
    if (nameStr.empty()) {
        return nullptr;
    }
    return getRegisteredType(nameStr);
}

llvm::Type* TypeMapping::getRegisteredType(const std::string& name) const {
    auto it = m_typeRegistry.find(name);
    if (it != m_typeRegistry.end()) {
        return it->second;
    }
    return nullptr;
}

bool TypeMapping::isTypeRegistered(InternedString name) const {
    std::string nameStr = internedToString(name);
    if (nameStr.empty()) {
        return false;
    }
    return isTypeRegistered(nameStr);
}

bool TypeMapping::isTypeRegistered(const std::string& name) const {
    return m_typeRegistry.find(name) != m_typeRegistry.end();
}

void TypeMapping::setDataLayout(const std::string& layout) {
    m_dataLayout = llvm::DataLayout(layout);
}

uint64_t TypeMapping::getTypeSize(llvm::Type* type) const {
    if (!type) {
        return 0;
    }
    return m_dataLayout.getTypeAllocSize(type);
}

// FIXED: getABITypeAlignment() → getABITypeAlign() + .value()
uint64_t TypeMapping::getTypeAlignment(llvm::Type* type) const {
    if (!type) {
        return 0;
    }
    return m_dataLayout.getABITypeAlign(type).value();
}

llvm::StructType* TypeMapping::buildTaggedType(llvm::Type* valueType, unsigned tagBits) {
    if (!valueType) {
        return nullptr;
    }

    // Check cache
    auto it = m_taggedTypeCache.find(valueType);
    if (it != m_taggedTypeCache.end()) {
        return it->second;
    }

    auto* structType = createTaggedStruct(valueType);
    m_taggedTypeCache[valueType] = structType;
    return structType;
}

llvm::StructType* TypeMapping::buildCombinedTaggedType(llvm::Type* valueType) {
    if (!valueType) {
        return nullptr;
    }

    // Check cache
    auto it = m_combinedTaggedTypeCache.find(valueType);
    if (it != m_combinedTaggedTypeCache.end()) {
        return it->second;
    }

    auto* structType = createCombinedTaggedStruct(valueType);
    m_combinedTaggedTypeCache[valueType] = structType;
    return structType;
}

llvm::IntegerType* TypeMapping::getTagType() const {
    return m_tagType;
}

llvm::ConstantInt* TypeMapping::getTagValue(bool isNil) const {
    uint8_t value = isNil ? TAG_NIL : TAG_ERR;
    return llvm::ConstantInt::get(m_tagType, value);
}

llvm::ConstantInt* TypeMapping::getTagConstant(int tag) const {
    return llvm::ConstantInt::get(m_tagType, tag);
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeMapping - Type Conversion
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* TypeMapping::convertPrimitive(PrimitiveTypeAST* primType) {
    if (!primType) {
        return nullptr;
    }
    return getPrimitiveType(primType->primitiveKind);
}

llvm::Type* TypeMapping::convertNamed(NamedTypeAST* namedType) {
    if (!namedType) {
        return nullptr;
    }

    std::string name = internedToString(namedType->name);

    // Check if it's a primitive type
    // Some primitives are represented as named types in the AST
    auto* primitive = getPrimitiveTypeFromName(name);
    if (primitive) {
        return primitive;
    }

    // Check registered types
    auto* registered = getRegisteredType(name);
    if (registered) {
        return registered;
    }

    // Check if it's a generic instantiation
    // For generic types, we need to handle the generic arguments
    // For now, just return a pointer to the type (forward declaration)
    // The actual type will be resolved later
    if (!namedType->genericArgs.empty()) {
        // Generic type - create a placeholder
        // FIXME: Proper generic type handling
        std::string genericName = name + "<";
        for (size_t i = 0; i < namedType->genericArgs.size(); ++i) {
            if (i > 0) genericName += ",";
            auto* argType = toLLVMType(namedType->genericArgs[i]);
            if (argType) {
                genericName += argType->getStructName().str();
            }
        }
        genericName += ">";
        
        // Check if already registered with this name
        auto* existing = getRegisteredType(genericName);
        if (existing) {
            return existing;
        }
        
        // Create a placeholder
        auto* placeholder = llvm::StructType::create(m_context, genericName);
        registerType(genericName, placeholder);
        return placeholder;
    }

    throw TypeMappingError(TypeMappingError::Kind::TypeNotFound,
                           "Type not found: " + name);
}

llvm::Type* TypeMapping::convertArray(ArrayTypeAST* arrayType) {
    if (!arrayType) {
        return nullptr;
    }

    auto* elementType = toLLVMType(arrayType->element);
    if (!elementType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Failed to convert array element type");
    }

    switch (arrayType->arrayKind) {
        case ArrayKind::Fixed: {
            // [N]T - fixed-size array
            if (arrayType->size == 0) {
                // Zero-sized array - return empty type
                return llvm::ArrayType::get(elementType, 0);
            }
            return llvm::ArrayType::get(elementType, arrayType->size);
        }
        case ArrayKind::Slice: {
            // [_]T - slice (pointer + size + capacity)
            // Represent as { T*, int64, int64 }
            auto* ptrType = llvm::PointerType::get(elementType, 0);
            auto* sizeType = llvm::Type::getInt64Ty(m_context);
            return llvm::StructType::get(m_context, {ptrType, sizeType, sizeType});
        }
        case ArrayKind::Dynamic: {
            // [*]T - dynamic array (pointer + size + capacity)
            // Represent as { T*, int64, int64 }
            auto* ptrType = llvm::PointerType::get(elementType, 0);
            auto* sizeType = llvm::Type::getInt64Ty(m_context);
            return llvm::StructType::get(m_context, {ptrType, sizeType, sizeType});
        }
        default:
            throw TypeMappingError(TypeMappingError::Kind::UnsupportedType,
                                   "Unsupported array kind");
    }
}

llvm::Type* TypeMapping::convertNullable(NullableTypeAST* nullableType) {
    if (!nullableType) {
        return nullptr;
    }

    auto* innerType = toLLVMType(nullableType->inner);
    if (!innerType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Failed to convert nullable inner type");
    }

    // T? -> { i8, T }
    return buildTaggedType(innerType);
}

llvm::Type* TypeMapping::convertFallible(FallibleTypeAST* fallibleType) {
    if (!fallibleType) {
        return nullptr;
    }

    auto* innerType = toLLVMType(fallibleType->inner);
    if (!innerType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Failed to convert fallible inner type");
    }

    // T! -> { i8, T }
    return buildTaggedType(innerType);
}

llvm::Type* TypeMapping::convertCombined(CombinedTypeAST* combinedType) {
    if (!combinedType) {
        return nullptr;
    }

    auto* innerType = toLLVMType(combinedType->inner);
    if (!innerType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Failed to convert combined inner type");
    }

    // T?! -> { i8, T } with 3-state tag (0=nil, 1=value, 2=err)
    return buildCombinedTaggedType(innerType);
}

llvm::Type* TypeMapping::convertRef(RefTypeAST* refType) {
    if (!refType) {
        return nullptr;
    }

    auto* innerType = toLLVMType(refType->inner);
    if (!innerType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Failed to convert reference inner type");
    }

    // &T -> T* (pointer to T)
    return llvm::PointerType::get(innerType, 0);
}

llvm::Type* TypeMapping::convertPtr(PtrTypeAST* ptrType) {
    if (!ptrType) {
        return nullptr;
    }

    auto* innerType = toLLVMType(ptrType->inner);
    if (!innerType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Failed to convert pointer inner type");
    }

    // *T -> i8* (opaque pointer for raw pointers)
    // We use i8* to prevent accidental dereferencing
    return llvm::PointerType::getUnqual(m_context);
}

llvm::Type* TypeMapping::convertFunc(FuncTypeAST* funcType) {
    if (!funcType) {
        return nullptr;
    }

    // Function types are converted to function pointers
    auto* fnType = toLLVMFunctionType(funcType);
    if (!fnType) {
        throw TypeMappingError(TypeMappingError::Kind::ConversionFailed,
                               "Failed to convert function type");
    }

    return llvm::PointerType::get(fnType, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeMapping - Primitive Type Helpers
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* TypeMapping::getPrimitiveType(PrimitiveKind kind) {
    switch (kind) {
        // Boolean
        case PrimitiveKind::Bool:
            return llvm::Type::getInt1Ty(m_context);

        // Signed integers (fixed-width)
        case PrimitiveKind::Int8:
        case PrimitiveKind::Byte:
            return llvm::Type::getInt8Ty(m_context);
        case PrimitiveKind::Int16:
        case PrimitiveKind::Short:
            return llvm::Type::getInt16Ty(m_context);
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int:
            return llvm::Type::getInt32Ty(m_context);
        case PrimitiveKind::Int64:
        case PrimitiveKind::Long:
            return llvm::Type::getInt64Ty(m_context);

        // Unsigned integers (fixed-width)
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Ubyte:
            return llvm::Type::getInt8Ty(m_context);
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Ushort:
            return llvm::Type::getInt16Ty(m_context);
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint:
            return llvm::Type::getInt32Ty(m_context);
        case PrimitiveKind::Uint64:
        case PrimitiveKind::Ulong:
            return llvm::Type::getInt64Ty(m_context);

        // Floating point
        case PrimitiveKind::Float:
            return llvm::Type::getFloatTy(m_context);
        case PrimitiveKind::Double:
            return llvm::Type::getDoubleTy(m_context);
        case PrimitiveKind::Decimal:
            // 128-bit decimal - use x86_fp80 or ppc_fp128
            // For now, use double
            return llvm::Type::getDoubleTy(m_context);

        // Text
        case PrimitiveKind::String:
            return llvm::PointerType::getUnqual(m_context);
        case PrimitiveKind::Char:
            return llvm::Type::getInt8Ty(m_context);

        default:
            throw TypeMappingError(TypeMappingError::Kind::UnsupportedType,
                                   "Unsupported primitive kind");
    }
}

llvm::Type* TypeMapping::getPrimitiveTypeFromName(const std::string& name) {
    // Map common type names to primitive kinds
    static const std::unordered_map<std::string, PrimitiveKind> nameMap = {
        {"bool", PrimitiveKind::Bool},
        {"int8", PrimitiveKind::Int8},
        {"int16", PrimitiveKind::Int16},
        {"int32", PrimitiveKind::Int32},
        {"int64", PrimitiveKind::Int64},
        {"byte", PrimitiveKind::Byte},
        {"short", PrimitiveKind::Short},
        {"int", PrimitiveKind::Int},
        {"long", PrimitiveKind::Long},
        {"uint8", PrimitiveKind::Uint8},
        {"uint16", PrimitiveKind::Uint16},
        {"uint32", PrimitiveKind::Uint32},
        {"uint64", PrimitiveKind::Uint64},
        {"ubyte", PrimitiveKind::Ubyte},
        {"ushort", PrimitiveKind::Ushort},
        {"uint", PrimitiveKind::Uint},
        {"ulong", PrimitiveKind::Ulong},
        {"float", PrimitiveKind::Float},
        {"double", PrimitiveKind::Double},
        {"decimal", PrimitiveKind::Decimal},
        {"string", PrimitiveKind::String},
        {"char", PrimitiveKind::Char},
    };

    auto it = nameMap.find(name);
    if (it != nameMap.end()) {
        return getPrimitiveType(it->second);
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeMapping - Tagged Type Helpers
// ─────────────────────────────────────────────────────────────────────────────

llvm::StructType* TypeMapping::createTaggedStruct(llvm::Type* valueType,
                                                  const std::string& name) {
    // { i8, T } - tag byte followed by value
    std::vector<llvm::Type*> fields = {
        m_tagType,  // tag (0=nil, 1=value)
        valueType   // value (only valid when tag == 1)
    };

    if (name.empty()) {
        return llvm::StructType::get(m_context, fields);
    }

    return llvm::StructType::create(m_context, fields, name);
}

llvm::StructType* TypeMapping::createCombinedTaggedStruct(llvm::Type* valueType,
                                                          const std::string& name) {
    // { i8, T } - tag byte followed by value
    // Tag values: 0=nil, 1=value, 2=err
    std::vector<llvm::Type*> fields = {
        m_tagType,  // tag (0=nil, 1=value, 2=err)
        valueType   // value (only valid when tag == 1)
    };

    if (name.empty()) {
        return llvm::StructType::get(m_context, fields);
    }

    return llvm::StructType::create(m_context, fields, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeMapping - Helper Methods
// ─────────────────────────────────────────────────────────────────────────────

std::string TypeMapping::internedToString(InternedString name) const {
    std::string_view view = m_stringPool.lookup(name);
    return std::string(view);
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeMapping - Debug Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

std::string typeToString(llvm::Type* type) {
    if (!type) {
        return "null";
    }
    std::string str;
    llvm::raw_string_ostream os(str);
    type->print(os);
    return str;
}

} // namespace detail

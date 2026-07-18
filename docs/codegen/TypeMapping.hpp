/**
 * @file TypeMapping.hpp
 * @brief Maps Lucid types to LLVM types.
 * 
 * @responsibility Converts Lucid type AST nodes to corresponding LLVM types.
 *                 Handles primitive types, structs, enums, arrays, pointers,
 *                 function types, and nullable/fallible types.
 * 
 * @related_files
 *   - src/compiler/IRLowering.hpp - uses TypeMapping for IR generation
 *   - src/ast/TypeAST.hpp - Lucid type AST nodes
 *   - src/interpreter/Interpreter.hpp - uses TypeMapping for JIT execution
 *   - src/compiler/aot/AOT.hpp - uses TypeMapping for AOT compilation
 */

#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DataLayout.h"

#include "../core/ast/TypeAST.hpp"
#include "../core/ast/DeclAST.hpp"
#include "../core/memory/InternedString.hpp"
#include "../core/memory/StringPool.hpp"

#include <llvm/IR/Constants.h>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

/**
 * @brief Exception thrown when type conversion fails.
 */
class TypeMappingError : public std::runtime_error {
public:
    enum class Kind {
        UnknownType,         // Unknown type kind
        UnsupportedType,     // Type not yet supported
        TypeNotFound,        // Type not found in registry
        ConversionFailed,    // Type conversion failed
        InvalidTaggedType,   // Invalid tagged type
    };

    TypeMappingError(Kind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    Kind getKind() const { return m_kind; }

private:
    Kind m_kind;
};

/**
 * @brief Maps Lucid types to LLVM types.
 * 
 * Handles conversion of all Lucid type AST nodes to LLVM types.
 * Maintains a registry of user-defined types (structs, enums, traits).
 * 
 * @par Usage Example
 * @code
 *   llvm::LLVMContext context;
 *   StringPool stringPool;
 *   TypeMapping mapper(context, stringPool);
 *   
 *   // Convert a primitive type
 *   auto* intType = mapper.toLLVMType(primitiveTypeAST);
 *   
 *   // Register a user-defined type
 *   mapper.registerType("Point", structType);
 *   
 *   // Get a registered type
 *   auto* pointType = mapper.getRegisteredType("Point");
 * @endcode
 * 
 * @par Tagged Type Layout
 * Nullable and fallible types are represented as tagged structs:
 *   - T? : { i8, T }  (tag byte + value)
 *   - T! : { i8, T }  (tag byte + value)
 *   - T?!: { i8, T }  (tag byte + value, tag encodes 0/1/2)
 * 
 * Tag values:
 *   - 0: nil or err (absent/failed)
 *   - 1: value present
 *   - 2: err (for T! and T?!)
 */
class TypeMapping {
public:
    /**
     * @brief Construct a TypeMapping instance.
     * 
     * @param context The LLVM context to use for type creation
     * @param stringPool The string pool for resolving type names
     */
    explicit TypeMapping(llvm::LLVMContext& context, StringPool& stringPool);

    /**
     * @brief Convert a Lucid type to an LLVM type.
     * 
     * @param type The Lucid type AST node
     * @return llvm::Type* The LLVM type, or nullptr on error
     * @throws TypeMappingError if conversion fails
     */
    llvm::Type* toLLVMType(TypeAST* type);

    /**
     * @brief Convert a Lucid function type to an LLVM function type.
     * 
     * @param funcType The Lucid function type
     * @return llvm::FunctionType* The LLVM function type
     * @throws TypeMappingError if conversion fails
     */
    llvm::FunctionType* toLLVMFunctionType(FuncTypeAST* funcType);

    /**
     * @brief Register a user-defined type.
     * 
     * @param name The type name (as InternedString)
     * @param llvmType The corresponding LLVM type
     */
    void registerType(InternedString name, llvm::Type* llvmType);

    /**
     * @brief Register a user-defined type by string name.
     * 
     * @param name The type name as string
     * @param llvmType The corresponding LLVM type
     */
    void registerType(const std::string& name, llvm::Type* llvmType);

    /**
     * @brief Get a registered user-defined type.
     * 
     * @param name The type name (as InternedString)
     * @return llvm::Type* The LLVM type, or nullptr if not found
     */
    llvm::Type* getRegisteredType(InternedString name) const;

    /**
     * @brief Get a registered user-defined type by string name.
     * 
     * @param name The type name as string
     * @return llvm::Type* The LLVM type, or nullptr if not found
     */
    llvm::Type* getRegisteredType(const std::string& name) const;

    /**
     * @brief Check if a type is registered.
     * 
     * @param name The type name (as InternedString)
     * @return true if the type is registered
     */
    bool isTypeRegistered(InternedString name) const;

    /**
     * @brief Check if a type is registered by string name.
     * 
     * @param name The type name as string
     * @return true if the type is registered
     */
    bool isTypeRegistered(const std::string& name) const;

    /**
     * @brief Get the LLVM context.
     * 
     * @return Reference to the LLVM context
     */
    llvm::LLVMContext& getContext() { return m_context; }

    /**
     * @brief Get the data layout.
     * 
     * @return Reference to the data layout
     */
    llvm::DataLayout& getDataLayout() { return m_dataLayout; }

    /**
     * @brief Set the data layout.
     * 
     * @param layout The data layout string
     */
    void setDataLayout(const std::string& layout);

    /**
     * @brief Get the size of a type in bytes.
     * 
     * @param type The LLVM type
     * @return uint64_t The size in bytes
     */
    uint64_t getTypeSize(llvm::Type* type) const;

    /**
     * @brief Get the alignment of a type in bytes.
     * 
     * @param type The LLVM type
     * @return uint64_t The alignment in bytes
     */
    uint64_t getTypeAlignment(llvm::Type* type) const;

    /**
     * @brief Build a tagged type for nullable/fallible values.
     * 
     * @param valueType The value type
     * @param tagBits The number of bits for the tag (default: 8)
     * @return llvm::StructType* The tagged struct type
     */
    llvm::StructType* buildTaggedType(llvm::Type* valueType, unsigned tagBits = 8);

    /**
     * @brief Build a combined tagged type for T?! values.
     * 
     * @param valueType The value type
     * @return llvm::StructType* The combined tagged struct type
     */
    llvm::StructType* buildCombinedTaggedType(llvm::Type* valueType);

    /**
     * @brief Get the tag type for tagged values.
     * 
     * @return llvm::IntegerType* The tag type
     */
    llvm::IntegerType* getTagType() const;

    /**
     * @brief Get the tag value for nil/err sentinels.
     * 
     * @param isNil True for nil, false for err
     * @return llvm::ConstantInt* The tag constant
     */
    llvm::ConstantInt* getTagValue(bool isNil) const;

    /**
     * @brief Get the tag constant for a specific value.
     * 
     * @param tag The tag value (0, 1, 2)
     * @return llvm::ConstantInt* The tag constant
     */
    llvm::ConstantInt* getTagConstant(int tag) const;

    // ─── Tag Constants ──────────────────────────────────────────────────────

    static constexpr unsigned TAG_BITS = 8;
    static constexpr uint8_t TAG_NIL = 0;     // nil sentinel
    static constexpr uint8_t TAG_VALUE = 1;   // value present
    static constexpr uint8_t TAG_ERR = 2;     // err sentinel

    /**
     * @brief Get the string pool.
     * 
     * @return Reference to the string pool
     */
    StringPool& getStringPool() { return m_stringPool; }

private:
    /**
     * @brief Convert a primitive type to an LLVM type.
     * 
     * @param primType The primitive type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertPrimitive(PrimitiveTypeAST* primType);

    /**
     * @brief Convert a named type to an LLVM type.
     * 
     * @param namedType The named type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertNamed(NamedTypeAST* namedType);

    /**
     * @brief Convert an array type to an LLVM type.
     * 
     * @param arrayType The array type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertArray(ArrayTypeAST* arrayType);

    /**
     * @brief Convert a nullable type to an LLVM type.
     * 
     * @param nullableType The nullable type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertNullable(NullableTypeAST* nullableType);

    /**
     * @brief Convert a fallible type to an LLVM type.
     * 
     * @param fallibleType The fallible type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertFallible(FallibleTypeAST* fallibleType);

    /**
     * @brief Convert a combined nullable+fallible type to an LLVM type.
     * 
     * @param combinedType The combined type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertCombined(CombinedTypeAST* combinedType);

    /**
     * @brief Convert a reference type to an LLVM type.
     * 
     * @param refType The reference type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertRef(RefTypeAST* refType);

    /**
     * @brief Convert a raw pointer type to an LLVM type.
     * 
     * @param ptrType The pointer type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertPtr(PtrTypeAST* ptrType);

    /**
     * @brief Convert a function type to an LLVM type.
     * 
     * @param funcType The function type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* convertFunc(FuncTypeAST* funcType);

    /**
     * @brief Get the LLVM type for a primitive kind.
     * 
     * @param kind The primitive kind
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* getPrimitiveType(PrimitiveKind kind);

    /**
     * @brief Get a primitive type from a string name.
     * 
     * @param name The type name
     * @return llvm::Type* The LLVM type, or nullptr if not a primitive
     */
    llvm::Type* getPrimitiveTypeFromName(const std::string& name);

    /**
     * @brief Create a struct type for a tagged value.
     * 
     * @param valueType The value type
     * @param name The struct name (optional)
     * @return llvm::StructType* The struct type
     */
    llvm::StructType* createTaggedStruct(llvm::Type* valueType, 
                                         const std::string& name = "");

    /**
     * @brief Create a struct type for a combined tagged value (T?!).
     * 
     * @param valueType The value type
     * @param name The struct name (optional)
     * @return llvm::StructType* The struct type
     */
    llvm::StructType* createCombinedTaggedStruct(llvm::Type* valueType,
                                                 const std::string& name = "");

    /**
     * @brief Convert InternedString to std::string for lookup.
     * 
     * @param name The interned string
     * @return std::string The string representation
     */
    std::string internedToString(InternedString name) const;

    // ─── Members ─────────────────────────────────────────────────────────────

    llvm::LLVMContext& m_context;
    StringPool& m_stringPool;
    llvm::DataLayout m_dataLayout;
    std::unordered_map<std::string, llvm::Type*> m_typeRegistry;
    llvm::IntegerType* m_tagType;

    // Cache for tagged types (value type -> tagged struct)
    std::unordered_map<llvm::Type*, llvm::StructType*> m_taggedTypeCache;
    std::unordered_map<llvm::Type*, llvm::StructType*> m_combinedTaggedTypeCache;
};
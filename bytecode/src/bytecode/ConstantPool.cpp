/// @file bytecode/ConstantPool.cpp
/// @brief Interned constants for one Bytecode.

#include "ConstantPool.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

#include <cstring>
#include <sstream>

namespace lucid::bytecode {

// ─────────────────────────────────────────────────────────────────────────────
// Canonical key
// ─────────────────────────────────────────────────────────────────────────────
//
// Two Constants are equal when their (kind, type, value) triples are
// equal. The dedup map is keyed by a canonical byte-string encoding of
// that triple. The encoding is internal to this file; only that equal
// Constants produce equal keys and unequal Constants produce unequal
// keys.
//
// The encoding is:
//
//   kind           : 1 byte
//   type           : a recursive encoding (see encodeType)
//   value          : a recursive encoding (see encodeValue)
//
// For Int, Float, Bool, Char, Nil: fixed-width bytes.
// For String: a u32 length prefix followed by the bytes.
// For Array: a u32 element count followed by each element's encoding.
// For Function: a u32 function index.
//
// The encoding does not need to be reversible; it only needs to be
// injective on the Constant space. Every field that distinguishes two
// Constants is written. Nothing else is.

namespace {

void encodeU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void encodeU64(std::string& out, uint64_t v) {
    encodeU32(out, static_cast<uint32_t>(v >> 32));
    encodeU32(out, static_cast<uint32_t>(v & 0xFFFFFFFF));
}

void encodeDouble(std::string& out, double d) {
    // A double is 8 bytes; the bit pattern is the key. -0.0 and 0.0
    // are distinct keys here, which is the right behavior: Sema would
    // not have folded them into the same constant if they meant the
    // same thing, and preserving the distinction is safer than
    // canonicalizing.
    uint64_t bits = 0;
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "double is not 64 bits on this platform");
    std::memcpy(&bits, &d, sizeof(bits));
    encodeU64(out, bits);
}

void encodeType(std::string& out, const TypeDescriptor& t) {
    out.push_back(static_cast<char>(t.kind));
    switch (t.kind) {
        case TypeDescriptor::Kind::Primitive:
            out.push_back(static_cast<char>(t.primitive));
            break;
        case TypeDescriptor::Kind::Named:
            encodeU32(out, static_cast<uint32_t>(t.namedMangled.size()));
            out.append(t.namedMangled);
            break;
        case TypeDescriptor::Kind::Array:
            out.push_back(static_cast<char>(t.arrayKind));
            encodeU64(out, t.fixedSize);
            if (t.component) encodeType(out, *t.component);
            break;
        case TypeDescriptor::Kind::RowRef:
            if (t.component) encodeType(out, *t.component);
            break;
        case TypeDescriptor::Kind::Function:
            encodeU32(out, static_cast<uint32_t>(t.params.size()));
            for (const auto& p : t.params) {
                if (p) encodeType(out, *p);
            }
            if (t.component) encodeType(out, *t.component);
            break;
        case TypeDescriptor::Kind::Nullable:
            if (t.component) encodeType(out, *t.component);
            break;
        case TypeDescriptor::Kind::Unknown:
            break;
    }
}

void encodeValue(std::string& out, const Constant& c) {
    switch (c.kind) {
        case Constant::Kind::Bool:
            out.push_back(std::get<bool>(c.value) ? 1 : 0);
            break;
        case Constant::Kind::Int:
            encodeU64(out, static_cast<uint64_t>(std::get<int64_t>(c.value)));
            break;
        case Constant::Kind::Float:
            encodeDouble(out, std::get<double>(c.value));
            break;
        case Constant::Kind::String:
        case Constant::Kind::Char: {
            const auto& s = std::get<std::string>(c.value);
            encodeU32(out, static_cast<uint32_t>(s.size()));
            out.append(s);
            break;
        }
        case Constant::Kind::Nil:
            break;
        case Constant::Kind::Array: {
            const auto& elems = std::get<std::vector<Constant>>(c.value);
            encodeU32(out, static_cast<uint32_t>(elems.size()));
            for (const auto& e : elems) encodeValue(out, e);
            break;
        }
        case Constant::Kind::Function:
            encodeU32(out, std::get<uint32_t>(c.value));
            break;
        case Constant::Kind::RowRef: {
            const auto& rr = std::get<RowRefConstant>(c.value);
            encodeU32(out, rr.tableIndex);
            encodeU32(out, rr.rowIndex);
            break;
        }
    }
}

std::string makeKey(const Constant& c) {
    std::string key;
    key.reserve(1 + 8 + c.type.namedMangled.size());
    key.push_back(static_cast<char>(c.kind));
    encodeType(key, c.type);
    encodeValue(key, c);
    return key;
}

bool isWellFormedUtf8(const std::string& s) {
    // Minimal UTF-8 validation: reject overlong encodings, surrogates,
    // and truncated sequences. This is not a full Unicode validator;
    // it is the check that catches the compiler bug "a String constant
    // is not valid UTF-8."
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        if (b < 0x80) { ++i; continue; }
        if ((b & 0xE0) == 0xC0) { n = 2; if (b < 0xC2) return false; }
        else if ((b & 0xF0) == 0xE0) { n = 3; }
        else if ((b & 0xF8) == 0xF0) { n = 4; if (b > 0xF4) return false; }
        else return false;

        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
        }
        i += n;
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

uint32_t ConstantPool::add(Constant c) {
    const std::string key = makeKey(c);

    auto it = m_dedup.find(key);
    if (it != m_dedup.end()) {
        return it->second;
    }

    const uint32_t index = static_cast<uint32_t>(m_constants.size());
    m_constants.push_back(std::move(c));
    m_dedup.emplace(std::move(key), index);
    return index;
}

const Constant& ConstantPool::at(uint32_t index) const {
    AST_ASSERT_MSG(index < m_constants.size(),
        "ConstantPool::at: index out of range — "
        "the caller assumed a constant index the pool does not have");
    return m_constants[index];
}

void ConstantPool::checkInvariants() const {
    for (size_t i = 0; i < m_constants.size(); ++i) {
        const Constant& c = m_constants[i];

        // The variant's active alternative must match the kind tag.
        // A mismatch is a compiler bug: the caller set `kind` and then
        // wrote a different alternative into `value`.
        switch (c.kind) {
            case Constant::Kind::Bool:
                AST_ASSERT_MSG(std::holds_alternative<bool>(c.value),
                    "ConstantPool: kind is Bool but the value is not a bool");
                break;
            case Constant::Kind::Int:
                AST_ASSERT_MSG(std::holds_alternative<int64_t>(c.value),
                    "ConstantPool: kind is Int but the value is not an int64");
                break;
            case Constant::Kind::Float:
                AST_ASSERT_MSG(std::holds_alternative<double>(c.value),
                    "ConstantPool: kind is Float but the value is not a double");
                break;
            case Constant::Kind::String:
            case Constant::Kind::Char: {
                AST_ASSERT_MSG(std::holds_alternative<std::string>(c.value),
                    "ConstantPool: kind is String/Char but the value is not "
                    "a string");
                const auto& s = std::get<std::string>(c.value);
                AST_ASSERT_MSG(isWellFormedUtf8(s),
                    "ConstantPool: a String/Char constant is not valid UTF-8");
                break;
            }
            case Constant::Kind::Nil:
                AST_ASSERT_MSG(std::holds_alternative<std::monostate>(c.value),
                    "ConstantPool: kind is Nil but the value is not the "
                    "monostate sentinel");
                break;
            case Constant::Kind::Array: {
                AST_ASSERT_MSG(
                    std::holds_alternative<std::vector<Constant>>(c.value),
                    "ConstantPool: kind is Array but the value is not an array");
                const auto& elems = std::get<std::vector<Constant>>(c.value);
                break;
            }
            case Constant::Kind::Function:
                AST_ASSERT_MSG(std::holds_alternative<uint32_t>(c.value),
                    "ConstantPool: kind is Function but the value is not a "
                    "function index");
                break;
            case Constant::Kind::RowRef:
                AST_ASSERT_MSG(
                    std::holds_alternative<RowRefConstant>(c.value),
                    "ConstantPool: kind is RowRef but the value is not "
                    "a RowRefConstant");
                break;    
        }
    }
}

} // namespace lucid::bytecode
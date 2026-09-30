/// @file bytecode/Serialize.cpp
/// @brief The .lucb codec: write a Bytecode to a stream, read one back.
///
/// ─── Format version 1 ─────────────────────────────────────────────────────
///
///   Section 1: Header
///     magic            u32    LUCB_MAGIC
///     formatVersion    u32    LUCB_FORMAT_VERSION
///     producerVersion  string LUCB_PRODUCER_VERSION (length-prefixed)
///
///   Section 2: Manifest
///     moduleCount      u32
///     for each module:
///       modulePath     string
///       importCount    u32
///       for each import:
///         alias        string
///         targetPath   string
///       functionCount  u32
///       for each:      string
///       bindingCount   u32
///       for each:      string
///       tableCount     u32
///       for each:      string
///     entryModulePath  string (empty if none)
///
///   Section 3: HostSymbolTable
///     symbolCount      u32
///     for each symbol:
///       kind           u8   (0 = Function, 1 = Type)
///       name           string
///
///   Section 4: ConstantPool
///     constantCount    u32
///     for each constant:
///       kind           u8
///       type           TypeDescriptor
///       value          (encoding depends on kind)
///
///   Section 5: StaticData
///     tableCount       u32
///     for each table:
///       mangledName    string
///       flags          u8 (bit 0 = isFixed, bit 1 = isReadonly,
///                          bit 2 = isPacked, bit 3 = isColumnar,
///                          bit 4 = isRequest, bit 5 = isHostBacked)
///       hostTypeSym    i32
///       hasReserve     u8 (0 = no, 1 = yes)
///       reserveCount   u64 (only if hasReserve)
///       columnCount    u32
///       for each column:
///         mangledName  string
///         type         TypeDescriptor
///         colFlags     u8 (bit 0 = isUnique, bit 1 = isPrimary,
///                          bit 2 = isReadonly)
///       rowCount       u32
///       for each row:
///         cellCount    u32
///         for each:    Constant
///     bindingCount     u32
///     for each binding:
///       mangledName    string
///       type           TypeDescriptor
///       value          Constant
///
///   Section 6: FunctionProtos
///     functionCount    u32
///     for each function:
///       name           string
///       signature      (params: u32 count, then each TypeDescriptor;
///                       returnType: TypeDescriptor)
///       codeLength     u32
///       code           bytes
///       lineCount      u32
///       for each line:
///         codeOffset   u32
///         line         u32
///         column       u32
///         filePath     string
///       localSlots     u32
///       maxStackDepth  u32
///       isSequence     u8
///       resumeCount    u32
///       for each resume:
///         resumeIndex  u32
///         slotCount    u32
///         for each:    u16
///
///   Section 7: (reserved for CRC32; not written in format version 1)
///
/// ─── Design: length-prefixed, no offsets ──────────────────────────────────
/// The format is a straight forward walk. Every variable-length item is
/// length-prefixed. There are no absolute offsets, no relocation table,
/// no cross-section pointers. The only cross-section references are
/// indices (constant index, function index, host symbol index), and
/// those are validated after deserialization by Bytecode's own
/// checkInvariants.
///
/// ─── Design: errors, not exceptions ───────────────────────────────────────
/// Every read checks the stream state and the length against the
/// remaining bytes. On failure, the function returns a SerializeError
/// with a DiagCode. The caller decides what to do.

#include "Serialize.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

#include <cstring>
#include <limits>
#include <vector>

using namespace lucid::diag;

namespace lucid::bytecode {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Write primitives
// ─────────────────────────────────────────────────────────────────────────────

void writeU8(std::ostream& os, uint8_t v) {
    os.put(static_cast<char>(v));
}

void writeU16(std::ostream& os, uint16_t v) {
    // Little-endian. Every multi-byte integer in the format is
    // little-endian. This is a choice; the format is internal to the
    // project and both writer and reader use the same convention.
    os.put(static_cast<char>(v & 0xFF));
    os.put(static_cast<char>((v >> 8) & 0xFF));
}

void writeU32(std::ostream& os, uint32_t v) {
    os.put(static_cast<char>(v & 0xFF));
    os.put(static_cast<char>((v >> 8) & 0xFF));
    os.put(static_cast<char>((v >> 16) & 0xFF));
    os.put(static_cast<char>((v >> 24) & 0xFF));
}

void writeU64(std::ostream& os, uint64_t v) {
    writeU32(os, static_cast<uint32_t>(v & 0xFFFFFFFF));
    writeU32(os, static_cast<uint32_t>(v >> 32));
}

void writeI32(std::ostream& os, int32_t v) {
    writeU32(os, static_cast<uint32_t>(v));
}

void writeString(std::ostream& os, const std::string& s) {
    writeU32(os, static_cast<uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Read primitives
// ─────────────────────────────────────────────────────────────────────────────

struct Reader {
    std::istream& is;
    bool          failed = false;
    std::string   errorMessage;

    bool fail(const std::string& msg) {
        failed = true;
        if (errorMessage.empty()) errorMessage = msg;
        return false;
    }

    bool readU8(uint8_t& out) {
        char c;
        if (!is.get(c)) return fail("unexpected end of stream reading u8");
        out = static_cast<uint8_t>(c);
        return true;
    }

    bool readU16(uint16_t& out) {
        uint8_t a, b;
        if (!readU8(a) || !readU8(b)) return false;
        out = static_cast<uint16_t>(a) | (static_cast<uint16_t>(b) << 8);
        return true;
    }

    bool readU32(uint32_t& out) {
        uint8_t a, b, c, d;
        if (!readU8(a) || !readU8(b) || !readU8(c) || !readU8(d)) return false;
        out = static_cast<uint32_t>(a)
            | (static_cast<uint32_t>(b) << 8)
            | (static_cast<uint32_t>(c) << 16)
            | (static_cast<uint32_t>(d) << 24);
        return true;
    }

    bool readU64(uint64_t& out) {
        uint32_t lo, hi;
        if (!readU32(lo) || !readU32(hi)) return false;
        out = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
        return true;
    }

    bool readI32(int32_t& out) {
        uint32_t u;
        if (!readU32(u)) return false;
        out = static_cast<int32_t>(u);
        return true;
    }

    bool readString(std::string& out) {
        uint32_t len;
        if (!readU32(len)) return false;
        // A sanity bound: no single string in a well-formed artifact is
        // larger than 256 MiB. A larger length is a corrupted file.
        constexpr uint32_t MAX_STRING = 256u * 1024u * 1024u;
        if (len > MAX_STRING) return fail("string length is implausibly large");
        out.resize(len);
        if (len > 0) {
            is.read(out.data(), static_cast<std::streamsize>(len));
            if (!is) return fail("unexpected end of stream reading string");
        }
        return true;
    }

    bool readBytes(std::vector<uint8_t>& out, uint32_t len) {
        // A sanity bound on code size: 512 MiB is far larger than any
        // real function.
        constexpr uint32_t MAX_CODE = 512u * 1024u * 1024u;
        if (len > MAX_CODE) return fail("code length is implausibly large");
        out.resize(len);
        if (len > 0) {
            is.read(reinterpret_cast<char*>(out.data()),
                    static_cast<std::streamsize>(len));
            if (!is) return fail("unexpected end of stream reading bytes");
        }
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeDescriptor codec
// ─────────────────────────────────────────────────────────────────────────────

void writeType(std::ostream& os, const TypeDescriptor& t) {
    writeU8(os, static_cast<uint8_t>(t.kind));
    switch (t.kind) {
        case TypeDescriptor::Kind::Primitive:
            writeU8(os, static_cast<uint8_t>(t.primitive));
            break;
        case TypeDescriptor::Kind::Named:
            writeString(os, t.namedMangled);
            break;
        case TypeDescriptor::Kind::Array:
            writeU8(os, static_cast<uint8_t>(t.arrayKind));
            writeU64(os, t.fixedSize);
            if (t.component) {
                writeU8(os, 1);
                writeType(os, *t.component);
            } else {
                writeU8(os, 0);
            }
            break;
        case TypeDescriptor::Kind::RowRef:
        case TypeDescriptor::Kind::Nullable:
            if (t.component) {
                writeU8(os, 1);
                writeType(os, *t.component);
            } else {
                writeU8(os, 0);
            }
            break;
        case TypeDescriptor::Kind::Function:
            writeU32(os, static_cast<uint32_t>(t.params.size()));
            for (const auto& p : t.params) {
                if (p) writeType(os, *p);
            }
            if (t.component) {
                writeU8(os, 1);
                writeType(os, *t.component);
            } else {
                writeU8(os, 0);
            }
            break;
        case TypeDescriptor::Kind::Unknown:
            break;
    }
}

bool readType(Reader& r, TypeDescriptor& out, int depth) {
    if (depth > 64) return r.fail("type nesting is too deep");

    uint8_t kindByte;
    if (!r.readU8(kindByte)) return false;
    out.kind = static_cast<TypeDescriptor::Kind>(kindByte);

    switch (out.kind) {
        case TypeDescriptor::Kind::Primitive: {
            uint8_t p;
            if (!r.readU8(p)) return false;
            out.primitive = static_cast<PrimitiveKind>(p);
            return true;
        }
        case TypeDescriptor::Kind::Named:
            return r.readString(out.namedMangled);
        case TypeDescriptor::Kind::Array: {
            uint8_t ak;
            if (!r.readU8(ak)) return false;
            out.arrayKind = static_cast<ArrayKind>(ak);
            if (!r.readU64(out.fixedSize)) return false;
            uint8_t hasComponent;
            if (!r.readU8(hasComponent)) return false;
            if (hasComponent) {
                out.component = std::make_shared<TypeDescriptor>();
                return readType(r, *out.component, depth + 1);
            }
            return true;
        }
        case TypeDescriptor::Kind::RowRef:
        case TypeDescriptor::Kind::Nullable: {
            uint8_t hasComponent;
            if (!r.readU8(hasComponent)) return false;
            if (hasComponent) {
                out.component = std::make_shared<TypeDescriptor>();
                return readType(r, *out.component, depth + 1);
            }
            return true;
        }
        case TypeDescriptor::Kind::Function: {
            uint32_t paramCount;
            if (!r.readU32(paramCount)) return false;
            if (paramCount > 1024) return r.fail("function type has too many params");
            out.params.reserve(paramCount);
            for (uint32_t i = 0; i < paramCount; ++i) {
                auto p = std::make_shared<TypeDescriptor>();
                if (!readType(r, *p, depth + 1)) return false;
                out.params.push_back(std::move(p));
            }
            uint8_t hasReturn;
            if (!r.readU8(hasReturn)) return false;
            if (hasReturn) {
                out.component = std::make_shared<TypeDescriptor>();
                return readType(r, *out.component, depth + 1);
            }
            return true;
        }
        case TypeDescriptor::Kind::Unknown:
            return true;
    }
    return r.fail("unknown TypeDescriptor kind");
}

// ─────────────────────────────────────────────────────────────────────────────
// Constant codec
// ─────────────────────────────────────────────────────────────────────────────

void writeConstant(std::ostream& os, const Constant& c) {
    writeU8(os, static_cast<uint8_t>(c.kind));
    writeType(os, c.type);
    switch (c.kind) {
        case Constant::Kind::Bool:
            writeU8(os, std::get<bool>(c.value) ? 1 : 0);
            break;
        case Constant::Kind::Int:
            writeU64(os, static_cast<uint64_t>(std::get<int64_t>(c.value)));
            break;
        case Constant::Kind::Float: {
            uint64_t bits;
            double d = std::get<double>(c.value);
            std::memcpy(&bits, &d, sizeof(bits));
            writeU64(os, bits);
            break;
        }
        case Constant::Kind::String:
        case Constant::Kind::Char:
            writeString(os, std::get<std::string>(c.value));
            break;
        case Constant::Kind::Nil:
            break;
        case Constant::Kind::Array: {
            const auto& elems = std::get<std::vector<Constant>>(c.value);
            writeU32(os, static_cast<uint32_t>(elems.size()));
            for (const auto& e : elems) writeConstant(os, e);
            break;
        }
        case Constant::Kind::Function:
            writeU32(os, std::get<uint32_t>(c.value));
            break;
    }
}

bool readConstant(Reader& r, Constant& out, int depth) {
    if (depth > 64) return r.fail("constant nesting is too deep");

    uint8_t kindByte;
    if (!r.readU8(kindByte)) return false;
    out.kind = static_cast<Constant::Kind>(kindByte);

    if (!readType(r, out.type, 0)) return false;

    switch (out.kind) {
        case Constant::Kind::Bool: {
            uint8_t b;
            if (!r.readU8(b)) return false;
            out.value = (b != 0);
            return true;
        }
        case Constant::Kind::Int: {
            uint64_t u;
            if (!r.readU64(u)) return false;
            out.value = static_cast<int64_t>(u);
            return true;
        }
        case Constant::Kind::Float: {
            uint64_t bits;
            if (!r.readU64(bits)) return false;
            double d;
            std::memcpy(&d, &bits, sizeof(d));
            out.value = d;
            return true;
        }
        case Constant::Kind::String:
        case Constant::Kind::Char: {
            std::string s;
            if (!r.readString(s)) return false;
            out.value = std::move(s);
            return true;
        }
        case Constant::Kind::Nil:
            out.value = std::monostate{};
            return true;
        case Constant::Kind::Array: {
            uint32_t count;
            if (!r.readU32(count)) return false;
            if (count > 1u << 24) return r.fail("array constant is too large");
            std::vector<Constant> elems;
            elems.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Constant e;
                if (!readConstant(r, e, depth + 1)) return false;
                elems.push_back(std::move(e));
            }
            out.value = std::move(elems);
            return true;
        }
        case Constant::Kind::Function: {
            uint32_t idx;
            if (!r.readU32(idx)) return false;
            out.value = idx;
            return true;
        }
    }
    return r.fail("unknown Constant kind");
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Serialize
// ─────────────────────────────────────────────────────────────────────────────

std::optional<SerializeError> serialize(const Bytecode& bc, std::ostream& os) {
    // ─── Section 1: header ─────────────────────────────────────────────
    writeU32(os, LUCB_MAGIC);
    writeU32(os, LUCB_FORMAT_VERSION);
    writeString(os, LUCB_PRODUCER_VERSION);

    // ─── Section 2: Manifest ───────────────────────────────────────────
    const Manifest& m = bc.manifest();
    writeU32(os, static_cast<uint32_t>(m.modules.size()));
    for (const auto& mod : m.modules) {
        writeString(os, mod.modulePath);

        writeU32(os, static_cast<uint32_t>(mod.imports.size()));
        for (const auto& imp : mod.imports) {
            writeString(os, imp.alias);
            writeString(os, imp.targetPath);
        }

        writeU32(os, static_cast<uint32_t>(mod.functions.size()));
        for (const auto& f : mod.functions) writeString(os, f);

        writeU32(os, static_cast<uint32_t>(mod.bindings.size()));
        for (const auto& b : mod.bindings) writeString(os, b);

        writeU32(os, static_cast<uint32_t>(mod.tables.size()));
        for (const auto& t : mod.tables) writeString(os, t);
    }
    writeString(os, m.entryModulePath);

    // ─── Section 3: HostSymbolTable ────────────────────────────────────
    const HostSymbolTable& hs = bc.hostSymbols();
    writeU32(os, static_cast<uint32_t>(hs.size()));
    for (const auto& sym : hs.all()) {
        writeU8(os, static_cast<uint8_t>(sym.kind));
        writeString(os, sym.name);
    }

    // ─── Section 4: ConstantPool ───────────────────────────────────────
    const ConstantPool& cp = bc.constants();
    writeU32(os, static_cast<uint32_t>(cp.size()));
    for (const auto& c : cp.all()) {
        writeConstant(os, c);
    }

    // ─── Section 5: StaticData ─────────────────────────────────────────
    const StaticData& sd = bc.staticData();

    writeU32(os, static_cast<uint32_t>(sd.tables().size()));
    for (const auto& table : sd.tables()) {
        writeString(os, table.mangledName);

        uint8_t flags = 0;
        if (table.isFixed)      flags |= 0x01;
        if (table.isReadonly)   flags |= 0x02;
        if (table.isPacked)     flags |= 0x04;
        if (table.isColumnar)   flags |= 0x08;
        if (table.isRequest)    flags |= 0x10;
        if (table.isHostBacked) flags |= 0x20;
        writeU8(os, flags);

        writeI32(os, table.hostTypeSymbolIndex);

        if (table.reservedCount.has_value()) {
            writeU8(os, 1);
            writeU64(os, *table.reservedCount);
        } else {
            writeU8(os, 0);
        }

        writeU32(os, static_cast<uint32_t>(table.columns.size()));
        for (const auto& col : table.columns) {
            writeString(os, col.mangledName);
            writeType(os, col.type);

            uint8_t colFlags = 0;
            if (col.isUnique)   colFlags |= 0x01;
            if (col.isPrimary)  colFlags |= 0x02;
            if (col.isReadonly) colFlags |= 0x04;
            writeU8(os, colFlags);
        }

        writeU32(os, static_cast<uint32_t>(table.rows.size()));
        for (const auto& row : table.rows) {
            writeU32(os, static_cast<uint32_t>(row.size()));
            for (const auto& cell : row) {
                writeConstant(os, cell);
            }
        }
    }

    writeU32(os, static_cast<uint32_t>(sd.bindings().size()));
    for (const auto& b : sd.bindings()) {
        writeString(os, b.mangledName);
        writeType(os, b.type);
        writeConstant(os, b.initialValue);
    }

    // ─── Section 6: FunctionProtos ─────────────────────────────────────
    const auto& fns = bc.functions();
    writeU32(os, static_cast<uint32_t>(fns.size()));
    for (const auto& fn : fns) {
        writeString(os, fn.name());

        const auto& sig = fn.signature();
        writeU32(os, static_cast<uint32_t>(sig.params.size()));
        for (const auto& p : sig.params) writeType(os, p);
        writeType(os, sig.returnType);

        writeU32(os, static_cast<uint32_t>(fn.code().size()));
        os.write(reinterpret_cast<const char*>(fn.code().data()),
                 static_cast<std::streamsize>(fn.code().size()));

        writeU32(os, static_cast<uint32_t>(fn.lineTable().size()));
        for (const auto& le : fn.lineTable()) {
            writeU32(os, le.codeOffset);
            writeU32(os, le.line);
            writeU32(os, le.column);
            writeString(os, le.filePath);
        }

        writeU32(os, fn.localSlots());
        writeU32(os, fn.maxStackDepth());
        writeU8(os, fn.isSequence() ? 1 : 0);

        writeU32(os, static_cast<uint32_t>(fn.resumeTable().size()));
        for (const auto& re : fn.resumeTable()) {
            writeU32(os, re.resumeIndex);
            writeU32(os, static_cast<uint32_t>(re.liveSlots.size()));
            for (uint16_t slot : re.liveSlots) {
                writeU16(os, slot);
            }
        }
    }

    if (!os) {
        return SerializeError{DiagCode::Bc_SerializationFailed,
                              "stream write failed"};
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Deserialize
// ─────────────────────────────────────────────────────────────────────────────

DeserializeResult deserialize(std::istream& is) {
    Reader r{is, false, {}};

    // ─── Section 1: header ─────────────────────────────────────────────
    uint32_t magic;
    if (!r.readU32(magic)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "stream ended before magic"}};
    }
    if (magic != LUCB_MAGIC) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_BadMagic,
                               "magic number does not match LUCB"}};
    }

    uint32_t formatVersion;
    if (!r.readU32(formatVersion)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "stream ended before version"}};
    }
    if (formatVersion != LUCB_FORMAT_VERSION) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_FormatVersionMismatch,
                               "bytecode format version " +
                               std::to_string(formatVersion) +
                               " does not match reader version " +
                               std::to_string(LUCB_FORMAT_VERSION)}};
    }

    std::string producerVersion;
    if (!r.readString(producerVersion)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated,
                               "stream ended before producer version"}};
    }
    (void)producerVersion;   // informational; not used by the loader

    // ─── Section 2: Manifest ───────────────────────────────────────────
    Manifest manifest;
    uint32_t moduleCount;
    if (!r.readU32(moduleCount)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "manifest module count"}};
    }
    if (moduleCount > 1u << 16) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_DeserializationFailed,
                               "manifest has an implausible module count"}};
    }
    manifest.modules.reserve(moduleCount);
    for (uint32_t mi = 0; mi < moduleCount; ++mi) {
        Manifest::Module mod;
        if (!r.readString(mod.modulePath)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "module path"}};
        }

        uint32_t importCount;
        if (!r.readU32(importCount)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "import count"}};
        }
        mod.imports.reserve(importCount);
        for (uint32_t ii = 0; ii < importCount; ++ii) {
            Manifest::Module::Import imp;
            if (!r.readString(imp.alias) || !r.readString(imp.targetPath)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "import"}};
            }
            mod.imports.push_back(std::move(imp));
        }

        auto readNameList = [&](std::vector<std::string>& out,
                                const char* what) -> bool {
            uint32_t count;
            if (!r.readU32(count)) {
                r.fail(std::string("name list count (") + what + ")");
                return false;
            }
            out.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                std::string s;
                if (!r.readString(s)) {
                    r.fail(std::string("name list entry (") + what + ")");
                    return false;
                }
                out.push_back(std::move(s));
            }
            return true;
        };

        if (!readNameList(mod.functions, "functions")) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }
        if (!readNameList(mod.bindings, "bindings")) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }
        if (!readNameList(mod.tables, "tables")) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }

        manifest.modules.push_back(std::move(mod));
    }

    if (!r.readString(manifest.entryModulePath)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "entry module path"}};
    }

    // ─── Section 3: HostSymbolTable ────────────────────────────────────
    HostSymbolTable hostSymbols;
    uint32_t symbolCount;
    if (!r.readU32(symbolCount)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "host symbol count"}};
    }
    if (symbolCount > 1u << 20) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_DeserializationFailed,
                               "implausible host symbol count"}};
    }
    for (uint32_t i = 0; i < symbolCount; ++i) {
        uint8_t kindByte;
        if (!r.readU8(kindByte)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "host symbol kind"}};
        }
        if (kindByte > 1) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_DeserializationFailed,
                                   "unknown host symbol kind"}};
        }
        HostSymbol sym;
        sym.kind = static_cast<HostSymbol::Kind>(kindByte);
        if (!r.readString(sym.name)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "host symbol name"}};
        }
        // add() deduplicates; a corrupted file with duplicate entries
        // is silently consolidated. This is acceptable: the artifact
        // remains well-formed.
        hostSymbols.add(std::move(sym));
    }

    // ─── Section 4: ConstantPool ───────────────────────────────────────
    ConstantPool constants;
    uint32_t constantCount;
    if (!r.readU32(constantCount)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "constant count"}};
    }
    if (constantCount > 1u << 24) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_DeserializationFailed,
                               "implausible constant count"}};
    }
    for (uint32_t i = 0; i < constantCount; ++i) {
        Constant c;
        if (!readConstant(r, c, 0)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }
        // Add preserves ordering; the pool's dedup is not invoked here
        // because the file already contains interned constants. If a
        // file was hand-edited to contain duplicates, add()'s dedup
        // would collapse them, which would invalidate any constant
        // index in the code. So the deserializer does NOT dedup; it
        // appends. This is a design decision: the on-disk form is the
        // source of truth for indices.
        constants.add(std::move(c));
    }

    // ─── Section 5: StaticData ─────────────────────────────────────────
    StaticData staticData;

    uint32_t tableCount;
    if (!r.readU32(tableCount)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "static table count"}};
    }
    if (tableCount > 1u << 16) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_DeserializationFailed,
                               "implausible static table count"}};
    }
    for (uint32_t ti = 0; ti < tableCount; ++ti) {
        BakedTable table;
        if (!r.readString(table.mangledName)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "table mangled name"}};
        }

        uint8_t flags;
        if (!r.readU8(flags)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "table flags"}};
        }
        table.isFixed      = (flags & 0x01) != 0;
        table.isReadonly   = (flags & 0x02) != 0;
        table.isPacked     = (flags & 0x04) != 0;
        table.isColumnar   = (flags & 0x08) != 0;
        table.isRequest    = (flags & 0x10) != 0;
        table.isHostBacked = (flags & 0x20) != 0;

        if (!r.readI32(table.hostTypeSymbolIndex)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "host type symbol index"}};
        }

        uint8_t hasReserve;
        if (!r.readU8(hasReserve)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "reserve flag"}};
        }
        if (hasReserve) {
            uint64_t reserved;
            if (!r.readU64(reserved)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "reserve count"}};
            }
            table.reservedCount = reserved;
        }

        uint32_t columnCount;
        if (!r.readU32(columnCount)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "column count"}};
        }
        if (columnCount > 1u << 12) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_DeserializationFailed,
                                   "implausible column count"}};
        }
        table.columns.reserve(columnCount);
        for (uint32_t ci = 0; ci < columnCount; ++ci) {
            BakedTable::Column col;
            if (!r.readString(col.mangledName)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "column name"}};
            }
            if (!readType(r, col.type, 0)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
            }
            uint8_t colFlags;
            if (!r.readU8(colFlags)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "column flags"}};
            }
            col.isUnique   = (colFlags & 0x01) != 0;
            col.isPrimary  = (colFlags & 0x02) != 0;
            col.isReadonly = (colFlags & 0x04) != 0;
            table.columns.push_back(std::move(col));
        }

        uint32_t rowCount;
        if (!r.readU32(rowCount)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "row count"}};
        }
        if (rowCount > 1u << 24) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_DeserializationFailed,
                                   "implausible row count"}};
        }
        table.rows.reserve(rowCount);
        for (uint32_t ri = 0; ri < rowCount; ++ri) {
            uint32_t cellCount;
            if (!r.readU32(cellCount)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "row cell count"}};
            }
            if (cellCount != columnCount) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_DeserializationFailed,
                                       "a row's cell count does not match "
                                       "the table's column count"}};
            }
            std::vector<Constant> row;
            row.reserve(cellCount);
            for (uint32_t ci = 0; ci < cellCount; ++ci) {
                Constant cell;
                if (!readConstant(r, cell, 0)) {
                    return {std::nullopt,
                            SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
                }
                row.push_back(std::move(cell));
            }
            table.rows.push_back(std::move(row));
        }

        staticData.tables().push_back(std::move(table));
    }

    uint32_t bindingCount;
    if (!r.readU32(bindingCount)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "static binding count"}};
    }
    if (bindingCount > 1u << 24) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_DeserializationFailed,
                               "implausible static binding count"}};
    }
    staticData.bindings().reserve(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i) {
        BakedBinding b;
        if (!r.readString(b.mangledName)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "binding name"}};
        }
        if (!readType(r, b.type, 0)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }
        if (!readConstant(r, b.initialValue, 0)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }
        staticData.bindings().push_back(std::move(b));
    }

    // ─── Section 6: FunctionProtos ─────────────────────────────────────
    uint32_t functionCount;
    if (!r.readU32(functionCount)) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_Truncated, "function count"}};
    }
    if (functionCount > 1u << 20) {
        return {std::nullopt,
                SerializeError{DiagCode::Bc_DeserializationFailed,
                               "implausible function count"}};
    }
    std::vector<FunctionProto> functions;
    functions.reserve(functionCount);
    for (uint32_t fi = 0; fi < functionCount; ++fi) {
        std::string name;
        if (!r.readString(name)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "function name"}};
        }

        FunctionSignature sig;
        uint32_t paramCount;
        if (!r.readU32(paramCount)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "signature param count"}};
        }
        if (paramCount > 1u << 12) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_DeserializationFailed,
                                   "implausible parameter count"}};
        }
        sig.params.reserve(paramCount);
        for (uint32_t pi = 0; pi < paramCount; ++pi) {
            TypeDescriptor p;
            if (!readType(r, p, 0)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
            }
            sig.params.push_back(std::move(p));
        }
        if (!readType(r, sig.returnType, 0)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }

        uint32_t codeLen;
        if (!r.readU32(codeLen)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "code length"}};
        }
        std::vector<uint8_t> code;
        if (!r.readBytes(code, codeLen)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, r.errorMessage}};
        }

        uint32_t lineCount;
        if (!r.readU32(lineCount)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "line count"}};
        }
        if (lineCount > 1u << 24) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_DeserializationFailed,
                                   "implausible line count"}};
        }
        std::vector<LineEntry> lineTable;
        lineTable.reserve(lineCount);
        for (uint32_t li = 0; li < lineCount; ++li) {
            LineEntry le;
            if (!r.readU32(le.codeOffset) || !r.readU32(le.line)
                || !r.readU32(le.column) || !r.readString(le.filePath)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "line entry"}};
            }
            lineTable.push_back(std::move(le));
        }

        uint32_t localSlots, maxStackDepth;
        if (!r.readU32(localSlots) || !r.readU32(maxStackDepth)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "frame sizes"}};
        }

        uint8_t isSeq;
        if (!r.readU8(isSeq)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "sequence flag"}};
        }

        uint32_t resumeCount;
        if (!r.readU32(resumeCount)) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_Truncated, "resume count"}};
        }
        if (resumeCount > 1u << 16) {
            return {std::nullopt,
                    SerializeError{DiagCode::Bc_DeserializationFailed,
                                   "implausible resume count"}};
        }
        std::vector<ResumeEntry> resumeTable;
        resumeTable.reserve(resumeCount);
        for (uint32_t ri = 0; ri < resumeCount; ++ri) {
            ResumeEntry re;
            if (!r.readU32(re.resumeIndex)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "resume index"}};
            }
            uint32_t slotCount;
            if (!r.readU32(slotCount)) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_Truncated, "live slot count"}};
            }
            if (slotCount > 1u << 12) {
                return {std::nullopt,
                        SerializeError{DiagCode::Bc_DeserializationFailed,
                                       "implausible live slot count"}};
            }
            re.liveSlots.reserve(slotCount);
            for (uint32_t si = 0; si < slotCount; ++si) {
                uint16_t slot;
                if (!r.readU16(slot)) {
                    return {std::nullopt,
                            SerializeError{DiagCode::Bc_Truncated, "live slot"}};
                }
                re.liveSlots.push_back(slot);
            }
            resumeTable.push_back(std::move(re));
        }

        FunctionProto fn(std::move(name),
                         std::move(sig),
                         std::move(code),
                         std::move(lineTable),
                         localSlots,
                         maxStackDepth,
                         isSeq != 0,
                         std::move(resumeTable));
        functions.push_back(std::move(fn));
    }

    // ─── Assemble ──────────────────────────────────────────────────────
    //
    // Bytecode's constructor calls checkInvariants on the whole
    // artifact. A structural failure that the per-section reads did
    // not catch (a cross-reference out of range, a duplicate mangled
    // name) will fire the assert inside the constructor. In a release
    // build where AST_ASSERT_MSG is compiled out, the artifact is
    // constructed anyway and the interpreter's own checks apply at
    // load time.
    Bytecode bc(std::move(manifest),
                std::move(constants),
                std::move(staticData),
                std::move(hostSymbols),
                std::move(functions));

    return {std::move(bc), std::nullopt};
}

} // namespace lucid::bytecode
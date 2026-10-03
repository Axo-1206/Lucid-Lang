/**
 * @file FunctionProto.hpp
 *
 * @responsibility One compiled function. The unit of serialization and
 *                 the unit the interpreter pushes as a frame.
 *
 * ─── Design: the name and signature are the only links back to the AST ────
 * A FunctionProto stores the mangled name and a serializable signature.
 * It does not store an FnDeclAST*, a TypeAST*, or any other pointer into
 * the compiler's input. Two functions with the same name in different
 * modules are distinguishable by their mangled names.
 *
 * ─── Design: resumeTable exists for @sequence functions only ──────────────
 * A @sequence function is lowered to a state machine (§9.2.6). Each
 * suspend point in the code has a resume index; the resumeTable records,
 * for each resume index, which local slots are live across the pause.
 * A non-sequence function has an empty resumeTable.
 */

#pragma once

#include "TypeDescriptor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lucid::bytecode {

/// @brief A serializable source location. Parallels SourceLocation but
///        owns no pointers and can be written to a byte stream.
struct LineEntry {
    uint32_t       codeOffset;
    uint32_t       line;
    uint32_t       column;
    std::string    filePath;   ///< module identity; owned string
};

/// @brief The live-slot set at one suspend point.
struct ResumeEntry {
    uint32_t              resumeIndex;
    std::vector<uint16_t> liveSlots;
};

/// @brief One compiled function.
class FunctionProto {
public:
    FunctionProto() = default;

    FunctionProto(std::string                 mangledName,
                  FunctionSignature           signature,
                  std::vector<uint8_t>        code,
                  std::vector<LineEntry>      lineTable,
                  uint32_t                    localSlots,
                  uint32_t                    maxStackDepth,
                  bool                        isSequence,
                  std::vector<ResumeEntry>    resumeTable);

    FunctionProto(const FunctionProto&)            = default;
    FunctionProto& operator=(const FunctionProto&) = default;
    FunctionProto(FunctionProto&&)                 = default;
    FunctionProto& operator=(FunctionProto&&)      = default;

    // ─── Access ─────────────────────────────────────────────────────────

    const std::string&         name()          const noexcept { return m_name; }
    const FunctionSignature&   signature()     const noexcept { return m_signature; }
    const std::vector<uint8_t>& code()         const noexcept { return m_code; }
    const std::vector<LineEntry>& lineTable()  const noexcept { return m_lineTable; }
    uint32_t                   localSlots()    const noexcept { return m_localSlots; }
    uint32_t                   maxStackDepth() const noexcept { return m_maxStackDepth; }
    bool                       isSequence()    const noexcept { return m_isSequence; }
    const std::vector<ResumeEntry>& resumeTable() const noexcept { return m_resumeTable; }

    // ─── Queries ────────────────────────────────────────────────────────

    /// The source location of a code offset, by binary search over
    /// lineTable. Returns the module path alone if no entry matches
    /// (which happens only for compiler-generated glue).
    struct ResolvedLoc { uint32_t line; uint32_t column; std::string_view filePath; };
    std::optional<ResolvedLoc> locationAt(uint32_t codeOffset) const;

    /// The resume entry with the given index, or nullopt if absent.
    /// A well-formed @sequence function has an entry for every suspend
    /// point; a non-sequence function has none.
    const ResumeEntry* findResume(uint32_t resumeIndex) const;

    // ─── Invariants ─────────────────────────────────────────────────────
    //
    // Checked in the constructor. A violation is a compiler bug.
    //
    //   - name is non-empty.
    //   - code is non-empty (a function with no instructions is
    //     impossible; a unit-returning function still has ReturnVoid).
    //   - maxStackDepth >= 1.
    //   - lineTable offsets are strictly increasing.
    //   - if isSequence is true, resumeTable is non-empty.
    //   - if isSequence is false, resumeTable is empty.
    //   - every liveSlots entry is < localSlots.
    void checkInvariants() const;

private:
    std::string              m_name;
    FunctionSignature        m_signature;
    std::vector<uint8_t>     m_code;
    std::vector<LineEntry>   m_lineTable;
    uint32_t                 m_localSlots    = 0;
    uint32_t                 m_maxStackDepth = 1;
    bool                     m_isSequence    = false;
    std::vector<ResumeEntry> m_resumeTable;
};

} // namespace lucid::bytecode
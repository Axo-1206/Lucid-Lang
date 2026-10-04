/// @file bytecode/FunctionProto.cpp
/// @brief One compiled function: constructor, queries, invariants.

#include "bytecode/FunctionProto.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

#include <algorithm>

namespace lucid::bytecode {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

FunctionProto::FunctionProto(std::string                 mangledName,
                             contract::FunctionSignature signature,
                             std::vector<uint8_t>        code,
                             std::vector<LineEntry>      lineTable,
                             uint32_t                    localSlots,
                             uint32_t                    maxStackDepth,
                             bool                        isSequence,
                             std::vector<ResumeEntry>    resumeTable)
    : m_name(std::move(mangledName))
    , m_signature(std::move(signature))
    , m_code(std::move(code))
    , m_lineTable(std::move(lineTable))
    , m_localSlots(localSlots)
    , m_maxStackDepth(maxStackDepth)
    , m_isSequence(isSequence)
    , m_resumeTable(std::move(resumeTable)) {
    checkInvariants();
}

// ─────────────────────────────────────────────────────────────────────────────
// Queries
// ─────────────────────────────────────────────────────────────────────────────

std::optional<FunctionProto::ResolvedLoc>
FunctionProto::locationAt(uint32_t codeOffset) const {
    if (m_lineTable.empty()) return std::nullopt;

    // Binary search for the last entry whose codeOffset is <= the
    // requested offset. The line table is sorted by codeOffset and
    // every code offset in the function's code is covered by the
    // entry that starts at or before it.
    //
    // std::upper_bound gives the first entry strictly greater than
    // codeOffset; the entry before it is the answer.
    auto it = std::upper_bound(
        m_lineTable.begin(), m_lineTable.end(), codeOffset,
        [](uint32_t off, const LineEntry& e) { return off < e.codeOffset; });

    if (it == m_lineTable.begin()) {
        // The requested offset precedes the first entry. This happens
        // only for compiler-generated glue that was emitted before the
        // first user statement; there is no location for it.
        return std::nullopt;
    }

    const LineEntry& e = *std::prev(it);
    return ResolvedLoc{e.line, e.column, e.filePath};
}

const ResumeEntry* FunctionProto::findResume(uint32_t resumeIndex) const {
    for (const auto& r : m_resumeTable) {
        if (r.resumeIndex == resumeIndex) return &r;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Invariants
// ─────────────────────────────────────────────────────────────────────────────

void FunctionProto::checkInvariants() const {
    AST_ASSERT_MSG(!m_name.empty(),
        "FunctionProto: a function has an empty mangled name");

    AST_ASSERT_MSG(!m_code.empty(),
        "FunctionProto: a function has no code — every function, "
        "including a unit-returning one, has at least a ReturnVoid");

    AST_ASSERT_MSG(m_maxStackDepth >= 1,
        "FunctionProto: maxStackDepth is 0 — a function with no "
        "expression stack is impossible");

    // The line table is strictly increasing in codeOffset. A duplicate
    // or a decrease would break locationAt's binary search.
    for (size_t i = 1; i < m_lineTable.size(); ++i) {
        AST_ASSERT_MSG(
            m_lineTable[i].codeOffset > m_lineTable[i - 1].codeOffset,
            "FunctionProto: the line table is not strictly increasing "
            "in codeOffset");
    }

    // Every line table offset is within the function's code. An offset
    // past the end means the compiler recorded a location for an
    // instruction that does not exist.
    if (!m_lineTable.empty()) {
        AST_ASSERT_MSG(
            m_lineTable.back().codeOffset < m_code.size(),
            "FunctionProto: the last line table entry points past the "
            "end of the function's code");
    }

    // Sequence-ness and the resume table are coupled: a @sequence
    // function has a resume entry for every suspend point; a
    // non-sequence function has none.
    if (m_isSequence) {
        AST_ASSERT_MSG(!m_resumeTable.empty(),
            "FunctionProto: a @sequence function has no resume entries — "
            "either the compiler forgot to lower the suspend points or "
            "the @sequence tag was applied to a function with no "
            "suspend points");
    } else {
        AST_ASSERT_MSG(m_resumeTable.empty(),
            "FunctionProto: a non-sequence function has resume entries — "
            "the compiler emitted suspend-point liveness for a function "
            "that cannot suspend");
    }

    // Every resume entry's live slots are within the function's frame.
    for (const auto& r : m_resumeTable) {
        for (uint16_t slot : r.liveSlots) {
            AST_ASSERT_MSG(slot < m_localSlots,
                "FunctionProto: a resume entry names a live slot that is "
                "outside the function's frame");
        }
    }

    // Resume indices are unique. Two entries with the same index would
    // make findResume ambiguous.
    for (size_t i = 1; i < m_resumeTable.size(); ++i) {
        AST_ASSERT_MSG(
            m_resumeTable[i].resumeIndex != m_resumeTable[i - 1].resumeIndex,
            "FunctionProto: two resume entries have the same index — "
            "the compiler allocated a resume index twice");
    }
}

} // namespace lucid::bytecode
/// @file bytecode/memory/OwnedValue.cpp
/// @brief The ownership-tracking stack.

#include "OwnedValue.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

namespace lucid::bytecode::memory {

Ownership OwnedValueStack::pop() {
    AST_ASSERT_MSG(!m_stack.empty(),
        "OwnedValueStack::pop: the stack is empty — the emitter "
        "consumed a value that was never produced");
    Ownership top = m_stack.back();
    m_stack.pop_back();
    return top;
}

Ownership OwnedValueStack::peek() const {
    AST_ASSERT_MSG(!m_stack.empty(),
        "OwnedValueStack::peek: the stack is empty");
    return m_stack.back();
}

Ownership OwnedValueStack::peekAt(size_t n) const {
    AST_ASSERT_MSG(n < m_stack.size(),
        "OwnedValueStack::peekAt: index out of range");
    return m_stack[m_stack.size() - 1 - n];
}

void OwnedValueStack::markTopAsMoved() {
    AST_ASSERT_MSG(!m_stack.empty(),
        "OwnedValueStack::markTopAsMoved: the stack is empty");
    m_stack.back() = Ownership::Moved;
}

void OwnedValueStack::assertEmpty() const {
    AST_ASSERT_MSG(m_stack.empty(),
        "OwnedValueStack::assertEmpty: the ownership stack is not "
        "empty at the end of a function — the emitter left values "
        "unconsumed");
}

} // namespace lucid::bytecode::memory
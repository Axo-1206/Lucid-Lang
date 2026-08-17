/// @file support/PanicHandler.cpp
/// @brief Runtime panic handling implementation.

#include "PanicHandler.hpp"

#include <iostream>
#include <exception>

namespace interpreter {

int PanicHandler::handle(const std::string& message) {
    // Prevent re-entrant panics
    if (m_panicking) {
        std::cerr << "FATAL: Recursive panic detected!\n";
        std::cerr << "Original: " << m_lastMessage << "\n";
        std::cerr << "New: " << message << "\n";
        return 1;
    }

    m_panicking = true;
    m_lastMessage = message;

    // Use custom callback if set
    if (m_callback) {
        // Exit code 1 is the default for panics
        m_callback(message, 1);
    } else {
        // Default behavior: print to stderr
        std::cerr << "Panic: " << message << "\n";
    }

    m_panicking = false;
    return 1;  // Standard exit code for panic
}

int PanicHandler::handle(const std::exception& exception) {
    return handle(std::string("Exception: ") + exception.what());
}

} // namespace interpreter
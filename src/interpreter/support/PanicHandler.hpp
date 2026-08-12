/// @file support/PanicHandler.hpp
/// @brief Runtime panic handling.

#pragma once

#include <string>
#include <functional>

namespace interpreter {

/// @brief Callback for handling runtime panics.
using PanicCallback = std::function<void(const std::string& message, int exitCode)>;

/// @brief Handles runtime panics in the interpreter.
class PanicHandler {
public:
    PanicHandler() = default;
    ~PanicHandler() = default;

    /// @brief Set a custom panic callback.
    void setCallback(PanicCallback callback) { m_callback = std::move(callback); }

    /// @brief Handle a panic with the given message.
    /// @return The exit code to return.
    int handle(const std::string& message);

    /// @brief Handle a panic from an exception.
    /// @return The exit code to return.
    int handle(const std::exception& exception);

    /// @brief Check if a panic is currently being handled.
    bool isPanicking() const { return m_panicking; }

    /// @brief Get the last panic message.
    const std::string& getLastPanicMessage() const { return m_lastMessage; }

private:
    PanicCallback m_callback;
    std::string m_lastMessage;
    bool m_panicking = false;
};

} // namespace interpreter
/**
 * @file DiagnosticMessages.hpp
 * @brief Provides access to human‑readable diagnostic message strings.
 *
 * Each DiagCode maps to a static message template with optional %s placeholders.
 */

#pragma once

#include "DiagnosticCodes.hpp"
#include <string_view>

namespace DiagnosticMessages {

/**
 * @brief Returns the message template for a given diagnostic code.
 * @param code The diagnostic code.
 * @return A string_view containing the message template.
 *
 * If the code is unknown, returns "Unknown diagnostic code".
 */
std::string_view getMessage(DiagCode code);

} // namespace DiagnosticMessages
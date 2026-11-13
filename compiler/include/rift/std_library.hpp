#pragma once
// The standard library's Rift source, compiled into riftc (spec 20: the library is written in
// Rift where possible). The text comes from library/std/ at build time.

#include <string_view>

namespace rift {

/// Path shown in diagnostics for the built-in std header.
inline constexpr std::string_view std_header_path = "<std>/std.rifth";

/// Text of library/std/std.rifth.
std::string_view std_header_text();

} // namespace rift

#pragma once
// Diagnostics: collection and rendering.
//
// Compiler phases never print. They report into a DiagnosticEngine; the driver renders the
// collected diagnostics at the end in one of two formats:
//
//   human  error[E1003]: unterminated string literal           (default; one screen per error)
//           --> main.rifts:3:9
//            |
//          3 | u32 x = "abc
//            |         ^^^^ string literal starts here
//
//   short  main.rifts:3:9: error[E1003]: unterminated string literal
//          (one line per diagnostic; stable, used by the feature-test runner)

#include "rift/source.hpp"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rift {

enum class Severity { error, warning, note };

enum class DiagCode {
#define RIFT_DIAG(code, severity, title) code,
#include "rift/diagnostic_codes.def"
#undef RIFT_DIAG
};

std::string_view to_string(Severity severity);
std::string_view code_name(DiagCode code);       // "E1003"
std::string_view code_title(DiagCode code);      // "unterminated string literal"
Severity         code_severity(DiagCode code);
std::optional<DiagCode> code_from_name(std::string_view name);

struct Note {
    std::string         message;
    std::optional<Span> span;
};

struct Diagnostic {
    DiagCode          code;
    Severity          severity;
    std::string       message;
    Span              span;
    std::string       label;   // text next to the carets; may be empty
    std::vector<Note> notes;
};

class DiagnosticEngine {
public:
    /// Reports a diagnostic with the code's default severity and returns it so callers can add
    /// notes: `diags.report(...).notes.push_back(...)`.
    Diagnostic& report(DiagCode code, Span span, std::string message, std::string label = {});

    [[nodiscard]] const std::vector<Diagnostic>& all() const { return diagnostics_; }

    /// Orders diagnostics by file and position (stable), so later phases' errors don't appear
    /// before earlier lines, and drops exact repeats (same code, place and message).
    void sort_by_location();
    [[nodiscard]] std::size_t error_count() const;
    [[nodiscard]] std::size_t warning_count() const;
    [[nodiscard]] bool        has_errors() const { return error_count() > 0; }

private:
    std::vector<Diagnostic> diagnostics_;
};

enum class DiagnosticFormat { human, short_form };

struct RenderOptions {
    DiagnosticFormat format = DiagnosticFormat::human;
    bool             color  = false;
};

void render(std::ostream& out, const Diagnostic& diag, const SourceManager& sources,
            const RenderOptions& options);

/// Renders every diagnostic in report order and, in human format, a closing summary line.
void render_all(std::ostream& out, const DiagnosticEngine& diags, const SourceManager& sources,
                const RenderOptions& options);

} // namespace rift

#include "rift/diagnostics.hpp"

#include <algorithm>
#include <array>
#include <ostream>

namespace rift {

namespace {

struct CodeInfo {
    std::string_view name;
    Severity         severity;
    std::string_view title;
};

constexpr std::array code_table = {
#define RIFT_DIAG(code, sev, title) CodeInfo{#code, Severity::sev, title},
#include "rift/diagnostic_codes.def"
#undef RIFT_DIAG
};

const CodeInfo& info(DiagCode code) { return code_table.at(static_cast<std::size_t>(code)); }

// ANSI colors; only used when RenderOptions::color is set.
struct Style {
    bool             on;
    std::string_view bold() const { return on ? "\x1b[1m" : ""; }
    std::string_view red() const { return on ? "\x1b[1;31m" : ""; }
    std::string_view yellow() const { return on ? "\x1b[1;33m" : ""; }
    std::string_view blue() const { return on ? "\x1b[1;34m" : ""; }
    std::string_view cyan() const { return on ? "\x1b[1;36m" : ""; }
    std::string_view reset() const { return on ? "\x1b[0m" : ""; }
    std::string_view severity(Severity s) const {
        switch (s) {
        case Severity::error: return red();
        case Severity::warning: return yellow();
        case Severity::note: return cyan();
        }
        return "";
    }
};

std::size_t digits(std::uint32_t n) {
    std::size_t d = 1;
    while (n >= 10) {
        n /= 10;
        ++d;
    }
    return d;
}

void render_location(std::ostream& out, const SourceFile& file, Span span) {
    LineCol lc = file.line_col(span.begin);
    out << file.path() << ':' << lc.line << ':' << lc.column;
}

void render_short(std::ostream& out, const Diagnostic& diag, const SourceManager& sources) {
    const SourceFile& file = sources.file(diag.span.file);
    render_location(out, file, diag.span);
    out << ": " << to_string(diag.severity) << '[' << code_name(diag.code) << "]: " << diag.message
        << '\n';
    for (const Note& note : diag.notes) {
        const SourceFile& note_file = sources.file(note.span ? note.span->file : diag.span.file);
        render_location(out, note_file, note.span.value_or(diag.span));
        out << ": note: " << note.message << '\n';
    }
}

struct Mark {
    Span             span;
    char             symbol;   // '^' primary, '-' secondary
    std::string_view color;
};

/// Prints one source line with markers underneath:
///
///   6 |     u32 b = (2 + 3;
///     |             -     ^ unexpected here
///
/// Multi-line spans are marked up to the end of their first line. Tabs in the source are
/// repeated in the marker line so the markers line up.
void render_snippet(std::ostream& out, const SourceFile& file, const std::vector<Mark>& marks,
                    std::string_view label, std::string_view label_color, std::size_t gutter,
                    const Style& style) {
    std::uint32_t    line_no    = file.line_col(marks.front().span.begin).line;
    std::string_view line       = file.line_text(line_no);
    std::uint32_t    line_start = file.line_start(line_no);
    std::uint32_t    line_end   = line_start + static_cast<std::uint32_t>(line.size());
    std::string      pad(gutter, ' ');

    out << style.blue() << pad << " |" << style.reset() << '\n';
    out << style.blue() << std::string(gutter - digits(line_no), ' ') << line_no << " | "
        << style.reset() << line << '\n';

    // Column (in code points) of every mark, and which source columns are tabs.
    std::vector<bool> is_tab;
    for (std::size_t i = 0; i < line.size();) {
        char32_t    cp  = 0;
        std::size_t len = decode_utf8(line, i, cp);
        is_tab.push_back(line[i] == '\t');
        i += len == 0 ? 1 : len;
    }
    struct Cell {
        char             symbol = 0;
        std::string_view color;
    };
    std::vector<Cell> cells;
    for (const Mark& m : marks) {
        std::uint32_t begin = std::clamp(m.span.begin, line_start, line_end);
        std::uint32_t end   = std::clamp(m.span.end, begin, line_end);
        std::uint32_t col   = count_code_points(line.substr(0, begin - line_start));
        std::uint32_t width = end > begin ? count_code_points(file.text().substr(begin, end - begin)) : 1;
        if (cells.size() < col + width) {
            cells.resize(col + width);
        }
        for (std::uint32_t c = col; c < col + width; ++c) {
            if (cells[c].symbol != '^') {   // primary wins where marks overlap
                cells[c] = Cell{m.symbol, m.color};
            }
        }
    }

    out << style.blue() << pad << " | " << style.reset();
    for (std::size_t c = 0; c < cells.size(); ++c) {
        if (cells[c].symbol == 0) {
            out << (c < is_tab.size() && is_tab[c] ? '\t' : ' ');
        } else {
            out << cells[c].color << cells[c].symbol << style.reset();
        }
    }
    if (!label.empty()) {
        out << label_color << ' ' << label << style.reset();
    }
    out << '\n';
}

void render_human(std::ostream& out, const Diagnostic& diag, const SourceManager& sources,
                  const Style& style) {
    const SourceFile& file = sources.file(diag.span.file);
    LineCol           lc   = file.line_col(diag.span.begin);

    std::uint32_t max_line = lc.line;
    for (const Note& note : diag.notes) {
        if (note.span) {
            max_line = std::max(max_line, sources.file(note.span->file).line_col(note.span->begin).line);
        }
    }
    std::size_t gutter = digits(max_line);
    std::string pad(gutter, ' ');

    out << style.severity(diag.severity) << to_string(diag.severity) << '['
        << code_name(diag.code) << ']' << style.reset() << style.bold() << ": " << diag.message
        << style.reset() << '\n';
    out << pad << style.blue() << "--> " << style.reset();
    render_location(out, file, diag.span);
    out << '\n';

    // Notes that point into the primary line are drawn as '-' markers on the same snippet.
    auto same_line = [&](const Note& note) {
        return note.span && note.span->file == diag.span.file &&
               file.line_col(note.span->begin).line == lc.line;
    };
    std::vector<Mark> marks{Mark{diag.span, '^', style.severity(diag.severity)}};
    for (const Note& note : diag.notes) {
        if (same_line(note)) {
            marks.push_back(Mark{*note.span, '-', style.blue()});
        }
    }
    render_snippet(out, file, marks, diag.label, style.severity(diag.severity), gutter, style);

    for (const Note& note : diag.notes) {
        if (note.span && !same_line(note)) {
            const SourceFile& note_file = sources.file(note.span->file);
            if (note.span->file != diag.span.file) {
                out << pad << style.blue() << "::: " << style.reset();
                render_location(out, note_file, *note.span);
                out << '\n';
            }
            render_snippet(out, note_file, {Mark{*note.span, '-', style.blue()}}, "", "", gutter, style);
        }
        out << pad << style.blue() << " = " << style.reset() << style.bold() << "note" << style.reset()
            << ": " << note.message << '\n';
    }
}

} // namespace

std::string_view to_string(Severity severity) {
    switch (severity) {
    case Severity::error: return "error";
    case Severity::warning: return "warning";
    case Severity::note: return "note";
    }
    return "?";
}

std::string_view code_name(DiagCode code) { return info(code).name; }
std::string_view code_title(DiagCode code) { return info(code).title; }
Severity         code_severity(DiagCode code) { return info(code).severity; }

std::optional<DiagCode> code_from_name(std::string_view name) {
    for (std::size_t i = 0; i < code_table.size(); ++i) {
        if (code_table[i].name == name) {
            return static_cast<DiagCode>(i);
        }
    }
    return std::nullopt;
}

Diagnostic& DiagnosticEngine::report(DiagCode code, Span span, std::string message,
                                     std::string label) {
    diagnostics_.push_back(
        Diagnostic{code, code_severity(code), std::move(message), span, std::move(label), {}});
    return diagnostics_.back();
}

void DiagnosticEngine::sort_by_location() {
    std::stable_sort(diagnostics_.begin(), diagnostics_.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.span.file != b.span.file) return a.span.file < b.span.file;
        return a.span.begin < b.span.begin;
    });
}

std::size_t DiagnosticEngine::error_count() const {
    return static_cast<std::size_t>(std::count_if(diagnostics_.begin(), diagnostics_.end(),
        [](const Diagnostic& d) { return d.severity == Severity::error; }));
}

std::size_t DiagnosticEngine::warning_count() const {
    return static_cast<std::size_t>(std::count_if(diagnostics_.begin(), diagnostics_.end(),
        [](const Diagnostic& d) { return d.severity == Severity::warning; }));
}

void render(std::ostream& out, const Diagnostic& diag, const SourceManager& sources,
            const RenderOptions& options) {
    if (options.format == DiagnosticFormat::short_form) {
        render_short(out, diag, sources);
    } else {
        render_human(out, diag, sources, Style{options.color});
    }
}

void render_all(std::ostream& out, const DiagnosticEngine& diags, const SourceManager& sources,
                const RenderOptions& options) {
    bool first = true;
    for (const Diagnostic& diag : diags.all()) {
        if (options.format == DiagnosticFormat::human && !first) {
            out << '\n';
        }
        render(out, diag, sources, options);
        first = false;
    }
    if (options.format == DiagnosticFormat::human && !diags.all().empty()) {
        std::size_t errors   = diags.error_count();
        std::size_t warnings = diags.warning_count();
        out << '\n' << errors << (errors == 1 ? " error" : " errors") << ", " << warnings
            << (warnings == 1 ? " warning" : " warnings") << '\n';
    }
}

} // namespace rift

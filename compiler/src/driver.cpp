#include "rift/driver.hpp"

#include "rift/ast_printer.hpp"
#include "rift/check.hpp"
#include "rift/diagnostics.hpp"
#include "rift/lexer.hpp"
#include "rift/modules.hpp"
#include "rift/parser.hpp"
#include "rift/resolve.hpp"
#include "rift/source.hpp"

#include <filesystem>
#include <format>
#include <optional>
#include <ostream>
#include <vector>

#ifndef RIFT_VERSION
#define RIFT_VERSION "0.0.0-dev"
#endif

namespace rift {

namespace {

constexpr std::string_view usage_text =
    "usage: riftc <command> [options] [file]\n"
    "\n"
    "commands:\n"
    "  lex <file>        print the token stream of a .rifts/.rifth file\n"
    "  parse <file>      print the syntax tree of one file\n"
    "  check <file>      check a module and everything it imports\n"
    "  symbols <file>    print the module's merged symbol table (header + source)\n"
    "  typed <file>      print the syntax tree with the type of every expression\n"
    "  explain [code]    describe a diagnostic code, or list all codes\n"
    "\n"
    "options:\n"
    "  --diagnostic-format=human|short   diagnostic layout (default: human)\n"
    "  --color=auto|always|never         colored diagnostics (default: auto)\n"
    "  --root=DIR                        project root for imports (default: the file's directory)\n"
    "  --stop-after=lex|parse|resolve|types  stop 'check' after a phase\n"
    "  -h, --help                        show this help\n"
    "  -V, --version                     show the compiler version\n"
    "\n"
    "exit codes: 0 success, 1 errors in input, 2 bad command line, 3 internal error\n";

enum class Phase { lex, parse, resolve, types };

struct Options {
    std::string_view command;
    std::string_view input;
    RenderOptions    render;
    bool             color_auto = true;
    std::string      root;
    Phase            stop_after = Phase::types;
};

ExitCode usage_error(DriverStreams& s, std::string_view message) {
    s.err << "riftc: error: " << message << "\nRun 'riftc --help' for usage.\n";
    return ExitCode::usage_error;
}

/// Parses options after the command. Returns an exit code on failure.
std::optional<ExitCode> parse_options(std::span<const std::string_view> args, Options& opts,
                                      DriverStreams& s) {
    for (std::string_view arg : args) {
        if (arg == "--diagnostic-format=human") {
            opts.render.format = DiagnosticFormat::human;
        } else if (arg == "--diagnostic-format=short") {
            opts.render.format = DiagnosticFormat::short_form;
        } else if (arg == "--color=always") {
            opts.render.color = true, opts.color_auto = false;
        } else if (arg == "--color=never") {
            opts.render.color = false, opts.color_auto = false;
        } else if (arg == "--color=auto") {
            opts.color_auto = true;
        } else if (arg.starts_with("--root=")) {
            opts.root = std::string(arg.substr(7));
        } else if (arg == "--stop-after=lex") {
            opts.stop_after = Phase::lex;
        } else if (arg == "--stop-after=parse") {
            opts.stop_after = Phase::parse;
        } else if (arg == "--stop-after=resolve") {
            opts.stop_after = Phase::resolve;
        } else if (arg == "--stop-after=types") {
            opts.stop_after = Phase::types;
        } else if (arg.starts_with("-") && arg != "-") {
            return usage_error(s, std::format("unknown option '{}'", arg));
        } else if (opts.input.empty()) {
            opts.input = arg;
        } else {
            return usage_error(s, std::format("unexpected extra argument '{}'", arg));
        }
    }
    if (opts.input.empty()) {
        return usage_error(s, std::format("'{}' needs an input file", opts.command));
    }
    if (opts.color_auto) {
        opts.render.color = s.err_is_terminal;
    }
    return std::nullopt;
}

std::string escape_for_display(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\0': out += "\\0"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out.push_back(c);
        }
    }
    return out;
}

void dump_tokens(std::ostream& out, const SourceFile& file, const std::vector<Token>& tokens) {
    for (const Token& t : tokens) {
        LineCol     lc  = file.line_col(t.span.begin);
        std::string pos = std::format("{}:{}", lc.line, lc.column);
        std::string line = std::format("{:<8}{:<24}{}", pos, token_kind_name(t.kind), t.text);
        switch (t.kind) {
        case TokenKind::int_literal:
            line += std::format("  value={}", to_string(t.int_value));
            break;
        case TokenKind::char_literal:
        case TokenKind::string_literal:
        case TokenKind::raw_string_literal:
        case TokenKind::format_string_literal:
        case TokenKind::doc_comment:
            line += std::format("  value=\"{}\"", escape_for_display(t.value));
            break;
        default:
            break;
        }
        if (!t.suffix.empty()) {
            line += std::format("  suffix={}", t.suffix);
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        out << line << '\n';
    }
}

ExitCode run_frontend(const Options& opts, DriverStreams& s) {
    std::string input(opts.input);
    if (!input.ends_with(".rifts") && !input.ends_with(".rifth")) {
        s.err << std::format("riftc: error[E0002]: '{}': {}\n", input,
                             code_title(DiagCode::E0002));
        return ExitCode::compile_error;
    }

    SourceManager    sources;
    DiagnosticEngine diags;

    if (opts.command == "lex" || opts.command == "parse" || opts.stop_after == Phase::lex) {
        // Single-file commands: no imports are followed.
        std::string           reason;
        std::optional<FileId> id = sources.load(input, reason);
        if (!id) {
            s.err << std::format("riftc: error[E0001]: cannot read '{}': {}\n", input, reason);
            return ExitCode::compile_error;
        }
        const SourceFile&  file   = sources.file(*id);
        std::vector<Token> tokens = tokenize(file, *id, diags);
        if (opts.command == "lex") {
            dump_tokens(s.out, file, tokens);
        } else if (!diags.has_errors() && opts.stop_after != Phase::lex) {
            ast::File tree = parse(file, *id, tokens, diags);
            ast::print(s.out, tree);
        }
        render_all(s.err, diags, sources, opts.render);
        return diags.has_errors() ? ExitCode::compile_error : ExitCode::success;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(input, ec)) {
        s.err << std::format("riftc: error[E0001]: cannot read '{}': cannot open file\n", input);
        return ExitCode::compile_error;
    }
    // Each phase only runs on input the previous phase accepted, so errors don't cascade.
    sema::Program program = load_program(input, LoadOptions{opts.root}, sources, diags);
    bool resolved = false;
    bool typed    = false;
    if (!diags.has_errors() && opts.stop_after >= Phase::resolve) {
        resolve_program(program, diags);
        resolved = true;
    }
    if (!diags.has_errors() && opts.stop_after >= Phase::types) {
        check_types(program, diags);
        typed = true;
    }
    // Future phases (ownership, code generation) plug in here.
    if (opts.command == "symbols" && resolved && program.entry) {
        print_symbols(s.out, *program.entry, sources);
    }
    if (opts.command == "typed" && typed && program.entry) {
        if (program.entry->header) ast::print(s.out, program.entry->header->ast, true);
        if (program.entry->source) ast::print(s.out, program.entry->source->ast, true);
    }
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    return diags.has_errors() ? ExitCode::compile_error : ExitCode::success;
}

ExitCode run_explain(std::span<const std::string_view> args, DriverStreams& s) {
    if (args.empty()) {
#define RIFT_DIAG(code, severity, title) \
    s.out << std::format("{}  {:<8} {}\n", #code, #severity, title);
#include "rift/diagnostic_codes.def"
#undef RIFT_DIAG
        return ExitCode::success;
    }
    if (args.size() > 1) {
        return usage_error(s, "'explain' takes at most one diagnostic code");
    }
    std::optional<DiagCode> code = code_from_name(args[0]);
    if (!code) {
        return usage_error(s, std::format("unknown diagnostic code '{}'", args[0]));
    }
    s.out << std::format("{} ({}): {}\n", code_name(*code), to_string(code_severity(*code)),
                         code_title(*code));
    return ExitCode::success;
}

} // namespace

std::string version_string() { return std::string("riftc ") + RIFT_VERSION; }

ExitCode run_driver(std::span<const std::string_view> args, DriverStreams streams) {
    if (args.empty()) {
        streams.err << usage_text;
        return ExitCode::usage_error;
    }
    std::string_view command = args[0];
    if (command == "-h" || command == "--help" || command == "help") {
        streams.out << usage_text;
        return ExitCode::success;
    }
    if (command == "-V" || command == "--version") {
        streams.out << version_string() << '\n';
        return ExitCode::success;
    }
    if (command == "explain") {
        return run_explain(args.subspan(1), streams);
    }
    if (command == "lex" || command == "parse" || command == "check" || command == "symbols" || command == "typed") {
        Options opts;
        opts.command = command;
        if (std::optional<ExitCode> failed = parse_options(args.subspan(1), opts, streams)) {
            return *failed;
        }
        return run_frontend(opts, streams);
    }
    return usage_error(streams, std::format("unknown command '{}'", command));
}

} // namespace rift

#include "rift/driver.hpp"

#include "rift/ast_printer.hpp"
#include "rift/check.hpp"
#include "rift/llvm_backend.hpp"
#include "rift/diagnostics.hpp"
#include "rift/lexer.hpp"
#include "rift/modules.hpp"
#include "rift/ownership.hpp"
#include "rift/parser.hpp"
#include "rift/resolve.hpp"
#include "rift/source.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <optional>
#include <ostream>
#include <vector>

extern char** environ;   // POSIX: the process environment, passed on to child processes

#ifndef RIFT_VERSION
#define RIFT_VERSION "0.0.0-dev"
#endif

namespace rift {

namespace {

constexpr std::string_view usage_text =
    "usage: riftc <command> [options] [file]\n"
    "\n"
    "commands:\n"
    "  lex <file>            print the token stream of a .rifts/.rifth file\n"
    "  parse <file>          print the syntax tree of one file\n"
    "  check <file>          check a module and everything it imports\n"
    "  symbols <file>        print the module's merged symbol table (header + source)\n"
    "  typed <file>          print the syntax tree with the type of every expression\n"
    "  build <file>          compile a program to build/<mode>/<name> (next to the file)\n"
    "  run <file> [-- args]  build a program in a temporary directory and run it\n"
    "  emit-llvm <file>      print the LLVM IR the program compiles to\n"
    "  explain [code]        describe a diagnostic code, or list all codes\n"
    "\n"
    "options:\n"
    "  --diagnostic-format=human|short   diagnostic layout (default: human)\n"
    "  --color=auto|always|never         colored diagnostics (default: auto)\n"
    "  --root=DIR                        project root for imports (default: the file's directory)\n"
    "  --stop-after=PHASE                lex, parse, resolve, types or ownership\n"
    "  --mode=MODE                       debug (default), release or release_with_debug_info\n"
    "  -o FILE, --output=FILE            where 'build' writes the executable\n"
    "  --cc=COMPILER                     linker for build/run (default: $RIFT_CC, else cc)\n"
    "  --all-functions                   generate every function, not only those main reaches (testing)\n"
    "  -h, --help                        show this help\n"
    "  -V, --version                     show the compiler version\n"
    "\n"
    "exit codes: 0 success, 1 errors in input, 2 bad command line, 3 internal error;\n"
    "            'run' exits with the program's own status (128 + signal if it crashed)\n";

enum class Phase { lex, parse, resolve, types, ownership };

struct Options {
    std::string_view command;
    std::string_view input;
    RenderOptions    render;
    bool             color_auto = true;
    std::string      root;
    Phase            stop_after = Phase::ownership;
    std::string      mode       = "debug";
    std::string      output;
    std::string      cc;
    bool             all_functions = false;   // --all-functions (testing the backend)
    std::vector<std::string> program_args;   // `run ... -- args`
};

constexpr int process_not_found = -1;

/// Runs a program and waits for it. Returns its exit status (128 + signal number if it was
/// killed), or process_not_found. With `captured`, stdout and stderr are collected there.
int run_process(const std::vector<std::string>& cmd, std::string* captured) {
    std::vector<char*> argv;
    for (const std::string& a : cmd) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int pipe_fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (captured) {
        if (pipe(pipe_fds) != 0) return process_not_found;
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }
    pid_t         pid = 0;
    int           rc  = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (captured) {
        close(pipe_fds[1]);
        if (rc == 0) {
            char    buf[4096];
            ssize_t n = 0;
            while ((n = read(pipe_fds[0], buf, sizeof buf)) > 0) captured->append(buf, static_cast<std::size_t>(n));
        }
        close(pipe_fds[0]);
    }
    if (rc != 0) return process_not_found;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return process_not_found;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

std::filesystem::path make_temp_dir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "riftc-run-XXXXXX").string();
    if (!mkdtemp(pattern.data())) return {};
    return pattern;
}

ExitCode usage_error(DriverStreams& s, std::string_view message) {
    s.err << "riftc: error: " << message << "\nRun 'riftc --help' for usage.\n";
    return ExitCode::usage_error;
}

/// Parses options after the command. Returns an exit code on failure.
std::optional<ExitCode> parse_options(std::span<const std::string_view> args, Options& opts,
                                      DriverStreams& s) {
    bool builds = opts.command == "build" || opts.command == "run" || opts.command == "emit-llvm";
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--" && opts.command == "run") {
            for (++i; i < args.size(); ++i) opts.program_args.emplace_back(args[i]);
            break;
        }
        if (builds && arg.starts_with("--mode=")) {
            opts.mode = std::string(arg.substr(7));
            if (opts.mode != "debug" && opts.mode != "release" && opts.mode != "release_with_debug_info") {
                return usage_error(s, std::format("unknown build mode '{}' (debug, release, release_with_debug_info; "
                                                  "user modes need build.rifts, which is not supported yet)",
                                                  opts.mode));
            }
            continue;
        }
        if (builds && (arg == "-o" || arg.starts_with("--output="))) {
            if (arg == "-o") {
                if (i + 1 >= args.size()) return usage_error(s, "'-o' needs a file name");
                opts.output = std::string(args[++i]);
            } else {
                opts.output = std::string(arg.substr(9));
            }
            continue;
        }
        if (builds && arg.starts_with("--cc=")) {
            opts.cc = std::string(arg.substr(5));
            continue;
        }
        if (builds && arg == "--all-functions") {
            opts.all_functions = true;
            continue;
        }
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
        } else if (arg == "--stop-after=ownership") {
            opts.stop_after = Phase::ownership;
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

/// The backend: LLVM IR text (emit-llvm), then an executable: the IR is compiled to an object file
/// for the host and linked with the system linker (--cc, RIFT_CC or cc).
ExitCode run_backend(const Options& opts, sema::Program& program, SourceManager& sources, DiagnosticEngine& diags,
                          DriverStreams& s) {
    std::string ir = generate_llvm_ir(program, sources, diags, LlvmOptions{opts.all_functions});
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    if (diags.has_errors()) return ExitCode::compile_error;
    if (opts.command == "emit-llvm") {
        s.out << ir;
        return ExitCode::success;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path        input(opts.input);
    fs::path        temp_dir;
    fs::path        exe;
    if (opts.command == "run") {
        temp_dir = make_temp_dir();
        if (temp_dir.empty()) {
            s.err << "riftc: error[E0004]: cannot create a temporary directory\n";
            return ExitCode::compile_error;
        }
        exe = temp_dir / input.stem();
    } else if (opts.output.empty()) {
        fs::path root = opts.root.empty() ? input.parent_path() : fs::path(opts.root);
        exe           = root / "build" / opts.mode / input.stem();
    } else {
        exe = opts.output;
    }
    if (!exe.parent_path().empty()) fs::create_directories(exe.parent_path(), ec);

    std::string linker = opts.cc;
    if (linker.empty()) {
        const char* env = std::getenv("RIFT_CC");
        linker          = env && *env ? env : "cc";
    }
    std::string error;
    if (!build_executable(ir, exe.string(), linker, error)) {
        s.err << std::format("riftc: internal compiler error: {}\n", error);
        if (!temp_dir.empty()) fs::remove_all(temp_dir, ec);
        return ExitCode::internal_error;
    }
    if (opts.command == "build") {
        s.out << std::format("built '{}' ({})\n", exe.string(), opts.mode);
        return ExitCode::success;
    }
    s.out.flush();
    s.err.flush();
    std::vector<std::string> run_cmd = {exe.string()};
    run_cmd.insert(run_cmd.end(), opts.program_args.begin(), opts.program_args.end());
    int program_status = run_process(run_cmd, nullptr);
    fs::remove_all(temp_dir, ec);
    return static_cast<ExitCode>(program_status);
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
        check_types(program, sources, diags);
        typed = true;
    }
    if (!diags.has_errors() && opts.stop_after >= Phase::ownership) {
        check_ownership(program, diags);
    }
    if ((opts.command == "build" || opts.command == "run" || opts.command == "emit-llvm") && !diags.has_errors()) {
        return run_backend(opts, program, sources, diags, s);
    }
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
    if (command == "lex" || command == "parse" || command == "check" || command == "symbols" || command == "typed" ||
        command == "build" || command == "run" || command == "emit-c" || command == "emit-llvm") {
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


#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <filesystem>
#include <cstdlib>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h>
#else
    #include <io.h>
#endif

#include "rift/driver.h"
#include "rift/diagnostic_engine.h"
#include "rift/lexer.h"
#include "rift/source.h"
#include "rift/token.h"
#include "rift/version.h"
#include "rift/ast.h"
#include "rift/ast_dump.h"
#include "rift/parser.h"



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    enum class color_mode {
        auto_,
        always,
        never
    };

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    void print_usage(std::ostream& os);


    int cmd_help(std::span<const std::string_view>);


    int cmd_version(std::span<const std::string_view>);


    std::optional<std::string> read_source(std::string_view path);


    int cmd_lex(std::span<const std::string_view> args, const diagnostic_style& style);


    int cmd_parse(std::span<const std::string_view> args, const diagnostic_style& style);

    
    int cmd_dump_ast(std::span<const std::string_view> args, const diagnostic_style& style);


    int cmd_compile(std::span<const std::string_view> args);


    bool parse_color_value(std::string_view s, color_mode& out);


    bool stderr_is_tty();


    bool resolve_use_color(color_mode m);


    // Strips --color=<mode> / --color <mode> from args, filling `mode`.
    // Returns false with a message on error.
    bool extract_color_option(std::span<const std::string_view> in, color_mode& mode, std::vector<std::string_view>& out, 
        std::string& error_msg);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    void print_usage(std::ostream& os) {

        os << "Usage: riftc <command> [options]\n"
            "\n"
            "Commands:\n"
            "  compile <file.rfts> -o <out>  Compile a Rift source file\n"
            "  lex <file.rfts>               Lex and dump the token stream\n"
            "  parse <file.rfts>             Parse (silent on success)\n"
            "  dump-ast <file.rfts>          Parse and pretty-print the AST\n"
            "  version                       Print version information\n"
            "  help                          Print this help message\n"
            "\n"
            "Options:\n"
            "  -h, --help                    Print this help message\n"
            "  -V, --version                 Print version information\n"
            "  --color=<mode>                Colorize diagnostics: auto|always|never (default: auto)\n";
    }


    int cmd_help(std::span<const std::string_view>) {

        print_usage(std::cout);
        return static_cast<int>(exit_code::ok);
    }


    int cmd_version(std::span<const std::string_view>) {

        std::cout << format_version() << '\n';
        return static_cast<int>(exit_code::ok);
    }


    // Reads an entire file, or stdin if path is "-"
    std::optional<std::string> read_source(std::string_view path) {

        if (path == "-") {
            std::string s((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
            return s;
        }

        std::ifstream in_stream(std::string(path), std::ios::binary);
        if (!in_stream)
            return std::nullopt;
        return std::string((std::istreambuf_iterator<char>(in_stream)), std::istreambuf_iterator<char>());
    }


    int cmd_lex(std::span<const std::string_view> args, const diagnostic_style& style) {

        std::string_view input;
        for (const auto arg : args) {
            if (arg.starts_with('-') && arg != "-") {
                std::cerr << "riftc: error: unknown option '" << arg << "'\n";
                return static_cast<int>(exit_code::usage_error);
            }
            if (!input.empty()) {
                std::cerr << "riftc: error: multiple input files given\n";
                return static_cast<int>(exit_code::usage_error);
            }
            input = arg;
        }
        if (input.empty()) {
            std::cerr << "riftc: error: 'lex' requires an input file\n";
            return static_cast<int>(exit_code::usage_error);
        }

        auto content = read_source(input);
        if (!content) {
            std::cerr << "riftc: error: input file '" << input << "' not found\n";
            return static_cast<int>(exit_code::usage_error);
        }

        source_manager sm;
        diagnostic_engine diag;
        const file_id file = sm.add(std::string(input), std::move(*content));
        lexer lex(sm, file, diag);
        const auto tokens = lex.tokenize();

        if (diag.has_errors()) {
            diag.print_all(std::cerr, sm, style);
            return static_cast<int>(exit_code::usage_error);
        }

        const auto& src = sm.get(file);
        for (const auto& tok : tokens) {
            const auto s = src.resolve(tok.span.begin);
            const auto e = src.resolve(tok.span.end);
            std::cout << std::format("{}:{}-{}:{}\t{}\t{}\n", s.line, s.column, e.line, e.column, token_kind_name(tok.kind), tok.lexeme);
        }
        return static_cast<int>(exit_code::ok);
    }


    int cmd_parse(std::span<const std::string_view> args, const diagnostic_style& style) {

        std::string_view input;
        for (const auto arg : args) {
            if (arg.starts_with('-') && arg != "-") {
                std::cerr << "riftc: error: unknown option '" << arg << "'\n";
                return static_cast<int>(exit_code::usage_error);
            }
            if (!input.empty()) {
                std::cerr << "riftc: error: multiple input files given\n";
                return static_cast<int>(exit_code::usage_error);
            }
            input = arg;
        }
        if (input.empty()) {
            std::cerr << "riftc: error: 'parse' requires an input file\n";
            return static_cast<int>(exit_code::usage_error);
        }

        auto content = read_source(input);
        if (!content) {
            std::cerr << "riftc: error: input file '" << input << "' not found\n";
            return static_cast<int>(exit_code::usage_error);
        }

        source_manager sm;
        diagnostic_engine diag;
        const file_id file = sm.add(std::string(input), std::move(*content));
        lexer lex(sm, file, diag);
        auto tokens = lex.tokenize();
        if (diag.has_errors()) {
            diag.print_all(std::cerr, sm, style);
            return static_cast<int>(exit_code::usage_error);
        }

        parser p(sm, file, diag, std::move(tokens));
        (void)p.parse_program();

        if (diag.has_errors()) {
            diag.print_all(std::cerr, sm, style);
            return static_cast<int>(exit_code::usage_error);
        }
        return static_cast<int>(exit_code::ok);
    }


    int cmd_dump_ast(std::span<const std::string_view> args, const diagnostic_style& style) {

        std::string_view input;
        for (const auto arg : args) {
            if (arg.starts_with('-') && arg != "-") {
                std::cerr << "riftc: error: unknown option '" << arg << "'\n";
                return static_cast<int>(exit_code::usage_error);
            }
            if (!input.empty()) {
                std::cerr << "riftc: error: multiple input files given\n";
                return static_cast<int>(exit_code::usage_error);
            }
            input = arg;
        }
        if (input.empty()) {
            std::cerr << "riftc: error: 'dump-ast' requires an input file\n";
            return static_cast<int>(exit_code::usage_error);
        }

        auto content = read_source(input);
        if (!content) {
            std::cerr << "riftc: error: input file '" << input << "' not found\n";
            return static_cast<int>(exit_code::usage_error);
        }

        source_manager sm;
        diagnostic_engine diag;
        const file_id file = sm.add(std::string(input), std::move(*content));
        lexer lex(sm, file, diag);
        auto tokens = lex.tokenize();
        if (diag.has_errors()) {
            diag.print_all(std::cerr, sm, style);
            return static_cast<int>(exit_code::usage_error);
        }

        parser p(sm, file, diag, std::move(tokens));
        const node_id program = p.parse_program();

        dump_ast(p.arena(), program, std::cout);

        if (diag.has_errors()) {
            diag.print_all(std::cerr, sm, style);
            return static_cast<int>(exit_code::usage_error);
        }
        return static_cast<int>(exit_code::ok);
    }


    int cmd_compile(std::span<const std::string_view> args) {
        
        std::string_view input;
        std::string_view output;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (arg == "-o" || arg == "--output") {
                if (i + 1 >= args.size()) {
                    std::cerr << "riftc: error: '" << arg << "' requires an argument\n";
                    return static_cast<int>(exit_code::usage_error);
                }
                output = args[++i];
            } else if (arg.starts_with('-') && arg != "-") {
                std::cerr << "riftc: error: unknown option '" << arg << "'\n";
                return static_cast<int>(exit_code::usage_error);
            } else {
                if (!input.empty()) {
                    std::cerr << "riftc: error: multiple input files given\n";
                    return static_cast<int>(exit_code::usage_error);
                }
                input = arg;
            }
        }

        if (input.empty()) {
            std::cerr << "riftc: error: no input file given\n\n";
            print_usage(std::cerr);
            return static_cast<int>(exit_code::usage_error);
        }

        auto content = read_source(input);
        if (!content) {
            std::cerr << "riftc: error: input file '" << input << "' not found\n";
            return static_cast<int>(exit_code::usage_error);
        }

        std::cerr << "riftc: 'compile' is not implemented yet (M1 — lexer only).\n       Try `riftc lex " << input << "` to dump tokens.\n";
        if (!output.empty()) {
            std::cerr << "       Output: " << output << '\n';
        }
        return static_cast<int>(exit_code::not_implemented);
    }


    bool parse_color_value(std::string_view s, color_mode& out) {
        if (s == "auto")    {
            out = color_mode::auto_;
            return true;
        }
        if (s == "always")  {
            out = color_mode::always;
            return true;
        }
        if (s == "never")   {
            out = color_mode::never;
            return true;
        }
        return false;
    }


    bool stderr_is_tty() {

        #if defined(_WIN32)
            return ::_isatty(::_fileno(stderr)) != 0;
        #else
            return ::isatty(::fileno(stderr)) != 0;
        #endif
    }


    bool resolve_use_color(color_mode m) {

        // https://no-color.org/  — NO_COLOR (non-empty) wins over everything.
        if (const char* no_color = std::getenv("NO_COLOR"); no_color != nullptr && no_color[0] != '\0')
            return false;

        // https://bixense.com/clicolors/ — CLICOLOR_FORCE (non-empty, non-"0") forces color.
        if (const char* force = std::getenv("CLICOLOR_FORCE"); force != nullptr && force[0] != '\0' && force[0] != '0')
            return true;

        switch (m) {
            case color_mode::always: return true;
            case color_mode::never:  return false;
            case color_mode::auto_:  return stderr_is_tty();
        }
        return false;
    }


    bool extract_color_option(std::span<const std::string_view> in, color_mode& mode, std::vector<std::string_view>& out, 
        std::string& error_msg) {

        for (std::size_t i = 0; i < in.size(); ++i) {
            const auto a = in[i];
            std::string_view value;
            if (a == "--color") {
                if (i + 1 >= in.size()) {
                    error_msg = "--color requires an argument (auto|always|never)";
                    return false;
                }
                value = in[++i];
            } else if (a.starts_with("--color=")) {
                value = a.substr(8);
            } else {
                out.push_back(a);
                continue;
            }
            if (!parse_color_value(value, mode)) {
                error_msg = "invalid --color value '" + std::string(value) + "' (expected auto|always|never)";
                return false;
            }
        }
        return true;
    }

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    int run_cli(std::span<const std::string_view> args) {

        if (args.empty()) {
            std::cerr << "riftc: error: no command given\n\n";
            print_usage(std::cerr);
            return static_cast<int>(exit_code::usage_error);
        }

        // Extract --color=… before dispatching, so it can appear anywhere on the command line (riftc --color=always
        // parse foo.rfts, or riftc parse foo.rfts --color=always)
        color_mode color = color_mode::auto_;
        std::vector<std::string_view> filtered;
        std::string color_err;
        if (!extract_color_option(args, color, filtered, color_err)) {
            std::cerr << "riftc: error: " << color_err << '\n';
            return static_cast<int>(exit_code::usage_error);
        }

        const diagnostic_style style{ resolve_use_color(color) };

        if (filtered.empty()) {
            std::cerr << "riftc: error: no command given\n\n";
            print_usage(std::cerr);
            return static_cast<int>(exit_code::usage_error);
        }

        const std::string_view first = filtered[0];
        const auto rest = std::span<const std::string_view>(filtered).subspan(1);

        if (first == "-h" || first == "--help" || first == "help")
            return cmd_help(rest);

        if (first == "-V" || first == "--version" || first == "version")
            return cmd_version(rest);

        if (first == "compile")
            return cmd_compile(rest);

        if (first == "lex")
            return cmd_lex(rest, style);

        if (first == "parse")
            return cmd_parse(rest, style);

        if (first == "dump-ast")
            return cmd_dump_ast(rest, style);

        std::cerr << "riftc: error: unknown command '" << first << "'\n\n";
        print_usage(std::cerr);
        return static_cast<int>(exit_code::usage_error);
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

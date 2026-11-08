
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <filesystem>

#include "rift/driver.h"
#include "rift/diagnostic_engine.h"
#include "rift/lexer.h"
#include "rift/source.h"
#include "rift/token.h"
#include "rift/version.h"



// FORWARD DECLARATIONS ================================================================================================

namespace rift {

    // CONSTANTS =======================================================================================================

    // MACROS ==========================================================================================================

    // TYPES ===========================================================================================================

    // STATIC VARIABLES ================================================================================================

    // INTERNAL TEMPLATE DECLARATION ===================================================================================

    // INTERNAL FUNCTION DECLARATION ===================================================================================

    void print_usage(std::ostream& os);


    int cmd_help(std::span<const std::string_view>);


    int cmd_version(std::span<const std::string_view>);


    std::optional<std::string> read_source(std::string_view path);


    int cmd_lex(std::span<const std::string_view> args);


    int cmd_compile(std::span<const std::string_view> args);

    // INTERNAL TEMPLATE IMPLEMENTATION ================================================================================

    // INTERNAL FUNCTION IMPLEMENTATION ================================================================================

    void print_usage(std::ostream& os) {

        os << "Usage: riftc <command> [options]\n"
            "\n"
            "Commands:\n"
            "  compile <file.rfts> -o <out>  Compile a Rift source file\n"
            "  lex <file.rfts>               Lex and dump the token stream\n"
            "  version                       Print version information\n"
            "  help                          Print this help message\n"
            "\n"
            "Options:\n"
            "  -h, --help                    Print this help message\n"
            "  -V, --version                 Print version information\n";
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


    int cmd_lex(std::span<const std::string_view> args) {
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
            diag.print_all(std::cerr, sm);
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

    // TEMPLATE IMPLEMENTATION =========================================================================================

    // FUNCTION IMPLEMENTATION =========================================================================================

    int run_cli(std::span<const std::string_view> args) {

        if (args.empty()) {
            std::cerr << "riftc: error: no command given\n\n";
            print_usage(std::cerr);
            return static_cast<int>(exit_code::usage_error);
        }

        const std::string_view first = args[0];
        if (first == "-h" || first == "--help" || first == "help")
            return cmd_help(args.subspan(1));

        if (first == "-V" || first == "--version" || first == "version")
            return cmd_version(args.subspan(1));

        if (first == "compile")
            return cmd_compile(args.subspan(1));

        if (first == "lex")
            return cmd_lex(args.subspan(1));

        std::cerr << "riftc: error: unknown command '" << first << "'\n\n";
        print_usage(std::cerr);
        return static_cast<int>(exit_code::usage_error);
    }

    // CLASS IMPLEMENTATION ============================================================================================

    // CLASS PUBLIC ====================================================================================================

    // CLASS PROTECTED =================================================================================================

    // CLASS PRIVATE ===================================================================================================

}

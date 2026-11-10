#pragma once
// Test helper: a throw-away project directory that is loaded, resolved and (optionally) type
// checked like `riftc check` does.

#include "rift/check.hpp"
#include "rift/modules.hpp"
#include "rift/resolve.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace rift_test {

enum class Stop { resolve, types };

struct Project {
    std::filesystem::path  root;
    rift::SourceManager    sources;
    rift::DiagnosticEngine diags;
    rift::sema::Program    program;

    explicit Project(const std::map<std::string, std::string>& files, Stop stop = Stop::types,
                     std::string entry = "main.rifts") {
        static int counter = 0;
        root = std::filesystem::temp_directory_path() / ("rift_unit_project_" + std::to_string(++counter));
        std::filesystem::remove_all(root);
        for (const auto& [name, text] : files) {
            std::filesystem::path p = root / name;
            std::filesystem::create_directories(p.parent_path());
            std::ofstream(p) << text;
        }
        program = rift::load_program((root / entry).string(), rift::LoadOptions{root.string()}, sources, diags);
        if (!diags.has_errors()) rift::resolve_program(program, diags);
        if (!diags.has_errors() && stop == Stop::types) rift::check_types(program, diags);
    }
    ~Project() { std::filesystem::remove_all(root); }
    Project(const Project&)            = delete;
    Project& operator=(const Project&) = delete;

    std::string codes() const {
        std::string out;
        for (const rift::Diagnostic& d : diags.all()) {
            out += (out.empty() ? "" : " ") + std::string(rift::code_name(d.code));
        }
        return out;
    }

    rift::sema::Module& module(const std::string& name) { return *program.modules.at(name); }

    std::string symbols(const std::string& name) {
        std::ostringstream out;
        rift::print_symbols(out, module(name), sources);
        return out.str();
    }
};

} // namespace rift_test

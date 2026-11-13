#include "rift/modules.hpp"

#include "rift/ast_printer.hpp"
#include "rift/lexer.hpp"
#include "rift/parser.hpp"
#include "rift/std_library.hpp"

#include <filesystem>
#include <format>
#include <map>
#include <system_error>

namespace rift {

namespace fs = std::filesystem;

namespace {

std::string display_path(const fs::path& p) {
    std::string s = p.lexically_normal().generic_string();
    return s.empty() ? "." : s;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string root_or_default(const std::string& entry, const std::string& root) {
    if (!root.empty()) {
        return root;
    }
    fs::path parent = fs::path(entry).parent_path();
    return parent.empty() ? "." : parent.generic_string();
}

class Loader {
public:
    Loader(std::string root, SourceManager& sources, DiagnosticEngine& diags)
        : root_(std::move(root)), sources_(sources), diags_(diags) {}

    sema::Program run(const std::string& entry) {
        fs::path    entry_path(entry);
        std::string name = module_name_for(entry, root_);
        auto        module = std::make_unique<sema::Module>();
        module->name       = name;

        fs::path header = entry_path, source = entry_path;
        header.replace_extension(".rifth");
        source.replace_extension(".rifts");
        if (entry_path.extension() == ".rifth" || is_file(header)) {
            load_file(*module, header, /*is_header=*/true);
        }
        if (entry_path.extension() == ".rifts" || is_file(source)) {
            load_file(*module, source, /*is_header=*/false);
        }

        sema::Module* raw = module.get();
        program_.modules.emplace(name, std::move(module));
        program_.entry = raw;
        state_[name]   = State::visiting;
        stack_.push_back(name);
        process_imports(*raw);
        stack_.pop_back();
        state_[name] = State::done;
        return std::move(program_);
    }

private:
    enum class State { visiting, done };

    void load_file(sema::Module& module, const fs::path& path, bool is_header) {
        std::string           reason;
        std::optional<FileId> id = sources_.load(display_path(path), reason);
        if (!id) {
            return;   // the driver already checked the entry file; imports are checked before
        }
        parse_into(module, *id, is_header);
    }

    void parse_into(sema::Module& module, FileId id_value, bool is_header) {
        std::optional<FileId> id = id_value;
        sema::ModuleFile file{*id, {}};
        file.ast.file = *id;
        std::size_t errors_before = diags_.error_count();
        std::vector<Token> tokens = tokenize(sources_.file(*id), *id, diags_);
        if (diags_.error_count() == errors_before) {
            file.ast = parse(sources_.file(*id), *id, tokens, diags_);
        }
        (is_header ? module.header : module.source) = std::move(file);
    }

    static std::vector<const ast::ImportDecl*> imports_of(const std::optional<sema::ModuleFile>& file) {
        std::vector<const ast::ImportDecl*> out;
        if (file) {
            for (const ast::DeclPtr& d : file->ast.decls) {
                if (d->kind == ast::DeclKind::import) {
                    out.push_back(&d->as<ast::ImportDecl>());
                }
            }
        }
        return out;
    }

    void process_imports(sema::Module& module) {
        for (int pass = 0; pass < 2; ++pass) {
            bool header = pass == 0;
            for (const ast::ImportDecl* imp : imports_of(header ? module.header : module.source)) {
                std::string name = ast::to_string(imp->path);
                if (imp->path.segments.front().name == "std") {
                    (header ? module.header_imports_std : module.source_imports_std) = true;
                    load_std();
                    continue;
                }
                if (name == module.name) {
                    diags_.report(DiagCode::E3004, imp->span,
                                  std::format("module '{}' imports itself", name), "remove this import")
                        .notes.push_back(Note{"a .rifts file sees its own .rifth automatically", std::nullopt});
                    continue;
                }
                if (sema::Module* target = load_module(name, *imp)) {
                    (header ? module.header_imports : module.source_imports).push_back(target);
                }
            }
        }
    }

    /// The standard library is built into riftc (library/std/std.rifth) and loaded once.
    void load_std() {
        if (program_.std_module) return;
        auto module  = std::make_unique<sema::Module>();
        module->name = "std";
        parse_into(*module, sources_.add(std::string(std_header_path), std::string(std_header_text())), true);
        program_.std_module = module.get();
        program_.modules.emplace("std", std::move(module));
        state_["std"] = State::done;
    }

    sema::Module* load_module(const std::string& name, const ast::ImportDecl& imp) {
        if (auto it = program_.modules.find(name); it != program_.modules.end()) {
            if (state_[name] == State::visiting) {
                std::string chain;
                bool        in_cycle = false;
                for (const std::string& m : stack_) {
                    in_cycle = in_cycle || m == name;
                    if (in_cycle) {
                        chain += m + " -> ";
                    }
                }
                diags_.report(DiagCode::E3002, imp.span, std::format("import cycle: {}{}", chain, name),
                              "this import closes the cycle")
                    .notes.push_back(Note{"module imports must form an acyclic graph, counting the "
                                          "imports of both .rifth and .rifts files",
                                          std::nullopt});
                return nullptr;
            }
            return it->second.get();
        }

        fs::path rel;
        for (const ast::PathSegment& seg : imp.path.segments) {
            rel /= seg.name;
        }
        fs::path header = fs::path(root_) / rel;
        header += ".rifth";
        fs::path source = fs::path(root_) / rel;
        source += ".rifts";
        if (!is_file(header)) {
            if (is_file(source)) {
                diags_.report(DiagCode::E3003, imp.path.span,
                              std::format("module '{}' has no header and cannot be imported", name),
                              std::format("found {} but no {}", display_path(source), display_path(header)))
                    .notes.push_back(Note{"a .rifts without a .rifth is a standalone file, like a program's main",
                                          std::nullopt});
            } else {
                diags_.report(DiagCode::E3001, imp.path.span, std::format("cannot find module '{}'", name),
                              std::format("looked for {}", display_path(header)));
            }
            return nullptr;
        }

        auto module  = std::make_unique<sema::Module>();
        module->name = name;
        load_file(*module, header, true);
        if (is_file(source)) {
            load_file(*module, source, false);
        }
        sema::Module* raw = module.get();
        program_.modules.emplace(name, std::move(module));
        state_[name] = State::visiting;
        stack_.push_back(name);
        process_imports(*raw);
        stack_.pop_back();
        state_[name] = State::done;
        return raw;
    }

    std::string                   root_;
    SourceManager&                sources_;
    DiagnosticEngine&             diags_;
    sema::Program                 program_;
    std::map<std::string, State>  state_;
    std::vector<std::string>      stack_;
};

} // namespace

std::string module_name_for(const std::string& path, const std::string& root) {
    fs::path rel = fs::path(path).lexically_normal().lexically_relative(fs::path(root).lexically_normal());
    if (rel.empty() || rel.generic_string().starts_with("..")) {
        rel = fs::path(path).filename();
    }
    rel.replace_extension();
    std::string name;
    for (const fs::path& part : rel) {
        if (part == ".") {
            continue;
        }
        name += (name.empty() ? "" : "::") + part.string();
    }
    return name;
}

sema::Program load_program(const std::string& entry_path, const LoadOptions& options,
                           SourceManager& sources, DiagnosticEngine& diags) {
    return Loader(root_or_default(entry_path, options.root), sources, diags).run(entry_path);
}

} // namespace rift

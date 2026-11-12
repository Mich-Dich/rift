#include "rift/llvm_backend.hpp"

#include "rift/ast_printer.hpp"
#include "rift/types.hpp"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace rift {
namespace {

using namespace ast;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;

/// The lines of `text`, each with its newline.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t                   start = 0;
    while (start < text.size()) {
        std::size_t end  = text.find('\n', start);
        std::size_t next = end == std::string_view::npos ? text.size() : end + 1;
        out.push_back(text.substr(start, next - start));
        start = next;
    }
    return out;
}

/// The data layout of the host, which decides the sizes and alignments of enum payloads.
llvm::DataLayout host_data_layout() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::string triple = llvm::sys::getDefaultTargetTriple();
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) llvm::report_fatal_error("internal error: no LLVM target for the host: " + llvm::Twine(error));
    std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple, "generic", "", llvm::TargetOptions(), llvm::Reloc::PIC_));
    return machine->createDataLayout();
}

// --- names ----------------------------------------------------------------------------------

/// Length-prefixed name parts, as in the C backend: `r_3geo6shapes1f`.
std::string mangle_parts(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) out += std::to_string(p.size()) + p;
    return out;
}

std::vector<std::string> split_module(const std::string& name) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (true) {
        std::size_t pos = name.find("::", start);
        out.push_back(name.substr(start, pos - start));
        if (pos == std::string::npos) break;
        start = pos + 2;
    }
    return out;
}

/// Thrown after an E6001 was reported for a construct the backend does not handle yet. The
/// function being generated is abandoned; the module is discarded because errors were reported.
struct Unsupported {};

class Gen {
public:
    Gen(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags, LlvmOptions options,
        llvm::LLVMContext& ctx)
        : program_(program), tc_(*program.types), sources_(sources), diags_(diags), options_(options), ctx_(ctx),
          mod_(std::make_unique<llvm::Module>("rift", ctx)), b_(ctx) {
        mod_->setDataLayout(host_data_layout());
    }

    std::unique_ptr<llvm::Module> run() {
        try {
            return build();
        } catch (const Unsupported&) {
            return nullptr;   // reported at the construct that threw
        }
    }

private:
    std::unique_ptr<llvm::Module> build() {
        Symbol* main = options_.all_functions ? find_main_quietly() : find_main();
        if (!main && !options_.all_functions) return nullptr;
        declare_runtime();
        if (main) rift_main_ = function_of(main);
        if (options_.all_functions) {
            for (auto& [name, m] : program_.modules) {
                for (Symbol* s : m->item_order) {
                    if (s->kind == SymbolKind::function && !s->function()->is_template) function_of(s);
                }
            }
        }
        while (!queue_.empty() || !global_queue_.empty() || !drop_queue_.empty()) {
            while (!drop_queue_.empty()) {
                const Symbol* d = drop_queue_.front();
                drop_queue_.pop_front();
                gen_drop(d);
            }
            while (!queue_.empty()) {
                const Symbol* s = queue_.front();
                queue_.pop_front();
                gen_function(s);
            }
            while (!global_queue_.empty()) {
                const Symbol* g = global_queue_.front();
                global_queue_.pop_front();
                gen_global(g);
            }
        }
        if (main) {
            emit_entry(main);
        } else {
            // Without a `main` (only with all_functions) the module still links: `main` returns 0.
            llvm::Function* entry = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx_), {}, false),
                                                           llvm::Function::ExternalLinkage, "main", mod_.get());
            b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", entry));
            b_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
        }
        if (diags_.has_errors()) return nullptr;
        return std::move(mod_);
    }

    // --- diagnostics ------------------------------------------------------------------------

    [[noreturn]] void unsupported(Span span, const std::string& what, const std::string& why = {}) {
        if (reported_.insert({span.file, span.begin, what}).second) {
            Diagnostic& d = diags_.report(DiagCode::E6001, span, std::format("code generation does not support {} yet", what),
                                          "not supported yet");
            if (!why.empty()) d.notes.push_back(Note{why, std::nullopt});
        }
        throw Unsupported{};
    }

    /// A private constant holding `text` (NUL-terminated); its address is a `ptr`.
    llvm::Constant* string_global(const std::string& text) {
        llvm::Constant* init = llvm::ConstantDataArray::getString(ctx_, text);
        return new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, ".str");
    }

    // --- program structure ------------------------------------------------------------------

    static const FunctionDecl* body_decl(const Symbol* s) {
        if (s->definition && s->definition->kind == DeclKind::function) return &s->definition->as<FunctionDecl>();
        return s->function();
    }

    Symbol* find_main_quietly() {
        if (!program_.entry) return nullptr;
        auto it = program_.entry->items.find("main");
        if (it == program_.entry->items.end()) return nullptr;
        for (Symbol* s : it->second) {
            const FunctionDecl* f = s->kind == SymbolKind::function ? body_decl(s) : nullptr;
            if (f && f->params.empty()) return s;
        }
        return nullptr;
    }

    Symbol* find_main() {
        Module* entry = program_.entry;
        if (entry) {
            if (auto it = entry->items.find("main"); it != entry->items.end()) {
                for (Symbol* s : it->second) {
                    if (s->kind == SymbolKind::function) {
                        const FunctionDecl* f = body_decl(s);
                        if (f && !f->params.empty()) {
                            unsupported(f->params.front().span, "parameters of 'main'");
                        }
                        return s;
                    }
                }
            }
        }
        Span where{};
        if (entry && entry->source) where = Span{entry->source->id, 0, 0};
        else if (entry && entry->header) where = Span{entry->header->id, 0, 0};
        diags_.report(DiagCode::E6002, where, "the program has no 'main' function", "no 'main'")
            .notes.push_back(Note{"a program starts at 'void main()' or 'u32 main()' in the entry file", std::nullopt});
        return nullptr;
    }

    /// Names as in the C backend: `r_<module parts><type><name>`, overloads numbered in order.
    std::string function_name(const Symbol* s) {
        std::vector<std::string> parts = split_module(s->module ? s->module->name : "");
        if (s->parent) {
            parts.push_back(s->parent->name);
            if (s->parent->template_of) parts.push_back("0i" + std::to_string(s->parent->instance_index));
        }
        parts.push_back(s->name == "~" + (s->parent ? s->parent->name : "") ? "drop" : s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
        std::string name = "r_" + mangle_parts(parts);
        const auto& table = s->parent ? s->parent->members : s->module->items;
        if (auto it = table.find(s->name); it != table.end() && it->second.size() > 1) {
            auto pos = std::find(it->second.begin(), it->second.end(), s);
            name += std::format("_{}", pos - it->second.begin());
        }
        return name;
    }

    /// The LLVM function for `s`, declared on first use; its body is generated later.
    llvm::Function* function_of(const Symbol* s) {
        if (auto it = fns_.find(s); it != fns_.end()) return it->second;
        const FunctionDecl* f = body_decl(s);
        if (!f) unsupported(s->span, "this kind of function");
        if (f->is_template && !s->template_of) unsupported(s->span, "calling templates");
        if (f->is_extern) unsupported(s->span, "extern functions");
        if (!f->body) unsupported(s->span, "functions without a body");
        if (f->result && f->result->kind == TypeKind::deduce) unsupported(f->result->span, "'deduce' return types");
        const auto* subst = sema::substitution_of(s);
        std::vector<llvm::Type*> params;
        if (has_self(s)) params.push_back(llvm::PointerType::get(ctx_, 0));
        for (const Param& p : f->params) {
            cur_ = p.span;
            params.push_back(lower_type(tc_.lower(*p.type, subst)));
        }
        cur_ = s->span;
        llvm::Type* ret = f->result ? lower_type(tc_.lower(*f->result, subst)) : llvm::Type::getVoidTy(ctx_);
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(ret, params, false), llvm::Function::ExternalLinkage,
                                          function_name(s), mod_.get());
        fns_[s] = fn;
        queue_.push_back(s);
        return fn;
    }

    // --- types ------------------------------------------------------------------------------

    /// The LLVM type of a value of Rift type `t`. Booleans are `i1` in values; they are stored as
    /// `i8` in memory once structs are lowered (see the interop layout notes).
    llvm::Type* lower_type(const types::Type* t) {
        if (!t) unsupported(cur_, "values of an unknown type");
        switch (t->kind) {
        case Kind::int_: return llvm::IntegerType::get(ctx_, static_cast<unsigned>(t->bits));
        case Kind::float_: return t->bits == 32 ? llvm::Type::getFloatTy(ctx_) : llvm::Type::getDoubleTy(ctx_);
        case Kind::bool_: return llvm::Type::getInt1Ty(ctx_);
        case Kind::char_: return llvm::Type::getInt32Ty(ctx_);
        case Kind::void_:
        case Kind::never: return llvm::Type::getVoidTy(ctx_);
        case Kind::reference:
        case Kind::pointer: return llvm::PointerType::get(ctx_, 0);
        case Kind::user: return user_type(t->symbol);
        case Kind::static_str: return str_type();
        case Kind::external:
            if (t->name == "std::str") return str_type();
            break;
        default: break;
        }
        unsupported(cur_, std::format("values of type '{}'", types::to_string(t)));
    }

    static bool is_void(const types::Type* t) { return !t || t->is(Kind::void_) || t->is(Kind::never); }

    static constexpr const char* no_std = "the standard library is not written yet";

    // --- functions --------------------------------------------------------------------------

    void gen_function(const Symbol* s) {
        const FunctionDecl* f  = body_decl(s);
        llvm::Function*     fn = fns_[s];
        cur_                   = s->span;
        try {
            cur_fn_ = fn;
            locals_.clear();
            loops_.clear();
            scopes_.clear();
            self_ = nullptr;
            const auto* subst = sema::substitution_of(s);
            result_           = f->result ? tc_.lower(*f->result, subst) : nullptr;
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
            b_.SetInsertPoint(entry);
            open_scope();
            unsigned offset = 0;
            if (has_self(s)) {
                self_   = fn->getArg(0);
                offset  = 1;
            }
            for (std::size_t i = 0; i < f->params.size(); ++i) {
                const Param& p = f->params[i];
                cur_           = p.span;
                llvm::Argument*   a    = fn->getArg(static_cast<unsigned>(i) + offset);
                llvm::AllocaInst* slot = entry_alloca(a->getType(), p.name);
                b_.CreateStore(a, slot);
                if (!p.symbol) continue;
                locals_[p.symbol] = slot;
                const types::Type* pt = tc_.lower(*p.type, subst);
                if (needs_drop(pt)) own(p.symbol, slot, pt, true);
            }
            llvm::Value* v = gen_block(*f->body, result_ && result_->is(Kind::reference));
            if (!terminated()) {
                destroy_from(0);
                if (fn->getReturnType()->isVoidTy()) b_.CreateRetVoid();
                else if (v) b_.CreateRet(v);
                else b_.CreateUnreachable();
            }
            scopes_.clear();
        } catch (const Unsupported&) {
            // Reported. The function keeps a partial body; the module is discarded.
        }
    }

    /// An alloca in the entry block, so that loops do not grow the stack.
    llvm::AllocaInst* entry_alloca(llvm::Type* t, const std::string& name) {
        llvm::BasicBlock& entry = cur_fn_->getEntryBlock();
        llvm::IRBuilder<> tmp(&entry, entry.begin());
        return tmp.CreateAlloca(t, nullptr, name);
    }

    bool terminated() const { return b_.GetInsertBlock()->getTerminator() != nullptr; }

    /// After a `return`, `break` or `continue`: following code goes into a block nothing jumps to.
    void open_dead_block() { b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "dead", cur_fn_)); }

    /// Panics with `message` at `span` unless `ok` holds (spec 16). `ok` is an i1.
    void check(llvm::Value* ok, Span span, const std::string& message) {
        llvm::BasicBlock* bad  = llvm::BasicBlock::Create(ctx_, "panic", cur_fn_);
        llvm::BasicBlock* good = llvm::BasicBlock::Create(ctx_, "ok", cur_fn_);
        b_.CreateCondBr(ok, good, bad);
        b_.SetInsertPoint(bad);
        b_.CreateCall(rift_panic_str_, {loc_global(span), message_bytes(message), llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), message.size())});
        b_.CreateUnreachable();
        b_.SetInsertPoint(good);
    }

    /// The source location `rift_loc` global for `span`: `{ file, line, column }`.
    llvm::Constant* loc_global(Span span) {
        auto key = std::make_pair(span.file, span.begin);
        if (auto it = locs_.find(key); it != locs_.end()) return it->second;
        const SourceFile& file = sources_.file(span.file);
        LineCol           lc   = file.line_col(span.begin);
        llvm::Constant*   path = string_global(file.path());
        llvm::Constant*   init = llvm::ConstantStruct::get(
            loc_type_, {path, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), lc.line),
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), lc.column)});
        auto* g = new llvm::GlobalVariable(*mod_, loc_type_, true, llvm::GlobalValue::PrivateLinkage, init,
                                           std::format("rift_loc_{}", locs_.size()));
        locs_[key] = g;
        return g;
    }

    // --- statements -------------------------------------------------------------------------

    /// A block: its locals are destroyed at its end. The value is the tail expression's, or its
    /// address when the function returns a reference.
    llvm::Value* gen_block(BlockExpr& blk, bool as_address = false) {
        open_scope();
        for (StmtPtr& st : blk.statements) gen_stmt(*st);
        llvm::Value* v = nullptr;
        if (blk.tail) {
            v = as_address ? object_address(*blk.tail) : gen_expr(*blk.tail);
            if (!terminated()) moved_out(*blk.tail);
        }
        close_scope();
        return v;
    }

    /// One statement. An unsupported construct is reported and the rest of the function still
    /// generates, so that every such construct gets its error.
    void gen_stmt(Stmt& s) {
        try {
            gen_stmt_inner(s);
        } catch (const Unsupported&) {
        }
    }

    void gen_stmt_inner(Stmt& s) {
        cur_ = s.span;
        switch (s.kind) {
        case StmtKind::variable: return variable_stmt(s.as<VariableStmt>());
        case StmtKind::assign: return assign_stmt(s.as<AssignStmt>());
        case StmtKind::discard: discard(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: discard(*s.as<ExprStmt>().expr); return;
        }
    }

    void variable_stmt(VariableStmt& v) {
        cur_ = v.name_span;
        if (v.storage != Storage::local) unsupported(v.name_span, "static and compile_time variables");
        if (!v.symbol || !v.symbol->type) unsupported(v.name_span, "variables of an unknown type");
        // The initializer first: if it is unsupported, that is the error to show (not its type).
        llvm::Value* init = nullptr;
        if (v.init) init = v.symbol->type->is(Kind::reference) ? object_address(*v.init) : gen_expr(*v.init);
        if (v.init && !v.symbol->type->is(Kind::reference)) moved_out(*v.init);
        llvm::AllocaInst* slot = entry_alloca(lower_type(v.symbol->type), v.name);
        locals_[v.symbol]      = slot;
        bool owns = needs_drop(v.symbol->type);
        if (owns) own(v.symbol, slot, v.symbol->type, false);
        if (init) b_.CreateStore(init, slot);
        if (owns && init) set_flag(v.symbol, true);
    }

    void assign_stmt(AssignStmt& a) {
        cur_ = a.span;
        if (!is_place(*a.target)) unsupported(a.target->span, "assignments to this place");
        const types::Type* dt = types::strip_ref(a.target->type);
        if (a.op == TokenKind::eq && needs_drop(dt)) {
            // The new value is computed first; then the old one is destroyed and replaced.
            llvm::Value* nv = gen_expr(*a.value);
            moved_out(*a.value);
            if (const OwnedLocal* o = owned_path(*a.target)) {
                drop_if(o->flag, o->drop, o->slot);
                b_.CreateStore(nv, o->slot);
                b_.CreateStore(llvm::ConstantInt::getTrue(ctx_), o->flag);
            } else {
                llvm::Value* addr = place(*a.target);
                b_.CreateCall(drop_function(dt), {addr});
                b_.CreateStore(nv, addr);
            }
            return;
        }
        llvm::Value*       addr = place(*a.target);
        const types::Type* t    = types::strip_ref(a.target->type);
        llvm::Value*       v    = nullptr;
        if (a.op == TokenKind::eq) {
            v = gen_expr(*a.value);
        } else {
            TokenKind op = compound_operator(a.op);
            if (op == TokenKind::error) unsupported(a.span, "this assignment operator");
            llvm::Value* cur = b_.CreateLoad(lower_type(t), addr);
            llvm::Value* rhs = gen_expr(*a.value);
            v = arith(op, t, cur, rhs, a.value->type, a.span);
        }
        b_.CreateStore(v, addr);
    }

    static TokenKind compound_operator(TokenKind op) {
        switch (op) {
        case TokenKind::plus_eq: return TokenKind::plus;
        case TokenKind::minus_eq: return TokenKind::minus;
        case TokenKind::star_eq: return TokenKind::star;
        case TokenKind::slash_eq: return TokenKind::slash;
        case TokenKind::percent_eq: return TokenKind::percent;
        case TokenKind::amp_eq: return TokenKind::amp;
        case TokenKind::pipe_eq: return TokenKind::pipe;
        case TokenKind::caret_eq: return TokenKind::caret;
        case TokenKind::shl_eq: return TokenKind::shl;
        case TokenKind::shr_eq: return TokenKind::shr;
        case TokenKind::plus_percent_eq: return TokenKind::plus_percent;
        case TokenKind::minus_percent_eq: return TokenKind::minus_percent;
        case TokenKind::star_percent_eq: return TokenKind::star_percent;
        default: return TokenKind::error;
        }
    }

    // --- expressions ------------------------------------------------------------------------

    /// The value of `e`, or null for an expression of type void or never.
    llvm::Value* gen_expr(Expr& e) {
        cur_ = e.span;
        switch (e.kind) {
        case ExprKind::int_literal: return int_literal(e.as<LiteralExpr>(), e.type);
        case ExprKind::float_literal: return float_literal(e.as<LiteralExpr>(), e.type);
        case ExprKind::bool_literal:
            return llvm::ConstantInt::getBool(ctx_, e.as<LiteralExpr>().bool_value);
        case ExprKind::char_literal: {
            const std::string& text = e.as<LiteralExpr>().value;
            char32_t           cp   = 0;
            if (text.empty() || decode_utf8(text, 0, cp) == 0) cp = text.empty() ? 0 : static_cast<unsigned char>(text[0]);
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), static_cast<std::uint64_t>(cp));
        }
        case ExprKind::path: return path_value(e);
        case ExprKind::member:
        case ExprKind::this_: return value_of_place(e);
        case ExprKind::struct_literal: return struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::lambda: unsupported(e.span, "lambdas");
        case ExprKind::match: return match_value(e.as<MatchExpr>(), e.type);
        case ExprKind::bit: return bit_value(e.as<BitExpr>(), e.type);
        case ExprKind::for_: return for_loop(e.as<ForExpr>());
        case ExprKind::cast: return cast_value(e.as<CastExpr>());
        case ExprKind::string_literal: {
            auto& lit = e.as<LiteralExpr>();
            if (lit.token_kind == TokenKind::format_string_literal) {
                unsupported(e.span, "f-strings outside 'std::print', 'std::println', 'std::eprintln' and 'std::panic'");
            }
            return str_constant(lit.value);
        }
        case ExprKind::default_: return default_value(types::strip_ref(e.type), e.span);
        case ExprKind::unary:
            if (e.as<UnaryExpr>().op == UnaryOp::deref) return value_of_place(e);
            return unary_value(e.as<UnaryExpr>(), e.type);
        case ExprKind::binary: return binary_value(e.as<BinaryExpr>());
        case ExprKind::call: return call_value(e.as<CallExpr>());
        case ExprKind::block: return gen_block(e.as<BlockExpr>());
        case ExprKind::if_: return if_value(e.as<IfExpr>(), e.type);
        case ExprKind::while_: return while_loop(e.as<WhileExpr>());
        case ExprKind::return_: return return_value(e.as<ReturnExpr>());
        case ExprKind::break_:
            if (loops_.empty()) unsupported(e.span, "'break' outside a loop");
            destroy_from(loop_scope());
            b_.CreateBr(loops_.back().exit);
            open_dead_block();
            return nullptr;
        case ExprKind::continue_:
            if (loops_.empty()) unsupported(e.span, "'continue' outside a loop");
            destroy_from(loop_scope());
            b_.CreateBr(loops_.back().cont);
            open_dead_block();
            return nullptr;
        default: unsupported(e.span, "this expression");
        }
    }

    llvm::Value* int_literal(const LiteralExpr& lit, const types::Type* t) {
        if (!t || !t->is_integer()) unsupported(lit.span, "integer literals of this type");
        std::uint64_t words[2] = {static_cast<std::uint64_t>(lit.int_value),
                                  static_cast<std::uint64_t>(lit.int_value >> 64)};
        llvm::APInt   value    = llvm::APInt(128, words).zextOrTrunc(static_cast<unsigned>(t->bits));
        return llvm::ConstantInt::get(ctx_, value);
    }

    llvm::Value* float_literal(const LiteralExpr& lit, const types::Type* t) {
        if (!t || !t->is_float()) unsupported(lit.span, "float literals of this type");
        std::string text;
        for (char c : lit.text) {
            if (c != '_') text += c;
        }
        return llvm::ConstantFP::get(lower_type(t), std::strtod(text.c_str(), nullptr));
    }

    llvm::Value* path_value(Expr& e) {
        const Symbol* sym = e.as<PathExpr>().symbol;
        if (!sym) unsupported(e.span, "this name");
        if (sym->kind == SymbolKind::alternative) {
            if (sym->alternative->payload) unsupported(e.span, "enum alternatives with a payload used without a value");
            return alternative_value(sym, nullptr);
        }
        if (sym->kind == SymbolKind::global) return value_of_place(e);
        if (sym->kind != SymbolKind::local || !locals_.count(sym)) {
            unsupported(e.span, "this name", "only local variables, parameters and enum alternatives are supported so far");
        }
        return value_of_place(e);
    }

    llvm::Value* unary_value(UnaryExpr& u, const types::Type* t) {
        if (u.op == UnaryOp::negate) {
            if (u.operand->kind == ExprKind::int_literal) {
                // `-128i8`: the literal is negated as a constant, so the minimum does not overflow.
                auto* lit = llvm::cast<llvm::ConstantInt>(int_literal(u.operand->as<LiteralExpr>(), t));
                return llvm::ConstantInt::get(ctx_, llvm::APInt(lit->getBitWidth(), 0) - lit->getValue());
            }
            llvm::Value* a = gen_expr(*u.operand);
            if (t && t->is_float()) return b_.CreateFNeg(a);
            if (!t || !t->is_integer()) unsupported(u.span, "'-' on this type");
            llvm::Value* minimum = llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(static_cast<unsigned>(t->bits)));
            check(b_.CreateICmpNE(a, minimum), u.span, std::format("integer overflow in '-' ({})", t->name));
            return b_.CreateSub(llvm::ConstantInt::get(a->getType(), 0), a);
        }
        if (u.op == UnaryOp::logical_not || u.op == UnaryOp::bit_not) return b_.CreateNot(gen_expr(*u.operand));
        if (u.op == UnaryOp::move || u.op == UnaryOp::mut_marker) return gen_expr(*u.operand);
        unsupported(u.span, "this unary operator");
    }

    llvm::Value* binary_value(BinaryExpr& bx) {
        if (bx.op == TokenKind::amp_amp || bx.op == TokenKind::pipe_pipe) return short_circuit(bx);
        const types::Type* lt = types::strip_ref(bx.lhs->type);
        llvm::Value*       a  = gen_expr(*bx.lhs);
        llvm::Value*       b  = gen_expr(*bx.rhs);
        switch (bx.op) {
        case TokenKind::eq_eq:
        case TokenKind::bang_eq: {
            bool eq = bx.op == TokenKind::eq_eq;
            if (lt && lt->is_float()) return eq ? b_.CreateFCmpOEQ(a, b) : b_.CreateFCmpUNE(a, b);
            if (is_str(lt)) return str_equal(a, b, eq);
            if (a->getType()->isIntegerTy()) return eq ? b_.CreateICmpEQ(a, b) : b_.CreateICmpNE(a, b);
            unsupported(bx.span, "comparing these values");
        }
        case TokenKind::less:
        case TokenKind::less_eq:
        case TokenKind::greater:
        case TokenKind::greater_eq: {
            if (lt && lt->is_float()) {
                switch (bx.op) {
                case TokenKind::less: return b_.CreateFCmpOLT(a, b);
                case TokenKind::less_eq: return b_.CreateFCmpOLE(a, b);
                case TokenKind::greater: return b_.CreateFCmpOGT(a, b);
                default: return b_.CreateFCmpOGE(a, b);
                }
            }
            if (!lt || !(lt->is_integer() || lt->is(Kind::char_))) unsupported(bx.span, "comparing these values");
            bool s = lt->is_integer() ? lt->is_signed : false;
            switch (bx.op) {
            case TokenKind::less: return s ? b_.CreateICmpSLT(a, b) : b_.CreateICmpULT(a, b);
            case TokenKind::less_eq: return s ? b_.CreateICmpSLE(a, b) : b_.CreateICmpULE(a, b);
            case TokenKind::greater: return s ? b_.CreateICmpSGT(a, b) : b_.CreateICmpUGT(a, b);
            default: return s ? b_.CreateICmpSGE(a, b) : b_.CreateICmpUGE(a, b);
            }
        }
        default: return arith(bx.op, lt, a, b, bx.rhs->type, bx.span);
        }
    }

    /// `&&` and `||`: the right operand is evaluated only when needed.
    llvm::Value* short_circuit(BinaryExpr& bx) {
        bool and_op = bx.op == TokenKind::amp_amp;
        llvm::Value* a = gen_expr(*bx.lhs);
        llvm::BasicBlock* from   = b_.GetInsertBlock();
        llvm::BasicBlock* rhs_bb = llvm::BasicBlock::Create(ctx_, "sc.rhs", cur_fn_);
        llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "sc.end", cur_fn_);
        if (and_op) b_.CreateCondBr(a, rhs_bb, end_bb);
        else b_.CreateCondBr(a, end_bb, rhs_bb);
        b_.SetInsertPoint(rhs_bb);
        llvm::Value*      r        = gen_expr(*bx.rhs);
        llvm::BasicBlock* rhs_done = b_.GetInsertBlock();
        b_.CreateBr(end_bb);
        b_.SetInsertPoint(end_bb);
        llvm::PHINode* phi = b_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 2);
        phi->addIncoming(llvm::ConstantInt::getBool(ctx_, !and_op), from);
        phi->addIncoming(r, rhs_done);
        return phi;
    }

    /// Arithmetic and bit operations on two values of type `t` (the left operand's type). `rt` is
    /// the right operand's type, which matters for shifts. Checked as spec 16 requires.
    llvm::Value* arith(TokenKind op, const types::Type* t, llvm::Value* a, llvm::Value* b, const types::Type* rt,
                       Span span) {
        if (!t) unsupported(span, "this operation");
        if (t->is_float()) {
            switch (op) {
            case TokenKind::plus: return b_.CreateFAdd(a, b);
            case TokenKind::minus: return b_.CreateFSub(a, b);
            case TokenKind::star: return b_.CreateFMul(a, b);
            case TokenKind::slash: return b_.CreateFDiv(a, b);
            case TokenKind::percent: return b_.CreateFRem(a, b);
            default: unsupported(span, std::format("this operator on {}", types::to_string(t)));
            }
        }
        if (!t->is_integer()) {
            if (t->is(Kind::bool_) && (op == TokenKind::amp || op == TokenKind::pipe || op == TokenKind::caret)) {
                if (op == TokenKind::amp) return b_.CreateAnd(a, b);
                if (op == TokenKind::pipe) return b_.CreateOr(a, b);
                return b_.CreateXor(a, b);
            }
            unsupported(span, std::format("this operator on {}", types::to_string(t)));
        }
        bool        s    = t->is_signed;
        std::string name = t->name;
        switch (op) {
        case TokenKind::plus_percent: return b_.CreateAdd(a, b);     // wrapping operators (spec 16)
        case TokenKind::minus_percent: return b_.CreateSub(a, b);
        case TokenKind::star_percent: return b_.CreateMul(a, b);
        default: break;
        }
        switch (op) {
        case TokenKind::plus: return checked(llvm::Intrinsic::sadd_with_overflow, llvm::Intrinsic::uadd_with_overflow, s, a, b, span,
                                             std::format("integer overflow in '+' ({})", name));
        case TokenKind::minus: return checked(llvm::Intrinsic::ssub_with_overflow, llvm::Intrinsic::usub_with_overflow, s, a, b,
                                              span, std::format("integer overflow in '-' ({})", name));
        case TokenKind::star: return checked(llvm::Intrinsic::smul_with_overflow, llvm::Intrinsic::umul_with_overflow, s, a, b,
                                             span, std::format("integer overflow in '*' ({})", name));
        case TokenKind::slash: {
            check(b_.CreateICmpNE(b, llvm::ConstantInt::get(b->getType(), 0)), span, "division by zero");
            if (!s) return b_.CreateUDiv(a, b);
            llvm::Value* minimum = llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(static_cast<unsigned>(t->bits)));
            llvm::Value* minus_one = llvm::ConstantInt::getAllOnesValue(b->getType());
            check(b_.CreateNot(b_.CreateAnd(b_.CreateICmpEQ(a, minimum), b_.CreateICmpEQ(b, minus_one))), span,
                  std::format("integer overflow in '/' ({})", name));
            return b_.CreateSDiv(a, b);
        }
        case TokenKind::percent: {
            check(b_.CreateICmpNE(b, llvm::ConstantInt::get(b->getType(), 0)), span, "division by zero in '%'");
            if (!s) return b_.CreateURem(a, b);
            // MIN % -1 is 0; LLVM's srem would be undefined there, so the divisor is replaced by 1.
            llvm::Value* minus_one = llvm::ConstantInt::getAllOnesValue(b->getType());
            llvm::Value* is_m1     = b_.CreateICmpEQ(b, minus_one);
            llvm::Value* safe      = b_.CreateSelect(is_m1, llvm::ConstantInt::get(b->getType(), 1), b);
            return b_.CreateSelect(is_m1, llvm::ConstantInt::get(b->getType(), 0), b_.CreateSRem(a, safe));
        }
        case TokenKind::plus_percent: return b_.CreateAdd(a, b);
        case TokenKind::minus_percent: return b_.CreateSub(a, b);
        case TokenKind::star_percent: return b_.CreateMul(a, b);
        case TokenKind::amp: return b_.CreateAnd(a, b);
        case TokenKind::pipe: return b_.CreateOr(a, b);
        case TokenKind::caret: return b_.CreateXor(a, b);
        case TokenKind::shl:
        case TokenKind::shr: {
            // The count is compared as a 128-bit value, so a negative count is out of range too.
            llvm::Value* count = rt && rt->is_signed ? b_.CreateSExtOrTrunc(b, llvm::Type::getInt128Ty(ctx_))
                                                     : b_.CreateZExtOrTrunc(b, llvm::Type::getInt128Ty(ctx_));
            check(b_.CreateICmpULT(count, llvm::ConstantInt::get(count->getType(), static_cast<std::uint64_t>(t->bits))),
                  span, std::format("shift count out of range for {}", name));
            llvm::Value* amount = b_.CreateZExtOrTrunc(count, a->getType());
            if (op == TokenKind::shl) return b_.CreateShl(a, amount);
            return s ? b_.CreateAShr(a, amount) : b_.CreateLShr(a, amount);
        }
        default: break;
        }
        unsupported(span, std::format("'{}' on {}", token_kind_spelling(op), types::to_string(t)));
    }

    /// `a op b` with an overflow intrinsic: panics with `message` on overflow.
    llvm::Value* checked(llvm::Intrinsic::ID signed_id, llvm::Intrinsic::ID unsigned_id, bool s, llvm::Value* a,
                         llvm::Value* b, Span span, const std::string& message) {
        llvm::Function* f = llvm::Intrinsic::getDeclaration(mod_.get(), s ? signed_id : unsigned_id, {a->getType()});
        llvm::Value*    r = b_.CreateCall(f, {a, b});
        check(b_.CreateNot(b_.CreateExtractValue(r, 1)), span, message);
        return b_.CreateExtractValue(r, 0);
    }

    llvm::Value* if_value(IfExpr& i, const types::Type* t) {
        if (i.is_compile_time) unsupported(i.span, "'if compile_time'");
        llvm::Value*      cond     = gen_expr(*i.condition);
        bool              has_value = !is_void(t);
        llvm::AllocaInst* slot      = has_value ? entry_alloca(lower_type(types::strip_ref(t)), "if.value") : nullptr;
        llvm::BasicBlock* then_bb   = llvm::BasicBlock::Create(ctx_, "then", cur_fn_);
        llvm::BasicBlock* else_bb   = i.else_branch ? llvm::BasicBlock::Create(ctx_, "else", cur_fn_) : nullptr;
        llvm::BasicBlock* end_bb    = llvm::BasicBlock::Create(ctx_, "endif", cur_fn_);
        b_.CreateCondBr(cond, then_bb, else_bb ? else_bb : end_bb);
        b_.SetInsertPoint(then_bb);
        llvm::Value* tv = gen_block(*i.then_block);
        if (!terminated()) {
            if (slot && tv) b_.CreateStore(tv, slot);
            b_.CreateBr(end_bb);
        }
        if (else_bb) {
            b_.SetInsertPoint(else_bb);
            llvm::Value* ev = gen_expr(*i.else_branch);
            if (!terminated()) {
                if (slot && ev) b_.CreateStore(ev, slot);
                b_.CreateBr(end_bb);
            }
        }
        b_.SetInsertPoint(end_bb);
        return slot ? b_.CreateLoad(lower_type(types::strip_ref(t)), slot) : nullptr;
    }

    llvm::Value* while_loop(WhileExpr& w) {
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx_, "while.cond", cur_fn_);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx_, "while.body", cur_fn_);
        llvm::BasicBlock* exit_bb = llvm::BasicBlock::Create(ctx_, "while.end", cur_fn_);
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(cond_bb);
        b_.CreateCondBr(gen_expr(*w.condition), body_bb, exit_bb);
        b_.SetInsertPoint(body_bb);
        loops_.push_back(Loop{exit_bb, cond_bb});
        open_scope(true);
        gen_block(*w.body);
        close_scope();
        loops_.pop_back();
        if (!terminated()) b_.CreateBr(cond_bb);
        b_.SetInsertPoint(exit_bb);
        return nullptr;
    }

    llvm::Value* return_value(ReturnExpr& r) {
        llvm::Type* rt = cur_fn_->getReturnType();
        llvm::Value* v = nullptr;
        if (r.value) {
            v = result_ && result_->is(Kind::reference) ? object_address(*r.value) : gen_expr(*r.value);
            moved_out(*r.value);
        }
        destroy_from(0);
        if (v && !rt->isVoidTy()) b_.CreateRet(v);
        else b_.CreateRetVoid();
        open_dead_block();
        return nullptr;
    }

    llvm::Value* call_value(CallExpr& c) {
        if (is_std_output(c)) {
            std_output(c);
            return nullptr;
        }
        Expr& callee = *c.callee;
        if (callee.kind == ExprKind::path) {
            const auto&   pe = callee.as<PathExpr>();
            const Symbol* s  = pe.symbol;
            if (!s) unsupported(callee.span, std::format("'{}'", to_string(pe.path)), no_std);
            if (s->kind == SymbolKind::alternative) {
                if (c.args.size() != 1 || !s->alternative->payload) return alternative_value(s, nullptr);
                llvm::Value* payload = gen_expr(*c.args[0]);
                moved_out(*c.args[0]);
                return alternative_value(s, payload);
            }
            if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) {
                llvm::Value* receiver = nullptr;
                if (has_self(s)) {
                    if (!self_) unsupported(c.span, "calling a method without a receiver");
                    receiver = self_;
                }
                return call_function(s, receiver, c);
            }
            unsupported(c.span, "calling lambdas and function values");
        }
        if (callee.kind == ExprKind::member) {
            auto&                 me   = callee.as<MemberExpr>();
            const Symbol*         s    = me.symbol;
            const types::Type*    obj  = types::strip_ref(me.object->type);
            std::string           name = me.name.segments.back().name;
            if (s && s->kind == SymbolKind::method) {
                if (me.arrow) unsupported(c.span, "method calls through raw pointers");
                return call_function(s, has_self(s) ? object_address(*me.object) : nullptr, c);
            }
            if (name == "clone" && c.args.empty() && obj && obj->is(Kind::user)) return gen_expr(*me.object);
            if (name == "len" && c.args.empty() && is_str(obj)) return b_.CreateExtractValue(gen_expr(*me.object), 1);
            if (name == "is_empty" && c.args.empty() && is_str(obj)) {
                return b_.CreateICmpEQ(b_.CreateExtractValue(gen_expr(*me.object), 1), llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0));
            }
            unsupported(c.span, std::format("the method '{}' on '{}'", name, obj ? types::to_string(obj) : "an unknown type"));
        }
        unsupported(c.span, "calling this expression");
    }

    llvm::Value* call_function(const Symbol* s, llvm::Value* receiver, CallExpr& c) {
        const FunctionDecl* f = body_decl(s);
        if (!f) unsupported(c.span, "this function");
        if ((f->is_template && !s->template_of) || (s->parent && s->parent->decl && s->parent->decl->is_template && !s->parent->template_of)) {
            unsupported(c.span, "calling templates");
        }
        if (f->is_virtual || f->is_abstract) unsupported(c.span, "virtual calls");
        std::vector<llvm::Value*> args;
        if (receiver) args.push_back(receiver);
        for (std::size_t i = 0; i < c.args.size() && i < f->params.size(); ++i) {
            const types::Type* pt = tc_.lower(*f->params[i].type, sema::substitution_of(s));
            if (pt->is(Kind::reference)) {
                args.push_back(object_address(*c.args[i]));
            } else {
                args.push_back(gen_expr(*c.args[i]));
                moved_out(*c.args[i]);
            }
        }
        llvm::Value* call = b_.CreateCall(function_of(s), args);
        const types::Type* result = f->result ? tc_.lower(*f->result, sema::substitution_of(s)) : nullptr;
        if (result && result->is(Kind::reference)) return b_.CreateLoad(lower_type(types::strip_ref(result)), call);
        return call;
    }

    /// The runtime functions the generated code calls. Their bodies are in llvm_runtime.cpp and
    /// are appended to the module text by generate_llvm_ir.
    void declare_runtime() {
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        auto* ptr = llvm::PointerType::get(ctx_, 0);
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        loc_type_ = llvm::StructType::get(ctx_, {ptr, i32, i32});
        rift_panic_str_ = llvm::Function::Create(llvm::FunctionType::get(vd, {ptr, ptr, i64}, false),
                                                 llvm::Function::ExternalLinkage, "rift_panic_str", mod_.get());
        rift_panic_str_->setDoesNotReturn();
        rift_exit_status_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i128, ptr}, false),
                                                   llvm::Function::ExternalLinkage, "rift_exit_status", mod_.get());
    }

    /// `message` as a constant byte array (not NUL-terminated), for `rift_panic_str`.
    llvm::Constant* message_bytes(const std::string& message) {
        llvm::Constant* init = llvm::ConstantDataArray::getString(ctx_, message, false);
        return new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, ".msg");
    }

    /// The C entry point: runs Rift's `main` and returns its status.
    void emit_entry(const Symbol* main) {
        const FunctionDecl* f      = body_decl(main);
        auto*               i32    = llvm::Type::getInt32Ty(ctx_);
        llvm::Function*     entry  = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false),
                                                            llvm::Function::ExternalLinkage, "main", mod_.get());
        llvm::BasicBlock*   block  = llvm::BasicBlock::Create(ctx_, "entry", entry);
        llvm::IRBuilder<>   b(block);
        b.CreateCall(emit_init_globals());
        llvm::CallInst*     call   = b.CreateCall(rift_main_, {});
        const types::Type*  result = f->result ? tc_.lower(*f->result, sema::substitution_of(main)) : nullptr;
        if (result && result->is_integer()) {
            llvm::Value* wide = result->is_signed ? b.CreateSExt(call, llvm::Type::getInt128Ty(ctx_))
                                                  : b.CreateZExt(call, llvm::Type::getInt128Ty(ctx_));
            b.CreateRet(b.CreateCall(rift_exit_status_, {wide, loc_global(main->span)}));
        } else {
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
        }
    }

    // --- structs, enums and places ----------------------------------------------------------

    std::map<const Symbol*, llvm::Type*> user_types_;

    static bool is_struct_symbol(const Symbol* s) {
        return s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_;
    }

    /// Methods take `self` (a pointer to the object) as their first parameter.
    static bool has_self(const Symbol* s) {
        const FunctionDecl* f = body_decl(s);
        return s->parent && f && !f->is_static;
    }

    static std::vector<const Symbol*> fields_of(const Symbol* s) {
        std::vector<const Symbol*> out;
        for (const Symbol* m : s->member_order) {
            if (m->kind == SymbolKind::field && !m->is_static) out.push_back(m);
        }
        return out;
    }

    static bool has_payloads(const Symbol* e) {
        for (const Symbol* a : e->member_order) {
            if (a->alternative && a->alternative->payload) return true;
        }
        return false;
    }

    const types::Type* field_type(const Symbol* f) {
        return tc_.lower(*f->decl->as<VariableDecl>().type, sema::substitution_of(f));
    }

    unsigned field_index(const Symbol* f) {
        std::vector<const Symbol*> fields = fields_of(f->parent);
        auto                       it     = std::find(fields.begin(), fields.end(), f);
        if (it == fields.end()) unsupported(f->span, "this field");
        return static_cast<unsigned>(it - fields.begin());
    }

    /// The LLVM type of a struct, class or enum. Structs keep the C layout of their fields (in
    /// declaration order); an empty struct holds one byte, as in C. An enum without payloads is its
    /// tag; with payloads it is `{ tag, [n x iK] }`, the payload storage sized and aligned for the
    /// largest payload on the host.
    llvm::Type* user_type(const Symbol* s) {
        if (s->decl && s->decl->is_template && !s->template_of) unsupported(s->span, "templates used without arguments");
        if (auto it = user_types_.find(s); it != user_types_.end()) return it->second;
        if (s->kind == SymbolKind::enum_) return enum_type(s);
        if (!is_struct_symbol(s)) unsupported(s->span, "values of this type");
        const auto& decl = s->decl->as<TypeDecl>();
        if (!decl.bases.empty() || s->has_opaque_base) unsupported(s->span, "inheritance and interfaces");
        auto* st       = llvm::StructType::create(ctx_, s->name);
        user_types_[s] = st;
        std::vector<llvm::Type*> elems;
        for (const Symbol* f : fields_of(s)) {
            cur_ = f->span;
            elems.push_back(lower_type(field_type(f)));
        }
        if (elems.empty()) elems.push_back(llvm::Type::getInt8Ty(ctx_));
        st->setBody(elems);
        return st;
    }

    llvm::Type* enum_tag_type(const Symbol* e) {
        return lower_type(tc_.lower(*e->decl->as<EnumDecl>().tag_type, sema::substitution_of(e)));
    }

    llvm::Type* enum_type(const Symbol* e) {
        llvm::Type* tag = enum_tag_type(e);
        if (!has_payloads(e)) {
            user_types_[e] = tag;
            return tag;
        }
        const llvm::DataLayout& layout = mod_->getDataLayout();
        std::uint64_t           size   = 0;
        unsigned                align  = 1;
        for (const Symbol* a : e->member_order) {
            if (!a->alternative || !a->alternative->payload) continue;
            cur_ = a->span;
            llvm::Type* p = lower_type(tc_.lower(*a->alternative->payload, sema::substitution_of(a)));
            size          = std::max<std::uint64_t>(size, layout.getTypeAllocSize(p).getFixedValue());
            align         = std::max(align, static_cast<unsigned>(layout.getABITypeAlign(p).value()));
        }
        auto* st       = llvm::StructType::create(ctx_, e->name);
        user_types_[e] = st;
        llvm::Type* elem = llvm::IntegerType::get(ctx_, align * 8);
        st->setBody({tag, llvm::ArrayType::get(elem, (size + align - 1) / align)});
        return st;
    }

    /// Tag value of each alternative (explicit values, otherwise previous + 1).
    std::map<const Symbol*, u128> tag_values(const Symbol* e) {
        std::map<const Symbol*, u128> out;
        u128                          next = 0;
        for (const Symbol* a : e->member_order) {
            if (!a->alternative) continue;
            if (const Expr* v = a->alternative->value.get()) {
                bool negative = false;
                if (v->kind == ExprKind::unary && v->as<UnaryExpr>().op == UnaryOp::negate) {
                    negative = true;
                    v        = v->as<UnaryExpr>().operand.get();
                }
                if (v->kind != ExprKind::int_literal) unsupported(v->span, "enum tags that are not integer literals");
                u128 m = v->as<LiteralExpr>().int_value;
                next   = negative ? u128{0} - m : m;
            }
            out[a] = next;
            next   = next + 1;
        }
        return out;
    }

    static llvm::APInt wide(u128 v, unsigned bits) {
        std::uint64_t words[2] = {static_cast<std::uint64_t>(v), static_cast<std::uint64_t>(v >> 64)};
        return llvm::APInt(128, words).zextOrTrunc(bits);
    }

    llvm::ConstantInt* tag_constant(const Symbol* alt) {
        const Symbol* e    = alt->parent;
        auto          bits = llvm::cast<llvm::IntegerType>(enum_tag_type(e))->getBitWidth();
        return llvm::ConstantInt::get(ctx_, wide(tag_values(e)[alt], bits));
    }

    /// The value of an alternative: its tag, or the enum with `payload` stored in it.
    llvm::Value* alternative_value(const Symbol* alt, llvm::Value* payload) {
        const Symbol* e   = alt->parent;
        llvm::Value*  tag = tag_constant(alt);
        if (!has_payloads(e)) return tag;
        llvm::Type*       et   = user_type(e);
        llvm::AllocaInst* slot = entry_alloca(et, "alternative");
        b_.CreateStore(tag, b_.CreateStructGEP(et, slot, 0));
        if (payload) b_.CreateStore(payload, b_.CreateStructGEP(et, slot, 1));
        return b_.CreateLoad(et, slot);
    }

    /// A struct value from the fields given in a literal; the others take their declared
    /// initializer, or the default of their type.
    llvm::Value* build_struct(const Symbol* s, const std::map<std::string, llvm::Value*>& given) {
        llvm::Type*       st   = user_type(s);
        llvm::AllocaInst* slot = entry_alloca(st, "value");
        for (const Symbol* f : fields_of(s)) {
            cur_ = f->span;
            llvm::Value* v = nullptr;
            if (auto it = given.find(f->name); it != given.end()) {
                v = it->second;
            } else {
                const auto& d = f->decl->as<VariableDecl>();
                v             = d.init ? gen_expr(*d.init) : default_value(field_type(f), f->span);
            }
            b_.CreateStore(v, b_.CreateStructGEP(st, slot, field_index(f)));
        }
        return b_.CreateLoad(st, slot);
    }

    llvm::Value* struct_literal(StructLiteralExpr& sl) {
        const types::Type* t = types::strip_ref(static_cast<Expr&>(sl).type);
        if (!t || !t->is(Kind::user) || !is_struct_symbol(t->symbol)) {
            unsupported(sl.span, std::format("building values of type '{}'", t ? types::to_string(t) : "unknown"));
        }
        // The given fields are evaluated in the order written, then the rest.
        std::map<std::string, llvm::Value*> given;
        for (FieldInit& fi : sl.fields) {
            given[fi.name] = gen_expr(*fi.value);
            moved_out(*fi.value);
        }
        return build_struct(t->symbol, given);
    }

    llvm::Value* default_value(const types::Type* t, Span where) {
        if (t && t->is(Kind::user) && is_struct_symbol(t->symbol)) return build_struct(t->symbol, {});
        if (t && (t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_) || t->is(Kind::pointer))) {
            return llvm::Constant::getNullValue(lower_type(t));
        }
        unsupported(where, std::format("'default' for '{}'", t ? types::to_string(t) : "unknown"));
    }

    bool is_place(const Expr& e) const {
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            return s && (s->kind == SymbolKind::local || s->kind == SymbolKind::global);
        }
        case ExprKind::this_:
        case ExprKind::member: return true;
        case ExprKind::unary: return e.as<UnaryExpr>().op == UnaryOp::deref;
        default: return false;
        }
    }

    /// The address of the object a place denotes. A reference (or `this`) denotes the object it
    /// refers to, so its address is the pointer it holds.
    llvm::Value* place(Expr& e) {
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            if (s && s->kind == SymbolKind::global) return global_of(s);
            if (!s || s->kind != SymbolKind::local) unsupported(e.span, "this place");
            auto it = locals_.find(s);
            if (it == locals_.end()) unsupported(e.span, "this name");
            if (s->type && s->type->is(Kind::reference)) return b_.CreateLoad(llvm::PointerType::get(ctx_, 0), it->second);
            return it->second;
        }
        case ExprKind::this_:
            if (!self_) unsupported(e.span, "'this' outside a method");
            return self_;
        case ExprKind::member: {
            auto&         m = e.as<MemberExpr>();
            const Symbol* f = m.symbol;
            if (!f || f->kind != SymbolKind::field || f->is_static) unsupported(e.span, "this member access");
            return b_.CreateStructGEP(user_type(f->parent), object_address(*m.object), field_index(f));
        }
        case ExprKind::unary:
            if (e.as<UnaryExpr>().op == UnaryOp::deref) return gen_expr(*e.as<UnaryExpr>().operand);
            break;
        default: break;
        }
        unsupported(e.span, "this place");
    }

    /// The address of the object `e` denotes: a place, a pointer or reference value, or a temporary.
    llvm::Value* object_address(Expr& e) {
        if (e.kind == ExprKind::unary) {
            UnaryOp op = e.as<UnaryExpr>().op;
            if (op == UnaryOp::mut_marker || op == UnaryOp::move) return object_address(*e.as<UnaryExpr>().operand);
        }
        if (e.type && e.type->is(Kind::pointer)) return gen_expr(e);
        if (e.type && e.type->is(Kind::reference)) return place(e);
        if (is_place(e)) return place(e);
        llvm::Value*      v    = gen_expr(e);
        llvm::AllocaInst* slot = entry_alloca(v->getType(), "temporary");
        b_.CreateStore(v, slot);
        return slot;
    }

    /// The value of a place expression (a reference is read through).
    llvm::Value* value_of_place(Expr& e) {
        return b_.CreateLoad(lower_type(types::strip_ref(e.type)), place(e));
    }

    struct Loop {
        llvm::BasicBlock* exit;
        llvm::BasicBlock* cont;
    };

    // --- strings, output and formatting (spec 19) ------------------------------------------

    llvm::Type* ptr_type() { return llvm::PointerType::get(ctx_, 0); }

    /// A `str` value: the bytes and their length, `{ ptr, i64 }`.
    llvm::StructType* str_type() { return llvm::StructType::get(ctx_, {ptr_type(), llvm::Type::getInt64Ty(ctx_)}); }

    /// The runtime's output buffer, `{ ptr, i64, i64 }`.
    llvm::StructType* buffer_type() {
        return llvm::StructType::get(ctx_, {ptr_type(), llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt64Ty(ctx_)});
    }

    static bool is_str(const types::Type* t) {
        return t && (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str"));
    }

    /// A `str` constant with the bytes of `text`.
    llvm::Constant* str_constant(const std::string& text) {
        llvm::Constant* bytes = message_bytes(text);
        return llvm::ConstantStruct::get(str_type(), {bytes, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), text.size())});
    }

    /// A function of the runtime (llvm_runtime.cpp), declared here and defined in the appended IR.
    llvm::Function* runtime(const char* name, llvm::Type* result, std::vector<llvm::Type*> params) {
        if (llvm::Function* f = mod_->getFunction(name)) return f;
        return llvm::Function::Create(llvm::FunctionType::get(result, params, false), llvm::Function::ExternalLinkage, name, mod_.get());
    }

    /// The `rift_spec` of a format hole: `{ align, plus, width, precision, radix }` (spec 19).
    llvm::Constant* spec_constant(char align, bool plus, int width, int precision, char radix) {
        auto* i8  = llvm::Type::getInt8Ty(ctx_);
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        llvm::Constant* init = llvm::ConstantStruct::getAnon(
            ctx_, {llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(align)), llvm::ConstantInt::getBool(ctx_, plus),
                   llvm::ConstantInt::getSigned(i32, width), llvm::ConstantInt::getSigned(i32, precision),
                   llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(radix))});
        return new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, ".spec");
    }

    llvm::Constant* plain_spec() { return spec_constant(0, false, -1, -1, 0); }

    /// The rift_stream for stdout (`err` false) or stderr.
    llvm::Value* stream(bool err) {
        return b_.CreateCall(runtime("rift_stream", ptr_type(), {llvm::Type::getInt1Ty(ctx_)}),
                             {llvm::ConstantInt::getBool(ctx_, err)});
    }

    static std::string std_name(const CallExpr& c) {
        if (c.callee->kind != ExprKind::path) return {};
        const auto& pe = c.callee->as<PathExpr>();
        if (pe.symbol || pe.path.segments.size() != 2 || pe.path.segments[0].name != "std") return {};
        return pe.path.segments[1].name;
    }

    static bool is_std_output(const CallExpr& c) {
        std::string n = std_name(c);
        return n == "print" || n == "println" || n == "eprint" || n == "eprintln" || n == "panic";
    }

    /// std::print / println / eprint / eprintln / panic. Output is written straight to the stream;
    /// panics print the message with the location of the call (spec 11).
    void std_output(CallExpr& c) {
        std::string name    = std_name(c);
        bool        panic   = name == "panic";
        bool        newline = name.ends_with("ln");
        bool        err     = name.starts_with("e") || panic;
        if (c.args.size() != 1) unsupported(c.span, std::format("'std::{}' with {} arguments", name, c.args.size()));
        Expr& arg = *c.args[0];
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* i1  = llvm::Type::getInt1Ty(ctx_);
        auto* loc = llvm::PointerType::get(ctx_, 0);
        if (arg.kind == ExprKind::string_literal && arg.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
            llvm::AllocaInst* buf = entry_alloca(buffer_type(), "buffer");
            b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
            for (FormatPart& part : arg.as<LiteralExpr>().format_parts) {
                if (part.expr) {
                    format_hole(buf, part);
                } else {
                    b_.CreateCall(runtime("rift_buf_put", llvm::Type::getVoidTy(ctx_), {loc, loc, i64}),
                                  {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
                }
            }
            if (panic) {
                b_.CreateCall(runtime("rift_panic_buf", llvm::Type::getVoidTy(ctx_), {loc, loc}), {loc_global(c.span), buf});
                b_.CreateUnreachable();
                open_dead_block();
            } else {
                b_.CreateCall(runtime("rift_flush", llvm::Type::getVoidTy(ctx_), {loc, loc, i1}),
                              {stream(err), buf, llvm::ConstantInt::getBool(ctx_, newline)});
            }
            return;
        }
        const types::Type* t = types::strip_ref(arg.type);
        if (!is_str(t)) unsupported(arg.span, std::format("'std::{}' with a value of type '{}'", name, t ? types::to_string(t) : "unknown"));
        llvm::Value* v = gen_expr(arg);
        llvm::Value* data = b_.CreateExtractValue(v, 0);
        llvm::Value* len  = b_.CreateExtractValue(v, 1);
        if (panic) {
            b_.CreateCall(runtime("rift_panic_str", llvm::Type::getVoidTy(ctx_), {loc, loc, i64}), {loc_global(c.span), data, len});
            b_.CreateUnreachable();
            open_dead_block();
        } else {
            b_.CreateCall(runtime("rift_write_str", llvm::Type::getVoidTy(ctx_), {loc, loc, i64, i1}),
                          {stream(err), data, len, llvm::ConstantInt::getBool(ctx_, newline)});
        }
    }

    /// One `{expr:spec}` hole of an f-string, formatted into `buf`.
    void format_hole(llvm::Value* buf, FormatPart& part) {
        Expr&                e    = *part.expr;
        const types::Type*   t    = types::strip_ref(e.type);
        llvm::Constant*      spec = part.spec.present
                                        ? spec_constant(part.spec.align, part.spec.plus, part.spec.width, part.spec.precision, part.spec.radix)
                                        : plain_spec();
        if (!t || !(t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_) || is_str(t))) {
            unsupported(e.span, std::format("formatting values of type '{}'", t ? types::to_string(t) : "unknown"));
        }
        llvm::Value*         v    = gen_expr(e);
        auto*                i1   = llvm::Type::getInt1Ty(ctx_);
        auto*                i128 = llvm::Type::getInt128Ty(ctx_);
        auto*                vd   = llvm::Type::getVoidTy(ctx_);
        auto*                p    = ptr_type();
        if (t && t->is_integer()) {
            if (!t->is_signed || part.spec.radix) {   // unsigned, or two's complement bits in another base
                b_.CreateCall(runtime("rift_fmt_u", vd, {p, i128, i1, p}),
                              {buf, b_.CreateZExt(v, i128), llvm::ConstantInt::getFalse(ctx_), spec});
            } else {
                b_.CreateCall(runtime("rift_fmt_i", vd, {p, i128, p}), {buf, b_.CreateSExt(v, i128), spec});
            }
        } else if (t && t->is_float()) {
            bool single = t->bits == 32;
            llvm::Value* d = single ? b_.CreateFPExt(v, llvm::Type::getDoubleTy(ctx_)) : v;
            b_.CreateCall(runtime("rift_fmt_f", vd, {p, llvm::Type::getDoubleTy(ctx_), i1, p}),
                          {buf, d, llvm::ConstantInt::getBool(ctx_, single), spec});
        } else if (t && t->is(Kind::bool_)) {
            b_.CreateCall(runtime("rift_fmt_bool", vd, {p, i1, p}), {buf, v, spec});
        } else if (t && t->is(Kind::char_)) {
            b_.CreateCall(runtime("rift_fmt_char", vd, {p, llvm::Type::getInt32Ty(ctx_), p}), {buf, v, spec});
        } else if (is_str(t)) {
            b_.CreateCall(runtime("rift_fmt_str", vd, {p, p, llvm::Type::getInt64Ty(ctx_), p}),
                          {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1), spec});
        } else {
            unsupported(e.span, std::format("formatting values of type '{}'", t ? types::to_string(t) : "unknown"));
        }
    }

    // --- match, for and casts -----------------------------------------------------------------

    llvm::Value* str_equal(llvm::Value* a, llvm::Value* b, bool eq) {
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        llvm::Value* same = b_.CreateCall(runtime("rift_str_eq", llvm::Type::getInt1Ty(ctx_), {ptr_type(), i64, ptr_type(), i64}),
                                          {b_.CreateExtractValue(a, 0), b_.CreateExtractValue(a, 1), b_.CreateExtractValue(b, 0),
                                           b_.CreateExtractValue(b, 1)});
        return eq ? same : b_.CreateNot(same);
    }

    /// Continues when `ok` holds; otherwise goes to `fail` (the next arm of a match).
    void require(llvm::Value* ok, llvm::BasicBlock* fail) {
        llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx_, "pattern", cur_fn_);
        b_.CreateCondBr(ok, cont, fail);
        b_.SetInsertPoint(cont);
    }

    const types::Type* payload_of(const Symbol* alt) {
        return tc_.lower(*alt->alternative->payload, sema::substitution_of(alt));
    }

    /// The field of a struct type named `name`, or null.
    static const Symbol* field_symbol(const Symbol* type, const std::string& name) {
        auto it = type->members.find(name);
        if (it == type->members.end() || it->second.front()->kind != SymbolKind::field) return nullptr;
        return it->second.front();
    }

    /// Checks the pattern `p` against the object at `addr` of type `t`. Falls through when it
    /// matches (binding its names) and branches to `fail` otherwise. Parts of a value are only read
    /// once the tests above them have passed, as in the C backend.
    void match_pattern(Pattern& p, llvm::Value* addr, const types::Type* t, llvm::BasicBlock* fail) {
        switch (p.kind) {
        case PatternKind::wildcard: return;
        case PatternKind::binding: {
            const Symbol* s = p.as<BindingPattern>().symbol;
            if (!s) return;
            if (needs_drop(t)) unsupported(p.span, "binding a value that needs a destructor in a pattern");
            llvm::Type*       lt   = lower_type(t);
            llvm::AllocaInst* slot = entry_alloca(lt, s->name);
            b_.CreateStore(b_.CreateLoad(lt, addr), slot);
            locals_[s] = slot;
            return;
        }
        case PatternKind::literal: {
            Expr&        lit = *p.as<LiteralPattern>().literal;
            llvm::Value* v   = b_.CreateLoad(lower_type(t), addr);
            llvm::Value* l   = gen_expr(lit);
            if (is_str(t)) return require(str_equal(v, l, true), fail);
            if (v->getType()->isFloatingPointTy()) return require(b_.CreateFCmpOEQ(v, l), fail);
            require(b_.CreateICmpEQ(v, l), fail);
            return;
        }
        case PatternKind::range: {
            auto&        r = p.as<RangePattern>();
            llvm::Value* v = b_.CreateLoad(lower_type(t), addr);
            if (r.lo) require(compare_op(TokenKind::greater_eq, v, gen_expr(*r.lo), t, p.span), fail);
            if (r.hi) require(compare_op(r.inclusive ? TokenKind::less_eq : TokenKind::less, v, gen_expr(*r.hi), t, p.span), fail);
            return;
        }
        case PatternKind::enum_alternative: {
            auto&         ep  = p.as<EnumPattern>();
            const Symbol* alt = ep.symbol;
            if (!alt || !alt->parent || alt->kind != SymbolKind::alternative) unsupported(p.span, "this pattern (standard library enums)");
            const Symbol* e = alt->parent;
            if (!has_payloads(e)) {
                require(b_.CreateICmpEQ(b_.CreateLoad(enum_tag_type(e), addr), tag_constant(alt)), fail);
                return;
            }
            llvm::Type*  et  = user_type(e);
            llvm::Value* tag = b_.CreateLoad(enum_tag_type(e), b_.CreateStructGEP(et, addr, 0));
            require(b_.CreateICmpEQ(tag, tag_constant(alt)), fail);
            if (ep.payload && alt->alternative->payload) {
                match_pattern(*ep.payload, b_.CreateStructGEP(et, addr, 1), payload_of(alt), fail);
            }
            return;
        }
        case PatternKind::struct_: {
            auto& sp = p.as<StructPattern>();
            if (!t->is(Kind::user) || !is_struct_symbol(t->symbol)) unsupported(p.span, "this pattern");
            llvm::Type* st = user_type(t->symbol);
            for (FieldPattern& fp : sp.fields) {
                const Symbol* f = field_symbol(t->symbol, fp.name);
                if (!f || !fp.pattern) continue;
                match_pattern(*fp.pattern, b_.CreateStructGEP(st, addr, field_index(f)), field_type(f), fail);
            }
            return;
        }
        case PatternKind::error: unsupported(p.span, "this pattern");
        }
    }

    /// `match`: the arms are tried in order; the first whose pattern (and guard) matches gives the
    /// value. If none matches, the program panics (spec 07).
    llvm::Value* match_value(MatchExpr& m, const types::Type* t) {
        Expr* scrutinee = m.scrutinee.get();
        if (scrutinee->kind == ExprKind::unary && scrutinee->as<UnaryExpr>().op == UnaryOp::mut_marker) {
            unsupported(scrutinee->span, "'match (mut x)'");
        }
        const types::Type* st   = types::strip_ref(scrutinee->type);
        if (needs_drop(st) && !owned_path(*scrutinee)) {
            unsupported(scrutinee->span, "matching on a value that needs a destructor", "store it in a local first");
        }
        llvm::Value*       addr = object_address(*scrutinee);
        bool               has_value = !is_void(t) && !(t && t->is_unknown());
        llvm::AllocaInst*  slot      = has_value ? entry_alloca(lower_type(types::strip_ref(t)), "match.value") : nullptr;
        llvm::BasicBlock*  end_bb    = llvm::BasicBlock::Create(ctx_, "match.end", cur_fn_);
        for (MatchArm& arm : m.arms) {
            llvm::BasicBlock* next = llvm::BasicBlock::Create(ctx_, "arm.next", cur_fn_);
            llvm::BasicBlock* body = llvm::BasicBlock::Create(ctx_, "arm.body", cur_fn_);
            match_pattern(*arm.pattern, addr, st, next);
            if (arm.guard) {
                b_.CreateCondBr(gen_expr(*arm.guard), body, next);
            } else {
                b_.CreateBr(body);
            }
            b_.SetInsertPoint(body);
            llvm::Value* v = gen_expr(*arm.body);
            if (!terminated()) {
                if (slot && v) b_.CreateStore(v, slot);
                b_.CreateBr(end_bb);
            }
            b_.SetInsertPoint(next);
        }
        check(llvm::ConstantInt::getFalse(ctx_), m.span, "no match arm matched");
        b_.CreateBr(end_bb);
        b_.SetInsertPoint(end_bb);
        return slot ? b_.CreateLoad(lower_type(types::strip_ref(t)), slot) : nullptr;
    }

    /// `a op b` for the ordering and equality operators on integers, chars, bools, enums and floats.
    llvm::Value* compare_op(TokenKind op, llvm::Value* a, llvm::Value* b, const types::Type* t, Span span) {
        if (a->getType()->isFloatingPointTy()) {
            switch (op) {
            case TokenKind::less: return b_.CreateFCmpOLT(a, b);
            case TokenKind::less_eq: return b_.CreateFCmpOLE(a, b);
            case TokenKind::greater: return b_.CreateFCmpOGT(a, b);
            case TokenKind::greater_eq: return b_.CreateFCmpOGE(a, b);
            default: unsupported(span, "comparing these values");
            }
        }
        if (!a->getType()->isIntegerTy()) unsupported(span, "comparing these values");
        bool s = t && t->is_integer() && t->is_signed;
        switch (op) {
        case TokenKind::less: return s ? b_.CreateICmpSLT(a, b) : b_.CreateICmpULT(a, b);
        case TokenKind::less_eq: return s ? b_.CreateICmpSLE(a, b) : b_.CreateICmpULE(a, b);
        case TokenKind::greater: return s ? b_.CreateICmpSGT(a, b) : b_.CreateICmpUGT(a, b);
        case TokenKind::greater_eq: return s ? b_.CreateICmpSGE(a, b) : b_.CreateICmpUGE(a, b);
        default: return b_.CreateICmpEQ(a, b);
        }
    }

    /// `bit(n)`: the integer with only bit `n` set; `n` must be below the bit width (spec 16).
    llvm::Value* bit_value(BitExpr& b, const types::Type* type) {
        const types::Type* t = types::strip_ref(type);
        if (!t || !t->is_integer()) unsupported(b.span, "'bit(n)' without an integer type");
        llvm::Type* lt   = lower_type(t);
        llvm::Value* n   = b_.CreateSExtOrTrunc(gen_expr(*b.operand), llvm::Type::getInt128Ty(ctx_));
        llvm::Value* in  = b_.CreateAnd(b_.CreateICmpSGE(n, llvm::ConstantInt::get(n->getType(), 0)),
                                        b_.CreateICmpSLT(n, llvm::ConstantInt::get(n->getType(), static_cast<std::uint64_t>(t->bits))));
        check(in, b.span, std::format("bit index out of range for {}", t->name));
        return b_.CreateShl(llvm::ConstantInt::get(lt, 1), b_.CreateTrunc(n, lt));
    }

    /// `for x in a..b` and `for x in a..=b` over integers. The inclusive form uses a flag so that
    /// `a..=MAX` never overflows (as in the C backend).
    llvm::Value* for_loop(ForExpr& f) {
        Expr* it = f.iterable.get();
        if (it->kind != ExprKind::range) unsupported(it->span, "'for' over anything but a range 'a..b'");
        auto&              r  = it->as<RangeExpr>();
        const types::Type* t  = f.symbol && f.symbol->type ? types::strip_ref(f.symbol->type) : nullptr;
        if (!t || !t->is_integer()) unsupported(f.span, "'for' over values of this type");
        llvm::Type*        lt = lower_type(t);
        llvm::Value*       lo = gen_expr(*r.lo);
        llvm::Value*       hi = gen_expr(*r.hi);
        llvm::AllocaInst*  iv = entry_alloca(lt, "for.i");
        llvm::AllocaInst*  go = r.inclusive ? entry_alloca(llvm::Type::getInt1Ty(ctx_), "for.go") : nullptr;
        b_.CreateStore(lo, iv);
        if (go) b_.CreateStore(compare_op(TokenKind::less_eq, lo, hi, t, f.span), go);
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx_, "for.cond", cur_fn_);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx_, "for.body", cur_fn_);
        llvm::BasicBlock* cont_bb = llvm::BasicBlock::Create(ctx_, "for.next", cur_fn_);
        llvm::BasicBlock* exit_bb = llvm::BasicBlock::Create(ctx_, "for.end", cur_fn_);
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(cond_bb);
        llvm::Value* more = go ? b_.CreateLoad(llvm::Type::getInt1Ty(ctx_), go)
                               : compare_op(TokenKind::less, b_.CreateLoad(lt, iv), hi, t, f.span);
        b_.CreateCondBr(more, body_bb, exit_bb);
        b_.SetInsertPoint(body_bb);
        if (f.symbol) {
            llvm::AllocaInst* var = entry_alloca(lt, f.symbol->name);
            b_.CreateStore(b_.CreateLoad(lt, iv), var);
            locals_[f.symbol] = var;
        }
        loops_.push_back(Loop{exit_bb, cont_bb});
        open_scope(true);
        gen_block(*f.body);
        close_scope();
        loops_.pop_back();
        if (!terminated()) b_.CreateBr(cont_bb);
        b_.SetInsertPoint(cont_bb);
        llvm::Value* cur = b_.CreateLoad(lt, iv);
        llvm::Value* one = llvm::ConstantInt::get(lt, 1);
        if (go) {
            llvm::Value* more_after = b_.CreateICmpNE(cur, hi);
            b_.CreateStore(b_.CreateSelect(more_after, b_.CreateAdd(cur, one), cur), iv);
            b_.CreateStore(more_after, go);
        } else {
            b_.CreateStore(b_.CreateAdd(cur, one), iv);
        }
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(exit_bb);
        return nullptr;
    }

    /// Clamps the integer value `v` (of type `from`) into the range of `to`: `cast::saturate`.
    llvm::Value* saturate_integer(llvm::Value* v, const types::Type* from, const types::Type* to) {
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        llvm::Value* w = from->is_signed ? b_.CreateSExt(v, i128) : b_.CreateZExt(v, i128);
        unsigned bits = static_cast<unsigned>(to->bits);
        if (to->is_signed) {
            llvm::APInt low  = llvm::APInt::getSignedMinValue(bits).sext(128);
            llvm::APInt high = llvm::APInt::getSignedMaxValue(bits).sext(128);
            w = b_.CreateSelect(b_.CreateICmpSLT(w, llvm::ConstantInt::get(ctx_, low)), llvm::ConstantInt::get(ctx_, low), w);
            w = b_.CreateSelect(b_.CreateICmpSGT(w, llvm::ConstantInt::get(ctx_, high)), llvm::ConstantInt::get(ctx_, high), w);
        } else {
            llvm::APInt high = llvm::APInt::getMaxValue(bits).zext(128);
            w = b_.CreateSelect(b_.CreateICmpSLT(w, llvm::ConstantInt::get(i128, 0)), llvm::ConstantInt::get(i128, 0), w);
            w = b_.CreateSelect(b_.CreateICmpSGT(w, llvm::ConstantInt::get(ctx_, high)), llvm::ConstantInt::get(ctx_, high), w);
        }
        return b_.CreateTrunc(w, lower_type(to));
    }

    /// Float to integer with saturation: NaN gives 0, out-of-range values give the bounds.
    llvm::Value* saturate_float(llvm::Value* v, const types::Type* to) {
        auto*    dt   = llvm::Type::getDoubleTy(ctx_);
        llvm::Value* f = b_.CreateFPExt(v, dt);
        unsigned bits = static_cast<unsigned>(to->bits);
        double   low  = to->is_signed ? -std::ldexp(1.0, static_cast<int>(bits) - 1) : 0.0;
        double   high = to->is_signed ? std::ldexp(1.0, static_cast<int>(bits) - 1) : std::ldexp(1.0, static_cast<int>(bits));
        llvm::Type* lt = lower_type(to);
        llvm::Value* converted = to->is_signed ? b_.CreateFPToSI(f, lt) : b_.CreateFPToUI(f, lt);
        llvm::Value* min_v = to->is_signed ? llvm::ConstantInt::get(lt, llvm::APInt::getSignedMinValue(bits))
                                           : llvm::ConstantInt::get(lt, 0);
        llvm::Value* max_v = to->is_signed ? llvm::ConstantInt::get(lt, llvm::APInt::getSignedMaxValue(bits))
                                           : llvm::ConstantInt::get(lt, llvm::APInt::getMaxValue(bits));
        llvm::Value* r = b_.CreateSelect(b_.CreateFCmpOGE(f, llvm::ConstantFP::get(dt, high)), max_v, converted);
        r = b_.CreateSelect(b_.CreateFCmpOLE(f, llvm::ConstantFP::get(dt, low)), min_v, r);
        return b_.CreateSelect(b_.CreateFCmpUNO(f, f), llvm::ConstantInt::get(lt, 0), r);
    }

    /// A conversion between numeric types, as in C: widening follows the source's signedness.
    llvm::Value* convert(llvm::Value* v, const types::Type* from, const types::Type* to) {
        llvm::Type* lt = lower_type(to);
        if (from->is(Kind::pointer) || to->is(Kind::pointer)) {
            if (to->is(Kind::pointer) && from->is(Kind::pointer)) return v;
            if (to->is(Kind::pointer)) return b_.CreateIntToPtr(v, lt);
            return b_.CreatePtrToInt(v, lt);
        }
        if (to->is_integer() || to->is(Kind::char_)) {
            if (from->is_float()) {
                bool s = to->is_integer() && to->is_signed;
                return s ? b_.CreateFPToSI(v, lt) : b_.CreateFPToUI(v, lt);
            }
            if (!v->getType()->isIntegerTy()) unsupported(cur_, "this conversion");
            unsigned fb = v->getType()->getIntegerBitWidth();
            unsigned tb = lt->getIntegerBitWidth();
            if (tb < fb) return b_.CreateTrunc(v, lt);
            if (tb > fb) return (from->is_integer() && from->is_signed) ? b_.CreateSExt(v, lt) : b_.CreateZExt(v, lt);
            return v;
        }
        if (to->is_float()) {
            if (from->is_float()) {
                if (from->bits == to->bits) return v;
                return from->bits < to->bits ? b_.CreateFPExt(v, lt) : b_.CreateFPTrunc(v, lt);
            }
            bool s = from->is_integer() && from->is_signed;
            return s ? b_.CreateSIToFP(v, lt) : b_.CreateUIToFP(v, lt);
        }
        if (to->is(Kind::bool_) && from->is(Kind::bool_)) return v;
        unsupported(cur_, std::format("converting to '{}'", types::to_string(to)));
    }

    llvm::Value* cast_value(CastExpr& c) {
        const types::Type* to   = types::strip_ref(c.type);
        const types::Type* from = types::strip_ref(c.operand->type);
        const std::string& variant = c.variant;
        if (variant == "checked" || variant == "dynamic") {
            unsupported(c.span, std::format("'cast::{}' (it returns std::option)", variant));
        }
        llvm::Value* v = gen_expr(*c.operand);
        if (variant == "reinterpret") {
            if (to->is(Kind::pointer) || from->is(Kind::pointer)) return convert(v, from, to);
            return b_.CreateBitCast(v, lower_type(to));
        }
        if (variant == "saturate" && to->is_integer()) {
            if (from->is_float()) return saturate_float(v, to);
            if (from->is_integer()) return saturate_integer(v, from, to);
        }
        // cast<T> (lossless), cast::truncate (keeps the low bits) and cast::round are conversions.
        return convert(v, from, to);
    }

    // --- globals --------------------------------------------------------------------------

    std::map<const Symbol*, llvm::GlobalVariable*> globals_;
    std::map<const Symbol*, llvm::Function*>       global_inits_;
    std::deque<const Symbol*>                      global_queue_;

    /// The storage of a global variable. Its initializer runs at startup (rift_init_globals).
    llvm::GlobalVariable* global_of(const Symbol* g) {
        if (auto it = globals_.find(g); it != globals_.end()) return it->second;
        cur_ = g->span;
        const auto&              v     = g->decl->as<VariableDecl>();
        const types::Type*       vt    = tc_.lower(*v.type, sema::substitution_of(g));
        if (needs_drop(vt)) unsupported(v.span, "global variables that need a destructor");
        llvm::Type*              t     = lower_type(vt);
        std::vector<std::string> parts = split_module(g->module ? g->module->name : "");
        parts.push_back(g->name);
        auto* gv = new llvm::GlobalVariable(*mod_, t, false, llvm::GlobalValue::InternalLinkage,
                                            llvm::Constant::getNullValue(t), "rg_" + mangle_parts(parts));
        globals_[g] = gv;
        global_queue_.push_back(g);
        return gv;
    }

    /// Generates the initializer of a global as a function that stores its value.
    void gen_global(const Symbol* g) {
        const auto& v    = g->decl->as<VariableDecl>();
        cur_             = g->span;
        llvm::GlobalVariable* gv = globals_[g];
        llvm::Function* init = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false),
                                                      llvm::Function::InternalLinkage, "init_" + gv->getName().str(), mod_.get());
        llvm::IRBuilderBase::InsertPoint saved_ip = b_.saveIP();
        llvm::Function*                  saved_fn = cur_fn_;
        llvm::Value*                     saved_self = self_;
        auto                             saved_locals = locals_;
        cur_fn_ = init;
        self_   = nullptr;
        locals_.clear();
        loops_.clear();
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", init));
        if (v.init) b_.CreateStore(gen_expr(*v.init), gv);
        b_.CreateRetVoid();
        cur_fn_ = saved_fn;
        self_   = saved_self;
        locals_ = saved_locals;
        b_.restoreIP(saved_ip);
        global_inits_[g] = init;
    }

    /// The modules in the order they are initialized: imported modules first (as in the C backend).
    std::vector<const Module*> module_order() {
        std::vector<const Module*> out;
        std::set<const Module*>    seen;
        std::function<void(const Module*)> visit = [&](const Module* m) {
            if (!m || !seen.insert(m).second) return;
            for (const Module* i : m->header_imports) visit(i);
            for (const Module* i : m->source_imports) visit(i);
            out.push_back(m);
        };
        visit(program_.entry);
        for (auto& [name, m] : program_.modules) visit(m.get());
        return out;
    }

    /// rift_init_globals: runs the initializers of the globals the program uses, module by module.
    llvm::Function* emit_init_globals() {
        llvm::Function* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false),
                                                    llvm::Function::InternalLinkage, "rift_init_globals", mod_.get());
        llvm::IRBuilderBase::InsertPoint saved_ip = b_.saveIP();
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        for (const Module* m : module_order()) {
            for (const Symbol* s : m->item_order) {
                if (auto it = global_inits_.find(s); it != global_inits_.end()) b_.CreateCall(it->second);
            }
        }
        b_.CreateRetVoid();
        b_.restoreIP(saved_ip);
        return fn;
    }

    // --- destructors (spec 12) -------------------------------------------------------------
    //
    // A local whose type needs a destructor has a flag: set when the local takes its value,
    // cleared when the value is moved out. Leaving a scope, `return`, `break` and `continue`
    // destroy the owned locals that still hold a value, innermost scope first and in reverse
    // declaration order. A panic aborts without running destructors (spec 11).

    struct OwnedLocal {
        llvm::AllocaInst* slot;   // the variable
        llvm::AllocaInst* flag;   // i1: nonzero while the variable owns its value
        llvm::Function*   drop;   // destroys a value of the variable's type
    };
    struct DropScope {
        std::vector<OwnedLocal> locals;
        bool                    loop = false;   // the body of a loop: break and continue stop here
    };
    std::vector<DropScope>                       scopes_;
    std::map<const Symbol*, OwnedLocal>          owned_;
    std::map<const Symbol*, bool>                needs_drop_memo_;
    std::map<const Symbol*, llvm::Function*>     drop_fns_;
    std::deque<const Symbol*>                    drop_queue_;
    const types::Type*                           result_ = nullptr;   // the result of the function

    /// A type needs a destructor when it has one, or when a field or payload does.
    bool needs_drop(const types::Type* t) {
        if (!t || !t->is(Kind::user) || !t->symbol) return false;
        return symbol_needs_drop(t->symbol);
    }

    bool symbol_needs_drop(const Symbol* s) {
        if (auto it = needs_drop_memo_.find(s); it != needs_drop_memo_.end()) return it->second;
        needs_drop_memo_[s] = false;   // a type cannot contain itself by value; this guards the scan
        bool result         = false;
        if (s->kind == SymbolKind::enum_) {
            for (const Symbol* a : s->member_order) {
                if (a->alternative && a->alternative->payload && needs_drop(payload_of(a))) result = true;
            }
        } else {
            for (const auto& [name, overloads] : s->members) {
                for (const Symbol* m : overloads) {
                    if (m->function() && m->function()->is_destructor) result = true;
                }
            }
            for (const Symbol* f : fields_of(s)) {
                if (needs_drop(field_type(f))) result = true;
            }
        }
        needs_drop_memo_[s] = result;
        return result;
    }

    /// The function that destroys a value of user type `t`; it is generated later.
    llvm::Function* drop_function(const types::Type* t) {
        const Symbol* s = t->symbol;
        if (auto it = drop_fns_.find(s); it != drop_fns_.end()) return it->second;
        std::vector<std::string> parts = split_module(s->module ? s->module->name : "");
        parts.push_back(s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {llvm::PointerType::get(ctx_, 0)}, false),
                                          llvm::Function::InternalLinkage, "rd_" + mangle_parts(parts), mod_.get());
        drop_fns_[s] = fn;
        drop_queue_.push_back(s);
        return fn;
    }

    /// Runs the destructor (if any), then destroys the fields in reverse declaration order, or the
    /// live payload of an enum.
    void gen_drop(const Symbol* s) {
        llvm::Function*                  fn       = drop_fns_[s];
        llvm::IRBuilderBase::InsertPoint saved_ip = b_.saveIP();
        llvm::Function*                  saved_fn = cur_fn_;
        llvm::Value*                     saved_self = self_;
        auto                             saved_locals = locals_;
        auto                             saved_scopes = scopes_;
        auto                             saved_loops  = loops_;
        cur_fn_ = fn;
        self_   = nullptr;
        locals_.clear();
        scopes_.clear();
        loops_.clear();
        llvm::Value* self = fn->getArg(0);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        if (s->kind == SymbolKind::enum_) {
            llvm::Type*  et  = user_type(s);
            llvm::Value* tag = b_.CreateLoad(enum_tag_type(s), b_.CreateStructGEP(et, self, 0));
            llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx_, "drop.done", fn);
            llvm::SwitchInst* sw   = b_.CreateSwitch(tag, done);
            for (const Symbol* a : s->member_order) {
                if (!a->alternative || !a->alternative->payload) continue;
                const types::Type* p = payload_of(a);
                if (!needs_drop(p)) continue;
                llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "drop.case", fn);
                sw->addCase(tag_constant(a), bb);
                b_.SetInsertPoint(bb);
                b_.CreateCall(drop_function(p), {b_.CreateStructGEP(et, self, 1)});
                b_.CreateBr(done);
            }
            b_.SetInsertPoint(done);
        } else {
            llvm::Type* st = user_type(s);
            for (const auto& [mname, overloads] : s->members) {
                for (const Symbol* m : overloads) {
                    if (m->function() && m->function()->is_destructor) b_.CreateCall(function_of(m), {self});
                }
            }
            std::vector<const Symbol*> fields = fields_of(s);
            for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
                const types::Type* ft = field_type(*it);
                if (needs_drop(ft)) b_.CreateCall(drop_function(ft), {b_.CreateStructGEP(st, self, field_index(*it))});
            }
        }
        b_.CreateRetVoid();
        cur_fn_    = saved_fn;
        self_      = saved_self;
        locals_    = saved_locals;
        scopes_    = saved_scopes;
        loops_     = saved_loops;
        b_.restoreIP(saved_ip);
    }

    void open_scope(bool loop = false) { scopes_.push_back(DropScope{{}, loop}); }

    /// Ends the innermost scope: its owned locals are destroyed, last declared first.
    void close_scope() {
        if (scopes_.empty()) return;
        if (!terminated()) destroy_from(scopes_.size() - 1);
        scopes_.pop_back();
    }

    /// Destroys the owned locals of the scopes from `first` inwards without leaving them (before
    /// `return`, `break` and `continue`).
    void destroy_from(std::size_t first) {
        for (std::size_t i = scopes_.size(); i-- > first;) {
            const auto& locals = scopes_[i].locals;
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) drop_if(it->flag, it->drop, it->slot);
        }
    }

    /// The index of the innermost loop body (0 when there is none).
    std::size_t loop_scope() const {
        for (std::size_t i = scopes_.size(); i-- > 0;) {
            if (scopes_[i].loop) return i;
        }
        return 0;
    }

    /// Calls `drop` on the object at `addr` when `flag` is set.
    void drop_if(llvm::AllocaInst* flag, llvm::Function* drop, llvm::Value* addr) {
        llvm::BasicBlock* yes = llvm::BasicBlock::Create(ctx_, "drop", cur_fn_);
        llvm::BasicBlock* no  = llvm::BasicBlock::Create(ctx_, "drop.done", cur_fn_);
        b_.CreateCondBr(b_.CreateLoad(llvm::Type::getInt1Ty(ctx_), flag), yes, no);
        b_.SetInsertPoint(yes);
        b_.CreateCall(drop, {addr});
        b_.CreateBr(no);
        b_.SetInsertPoint(no);
    }

    /// Declares the owned local `sym` (stored at `slot`). `owns_now` is true for parameters.
    void own(const Symbol* sym, llvm::AllocaInst* slot, const types::Type* t, bool owns_now) {
        llvm::AllocaInst* flag = entry_alloca(llvm::Type::getInt1Ty(ctx_), "df_" + sym->name);
        b_.CreateStore(llvm::ConstantInt::getBool(ctx_, owns_now), flag);
        OwnedLocal o{slot, flag, drop_function(t)};
        scopes_.back().locals.push_back(o);
        owned_[sym] = o;
    }

    void set_flag(const Symbol* sym, bool value) {
        b_.CreateStore(llvm::ConstantInt::getBool(ctx_, value), owned_[sym].flag);
    }

    /// The owned local that `e` names, looking through `move`; null for anything else.
    const OwnedLocal* owned_path(const Expr& e) {
        const Expr* x = &e;
        if (x->kind == ExprKind::unary && x->as<UnaryExpr>().op == UnaryOp::move) x = x->as<UnaryExpr>().operand.get();
        if (x->kind != ExprKind::path) return nullptr;
        auto it = owned_.find(x->as<PathExpr>().symbol);
        return it == owned_.end() ? nullptr : &it->second;
    }

    /// After a value was moved out of `e`, the local no longer owns it.
    void moved_out(const Expr& e) {
        if (const OwnedLocal* o = owned_path(e)) b_.CreateStore(llvm::ConstantInt::getFalse(ctx_), o->flag);
    }

    /// Evaluates `e` for its effects. A value that needs a destructor is destroyed at once.
    void discard(Expr& e) {
        if (!needs_drop(e.type) || e.kind == ExprKind::path) {
            gen_expr(e);
            return;
        }
        llvm::Value* v = gen_expr(e);
        if (!v || terminated()) return;
        llvm::AllocaInst* slot = entry_alloca(v->getType(), "discarded");
        b_.CreateStore(v, slot);
        b_.CreateCall(drop_function(e.type), {slot});
    }

    // --- state ------------------------------------------------------------------------------

    sema::Program&                                    program_;
    types::TypeContext&                               tc_;
    const SourceManager&                              sources_;
    DiagnosticEngine&                                 diags_;
    LlvmOptions                                       options_;
    llvm::LLVMContext&                                ctx_;
    std::unique_ptr<llvm::Module>                     mod_;
    llvm::IRBuilder<>                                 b_;
    Span                                              cur_{};
    llvm::Function*                                   cur_fn_ = nullptr;
    llvm::Function*                                   rift_main_ = nullptr;
    llvm::Function*                                   rift_panic_str_ = nullptr;
    llvm::Function*                                   rift_exit_status_ = nullptr;
    llvm::Value*                                      self_ = nullptr;
    llvm::StructType*                                 loc_type_ = nullptr;
    std::map<const Symbol*, llvm::Function*>          fns_;
    std::deque<const Symbol*>                         queue_;
    std::map<const Symbol*, llvm::AllocaInst*>        locals_;
    std::map<std::pair<FileId, std::uint32_t>, llvm::Constant*> locs_;
    std::vector<Loop>                                 loops_;
    std::set<std::tuple<FileId, std::uint32_t, std::string>>    reported_;
};

} // namespace

std::string generate_llvm_ir(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                             LlvmOptions options) {
    llvm::LLVMContext ctx;
    Gen               gen(program, sources, diags, options, ctx);
    std::unique_ptr<llvm::Module> mod = gen.run();
    if (!mod) return {};
    std::string           printed;
    llvm::raw_string_ostream module_out(printed);
    mod->print(module_out, nullptr);
    module_out.flush();
    // The runtime defines the functions and intrinsics it shares with the module; the module's
    // declarations of them would clash with those.
    std::string runtime_text = llvm_runtime_ir();
    std::set<std::string> provided;
    for (std::string_view line : split_lines(runtime_text)) {
        if (!line.starts_with("define ") && !line.starts_with("declare ")) continue;
        std::size_t at = line.find('@');
        if (at == std::string_view::npos) continue;
        std::size_t end = line.find('(', at);
        provided.insert(std::string(line.substr(at + 1, end - at - 1)));
    }
    std::string text;
    for (std::string_view line : split_lines(printed)) {
        if (line.starts_with("declare ")) {
            std::size_t at  = line.find('@');
            std::size_t end = at == std::string_view::npos ? at : line.find('(', at);
            if (end != std::string_view::npos && provided.count(std::string(line.substr(at + 1, end - at - 1)))) continue;
        }
        text.append(line);
    }
    return text + "\n" + runtime_text;
}

bool build_executable(std::string_view ir, const std::string& out_path, const std::string& linker, std::string& error) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> mod = llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(ir.data(), ir.size()), "rift.ll"), err, ctx);
    if (!mod) {
        std::string text;
        llvm::raw_string_ostream os(text);
        err.print("riftc", os);
        error = "internal error: the generated LLVM IR does not parse\n" + text;
        return false;
    }
    std::string bad;
    llvm::raw_string_ostream verify_out(bad);
    if (llvm::verifyModule(*mod, &verify_out)) {
        error = "internal error: the generated LLVM IR is invalid\n" + verify_out.str();
        return false;
    }
    std::string triple_name = llvm::sys::getDefaultTargetTriple();
    std::string lookup_error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple_name, lookup_error);
    if (!target) {
        error = "no LLVM target for " + triple_name + ": " + lookup_error;
        return false;
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple_name, "generic", "", options, llvm::Reloc::PIC_));
    mod->setTargetTriple(triple_name);
    mod->setDataLayout(machine->createDataLayout());

    std::string object_path = out_path + ".o";
    std::error_code ec;
    {
        llvm::raw_fd_ostream file(object_path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            error = "cannot write " + object_path + ": " + ec.message();
            return false;
        }
        llvm::legacy::PassManager pm;
        if (machine->addPassesToEmitFile(pm, file, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            error = "the target cannot emit object files";
            return false;
        }
        pm.run(*mod);
    }
    std::string command = linker + " -o '" + out_path + "' '" + object_path + "' -lm";
    int         status  = std::system(command.c_str());
    llvm::sys::fs::remove(object_path);
    if (status != 0) {
        error = "the linker '" + linker + "' failed";
        return false;
    }
    return true;
}

} // namespace rift

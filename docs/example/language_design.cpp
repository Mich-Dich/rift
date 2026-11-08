// employees.slate
import std;

struct employee {
    std::string name;
    u32         age;
    f64         salary;
};

enum parse_error : u8 {
    bad_line = 0,
    bad_number,
};


std::result<employee, parse_error> parse_line(str line) {

    const std::list<str> parts = line.split(',');
    if (parts.len() != 3)
        return std::err(parse_error::bad_line);

    const u32 age = parts[1].parse<u32>()?;
    const f64 salary = parts[2].parse<f64>()?;

    return std::ok(employee {
        .name   = std::string::from(parts[0]),
        .age    = age,
        .salary = salary,
    });
}


std::result<std::list<employee>, std::io_error> load(std::filesystem::path path) {

    std::file in = std::file::open(path)?;
    mut std::list<employee> employees = std::list<employee>::new();

    for (const str line : in.lines()) {
        if (line.is_empty())
            continue;

        match (parse_line(line)) {
            std::ok(e)  => { employees.push(e); },
            std::err(_) => { std::eprintln("skipping bad line: {line}"); },
        }
    }
    return std::ok(employees);
}


std::exit_code main() {

    const std::list<employee> employees = match (load(std::filesystem::get_executable_path() / "employees.csv")) {
        std::ok(list) => list,
        std::err(e)   => {
            std::eprintln("error: {e}");
            return std::exit_code::failure;
        },
    };

    mut std::list<employee> filtered = employees.filter([](const employee& e) { return e.age >= 30; });

    filtered.sort_by([](const employee& {a, b}) { return b.salary <=> a.salary; });

    for (const employee& e : filtered)
        std::println("{e.name:<20} {e.age:>3} {e.salary:>10.2}");

    return std::exit_code::success;
}

/*
═══════════════════════════════════════════════════════════════════
SLATE — DESIGN RULES  (working name, subject to change)
═══════════════════════════════════════════════════════════════════

-- NAMING --------------------------------------------------------
· Everything is snake_case: types, functions, fields, enum values, modules. No PascalCase, no camelCase, no SCREAMING_CASE

-- NUMERIC TYPES (language built-ins, no std namespace) ----------
· Signed:   i8, i16, i32, i64, i128
· Unsigned: u8, u16, u32, u64, u128
· Float:    f16, f32, f64, f128
· FORBIDDEN: int, long, short, unsigned, float, double, char, size_t, ptrdiff_t — these do not exist
· No implicit numeric conversions, no implicit narrowing. Every conversion is explicit
· Integer literals take their type from context or from an explicit suffix (5u8, 5i64, 5u128). Silent truncation is a compile error

-- BOOLEANS ------------------------------------------------------
· bool is the only boolean type
· No int↔bool, no pointer truthiness. if/while/match require bool. `if (ptr)` is a compile error; write `if ptr != null`

-- VARIABLES -----------------------------------------------------
· Const by default:  u32 x = 5;    → immutable
· Mutation must be stated:  mut u32 x = 5;
· Uninitialized variables are FORBIDDEN. Every declaration must include an initializer.  `u32 x;` is a compile error
· `const` keyword is legal but redundant (it is the default)

-- ENUMS ---------------------------------------------------------
· Underlying type is REQUIRED: enum name : <integer_type> { ... };
· Values auto-increment from the last explicit value: enum e : u8 { a = 0, b, c }   // a=0, b=1, c=2
· Always scoped: name::value. Never implicitly convertible to int
· std::exit_code is provided by the standard library

-- FUNCTIONS -----------------------------------------------------
· Signature:  <return_type> <name>(<parameters>) { ... }
· No `fn`, no `let`, no `def`, no `func`, no `var`
· Parameters are const by default; `mut` allows mutation through the parameter
· Parameter grouping: adjacent parameters of the same type can be grouped with braces: (const T& a, const T& b)  ==  (const T& {a, b})

-- MAIN ----------------------------------------------------------
· `main` returns any sized integer or std::exit_code (The runtime truncates to the OS exit code.)
· `main` may take no arguments, or: std::array<std::string> parameter

-- STANDARD LIBRARY ----------------------------------------------
· All stdlib types and functions live in `std::`
· Canonical names:
std::string   std::str      std::list     std::array
std::result   std::ok       std::err      std::option
std::some     std::none     std::file     std::io_error
std::exit_code
std::filesystem::path
std::filesystem::get_executable_path
std::println  std::eprintln
· `import std;` brings the stdlib into scope

-- ERRORS --------------------------------------------------------
· std::result<T, E> for all fallible operations
· Construct with std::ok(x) / std::err(e)
· `?` propagates errors, converting E to the caller's error type via std::convert
· `match` is the canonical way to consume a result — exhaustive, no fallthrough
· No exceptions in safe code

-- MATCH ---------------------------------------------------------
· Exhaustive: compiler error if any case is missing
· Expression: returns a value
· No fallthrough: every arm is independent
· Destructures std::result, std::option, tuples, structs

-- FILE PATHS ----------------------------------------------------
· String LITERALS implicitly convertible to std::filesystem::path trigger a WARNING when passed to a filesystem function
· GOOD:  load(std::filesystem::get_executable_path() / "x.csv")
· WARN:  load("x.csv")
· Non-literal strings (from user input, etc.) are the caller's responsibility and do not warn

-- MEMORY --------------------------------------------------------
· Safe by default. No manual new/delete in safe code
· `unsafe { ... }` for raw pointers and manual memory
· Borrowed std::str and owned std::string are distinct types; no implicit conversion
· RAII: values own resources; scope end releases them

-- OTHER ---------------------------------------------------------
· `struct` = plain-data record, value semantics, no vtables
· Lambdas: [](<params>) { <body> }  — explicit types preferred
· for-each:  for (const T& x : collection) { ... }
· Explicit beats implicit everywhere. If it isn't written down, the compiler refuses to guess

═══════════════════════════════════════════════════════════════════
*/

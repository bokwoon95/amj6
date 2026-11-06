## General
- DO NOT modify files unless explicitly asked. If a problem/error is pasted, EXPLAIN how it came about, not FIX IT. Present your findings, I will decide whether or not you should fix it.
- DO NOT USE UTILITY FUNCTIONS IF ITS BODY CAN BE REPLICATED IN A FEW STATEMENTS.
- DO NOT MODIFY AGENTS.md UNLESS EXPLICITLY STATED.
- Push Ifs Up And Fors Down. If you have complex control flow, better to fit it on a screen in a single function, rather than spread throughout the file.
- When we engage in exploratory back-and-forth conversational questioning, keep your answers concise so you don't bloat the context window.
- Avoid using line continuations for shell script documentation or code. It's easier to copy paste.
- Don't add tests unless explicitly asked for it.
- Files are written in such a way that each successive file only depends on files that come before it. This makes it easy to rewrite the codebase from scratch file-by-file.
- DO NOT use short form variable names for longer type names. If a type is ProviderOptions, the variable _SHOULD_ be providerOptions, NOT options. If the variable name is too long, come up with a shorter type name. This rule helps prevent future variable name collisions.

## C++
- Follow .clang-format.
- Always run ./scripts/lint.ps1 -changed -quiet when source files were added or changed.
- Use ./scripts/build.ps1 -quiet to build the project quietly and not pollute the context window.
- Write C++ like Go: prefer straightforward conditionals and loops, useful zero values, and structs/classes.
- Write C++ that is NOT intimidating for a Go/Java programmer to read!! If code can be expressed CORRECTLY in slower but familiar Go/Java constructs OR faster but completely alien C++ constructs, pick the simple Go/Java constructs everytime!!
    - We are writing a networked desktop app, NOT a high performance number cruncher!! Correctness, readability, performance, in that order!! We are not competing against other C++ code, our comptetitors are programs written in Python or JavaScript so simply being written in C++ is already a huge performance win!!
    - DO NOT use member initializer lists. Always set each this->field manually like it was a dumb Java program.
    - Avoid using std::move() as a microoptimization if stupidly copying is not catastrophically slow. It's not about being imposing a small syntactic overhead, it's about imposing a _huge_ understanding overhead as a Go programmer would have to ask why std::move is even needed (when omitting it would be JUST as correct just with a little more allocations).
    - DO NOT use emplace variants unless required for correctness. Prefer explicit values with push_back, insert, or insert_or_assign, preserving overwrite semantics.
    - DO NOT use function-try-blocks; put try/catch inside the function body.
- Use std types where possible.
- Prefer adding minimal structs and fields.
- DO NOT define macros. There is always a non-macro way to do something, or else just bite the bullet and repeat the code.
- DO NOT use auto when the type can be explicitly named EXCEPT for lambdas, so we don't have to step through a std::function wrapper during debugging.
- DO NOT use *T t = new T, use *T t = new T().
- DO NOT use uniform initialization syntax i.e. int64_t num{0}.
- DO NOT use direct-list initialization for named variables (`T t{value}`).
    1. Use `T t = value;` for literals and existing values.
    2. For default construction, use `T t;` (not `T t{};`).
    3. For parameterized construction, use `T t(a, b);`. If this triggers the Most Vexing Parse, wrap constructor-call arguments in extra parentheses: `T t((A()));`.
    4. Braced initializers are encouraged for containers, pairs, tuples, and structs: `T t = {...};` or `push_back({...})`.
- Every struct data member MUST have an explicit default member initializer, even when omitting it would produce identical behavior. Use = {}; for containers, = ""; for strings, = nullptr; for pointers, and the appropriate zero value for scalars. Never omit an initializer merely because the type default-constructs itself. Before completing a struct change, check every field for compliance.
    - Default member initializers should default to the type's zero value, like Go. It is up to the callers to set the fields to any other meaningful value besides the zero value.
- Prefer struct designated initializers (aka struct literals) over struct constructors.
    - Prefer struct literals if it means we can avoid declaring a variable.
    - Do NOT define designated initializers in one line. EACH MEMBER SHOULD BE IN ITS OWN LINE.
        - /* BAD */ throw detail::Failure{.error = detail::portableError("InvalidResult", "Invalid iterator cleanup result")};
        - /* GOOD */ throw detail::Failure{
            .error = detail::portableError("InvalidResult", "Invalid iterator cleanup result"),
        }
- DO NOT use underscore suffixes or m_ prefixes to denote member fields. Just use normal names. If a private field and a public getter method would clash, just use xxx and getXXX().
- DO NOT use default arguments. Be pedantic that callers should supply ALL arguments. Just like in Go.
- Use const BEFORE type i.e. west const.
- Floating point literals MUST always specify both the integer part and fractional part i.e. 0.5.
- Explicitly handle division by zero with an if check.
- Don't use unsigned ints if you can help it. Avoid size_t. Prefer int64_t. The only exceptions are when working with standard library functions, in which case we should be as idiomatic as possible.
- We are not writing high performance code, readability is more important:
    - Do not use struct of arrays if array of structs is more natural.
    - Semantic order of class members is more important than struct packing.
    - Do NOT use #ifdef. Use only #if defined().
- Add NOLINT(bugprone-throwing-static-initialization) to globals whose only potential initialization failure is allocation failure; startup termination is acceptable. Account for Windows STL allocations even when macOS lint passes.
- NEVER mix platform-specific implementations in one file. Put them in separate \*\_windows.cpp, \*\_darwin.cpp, and \*\_linux.cpp files. Guard each ENTIRE file, including all includes, with #if defined(_WIN32), #if defined(__APPLE__), or #if defined(__linux__), respectively.
- Do NOT use #ifdef. Use only #if defined().
- For Apple, highly prefer the internal headers (sys/fcntl.h) over the portable ones (fcntl.h) to placate the clangd linter.
- Never implicitly convert between integers and bools. Test integers with == 0 / != 0; test bools directly or with !. Class types with explicit operator bool() (e.g. std::error_code) may be tested directly.
- Prefer simple container operations: .at() for indexed access, .push_back() for single-value appends, and .insert_or_assign() for map writes.
- For map reads where absence is expected, use .contains() followed by .at().
- Use emplacement only where modernize-use-emplace requires it; do not disable or suppress that check.
- Preserve bulk/range operations such as .insert(end(), first, last); do not replace them with single-element loops.
- Preserve existing semantics: guard writes with .contains() when existing entries must not be overwritten.

## JavaScript
- JSDoc: Use () => Type, not { (): Type }, for plain function types.
- JSDoc: Use Array<Type> and ReadonlyArray<Type>, never Type[] or readonly Type[].

## Python
- Use typed Python as much as possible.
- Write Python like Go: prefer straightforward conditionals and loops, useful zero values, and dataclasses for structs.
- DO NOT use list comprehensions.
- DO NOT use `from foo.bar import Baz`; import the module and fully qualify names `import foo; a = foo.bar.Baz()`.
- DO NOT use vanilla python or pip, only use uv. Give the user the command(s) that installs uv if uv is not available.
- DO NOT use python function scoping, treat it as though Python only supports strict block-level lexical scoping.
- DO NOT use python ternary expressions. Declare a default value first if possible, the overwrite it if some condition holds true.
- Before completing Python changes, run `uv run ruff format .`, `uv run ruff check --fix .` and `uv run pyright`.

## SQL
- Table names should use singular form, not plural form.
- Never use the bare "id" by itself e.g. use "document_id" for a "document" table.
- Prefer UUIDv7 over autoincrement for primary keys. Either generate it application-side or use uuidv7() builtins.
- Use Postgres-naming convention for constraint/index names.
- For schema migrations, strictly use a roll-forward only migration strategy. Never do a down migration.

## C++ Safety Rules

Prioritize memory safety, defined behavior, and predictable failure over micro-optimizations. Prefer checked operations even when they incur runtime overhead.

### Checked access
- Use `.at(index)` instead of `operator[]` for vectors, arrays, strings, and other containers providing checked indexing.
- Use `std::span::at()` when available (C++26); otherwise explicitly validate indices before indexing spans.
- Use `.value()` instead of `*` or `->` when accessing `std::optional` and `std::expected` values.
- Use `.at(key)` for map lookups where the key must exist. Use `.find()` or `.contains()` where absence is expected. Use `operator[]` only when insertion is intentional.
- Use `std::get_if` instead of `std::get` for variants when a missing alternative should be handled without exceptions.
- Prefer pointer-returning `std::any_cast` when an incorrect type is an expected possibility.
- Check `.empty()` before calling `.front()`, `.back()`, or `.pop_back()`.

### Memory and lifetime safety
- Prefer stack allocation, RAII, standard containers, and smart pointers.
- Avoid explicit `new`, `delete`, `malloc`, and `free`.
- Prefer `std::unique_ptr` for dynamic ownership; use `std::shared_ptr` only when shared ownership is necessary.
- Prefer references for required borrowed objects and pointers for nullable borrowed objects.
- Never dereference a pointer unless its validity is established.
- Never dereference end iterators or use invalidated iterators.
- Never retain references, pointers, spans, string_views, or iterators beyond the lifetime or validity of their underlying storage.
- Account for iterator, pointer, and reference invalidation whenever containers are mutated.
- Prefer typed containers and algorithms over raw pointer arithmetic and byte manipulation.
- Avoid `reinterpret_cast`, C-style casts, and unions for type punning.
- Use `std::bit_cast` where its requirements are satisfied.
- Avoid returning references or pointers to local objects.
- Do not use `std::move` on objects that must retain their original value.

### Arithmetic safety
- Validate divisors before integer division or modulo; handle signed minimum divided by -1.
- Prevent signed integer overflow and unsigned wraparound unless explicitly intended.
- Use checked arithmetic for values that may exceed their type's range.
- Validate numeric ranges before narrowing conversions.
- Validate shift counts and signedness before bit shifts.
- Prefer `std::from_chars` or explicitly checked conversions over `atoi`/`atof`.

### C APIs and buffer handling
- Prefer `std::string`, `std::vector`, `std::array`, and `std::span` over C-style buffers.
- Avoid unbounded C string functions such as `strcpy`, `strcat`, and `sprintf`.
- When using C APIs, explicitly validate buffer capacities, null termination, ownership, and lifetimes.
- Do not assume `std::copy`, `memcpy`, or `memmove` automatically checks buffer bounds.

### Exceptions and error handling
- Prefer checked operations that throw or return explicit errors over unchecked operations with undefined behavior.
- Handle expected failures explicitly.
- Never use `assert()` as the only validation for inputs or conditions that can fail in release builds.
- Do not suppress exceptions or errors merely to allow execution to continue.

### Performance exceptions
- Do not replace checked operations with unchecked alternatives solely for presumed performance.
- Permit unchecked operations only where invariants make them provably safe and a measured performance benefit justifies them.
- Document the relevant invariant and its enforcement whenever an unchecked operation is intentionally used.

## Namespaces and type names
- Use unqualified project-defined names within their declaring namespace. This also applies in both directions between a namespace and its nested anonymous namespaces.
- Outside their declaring namespace, qualify functions, types, and other namespace members with exactly one namespace qualifier, using the namespace’s basename. Write `provider::Provider`, `errors::Error`, and `context::Context`, not `amj::provider::Provider` or unqualified imported names. Members declared in enclosing or sibling named namespaces MUST also be qualified.
- Namespace aliases must be declared independently in every .hpp and .cpp file that uses them, at the beginning of the relevant namespace body. Never rely on an alias declared in an included header, even when it is visible and refers to the same namespace. Repeat identical alias declarations where necessary. For example, if both example.hpp and example.cpp use chrono::steady_clock, both files MUST explicitly declare namespace chrono = std::chrono;.
- Declare namespace aliases at the beginning of the namespace body, before other declarations, whenever the namespace would otherwise require multiple qualification levels. Alias names MUST match the target namespace’s basename. This applies to project and external namespaces alike:
```cpp
namespace amj::example {
namespace filesystem = std::filesystem;
namespace chrono = std::chrono;
namespace ranges = std::ranges;
namespace errors = amj::errors;
namespace context = amj::context;
namespace keyring = amj::keyring;
namespace provider = amj::provider;
}
```
- BOTH .cpp and .hpp files should define THEIR OWN namespace aliases. Do not rely on a namespace alias defined in a .hpp file.
- Namespace declarations and namespace-alias targets may use fully qualified namespace paths; references to namespace members MUST use at most one namespace qualifier. For example, use std::vector and filesystem::path, not std::filesystem::path.
- Do not introduce type aliases, using-declarations, or using-directives merely to shorten names. Use namespace aliases instead.
- Namespaces MUST exactly mirror the directory structure beneath `src/`, with `amj` corresponding to `src/`. For example, types declared in `src/provider/openai/client.hpp` belong to `amj::provider::openai`.
- Do not add namespace levels that have no corresponding directory, or omit namespace levels corresponding to directories.
- Namespace basenames MUST be unique across the codebase, regardless of their parent namespace or directory. Choose distinct basenames so namespace aliases never collide.

## Header include order
- Group `#include` directives into separate blocks in this order:
    1. The source file’s corresponding header, alone (source files only).
    2. Other project headers.
    3. Third-party library headers.
    4. Standard-library headers.
    5. System/platform headers.
- Separate blocks with exactly one blank line. Omit empty blocks.
- Sort headers alphabetically within each block; never merge or reorder blocks.
- When headers require a specific dependency order, preserve it using narrowly scoped `clang-format off/on` comments and explain why.
- Platform-detection headers needed by preprocessor guards are exempt from normal include ordering. Place them immediately after the outer platform guard, followed immediately by the more specific guard.
- For macOS implementation files, use this opening, then follow normal include ordering inside `#if TARGET_OS_OSX`:
```cpp
    #if defined(__APPLE__)
    #include <TargetConditionals.h> // IWYU pragma: keep
    #if TARGET_OS_OSX

    #include "keyring.hpp"
```

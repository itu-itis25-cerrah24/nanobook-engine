#pragma once

// Minimal self-contained unit test harness (no external dependencies).

#include <cstddef>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace nbtest {

struct TestCase {
    std::string_view name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline std::size_t& current_failures() {
    static std::size_t failures = 0;
    return failures;
}

struct Registrar {
    Registrar(std::string_view name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <class T>
void print_value(std::ostream& os, const T& value);

template <class T>
void print_value(std::ostream& os, const std::optional<T>& value) {
    if (value) {
        print_value(os, *value);
    } else {
        os << "nullopt";
    }
}

template <class T>
void print_value(std::ostream& os, const T& value) {
    if constexpr (requires { to_string(value); }) {
        os << to_string(value);
    } else if constexpr (requires { os << value; }) {
        os << value;
    } else {
        os << "<unprintable>";
    }
}

inline void fail(const char* file, int line, std::string_view expr) {
    ++current_failures();
    std::cerr << "    FAILED " << file << ':' << line << "  " << expr << '\n';
}

template <class A, class B>
void check_eq(const A& a, const B& b, const char* file, int line, const char* expr_a,
              const char* expr_b) {
    if (a == b) return;
    ++current_failures();
    std::cerr << "    FAILED " << file << ':' << line << "  " << expr_a << " == " << expr_b
              << "\n           lhs = ";
    print_value(std::cerr, a);
    std::cerr << ", rhs = ";
    print_value(std::cerr, b);
    std::cerr << '\n';
}

inline int run_all() {
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& tc : registry()) {
        current_failures() = 0;
        tc.fn();
        if (current_failures() == 0) {
            ++passed;
            std::cout << "[ PASS ] " << tc.name << '\n';
        } else {
            ++failed;
            std::cout << "[ FAIL ] " << tc.name << " (" << current_failures() << " check(s))\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << registry().size()
              << " total\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace nbtest

#define NB_TEST(name)                                                   \
    static void name();                                                 \
    static const ::nbtest::Registrar name##_registrar{#name, &name};    \
    static void name()

#define NB_CHECK(cond)                                                  \
    do {                                                                \
        if (!(cond)) ::nbtest::fail(__FILE__, __LINE__, #cond);         \
    } while (0)

#define NB_CHECK_EQ(a, b) ::nbtest::check_eq((a), (b), __FILE__, __LINE__, #a, #b)

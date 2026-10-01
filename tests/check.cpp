// Runs the tests of zchat_tests (see check.hpp): all of them, or those whose name contains the first argument.

#include "check.hpp"

#include <exception>

int main(int argc, char** argv) {
    using namespace zchat::check;
    const std::string_view filter = argc > 1 ? argv[1] : "";
    int run = 0;
    for (const Test& test : tests()) {
        if (test.name.find(filter) == std::string::npos) {
            continue;
        }
        ++run;
        const int before = failures();
        try {
            test.run();
        } catch (const Stop&) {
        } catch (const std::exception& e) {
            fail(test.name, 0, std::format("exception: {}", e.what()));
        } catch (...) {
            fail(test.name, 0, "unknown exception");
        }
        std::cout << std::format("{} {}\n", failures() == before ? "ok  " : "FAIL", test.name);
    }
    std::cout << std::format("{} tests, {} failed checks\n", run, failures());
    return failures() == 0 && run > 0 ? 0 : 1;
}

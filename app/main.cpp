#include <exception>
#include <iostream>

#include "cli.h"

int main(int argc, char** argv) {
    try {
        return tree::run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";

        return 1;
    } catch (...) {
        std::cerr << "error: unknown fatal error\n";

        return 1;
    }
}
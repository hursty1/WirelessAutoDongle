#include "iap2Probe.h"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    iap2probe::Limits limits{180, 250, 300, 40};
    auto report = [](const std::string& stage, const std::string& outcome, const std::string& detail) {
        std::cout << stage << '\t' << outcome << '\t' << detail << std::endl;
    };
    return static_cast<int>(iap2probe::run(std::atoi(argv[1]), report, limits));
}

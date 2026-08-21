#include "core/semver.h"

#include <iostream>
#include <string>
#include <vector>

int main() {
    struct Case {
        std::string candidate;
        std::string current;
        bool expected;
    };
    const std::vector<Case> cases{
        {"1.0.1", "1.0.0", true},
        {"1.0.0", "1.0.0-rc.6", true},
        {"0.1.0-rc.7", "0.1.0-rc.6", true},
        {"0.1.0-rc.5", "0.1.0-rc.6", false},
        {"2.0.0", "10.0.0", false},
    };
    for (const auto& test : cases) {
        const auto actual = dsh::version::is_newer(test.candidate, test.current);
        if (actual != test.expected) {
            std::cerr << "is_newer(" << test.candidate << ", " << test.current << ") failed\n";
            return 1;
        }
    }
    return 0;
}


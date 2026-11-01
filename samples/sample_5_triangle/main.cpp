#include "scenario.h"
int main() {
    try {
        const auto result = NTriangle::Run(3, 2, 240, true);
        std::cout << "submitted=" << result.Submitted << " completed=" << result.Completed
                  << " recycled=" << result.Recycled << '\n';
        return result.Submitted == result.Completed && result.Completed == result.Recycled ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

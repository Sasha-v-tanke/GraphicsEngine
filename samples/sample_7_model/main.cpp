#include "scenario.h"

int main() {
    try {
        const auto result = NTriangle::Run<NModel::Scene>(3, 2, 600, true);
        std::cout << "submitted=" << result.Submitted << " completed=" << result.Completed
                  << " recycled=" << result.Recycled << '\n';
        return result.Submitted == 600 && result.Submitted == result.Completed && result.Completed == result.Recycled
                     ? 0
                     : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

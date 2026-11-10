#include "scenario.h"

int main() {
    try {
        const auto result = NSceneSwitching::Run(3, 2, 600, true);
        std::cout << "submitted=" << result.Submitted << " completed=" << result.Completed
                  << " recycled=" << result.Recycled << " loading=" << result.LoadingFrames
                  << " ready=" << result.ReadyFrames << " empty=" << result.EmptyFrames << '\n';
        return result.Submitted == 600 && result.Submitted == result.Completed && result.Completed == result.Recycled &&
                               result.LoadingFrames > 0 && result.ReadyFrames > 0 && result.EmptyFrames > 0
                     ? 0
                     : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

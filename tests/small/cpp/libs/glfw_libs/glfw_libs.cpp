#include "glfw.h"

#include <iostream>

int main() {
    GLFWwindow* window{};

    std::cout << "GraphicsEngine glfw lib smoke test\n"
              << "glfw window: " << window << '\n';

    return 0;
}

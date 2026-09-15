#include "glfw.h"
#include "glm.h"
#include "stb_image.h"
#include "tiny_obj_loader.h"
#include "vulkan.h"

#include <iostream>

int main() {
    const glm::mat4 identity{1.0F};
    const tinyobj::attrib_t attributes{};
    GLFWwindow* window{};
    VkInstance instance{};

    const unsigned char invalidImage[] = {};
    int width{};
    int height{};
    int channels{};
    const int imageInfo = stbi_info_from_memory(
        invalidImage,
        0,
        &width,
        &height,
        &channels
    );

    std::cout
        << "GraphicsEngine external smoke test\n"
        << "glm: " << identity[0][0] << '\n'
        << "tinyobj vertices: " << attributes.vertices.size() << '\n'
        << "glfw window: " << window << '\n'
        << "vulkan instance: " << instance << '\n'
        << "stb image info: " << imageInfo << '\n';

    return 0;
}

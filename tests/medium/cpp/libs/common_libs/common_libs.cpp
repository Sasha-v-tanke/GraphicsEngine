#include "glm.h"
#include "stb_image.h"
#include "tiny_obj_loader.h"

#include <iostream>

int main() {
    const glm::mat4 identity{1.0F};
    const tinyobj::attrib_t attributes{};

    const unsigned char invalidImage[] = {0};

    int width{};
    int height{};
    int channels{};

    const int imageInfo = stbi_info_from_memory(invalidImage, 1, &width, &height, &channels);

    std::cout << "GraphicsEngine common libraries smoke test\n"
              << "glm: " << identity[0][0] << '\n'
              << "tinyobj vertices: " << attributes.vertices.size() << '\n'
              << "stb image info: " << imageInfo << '\n';

    return 0;
}

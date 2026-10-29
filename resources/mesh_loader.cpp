#include "mesh_loader.h"
#include "mesh_loader_internal.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tiny_obj_loader.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NResources {

namespace {

using NMeshLoaderInternal::CanAppendMeshElements;

std::string_view NextToken(std::string_view& line) {
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        line = {};
        return {};
    }
    line.remove_prefix(first);
    const std::size_t end = line.find_first_of(" \t\r");
    const std::string_view token = line.substr(0, end);
    line.remove_prefix(token.size());
    return token;
}

bool IsFiniteNumber(std::string_view token) {
    if (token.starts_with('+')) {
        token.remove_prefix(1);
        if (token.starts_with('-')) {
            return false;
        }
    }
    float value = 0.0F;
    const std::from_chars_result result = std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size() && std::isfinite(value);
}

bool IsValidIndex(std::string_view token, std::size_t count) {
    if (token.starts_with('+')) {
        token.remove_prefix(1);
        if (token.starts_with('-')) {
            return false;
        }
    }
    int value = 0;
    const std::from_chars_result result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || value == 0) {
        return false;
    }
    const std::int64_t index = value;
    return index > 0 ? static_cast<std::uint64_t>(index) <= count : static_cast<std::uint64_t>(-index) <= count;
}

bool IsValidCorner(std::string_view token, const std::array<std::size_t, 3>& counts) {
    const std::size_t firstSlash = token.find('/');
    if (!IsValidIndex(token.substr(0, firstSlash), counts[0])) {
        return false;
    }
    if (firstSlash == std::string_view::npos) {
        return true;
    }
    token.remove_prefix(firstSlash + 1);
    const std::size_t secondSlash = token.find('/');
    if (secondSlash == std::string_view::npos) {
        return IsValidIndex(token, counts[2]);
    }
    const std::string_view texcoord = token.substr(0, secondSlash);
    return (texcoord.empty() || IsValidIndex(texcoord, counts[2])) &&
           IsValidIndex(token.substr(secondSlash + 1), counts[1]);
}

struct GeometrySource {
    std::string Text;
    std::size_t IndexCount = 0;
};

// tinyobjloader accepts some malformed tokens and silently skips invalid polygons.
// Validate the supported geometry subset before triangulation can discard that evidence.
std::expected<GeometrySource, std::string> ReadGeometry(std::istream& file) {
    std::string geometry;
    std::string line;
    std::array<std::size_t, 3> counts{};
    std::size_t indexCount = 0;
    std::size_t lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        std::string_view content = line;
        if (lineNumber == 1 && content.starts_with("\xEF\xBB\xBF")) {
            content.remove_prefix(3);
        }
        content = content.substr(0, content.find('#'));
        const std::string_view geometryLine = content;
        const std::string_view command = NextToken(content);
        if (command != "v" && command != "vn" && command != "vt" && command != "f") {
            continue;
        }
        const auto failure = [lineNumber](const std::string& reason) {
            return std::unexpected("line " + std::to_string(lineNumber) + ": " + reason);
        };
        std::size_t tokenCount = 0;
        for (std::string_view token = NextToken(content); !token.empty(); token = NextToken(content)) {
            ++tokenCount;
            if (command == "f" ? !IsValidCorner(token, counts) : !IsFiniteNumber(token)) {
                return failure("invalid, nonfinite or out-of-range geometry token");
            }
        }
        if (command == "f") {
            if (tokenCount < 3) {
                return failure("face requires at least three corners");
            }
            const std::size_t triangles = tokenCount - 2;
            if (triangles > std::numeric_limits<std::uint32_t>::max() / 3 ||
                !CanAppendMeshElements(indexCount, triangles * 3)) {
                return failure("triangle index count exceeds uint32 limit");
            }
            indexCount += triangles * 3;
        } else {
            const bool hasValidArity = command == "v"
                                             ? tokenCount == 3 || tokenCount == 4 || tokenCount == 6 || tokenCount == 7
                                     : command == "vn" ? tokenCount == 3
                                                       : tokenCount >= 1 && tokenCount <= 3;
            if (!hasValidArity) {
                return failure("invalid attribute component count");
            }
            const std::size_t attribute = command == "v" ? 0 : command == "vn" ? 1 : 2;
            if (counts[attribute] >= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                return failure("source attribute count exceeds tinyobjloader index limit");
            }
            ++counts[attribute];
        }
        // Ignore groups, materials and other non-geometry directives. In particular no .mtl I/O occurs.
        geometry.append(geometryLine);
        geometry.push_back('\n');
    }
    if (file.bad() || (!file.eof() && file.fail())) {
        return std::unexpected("cannot read OBJ data");
    }
    if (indexCount == 0) {
        return std::unexpected("OBJ has no triangle geometry");
    }
    return GeometrySource{std::move(geometry), indexCount};
}

struct Corner {
    int Position;
    int Normal;
    int Texcoord;

    bool operator==(const Corner&) const = default;
};

struct CornerHash {
    std::size_t operator()(const Corner& corner) const noexcept {
        std::size_t hash = 0;
        for (const int index: {corner.Position, corner.Normal, corner.Texcoord}) {
            hash ^= std::hash<int>{}(index) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        }
        return hash;
    }
};

std::expected<MeshData, std::string> ImportMesh(std::istream& file) {
    const std::expected<GeometrySource, std::string> geometry = ReadGeometry(file);
    if (!geometry) {
        return std::unexpected(geometry.error());
    }
    std::istringstream source{geometry->Text};
    tinyobj::attrib_t attributes;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warning;
    std::string error;
    if (!tinyobj::LoadObj(&attributes, &shapes, &materials, &warning, &error, &source, nullptr, true, false) ||
        !warning.empty() || !error.empty()) {
        return std::unexpected("invalid OBJ: " + error + warning);
    }
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint8_t> masks;
    std::unordered_map<Corner, std::uint32_t, CornerHash> corners;
    for (const tinyobj::shape_t& shape: shapes) {
        if (shape.mesh.indices.size() % 3 != 0 || !CanAppendMeshElements(indices.size(), shape.mesh.indices.size())) {
            return std::unexpected("invalid triangle topology or uint32 index overflow");
        }
        for (const tinyobj::index_t& index: shape.mesh.indices) {
            if (index.vertex_index < 0 ||
                static_cast<std::size_t>(index.vertex_index) >= attributes.vertices.size() / 3 ||
                index.normal_index < -1 ||
                (index.normal_index >= 0 &&
                 static_cast<std::size_t>(index.normal_index) >= attributes.normals.size() / 3) ||
                index.texcoord_index < -1 ||
                (index.texcoord_index >= 0 &&
                 static_cast<std::size_t>(index.texcoord_index) >= attributes.texcoords.size() / 2)) {
                return std::unexpected("OBJ attribute index out of range");
            }
            const Corner corner{index.vertex_index, index.normal_index, index.texcoord_index};
            const auto found = corners.find(corner);
            if (found != corners.end()) {
                indices.push_back(found->second);
                continue;
            }
            if (!CanAppendMeshElements(vertices.size(), 1)) {
                return std::unexpected("vertex count exceeds uint32 limit");
            }
            MeshVertex vertex{};
            const std::size_t position = static_cast<std::size_t>(index.vertex_index) * 3;
            vertex.Position = {attributes.vertices[position],
                               attributes.vertices[position + 1],
                               attributes.vertices[position + 2]};
            std::uint8_t mask = 0;
            if (index.normal_index >= 0) {
                const std::size_t normal = static_cast<std::size_t>(index.normal_index) * 3;
                vertex.Normal = {attributes.normals[normal],
                                 attributes.normals[normal + 1],
                                 attributes.normals[normal + 2]};
                mask |= MeshData::NORMAL_PRESENT;
            }
            if (index.texcoord_index >= 0) {
                const std::size_t texcoord = static_cast<std::size_t>(index.texcoord_index) * 2;
                vertex.Texcoord = {attributes.texcoords[texcoord], attributes.texcoords[texcoord + 1]};
                mask |= MeshData::TEXCOORD_PRESENT;
            }
            const std::uint32_t vertexIndex = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back(vertex);
            masks.push_back(mask);
            corners.emplace(corner, vertexIndex);
            indices.push_back(vertexIndex);
        }
    }
    if (indices.size() != geometry->IndexCount) {
        return std::unexpected("OBJ triangulation discarded geometry");
    }
    return MeshData{std::move(vertices), std::move(indices), std::move(masks)};
}

} // namespace

ResourceHandle<MeshData>
LoadMesh(ResourceManager& resources, ResourceIdentity identity, const std::filesystem::path& path) {
    const ResourceIdentity failureIdentity = identity;
    const ResourceHandle<MeshData> handle = resources.Request<MeshData>(std::move(identity));
    const EResourceState state = resources.GetState(handle);
    if (state != EResourceState::UNLOADED && state != EResourceState::FAILED) {
        return handle;
    }
    const ResourceOperation<MeshData> operation = resources.BeginLoading(handle);
    const auto fail = [&](NCommon::EError code, const std::string& reason) {
        resources.Fail(operation,
                       NCommon::ErrorInfo{
                               .Code = code,
                               .Message = "Mesh resource '" + std::string{failureIdentity.GetKey()} + "' at '" +
                                          path.string() + "': " + reason,
                       });
    };
    try {
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            fail(NCommon::EError::IO_ERROR, "missing or unreadable OBJ data");
            return handle;
        }
        std::expected<MeshData, std::string> mesh = ImportMesh(file);
        if (!mesh) {
            fail(NCommon::EError::INVALID_ARGUMENT, mesh.error());
            return handle;
        }
        resources.PublishReady(operation, std::make_shared<MeshData>(std::move(*mesh)));
    } catch (const std::bad_alloc&) {
        fail(NCommon::EError::OUT_OF_MEMORY, "mesh allocation failed");
    } catch (const std::length_error&) {
        fail(NCommon::EError::INVALID_ARGUMENT, "mesh size exceeds container limit");
    }
    return handle;
}

} // namespace NResources

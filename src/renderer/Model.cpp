#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include "renderer/Model.h"
#include "renderer/Device.h"
#include "renderer/Buffer.h"

#include <spdlog/spdlog.h>

#include <unordered_map>
#include <stdexcept>
#include <fstream>

namespace glory {

namespace {
struct VertexHash {
    size_t operator()(const Vertex& v) const {
        size_t h = 0;
        auto hf = std::hash<float>{};
        for (int i = 0; i < 3; ++i) h ^= hf(v.position[i]) << (i * 3);
        for (int i = 0; i < 3; ++i) h ^= hf(v.normal[i])   << (i * 5 + 9);
        for (int i = 0; i < 2; ++i) h ^= hf(v.texCoord[i]) << (i * 7 + 18);
        return h;
    }
};
} // anonymous namespace

Model Model::loadFromOBJ(const Device& device, VmaAllocator allocator,
                          const std::string& filepath)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filepath.c_str())) {
        throw std::runtime_error("Failed to load OBJ: " + filepath + " " + warn + err);
    }
    if (!warn.empty()) spdlog::warn("OBJ: {}", warn);

    Model model;

    for (const auto& shape : shapes) {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;
        std::unordered_map<Vertex, uint32_t, VertexHash> uniqueVerts;

        for (const auto& idx : shape.mesh.indices) {
            Vertex vertex{};
            vertex.position = {
                attrib.vertices[3 * idx.vertex_index + 0],
                attrib.vertices[3 * idx.vertex_index + 1],
                attrib.vertices[3 * idx.vertex_index + 2]
            };
            vertex.color = {1.0f, 1.0f, 1.0f};

            if (idx.normal_index >= 0) {
                vertex.normal = {
                    attrib.normals[3 * idx.normal_index + 0],
                    attrib.normals[3 * idx.normal_index + 1],
                    attrib.normals[3 * idx.normal_index + 2]
                };
            }

            if (idx.texcoord_index >= 0) {
                vertex.texCoord = {
                    attrib.texcoords[2 * idx.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]
                };
            }

            if (auto it = uniqueVerts.find(vertex); it != uniqueVerts.end()) {
                indices.push_back(it->second);
            } else {
                auto i = static_cast<uint32_t>(vertices.size());
                uniqueVerts[vertex] = i;
                vertices.push_back(vertex);
                indices.push_back(i);
            }
        }

        spdlog::info("Loaded shape '{}': {} vertices, {} indices",
                     shape.name, vertices.size(), indices.size());
        model.m_meshes.emplace_back(device, allocator, vertices, indices);
    }

    return model;
}

bool Model::cookOBJ(const std::string& objPath, const std::string& gmeshPath)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, objPath.c_str())) {
        spdlog::error("cookOBJ: failed to load {}: {}{}", objPath, warn, err);
        return false;
    }
    if (!warn.empty()) spdlog::warn("cookOBJ: {}", warn);

    // Build per-shape vertex/index arrays (same dedup as loadFromOBJ)
    struct MeshData {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;
    };
    std::vector<MeshData> meshes;
    meshes.reserve(shapes.size());

    for (const auto& shape : shapes) {
        MeshData md;
        std::unordered_map<Vertex, uint32_t, VertexHash> uniqueVerts;

        for (const auto& idx : shape.mesh.indices) {
            Vertex vertex{};
            vertex.position = {
                attrib.vertices[3 * idx.vertex_index + 0],
                attrib.vertices[3 * idx.vertex_index + 1],
                attrib.vertices[3 * idx.vertex_index + 2]
            };
            vertex.color = {1.0f, 1.0f, 1.0f};

            if (idx.normal_index >= 0) {
                vertex.normal = {
                    attrib.normals[3 * idx.normal_index + 0],
                    attrib.normals[3 * idx.normal_index + 1],
                    attrib.normals[3 * idx.normal_index + 2]
                };
            }

            if (idx.texcoord_index >= 0) {
                vertex.texCoord = {
                    attrib.texcoords[2 * idx.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]
                };
            }

            if (auto it = uniqueVerts.find(vertex); it != uniqueVerts.end()) {
                md.indices.push_back(it->second);
            } else {
                auto i = static_cast<uint32_t>(md.vertices.size());
                uniqueVerts[vertex] = i;
                md.vertices.push_back(vertex);
                md.indices.push_back(i);
            }
        }
        meshes.push_back(std::move(md));
    }

    // Write binary .gmesh file
    std::ofstream out(gmeshPath, std::ios::binary);
    if (!out) {
        spdlog::error("cookOBJ: cannot open output {}", gmeshPath);
        return false;
    }

    GMeshHeader header{};
    header.meshCount = static_cast<uint32_t>(meshes.size());
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write entry table
    for (const auto& md : meshes) {
        GMeshEntry entry{};
        entry.vertexCount = static_cast<uint32_t>(md.vertices.size());
        entry.indexCount  = static_cast<uint32_t>(md.indices.size());
        out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }

    // Write vertex and index data for each mesh
    for (const auto& md : meshes) {
        out.write(reinterpret_cast<const char*>(md.vertices.data()),
                  md.vertices.size() * sizeof(Vertex));
        out.write(reinterpret_cast<const char*>(md.indices.data()),
                  md.indices.size() * sizeof(uint32_t));
    }

    out.close();

    uint32_t totalVerts = 0, totalIdx = 0;
    for (const auto& md : meshes) {
        totalVerts += static_cast<uint32_t>(md.vertices.size());
        totalIdx   += static_cast<uint32_t>(md.indices.size());
    }
    spdlog::info("Cooked {} -> {}: {} meshes, {} vertices, {} indices",
                 objPath, gmeshPath, meshes.size(), totalVerts, totalIdx);
    return true;
}

Model Model::loadFromGMesh(const Device& device, VmaAllocator allocator,
                            const std::string& filepath)
{
    std::ifstream in(filepath, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open gmesh: " + filepath);
    }

    GMeshHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic[0] != 'G' || header.magic[1] != 'M' ||
        header.magic[2] != 'S' || header.magic[3] != 'H') {
        throw std::runtime_error("Invalid gmesh magic in: " + filepath);
    }
    if (header.version != 1) {
        throw std::runtime_error("Unsupported gmesh version " +
                                 std::to_string(header.version) + " in: " + filepath);
    }

    // Read entry table
    std::vector<GMeshEntry> entries(header.meshCount);
    in.read(reinterpret_cast<char*>(entries.data()),
            header.meshCount * sizeof(GMeshEntry));

    Model model;
    for (uint32_t i = 0; i < header.meshCount; ++i) {
        std::vector<Vertex> vertices(entries[i].vertexCount);
        std::vector<uint32_t> indices(entries[i].indexCount);

        in.read(reinterpret_cast<char*>(vertices.data()),
                entries[i].vertexCount * sizeof(Vertex));
        in.read(reinterpret_cast<char*>(indices.data()),
                entries[i].indexCount * sizeof(uint32_t));

        spdlog::info("Loaded gmesh shape {}: {} vertices, {} indices",
                     i, vertices.size(), indices.size());
        model.m_meshes.emplace_back(device, allocator, vertices, indices);
    }

    return model;
}

Model Model::createTriangle(const Device& device, VmaAllocator allocator) {
    std::vector<Vertex> vertices = {
        {{ 0.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 0.0f}},
        {{ 0.5f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    };
    std::vector<uint32_t> indices = { 0, 1, 2 };

    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createCube(const Device& device, VmaAllocator allocator) {
    const glm::vec3 w{1.0f, 1.0f, 1.0f}; // white
    std::vector<Vertex> v = {
        // Front
        {{-0.5f, -0.5f,  0.5f}, w, { 0, 0, 1}, {0,0}},
        {{ 0.5f, -0.5f,  0.5f}, w, { 0, 0, 1}, {1,0}},
        {{ 0.5f,  0.5f,  0.5f}, w, { 0, 0, 1}, {1,1}},
        {{-0.5f,  0.5f,  0.5f}, w, { 0, 0, 1}, {0,1}},
        // Back
        {{ 0.5f, -0.5f, -0.5f}, w, { 0, 0,-1}, {0,0}},
        {{-0.5f, -0.5f, -0.5f}, w, { 0, 0,-1}, {1,0}},
        {{-0.5f,  0.5f, -0.5f}, w, { 0, 0,-1}, {1,1}},
        {{ 0.5f,  0.5f, -0.5f}, w, { 0, 0,-1}, {0,1}},
        // Right
        {{ 0.5f, -0.5f,  0.5f}, w, { 1, 0, 0}, {0,0}},
        {{ 0.5f, -0.5f, -0.5f}, w, { 1, 0, 0}, {1,0}},
        {{ 0.5f,  0.5f, -0.5f}, w, { 1, 0, 0}, {1,1}},
        {{ 0.5f,  0.5f,  0.5f}, w, { 1, 0, 0}, {0,1}},
        // Left
        {{-0.5f, -0.5f, -0.5f}, w, {-1, 0, 0}, {0,0}},
        {{-0.5f, -0.5f,  0.5f}, w, {-1, 0, 0}, {1,0}},
        {{-0.5f,  0.5f,  0.5f}, w, {-1, 0, 0}, {1,1}},
        {{-0.5f,  0.5f, -0.5f}, w, {-1, 0, 0}, {0,1}},
        // Top
        {{-0.5f,  0.5f,  0.5f}, w, { 0, 1, 0}, {0,0}},
        {{ 0.5f,  0.5f,  0.5f}, w, { 0, 1, 0}, {1,0}},
        {{ 0.5f,  0.5f, -0.5f}, w, { 0, 1, 0}, {1,1}},
        {{-0.5f,  0.5f, -0.5f}, w, { 0, 1, 0}, {0,1}},
        // Bottom
        {{-0.5f, -0.5f, -0.5f}, w, { 0,-1, 0}, {0,0}},
        {{ 0.5f, -0.5f, -0.5f}, w, { 0,-1, 0}, {1,0}},
        {{ 0.5f, -0.5f,  0.5f}, w, { 0,-1, 0}, {1,1}},
        {{-0.5f, -0.5f,  0.5f}, w, { 0,-1, 0}, {0,1}},
    };
    std::vector<uint32_t> idx;
    for (uint32_t face = 0; face < 6; ++face) {
        uint32_t b = face * 4;
        idx.insert(idx.end(), {b, b+1, b+2, b+2, b+3, b});
    }

    Model model;
    model.m_meshes.emplace_back(device, allocator, v, idx);
    return model;
}

Model Model::createSphere(const Device& device, VmaAllocator allocator,
                           uint32_t stacks, uint32_t slices)
{
    const float PI = 3.14159265358979323846f;
    const glm::vec3 white{1.0f, 1.0f, 1.0f};

    std::vector<Vertex> vertices;
    vertices.reserve((stacks + 1) * (slices + 1));

    for (uint32_t i = 0; i <= stacks; ++i) {
        float phi = PI * static_cast<float>(i) / static_cast<float>(stacks);
        float y   = std::cos(phi);
        float r   = std::sin(phi);

        for (uint32_t j = 0; j <= slices; ++j) {
            float theta = 2.0f * PI * static_cast<float>(j) / static_cast<float>(slices);
            float x = r * std::cos(theta);
            float z = r * std::sin(theta);

            glm::vec3 pos(x * 0.5f, y * 0.5f, z * 0.5f); // radius 0.5 to match cube bounds
            glm::vec3 normal = glm::normalize(glm::vec3(x, y, z));
            glm::vec2 uv(static_cast<float>(j) / static_cast<float>(slices),
                         static_cast<float>(i) / static_cast<float>(stacks));

            vertices.push_back({pos, white, normal, uv});
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(stacks * slices * 6);

    for (uint32_t i = 0; i < stacks; ++i) {
        for (uint32_t j = 0; j < slices; ++j) {
            uint32_t row0 = i * (slices + 1);
            uint32_t row1 = (i + 1) * (slices + 1);

            indices.push_back(row0 + j);
            indices.push_back(row1 + j);
            indices.push_back(row1 + j + 1);

            indices.push_back(row0 + j);
            indices.push_back(row1 + j + 1);
            indices.push_back(row0 + j + 1);
        }
    }

    spdlog::info("Created UV sphere: {} vertices, {} indices", vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createTorus(const Device& device, VmaAllocator allocator,
                          float majorR, float minorR,
                          uint32_t rings, uint32_t sides)
{
    const float PI = 3.14159265358979323846f;
    const glm::vec3 white{1.0f, 1.0f, 1.0f};

    std::vector<Vertex> vertices;
    vertices.reserve((rings + 1) * (sides + 1));

    for (uint32_t i = 0; i <= rings; ++i) {
        float u     = static_cast<float>(i) / static_cast<float>(rings);
        float theta = u * 2.0f * PI;
        float ct    = std::cos(theta);
        float st    = std::sin(theta);

        for (uint32_t j = 0; j <= sides; ++j) {
            float v   = static_cast<float>(j) / static_cast<float>(sides);
            float phi = v * 2.0f * PI;
            float cp  = std::cos(phi);
            float sp  = std::sin(phi);

            float x = (majorR + minorR * cp) * ct;
            float y = minorR * sp;
            float z = (majorR + minorR * cp) * st;

            // Normal: direction from ring center to surface point
            float nx = cp * ct;
            float ny = sp;
            float nz = cp * st;

            vertices.push_back({{x, y, z}, white, glm::normalize(glm::vec3(nx, ny, nz)), {u, v}});
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(rings * sides * 6);

    for (uint32_t i = 0; i < rings; ++i) {
        for (uint32_t j = 0; j < sides; ++j) {
            uint32_t row0 = i * (sides + 1);
            uint32_t row1 = (i + 1) * (sides + 1);

            indices.push_back(row0 + j);
            indices.push_back(row1 + j);
            indices.push_back(row1 + j + 1);

            indices.push_back(row0 + j);
            indices.push_back(row1 + j + 1);
            indices.push_back(row0 + j + 1);
        }
    }

    spdlog::info("Created torus: {} vertices, {} indices", vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createCylinder(const Device& device, VmaAllocator allocator,
                            float radius, float height, uint32_t slices)
{
    const float PI = 3.14159265358979323846f;
    const glm::vec3 white{1.0f, 1.0f, 1.0f};
    const float halfH = height * 0.5f;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Side wall: two rings (top + bottom)
    for (uint32_t i = 0; i <= slices; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(slices);
        float theta = u * 2.0f * PI;
        float ct = std::cos(theta), st = std::sin(theta);
        glm::vec3 normal = glm::normalize(glm::vec3(ct, 0.0f, st));

        // Bottom vertex
        vertices.push_back({{radius * ct, -halfH, radius * st}, white, normal, {u, 1.0f}});
        // Top vertex
        vertices.push_back({{radius * ct,  halfH, radius * st}, white, normal, {u, 0.0f}});
    }

    // Side indices (quad strip)
    for (uint32_t i = 0; i < slices; ++i) {
        uint32_t b = i * 2;
        indices.insert(indices.end(), {b, b+2, b+3, b, b+3, b+1});
    }

    // Top cap
    uint32_t topCenterIdx = static_cast<uint32_t>(vertices.size());
    vertices.push_back({{0.0f, halfH, 0.0f}, white, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f}});
    uint32_t topRingStart = static_cast<uint32_t>(vertices.size());
    for (uint32_t i = 0; i <= slices; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(slices);
        float theta = u * 2.0f * PI;
        float ct = std::cos(theta), st = std::sin(theta);
        vertices.push_back({{radius * ct, halfH, radius * st}, white,
                            {0.0f, 1.0f, 0.0f}, {ct * 0.5f + 0.5f, st * 0.5f + 0.5f}});
    }
    for (uint32_t i = 0; i < slices; ++i) {
        indices.push_back(topCenterIdx);
        indices.push_back(topRingStart + i + 1);
        indices.push_back(topRingStart + i);
    }

    // Bottom cap
    uint32_t botCenterIdx = static_cast<uint32_t>(vertices.size());
    vertices.push_back({{0.0f, -halfH, 0.0f}, white, {0.0f, -1.0f, 0.0f}, {0.5f, 0.5f}});
    uint32_t botRingStart = static_cast<uint32_t>(vertices.size());
    for (uint32_t i = 0; i <= slices; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(slices);
        float theta = u * 2.0f * PI;
        float ct = std::cos(theta), st = std::sin(theta);
        vertices.push_back({{radius * ct, -halfH, radius * st}, white,
                            {0.0f, -1.0f, 0.0f}, {ct * 0.5f + 0.5f, st * 0.5f + 0.5f}});
    }
    for (uint32_t i = 0; i < slices; ++i) {
        indices.push_back(botCenterIdx);
        indices.push_back(botRingStart + i);
        indices.push_back(botRingStart + i + 1);
    }

    spdlog::info("Created cylinder: {} vertices, {} indices", vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createCone(const Device& device, VmaAllocator allocator,
                         float radius, float height, uint32_t slices)
{
    const float PI = 3.14159265358979323846f;
    const glm::vec3 white{1.0f, 1.0f, 1.0f};
    const float halfH = height * 0.5f;
    const float slopeLen = std::sqrt(radius * radius + height * height);
    const float ny = radius / slopeLen;
    const float nr = height / slopeLen;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Tip vertex
    uint32_t tipIdx = 0;
    vertices.push_back({{0.0f, halfH, 0.0f}, white, {0.0f, 1.0f, 0.0f}, {0.5f, 0.0f}});

    // Side ring at base
    for (uint32_t i = 0; i <= slices; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(slices);
        float theta = u * 2.0f * PI;
        float ct = std::cos(theta), st = std::sin(theta);
        glm::vec3 pos(radius * ct, -halfH, radius * st);
        glm::vec3 normal = glm::normalize(glm::vec3(nr * ct, ny, nr * st));
        vertices.push_back({pos, white, normal, {u, 1.0f}});
    }

    // Side triangles (fan from tip)
    for (uint32_t i = 0; i < slices; ++i) {
        indices.push_back(tipIdx);
        indices.push_back(1 + i + 1);
        indices.push_back(1 + i);
    }

    // Base cap center
    uint32_t baseCenterIdx = static_cast<uint32_t>(vertices.size());
    vertices.push_back({{0.0f, -halfH, 0.0f}, white, {0.0f, -1.0f, 0.0f}, {0.5f, 0.5f}});

    // Base cap ring
    uint32_t baseRingStart = static_cast<uint32_t>(vertices.size());
    for (uint32_t i = 0; i <= slices; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(slices);
        float theta = u * 2.0f * PI;
        float ct = std::cos(theta), st = std::sin(theta);
        vertices.push_back({{radius * ct, -halfH, radius * st}, white,
                            {0.0f, -1.0f, 0.0f}, {ct * 0.5f + 0.5f, st * 0.5f + 0.5f}});
    }

    // Base cap triangles
    for (uint32_t i = 0; i < slices; ++i) {
        indices.push_back(baseCenterIdx);
        indices.push_back(baseRingStart + i);
        indices.push_back(baseRingStart + i + 1);
    }

    spdlog::info("Created cone: {} vertices, {} indices", vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createCapsule(const Device& device, VmaAllocator allocator,
                            float radius, float height, uint32_t stacks, uint32_t slices)
{
    const float PI = 3.14159265358979323846f;
    const glm::vec3 white{1.0f, 1.0f, 1.0f};
    const float halfH = height * 0.5f;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Top hemisphere
    for (uint32_t j = 0; j <= stacks; ++j) {
        float phi = (static_cast<float>(j) / static_cast<float>(stacks)) * PI * 0.5f;
        float sp = std::sin(phi), cp = std::cos(phi);
        for (uint32_t i = 0; i <= slices; ++i) {
            float theta = (static_cast<float>(i) / static_cast<float>(slices)) * 2.0f * PI;
            float ct = std::cos(theta), st = std::sin(theta);
            glm::vec3 n(cp * ct, sp, cp * st);
            glm::vec3 p = n * radius + glm::vec3(0.0f, halfH, 0.0f);
            float u = static_cast<float>(i) / static_cast<float>(slices);
            float v = static_cast<float>(j) / static_cast<float>(stacks * 2 + 2);
            vertices.push_back({p, white, n, {u, v}});
        }
    }

    // Cylinder body: bottom ring + top ring
    uint32_t cylStart = static_cast<uint32_t>(vertices.size());
    for (int side = 0; side < 2; ++side) {
        float y = (side == 0) ? halfH : -halfH;
        float v = (side == 0) ? 0.25f : 0.75f;
        for (uint32_t i = 0; i <= slices; ++i) {
            float theta = (static_cast<float>(i) / static_cast<float>(slices)) * 2.0f * PI;
            float ct = std::cos(theta), st = std::sin(theta);
            glm::vec3 n(ct, 0.0f, st);
            glm::vec3 p(radius * ct, y, radius * st);
            float u = static_cast<float>(i) / static_cast<float>(slices);
            vertices.push_back({p, white, n, {u, v}});
        }
    }

    // Bottom hemisphere
    uint32_t botStart = static_cast<uint32_t>(vertices.size());
    for (uint32_t j = 0; j <= stacks; ++j) {
        float phi = (static_cast<float>(j) / static_cast<float>(stacks)) * PI * 0.5f;
        float sp = std::sin(phi), cp = std::cos(phi);
        for (uint32_t i = 0; i <= slices; ++i) {
            float theta = (static_cast<float>(i) / static_cast<float>(slices)) * 2.0f * PI;
            float ct = std::cos(theta), st = std::sin(theta);
            glm::vec3 n(cp * ct, -sp, cp * st);
            glm::vec3 p = n * radius + glm::vec3(0.0f, -halfH + radius * (1.0f - sp), 0.0f);
            p = glm::vec3(radius * cp * ct, -halfH - radius * sp, radius * cp * st);
            float u = static_cast<float>(i) / static_cast<float>(slices);
            float v = 0.75f + static_cast<float>(j) / static_cast<float>(stacks * 2 + 2);
            vertices.push_back({p, white, n, {u, v}});
        }
    }

    auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        indices.insert(indices.end(), {a, b, c, a, c, d});
    };

    uint32_t cols = slices + 1;

    // Top hemisphere indices
    for (uint32_t j = 0; j < stacks; ++j)
        for (uint32_t i = 0; i < slices; ++i) {
            uint32_t r0 = j * cols + i;
            quad(r0, r0 + cols, r0 + cols + 1, r0 + 1);
        }

    // Connect top hemisphere bottom ring to cylinder top ring
    uint32_t topHemBottom = stacks * cols;
    for (uint32_t i = 0; i < slices; ++i)
        quad(topHemBottom + i, cylStart + i, cylStart + i + 1, topHemBottom + i + 1);

    // Cylinder body
    for (uint32_t i = 0; i < slices; ++i)
        quad(cylStart + i, cylStart + cols + i, cylStart + cols + i + 1, cylStart + i + 1);

    // Connect cylinder bottom ring to bottom hemisphere top ring
    uint32_t cylBottom = cylStart + cols;
    for (uint32_t i = 0; i < slices; ++i)
        quad(cylBottom + i, botStart + i, botStart + i + 1, cylBottom + i + 1);

    // Bottom hemisphere indices
    for (uint32_t j = 0; j < stacks; ++j)
        for (uint32_t i = 0; i < slices; ++i) {
            uint32_t r0 = botStart + j * cols + i;
            quad(r0, r0 + cols, r0 + cols + 1, r0 + 1);
        }

    spdlog::info("Created capsule: {} vertices, {} indices", vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createTerrain(const Device& device, VmaAllocator allocator,
                            float size, uint32_t resolution, float heightScale)
{
    const glm::vec3 white{1.0f, 1.0f, 1.0f};
    const float half = size * 0.5f;

    // Simple hash-based noise for height
    auto hash = [](int x, int y) -> float {
        int n = x + y * 57;
        n = (n << 13) ^ n;
        return 1.0f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) & 0x7FFFFFFF)
               / 1073741824.0f;
    };

    auto smoothNoise = [&](float fx, float fy) -> float {
        int ix = static_cast<int>(std::floor(fx));
        int iy = static_cast<int>(std::floor(fy));
        float fracX = fx - static_cast<float>(ix);
        float fracY = fy - static_cast<float>(iy);
        float v00 = hash(ix, iy),       v10 = hash(ix + 1, iy);
        float v01 = hash(ix, iy + 1),   v11 = hash(ix + 1, iy + 1);
        float i0 = v00 + fracX * (v10 - v00);
        float i1 = v01 + fracX * (v11 - v01);
        return i0 + fracY * (i1 - i0);
    };

    auto fbm = [&](float x, float y) -> float {
        float val = 0.0f, amp = 1.0f, freq = 1.0f, total = 0.0f;
        for (int i = 0; i < 4; ++i) {
            val += smoothNoise(x * freq, y * freq) * amp;
            total += amp;
            amp *= 0.5f;
            freq *= 2.0f;
        }
        return val / total;
    };

    // Generate height values
    uint32_t vRes = resolution + 1;
    std::vector<float> heights(vRes * vRes);
    for (uint32_t z = 0; z < vRes; ++z) {
        for (uint32_t x = 0; x < vRes; ++x) {
            float fx = static_cast<float>(x) / static_cast<float>(resolution) * 4.0f;
            float fz = static_cast<float>(z) / static_cast<float>(resolution) * 4.0f;
            heights[z * vRes + x] = fbm(fx + 0.5f, fz + 0.5f) * heightScale;
        }
    }

    // Build vertices
    std::vector<Vertex> vertices;
    vertices.reserve(vRes * vRes);
    for (uint32_t z = 0; z < vRes; ++z) {
        for (uint32_t x = 0; x < vRes; ++x) {
            float px = -half + static_cast<float>(x) / static_cast<float>(resolution) * size;
            float pz = -half + static_cast<float>(z) / static_cast<float>(resolution) * size;
            float py = heights[z * vRes + x];

            float u = static_cast<float>(x) / static_cast<float>(resolution);
            float v = static_cast<float>(z) / static_cast<float>(resolution);

            // Compute normal from finite differences
            float hL = (x > 0) ? heights[z * vRes + (x - 1)] : py;
            float hR = (x < resolution) ? heights[z * vRes + (x + 1)] : py;
            float hD = (z > 0) ? heights[(z - 1) * vRes + x] : py;
            float hU = (z < resolution) ? heights[(z + 1) * vRes + x] : py;
            float dx = size / static_cast<float>(resolution);
            glm::vec3 normal = glm::normalize(glm::vec3(hL - hR, 2.0f * dx, hD - hU));

            vertices.push_back({{px, py, pz}, white, normal, {u * 4.0f, v * 4.0f}});
        }
    }

    // Build indices
    std::vector<uint32_t> indices;
    indices.reserve(resolution * resolution * 6);
    for (uint32_t z = 0; z < resolution; ++z) {
        for (uint32_t x = 0; x < resolution; ++x) {
            uint32_t tl = z * vRes + x;
            uint32_t tr = tl + 1;
            uint32_t bl = (z + 1) * vRes + x;
            uint32_t br = bl + 1;
            indices.insert(indices.end(), {tl, bl, tr, tr, bl, br});
        }
    }

    spdlog::info("Created terrain: {} vertices, {} indices", vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createTorusKnot(const Device& device, VmaAllocator allocator,
                              uint32_t p, uint32_t q,
                              float radius, float tubeRadius,
                              uint32_t segments, uint32_t sides)
{
    const float PI = 3.14159265358979323846f;
    const glm::vec3 white{1.0f, 1.0f, 1.0f};

    // Evaluate torus knot curve point
    auto knotPoint = [&](float t) -> glm::vec3 {
        float r = radius * (2.0f + std::cos(static_cast<float>(q) * t));
        return glm::vec3(
            r * std::cos(static_cast<float>(p) * t),
            radius * std::sin(static_cast<float>(q) * t),
            r * std::sin(static_cast<float>(p) * t)
        );
    };

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    for (uint32_t i = 0; i <= segments; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * PI;
        float tNext = static_cast<float>(i + 1) / static_cast<float>(segments) * 2.0f * PI;

        glm::vec3 center = knotPoint(t);
        glm::vec3 next   = knotPoint(tNext);

        // Frenet frame
        glm::vec3 T = glm::normalize(next - center);
        glm::vec3 up = std::abs(T.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 N = glm::normalize(glm::cross(T, up));
        glm::vec3 B = glm::cross(T, N);

        for (uint32_t j = 0; j <= sides; ++j) {
            float theta = static_cast<float>(j) / static_cast<float>(sides) * 2.0f * PI;
            float ct = std::cos(theta), st = std::sin(theta);

            glm::vec3 normal = N * ct + B * st;
            glm::vec3 pos = center + normal * tubeRadius;
            float u = static_cast<float>(i) / static_cast<float>(segments) * 4.0f;
            float v = static_cast<float>(j) / static_cast<float>(sides);

            vertices.push_back({pos, white, glm::normalize(normal), {u, v}});
        }
    }

    uint32_t cols = sides + 1;
    for (uint32_t i = 0; i < segments; ++i) {
        for (uint32_t j = 0; j < sides; ++j) {
            uint32_t a = i * cols + j;
            uint32_t b = a + cols;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }

    spdlog::info("Created torus knot ({},{}): {} vertices, {} indices",
                 p, q, vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createIcosphere(const Device& device, VmaAllocator allocator,
                              uint32_t subdivisions, float radius)
{
    const glm::vec3 white{1.0f, 1.0f, 1.0f};

    // Golden ratio for icosahedron construction
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;

    std::vector<glm::vec3> positions = {
        glm::normalize(glm::vec3(-1,  t,  0)),
        glm::normalize(glm::vec3( 1,  t,  0)),
        glm::normalize(glm::vec3(-1, -t,  0)),
        glm::normalize(glm::vec3( 1, -t,  0)),
        glm::normalize(glm::vec3( 0, -1,  t)),
        glm::normalize(glm::vec3( 0,  1,  t)),
        glm::normalize(glm::vec3( 0, -1, -t)),
        glm::normalize(glm::vec3( 0,  1, -t)),
        glm::normalize(glm::vec3( t,  0, -1)),
        glm::normalize(glm::vec3( t,  0,  1)),
        glm::normalize(glm::vec3(-t,  0, -1)),
        glm::normalize(glm::vec3(-t,  0,  1)),
    };

    std::vector<uint32_t> tris = {
        0,11,5,  0,5,1,  0,1,7,  0,7,10, 0,10,11,
        1,5,9,   5,11,4, 11,10,2, 10,7,6, 7,1,8,
        3,9,4,   3,4,2,  3,2,6,  3,6,8,  3,8,9,
        4,9,5,   2,4,11, 6,2,10, 8,6,7,  9,8,1
    };

    // Midpoint cache for subdivision
    auto midpointKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    };

    for (uint32_t sub = 0; sub < subdivisions; ++sub) {
        std::unordered_map<uint64_t, uint32_t> cache;
        std::vector<uint32_t> newTris;

        auto getMidpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
            uint64_t key = midpointKey(a, b);
            auto it = cache.find(key);
            if (it != cache.end()) return it->second;
            glm::vec3 mid = glm::normalize((positions[a] + positions[b]) * 0.5f);
            uint32_t idx = static_cast<uint32_t>(positions.size());
            positions.push_back(mid);
            cache[key] = idx;
            return idx;
        };

        for (size_t i = 0; i < tris.size(); i += 3) {
            uint32_t a = tris[i], b = tris[i+1], c = tris[i+2];
            uint32_t ab = getMidpoint(a, b);
            uint32_t bc = getMidpoint(b, c);
            uint32_t ca = getMidpoint(c, a);
            newTris.insert(newTris.end(), {a, ab, ca});
            newTris.insert(newTris.end(), {b, bc, ab});
            newTris.insert(newTris.end(), {c, ca, bc});
            newTris.insert(newTris.end(), {ab, bc, ca});
        }
        tris = std::move(newTris);
    }

    // Build vertices
    std::vector<Vertex> vertices;
    vertices.reserve(positions.size());
    for (const auto& p : positions) {
        glm::vec3 pos = p * radius;
        // Spherical UV from normal direction
        float u = 0.5f + std::atan2(p.z, p.x) / (2.0f * 3.14159265358979323846f);
        float v = 0.5f - std::asin(std::clamp(p.y, -1.0f, 1.0f)) / 3.14159265358979323846f;
        vertices.push_back({pos, white, p, {u, v}});
    }

    spdlog::info("Created icosphere (sub {}): {} vertices, {} indices",
                 subdivisions, vertices.size(), tris.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, tris);
    return model;
}

Model Model::createSpring(const Device& device, VmaAllocator allocator,
                            float coilRadius, float tubeRadius, float height,
                            uint32_t coils, uint32_t segments, uint32_t sides) {
    const float PI = 3.14159265358979323846f;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    for (uint32_t i = 0; i <= segments; i++) {
        float t = static_cast<float>(i) / segments;
        float angle = t * coils * 2.0f * PI;
        // Helix spine position
        float cx = coilRadius * std::cos(angle);
        float cz = coilRadius * std::sin(angle);
        float cy = -height * 0.5f + t * height;

        // Frenet frame: tangent along helix
        float tx = -coilRadius * std::sin(angle) * coils * 2.0f * PI;
        float ty = height;
        float tz = coilRadius * std::cos(angle) * coils * 2.0f * PI;
        float tLen = std::sqrt(tx*tx + ty*ty + tz*tz);
        tx /= tLen; ty /= tLen; tz /= tLen;

        // Normal/binormal via cross with up hint
        float nx, ny, nz;
        float ux = 0, uy = 1, uz = 0;
        // binormal = tangent x up
        float bx = ty * uz - tz * uy;
        float by = tz * ux - tx * uz;
        float bz = tx * uy - ty * ux;
        float bLen = std::sqrt(bx*bx + by*by + bz*bz);
        if (bLen < 1e-6f) { bx = 1; by = 0; bz = 0; bLen = 1; }
        bx /= bLen; by /= bLen; bz /= bLen;
        // normal = binormal x tangent
        nx = by * tz - bz * ty;
        ny = bz * tx - bx * tz;
        nz = bx * ty - by * tx;

        for (uint32_t j = 0; j <= sides; j++) {
            float phi = static_cast<float>(j) / sides * 2.0f * PI;
            float cPhi = std::cos(phi), sPhi = std::sin(phi);

            float px = cx + tubeRadius * (cPhi * nx + sPhi * bx);
            float py = cy + tubeRadius * (cPhi * ny + sPhi * by);
            float pz = cz + tubeRadius * (cPhi * nz + sPhi * bz);

            float vnx = cPhi * nx + sPhi * bx;
            float vny = cPhi * ny + sPhi * by;
            float vnz = cPhi * nz + sPhi * bz;

            Vertex v{};
            v.position = {px, py, pz};
            v.normal = {vnx, vny, vnz};
            v.texCoord = {t * coils * 2.0f, static_cast<float>(j) / sides};
            v.color = {1.0f, 1.0f, 1.0f};
            vertices.push_back(v);
        }
    }

    for (uint32_t i = 0; i < segments; i++) {
        for (uint32_t j = 0; j < sides; j++) {
            uint32_t a = i * (sides + 1) + j;
            uint32_t b = a + sides + 1;
            indices.push_back(a); indices.push_back(b); indices.push_back(a + 1);
            indices.push_back(a + 1); indices.push_back(b); indices.push_back(b + 1);
        }
    }

    spdlog::info("Created spring ({} coils): {} vertices, {} indices",
                 coils, vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createGear(const Device& device, VmaAllocator allocator,
                          float outerRadius, float innerRadius, float hubRadius,
                          float thickness, uint32_t teeth) {
    const float PI = 3.14159265358979323846f;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Each tooth occupies 2*PI/teeth. Tooth takes ~60% of slot, gap takes ~40%.
    float toothAngle = 2.0f * PI / teeth;
    float toothWidth = toothAngle * 0.6f;
    float halfT = thickness * 0.5f;

    auto addVert = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        Vertex vert{};
        vert.position = {x, y, z};
        vert.normal   = {nx, ny, nz};
        vert.texCoord = {u, v};
        vert.color    = {1.0f, 1.0f, 1.0f};
        uint32_t idx = static_cast<uint32_t>(vertices.size());
        vertices.push_back(vert);
        return idx;
    };

    auto addQuad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        indices.push_back(a); indices.push_back(b); indices.push_back(c);
        indices.push_back(a); indices.push_back(c); indices.push_back(d);
    };

    // Generate gear profile for top and bottom faces, plus side walls
    for (uint32_t face = 0; face < 2; face++) {
        float y = (face == 0) ? halfT : -halfT;
        float ny = (face == 0) ? 1.0f : -1.0f;

        // Hub center vertex
        uint32_t center = addVert(0, y, 0, 0, ny, 0, 0.5f, 0.5f);

        // For each tooth: generate hub ring, inner ring, and tooth tip
        for (uint32_t t = 0; t < teeth; t++) {
            float a0 = t * toothAngle;
            float a1 = a0 + (toothAngle - toothWidth) * 0.5f; // tooth start
            float a2 = a1 + toothWidth;                         // tooth end
            float a3 = (t + 1) * toothAngle;                    // next tooth start

            // Hub ring points
            uint32_t h0 = addVert(hubRadius * std::cos(a0), y, hubRadius * std::sin(a0), 0, ny, 0, 0.5f + 0.2f * std::cos(a0), 0.5f + 0.2f * std::sin(a0));
            uint32_t h3 = addVert(hubRadius * std::cos(a3), y, hubRadius * std::sin(a3), 0, ny, 0, 0.5f + 0.2f * std::cos(a3), 0.5f + 0.2f * std::sin(a3));

            // Inner ring (rim base)
            uint32_t r0 = addVert(innerRadius * std::cos(a0), y, innerRadius * std::sin(a0), 0, ny, 0, 0.5f + 0.4f * std::cos(a0), 0.5f + 0.4f * std::sin(a0));
            uint32_t r1 = addVert(innerRadius * std::cos(a1), y, innerRadius * std::sin(a1), 0, ny, 0, 0.5f + 0.4f * std::cos(a1), 0.5f + 0.4f * std::sin(a1));
            uint32_t r2 = addVert(innerRadius * std::cos(a2), y, innerRadius * std::sin(a2), 0, ny, 0, 0.5f + 0.4f * std::cos(a2), 0.5f + 0.4f * std::sin(a2));
            uint32_t r3 = addVert(innerRadius * std::cos(a3), y, innerRadius * std::sin(a3), 0, ny, 0, 0.5f + 0.4f * std::cos(a3), 0.5f + 0.4f * std::sin(a3));

            // Tooth tip points (outer radius)
            uint32_t t1 = addVert(outerRadius * std::cos(a1), y, outerRadius * std::sin(a1), 0, ny, 0, 0.5f + 0.5f * std::cos(a1), 0.5f + 0.5f * std::sin(a1));
            uint32_t t2 = addVert(outerRadius * std::cos(a2), y, outerRadius * std::sin(a2), 0, ny, 0, 0.5f + 0.5f * std::cos(a2), 0.5f + 0.5f * std::sin(a2));

            // Hub triangles (fan from center to hub ring)
            if (face == 0) {
                indices.push_back(center); indices.push_back(h0); indices.push_back(h3);
            } else {
                indices.push_back(center); indices.push_back(h3); indices.push_back(h0);
            }

            // Rim between hub and inner ring
            if (face == 0) {
                addQuad(h0, r0, r3, h3);
            } else {
                addQuad(h0, h3, r3, r0);
            }

            // Gap segments (inner ring between teeth)
            if (face == 0) {
                addQuad(r0, r1, r1, r0); // degenerate — skip gap flat
                // Tooth face
                addQuad(r1, t1, t2, r2);
            } else {
                addQuad(r1, r2, t2, t1);
            }

            // Gap arcs (inner ring from tooth end to next tooth start)
            if (face == 0) {
                addQuad(r2, r2, r3, r3); // degenerate for gap
            }
        }
    }

    // Side walls: outer tooth edges
    for (uint32_t t = 0; t < teeth; t++) {
        float a0 = t * toothAngle;
        float a1 = a0 + (toothAngle - toothWidth) * 0.5f;
        float a2 = a1 + toothWidth;

        // Tooth outer wall
        for (int s = 0; s < 2; s++) {
            float angle = (s == 0) ? a1 : a2;
            float nextA = (s == 0) ? a2 : a1;
            if (s == 1) { // swap for correct winding
                float tmp = angle; angle = nextA; nextA = tmp;
            }
            float cx1 = outerRadius * std::cos(angle), cz1 = outerRadius * std::sin(angle);
            float cx2 = outerRadius * std::cos(nextA), cz2 = outerRadius * std::sin(nextA);
            float nx1 = std::cos(angle), nz1 = std::sin(angle);
            float nx2 = std::cos(nextA), nz2 = std::sin(nextA);

            uint32_t v0 = addVert(cx1, halfT,  cz1, nx1, 0, nz1, 0, 0);
            uint32_t v1 = addVert(cx2, halfT,  cz2, nx2, 0, nz2, 1, 0);
            uint32_t v2 = addVert(cx2, -halfT, cz2, nx2, 0, nz2, 1, 1);
            uint32_t v3 = addVert(cx1, -halfT, cz1, nx1, 0, nz1, 0, 1);
            addQuad(v0, v1, v2, v3);
        }
    }

    // Hub inner wall
    uint32_t hubSegs = teeth * 4;
    for (uint32_t i = 0; i < hubSegs; i++) {
        float a0 = 2.0f * PI * i / hubSegs;
        float a1 = 2.0f * PI * (i + 1) / hubSegs;
        float c0 = hubRadius * std::cos(a0), s0 = hubRadius * std::sin(a0);
        float c1 = hubRadius * std::cos(a1), s1 = hubRadius * std::sin(a1);
        uint32_t v0 = addVert(c0, halfT,  s0, -std::cos(a0), 0, -std::sin(a0), 0, 0);
        uint32_t v1 = addVert(c1, halfT,  s1, -std::cos(a1), 0, -std::sin(a1), 1, 0);
        uint32_t v2 = addVert(c1, -halfT, s1, -std::cos(a1), 0, -std::sin(a1), 1, 1);
        uint32_t v3 = addVert(c0, -halfT, s0, -std::cos(a0), 0, -std::sin(a0), 0, 1);
        addQuad(v1, v0, v3, v2);
    }

    spdlog::info("Created gear ({} teeth): {} vertices, {} indices",
                 teeth, vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

Model Model::createPyramid(const Device& device, VmaAllocator allocator,
                             float baseSize, float height) {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    float h = baseSize * 0.5f;

    glm::vec3 apex(0.0f, height, 0.0f);
    glm::vec3 bl(-h, 0.0f, -h), br(h, 0.0f, -h);
    glm::vec3 fl(-h, 0.0f,  h), fr(h, 0.0f,  h);

    auto addFace = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, float u0, float v0,
                       float u1, float v1, float u2, float v2) {
        glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        uint32_t base = static_cast<uint32_t>(vertices.size());
        Vertex va{}; va.position = a; va.normal = n; va.texCoord = {u0, v0}; va.color = {1,1,1};
        Vertex vb{}; vb.position = b; vb.normal = n; vb.texCoord = {u1, v1}; vb.color = {1,1,1};
        Vertex vc{}; vc.position = c; vc.normal = n; vc.texCoord = {u2, v2}; vc.color = {1,1,1};
        vertices.push_back(va); vertices.push_back(vb); vertices.push_back(vc);
        indices.push_back(base); indices.push_back(base+1); indices.push_back(base+2);
    };

    // 4 side faces
    addFace(apex, bl, br, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f); // back
    addFace(apex, br, fr, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f); // right
    addFace(apex, fr, fl, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f); // front
    addFace(apex, fl, bl, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f); // left

    // Bottom face (2 triangles)
    addFace(bl, fl, fr, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    addFace(bl, fr, br, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f);

    spdlog::info("Created pyramid: {} vertices, {} indices",
                 vertices.size(), indices.size());
    Model model;
    model.m_meshes.emplace_back(device, allocator, vertices, indices);
    return model;
}

void Model::draw(VkCommandBuffer cmd) const {
    for (const auto& mesh : m_meshes) {
        mesh.bind(cmd);
        mesh.draw(cmd);
    }
}

void Model::drawInstanced(VkCommandBuffer cmd, uint32_t instanceCount, uint32_t firstInstance) const {
    for (const auto& mesh : m_meshes) {
        mesh.bind(cmd);
        mesh.drawInstanced(cmd, instanceCount, firstInstance);
    }
}

void Model::drawIndirect(VkCommandBuffer cmd, VkBuffer indirectBuffer, VkDeviceSize offset) const {
    for (size_t i = 0; i < m_meshes.size(); ++i) {
        m_meshes[i].bind(cmd);
        m_meshes[i].drawIndirect(cmd, indirectBuffer,
                                  offset + i * sizeof(VkDrawIndexedIndirectCommand));
    }
}

void Model::drawMeshIndirect(VkCommandBuffer cmd, uint32_t meshIdx, VkBuffer indirectBuffer, VkDeviceSize offset) const {
    if (meshIdx < m_meshes.size()) {
        m_meshes[meshIdx].bind(cmd);
        m_meshes[meshIdx].drawIndirect(cmd, indirectBuffer, offset);
    }
}

uint32_t Model::getIndexCount() const {
    uint32_t total = 0;
    for (const auto& mesh : m_meshes)
        total += mesh.getIndexCount();
    return total;
}

uint32_t Model::getMeshCount() const {
    return static_cast<uint32_t>(m_meshes.size());
}

uint32_t Model::getMeshIndexCount(uint32_t meshIdx) const {
    return m_meshes[meshIdx].getIndexCount();
}

int Model::getMeshMaterialIndex(uint32_t meshIdx) const {
    if (meshIdx < m_meshMaterialIndices.size())
        return m_meshMaterialIndices[meshIdx];
    return -1;
}

} // namespace glory

// Standalone mesh cook tool: converts OBJ files to binary .gmesh format
// Usage: mesh_cook <input.obj> <output.gmesh>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

// Mirror the engine's Vertex layout exactly
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec3 normal;
    glm::vec2 texCoord;

    bool operator==(const Vertex& other) const {
        return position == other.position && color == other.color &&
               normal == other.normal && texCoord == other.texCoord;
    }
};

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

struct GMeshHeader {
    char     magic[4] = {'G','M','S','H'};
    uint32_t version  = 1;
    uint32_t meshCount = 0;
};

struct GMeshEntry {
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
};

struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <input.obj> <output.gmesh>\n", argv[0]);
        return 1;
    }

    const std::string objPath   = argv[1];
    const std::string gmeshPath = argv[2];

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, objPath.c_str())) {
        std::fprintf(stderr, "Error loading %s: %s%s\n", objPath.c_str(), warn.c_str(), err.c_str());
        return 1;
    }
    if (!warn.empty()) {
        std::fprintf(stderr, "Warning: %s\n", warn.c_str());
    }

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

    std::ofstream out(gmeshPath, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "Cannot open output: %s\n", gmeshPath.c_str());
        return 1;
    }

    GMeshHeader header{};
    header.meshCount = static_cast<uint32_t>(meshes.size());
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    for (const auto& md : meshes) {
        GMeshEntry entry{};
        entry.vertexCount = static_cast<uint32_t>(md.vertices.size());
        entry.indexCount  = static_cast<uint32_t>(md.indices.size());
        out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }

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
    std::printf("Cooked %s -> %s: %zu meshes, %u vertices, %u indices\n",
                objPath.c_str(), gmeshPath.c_str(), meshes.size(), totalVerts, totalIdx);
    return 0;
}

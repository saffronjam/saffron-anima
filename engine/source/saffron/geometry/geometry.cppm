module;

// cgltf + tinyobjloader + glm are header-heavy, so this module uses classic
// includes (no `import std`), like the rendering/scene modules.
#include <cgltf.h>
#include <tiny_obj_loader.h>
#include <glm/glm.hpp>

#include <array>
#include <cctype>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

export module Saffron.Geometry;

import Saffron.Core;

export namespace se
{
    // One interleaved vertex stream. 32 bytes; tangents are deferred to material time.
    struct Vertex
    {
        glm::vec3 position{ 0.0f };
        glm::vec3 normal{ 0.0f };
        glm::vec2 uv0{ 0.0f };
    };

    // One drawIndexed range over the shared vertex+index buffers. vertexOffset is
    // signed to match vkCmdDrawIndexed; materialSlot is reserved (0) until materials.
    struct Submesh
    {
        u32 firstIndex = 0;
        u32 indexCount = 0;
        i32 vertexOffset = 0;
        u32 materialSlot = 0;
    };

    // The canonical CPU-side mesh every importer converts into.
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<u32> indices;
        std::vector<Submesh> submeshes;
    };

    inline constexpr u32 MeshFormatVersion = 1;

    std::expected<Mesh, std::string> importGltf(const std::string& path);
    std::expected<Mesh, std::string> importObj(const std::string& path);
    std::expected<Mesh, std::string> importModelFile(const std::string& path);  // dispatch by extension

    std::expected<void, std::string> saveMesh(const Mesh& mesh, const std::string& path);  // baked .smesh
    std::expected<Mesh, std::string> loadMesh(const std::string& path);

    // Recomputes smooth vertex normals from the triangles. Used when a source omits them.
    void generateNormals(Mesh& mesh);

    // Headless check: import cube.obj + cube.gltf from modelsDir, bake one to a
    // .smesh and read it back, logging the outcome.
    void runGeometrySelfTest(const std::string& modelsDir);
}

namespace se
{
    static_assert(sizeof(Vertex) == 32, "Vertex must stay 32 bytes (the .smesh on-disk stride)");
    static_assert(sizeof(Submesh) == 16, "Submesh must stay 16 bytes (baked directly into .smesh)");

    namespace
    {
        // 64-byte fixed header; three contiguous raw arrays follow at the offsets.
        struct SMeshHeader
        {
            char magic[4];        // 'S','M','S','H'
            u32 version;
            u32 flags;            // reserved (0)
            u32 vertexStride;     // == sizeof(Vertex)
            u32 vertexCount;
            u32 indexCount;
            u32 indexWidth;       // bytes per index (4)
            u32 submeshCount;
            u64 verticesOffset;
            u64 indicesOffset;
            u64 submeshesOffset;
            u32 reserved[2];
        };
        static_assert(sizeof(SMeshHeader) == 64, "SMeshHeader must be exactly 64 bytes");

        bool endsWithIgnoreCase(const std::string& text, const std::string& suffix)
        {
            if (text.size() < suffix.size())
            {
                return false;
            }
            const std::size_t base = text.size() - suffix.size();
            for (std::size_t i = 0; i < suffix.size(); i = i + 1)
            {
                const int a = std::tolower(static_cast<unsigned char>(text[base + i]));
                const int b = std::tolower(static_cast<unsigned char>(suffix[i]));
                if (a != b)
                {
                    return false;
                }
            }
            return true;
        }

        bool anyNormalsPresent(const Mesh& mesh)
        {
            for (const Vertex& vertex : mesh.vertices)
            {
                if (glm::dot(vertex.normal, vertex.normal) > 1e-12f)
                {
                    return true;
                }
            }
            return false;
        }
    }

    void generateNormals(Mesh& mesh)
    {
        for (Vertex& vertex : mesh.vertices)
        {
            vertex.normal = glm::vec3(0.0f);
        }
        for (const Submesh& submesh : mesh.submeshes)
        {
            for (u32 i = 0; i + 2 < submesh.indexCount; i = i + 3)
            {
                const std::size_t base = submesh.firstIndex + i;
                const std::size_t a = static_cast<std::size_t>(submesh.vertexOffset) + mesh.indices[base + 0];
                const std::size_t b = static_cast<std::size_t>(submesh.vertexOffset) + mesh.indices[base + 1];
                const std::size_t c = static_cast<std::size_t>(submesh.vertexOffset) + mesh.indices[base + 2];
                const glm::vec3 faceNormal =
                    glm::cross(mesh.vertices[b].position - mesh.vertices[a].position,
                               mesh.vertices[c].position - mesh.vertices[a].position);
                mesh.vertices[a].normal += faceNormal;
                mesh.vertices[b].normal += faceNormal;
                mesh.vertices[c].normal += faceNormal;
            }
        }
        for (Vertex& vertex : mesh.vertices)
        {
            if (glm::dot(vertex.normal, vertex.normal) > 1e-12f)
            {
                vertex.normal = glm::normalize(vertex.normal);
            }
            else
            {
                vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }
    }

    std::expected<Mesh, std::string> importGltf(const std::string& path)
    {
        cgltf_options options{};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
        {
            return std::unexpected(std::format("cgltf: cannot parse '{}'", path));
        }
        if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
        {
            cgltf_free(data);
            return std::unexpected(std::format("cgltf: cannot load buffers for '{}'", path));
        }

        Mesh mesh;
        for (cgltf_size m = 0; m < data->meshes_count; m = m + 1)
        {
            const cgltf_mesh& gltfMesh = data->meshes[m];
            for (cgltf_size p = 0; p < gltfMesh.primitives_count; p = p + 1)
            {
                const cgltf_primitive& prim = gltfMesh.primitives[p];
                if (prim.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }

                const cgltf_accessor* positions = nullptr;
                const cgltf_accessor* normals = nullptr;
                const cgltf_accessor* texcoords = nullptr;
                for (cgltf_size a = 0; a < prim.attributes_count; a = a + 1)
                {
                    const cgltf_attribute& attr = prim.attributes[a];
                    if (attr.type == cgltf_attribute_type_position)
                    {
                        positions = attr.data;
                    }
                    else if (attr.type == cgltf_attribute_type_normal)
                    {
                        normals = attr.data;
                    }
                    else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0)
                    {
                        texcoords = attr.data;
                    }
                }
                if (positions == nullptr)
                {
                    continue;
                }

                const i32 vertexOffset = static_cast<i32>(mesh.vertices.size());
                const u32 firstIndex = static_cast<u32>(mesh.indices.size());
                const cgltf_size vertexCount = positions->count;
                for (cgltf_size i = 0; i < vertexCount; i = i + 1)
                {
                    Vertex vertex;
                    cgltf_float tmp[3] = { 0.0f, 0.0f, 0.0f };
                    cgltf_accessor_read_float(positions, i, tmp, 3);
                    vertex.position = glm::vec3(tmp[0], tmp[1], tmp[2]);
                    if (normals != nullptr)
                    {
                        cgltf_accessor_read_float(normals, i, tmp, 3);
                        vertex.normal = glm::vec3(tmp[0], tmp[1], tmp[2]);
                    }
                    if (texcoords != nullptr)
                    {
                        cgltf_float uv[2] = { 0.0f, 0.0f };
                        cgltf_accessor_read_float(texcoords, i, uv, 2);
                        vertex.uv0 = glm::vec2(uv[0], uv[1]);
                    }
                    mesh.vertices.push_back(vertex);
                }

                if (prim.indices != nullptr)
                {
                    for (cgltf_size i = 0; i < prim.indices->count; i = i + 1)
                    {
                        mesh.indices.push_back(static_cast<u32>(cgltf_accessor_read_index(prim.indices, i)));
                    }
                }
                else
                {
                    for (cgltf_size i = 0; i < vertexCount; i = i + 1)
                    {
                        mesh.indices.push_back(static_cast<u32>(i));
                    }
                }

                Submesh submesh;
                submesh.firstIndex = firstIndex;
                submesh.indexCount = static_cast<u32>(mesh.indices.size()) - firstIndex;
                submesh.vertexOffset = vertexOffset;
                submesh.materialSlot = 0;
                mesh.submeshes.push_back(submesh);
            }
        }
        cgltf_free(data);

        if (mesh.vertices.empty())
        {
            return std::unexpected(std::format("cgltf: '{}' has no triangle geometry", path));
        }
        if (!anyNormalsPresent(mesh))
        {
            generateNormals(mesh);
        }
        return mesh;
    }

    std::expected<Mesh, std::string> importObj(const std::string& path)
    {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string err;  // tinyobjloader 1.0.6 combines warnings + errors here
        const bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &err, path.c_str());
        if (!ok)
        {
            return std::unexpected(err.empty() ? std::format("tinyobjloader: cannot load '{}'", path) : err);
        }

        Mesh mesh;
        // De-duplicate (position, normal, texcoord) triples into unique vertices.
        std::map<std::array<int, 3>, u32> uniqueVertices;
        for (const tinyobj::shape_t& shape : shapes)
        {
            const u32 firstIndex = static_cast<u32>(mesh.indices.size());
            for (const tinyobj::index_t& index : shape.mesh.indices)
            {
                const std::array<int, 3> key{ index.vertex_index, index.normal_index, index.texcoord_index };
                auto it = uniqueVertices.find(key);
                if (it == uniqueVertices.end())
                {
                    Vertex vertex;
                    vertex.position = glm::vec3(
                        attrib.vertices[3 * index.vertex_index + 0],
                        attrib.vertices[3 * index.vertex_index + 1],
                        attrib.vertices[3 * index.vertex_index + 2]);
                    if (index.normal_index >= 0)
                    {
                        vertex.normal = glm::vec3(
                            attrib.normals[3 * index.normal_index + 0],
                            attrib.normals[3 * index.normal_index + 1],
                            attrib.normals[3 * index.normal_index + 2]);
                    }
                    if (index.texcoord_index >= 0)
                    {
                        // OBJ texture V origin is bottom-left; Vulkan samples top-left.
                        vertex.uv0 = glm::vec2(
                            attrib.texcoords[2 * index.texcoord_index + 0],
                            1.0f - attrib.texcoords[2 * index.texcoord_index + 1]);
                    }
                    const u32 newIndex = static_cast<u32>(mesh.vertices.size());
                    mesh.vertices.push_back(vertex);
                    uniqueVertices.emplace(key, newIndex);
                    mesh.indices.push_back(newIndex);
                }
                else
                {
                    mesh.indices.push_back(it->second);
                }
            }

            Submesh submesh;
            submesh.firstIndex = firstIndex;
            submesh.indexCount = static_cast<u32>(mesh.indices.size()) - firstIndex;
            submesh.vertexOffset = 0;  // indices already reference the shared vertex array
            submesh.materialSlot = 0;
            if (submesh.indexCount > 0)
            {
                mesh.submeshes.push_back(submesh);
            }
        }

        if (mesh.vertices.empty())
        {
            return std::unexpected(std::format("tinyobjloader: '{}' has no geometry", path));
        }
        if (!anyNormalsPresent(mesh))
        {
            generateNormals(mesh);
        }
        return mesh;
    }

    std::expected<Mesh, std::string> importModelFile(const std::string& path)
    {
        if (endsWithIgnoreCase(path, ".gltf") || endsWithIgnoreCase(path, ".glb"))
        {
            return importGltf(path);
        }
        if (endsWithIgnoreCase(path, ".obj"))
        {
            return importObj(path);
        }
        return std::unexpected(std::format("unsupported model format: '{}' (expected .gltf/.glb/.obj)", path));
    }

    std::expected<void, std::string> saveMesh(const Mesh& mesh, const std::string& path)
    {
        SMeshHeader header{};
        std::memcpy(header.magic, "SMSH", 4);
        header.version = MeshFormatVersion;
        header.flags = 0;
        header.vertexStride = sizeof(Vertex);
        header.vertexCount = static_cast<u32>(mesh.vertices.size());
        header.indexCount = static_cast<u32>(mesh.indices.size());
        header.indexWidth = sizeof(u32);
        header.submeshCount = static_cast<u32>(mesh.submeshes.size());
        header.verticesOffset = sizeof(SMeshHeader);
        header.indicesOffset = header.verticesOffset + static_cast<u64>(header.vertexCount) * sizeof(Vertex);
        header.submeshesOffset = header.indicesOffset + static_cast<u64>(header.indexCount) * sizeof(u32);

        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return std::unexpected(std::format("cannot open '{}' for writing", path));
        }
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(mesh.vertices.data()),
                  static_cast<std::streamsize>(mesh.vertices.size() * sizeof(Vertex)));
        out.write(reinterpret_cast<const char*>(mesh.indices.data()),
                  static_cast<std::streamsize>(mesh.indices.size() * sizeof(u32)));
        out.write(reinterpret_cast<const char*>(mesh.submeshes.data()),
                  static_cast<std::streamsize>(mesh.submeshes.size() * sizeof(Submesh)));
        if (!out)
        {
            return std::unexpected(std::format("write failed for '{}'", path));
        }
        return {};
    }

    std::expected<Mesh, std::string> loadMesh(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return std::unexpected(std::format("cannot open '{}'", path));
        }
        const std::streamsize fileSize = in.tellg();
        in.seekg(0);
        if (fileSize < static_cast<std::streamsize>(sizeof(SMeshHeader)))
        {
            return std::unexpected(std::format("'{}' is too small to be a .smesh", path));
        }

        SMeshHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (std::memcmp(header.magic, "SMSH", 4) != 0)
        {
            return std::unexpected(std::format("'{}' is not a .smesh (bad magic)", path));
        }
        if (header.version != MeshFormatVersion)
        {
            return std::unexpected(std::format("'{}' has unsupported mesh version {}", path, header.version));
        }
        if (header.vertexStride != sizeof(Vertex) || header.indexWidth != sizeof(u32))
        {
            return std::unexpected(std::format("'{}' has an incompatible vertex/index layout", path));
        }
        const u64 expectedSize = header.submeshesOffset + static_cast<u64>(header.submeshCount) * sizeof(Submesh);
        if (static_cast<u64>(fileSize) < expectedSize)
        {
            return std::unexpected(std::format("'{}' is truncated", path));
        }

        Mesh mesh;
        mesh.vertices.resize(header.vertexCount);
        mesh.indices.resize(header.indexCount);
        mesh.submeshes.resize(header.submeshCount);
        in.seekg(static_cast<std::streamoff>(header.verticesOffset));
        in.read(reinterpret_cast<char*>(mesh.vertices.data()),
                static_cast<std::streamsize>(header.vertexCount * sizeof(Vertex)));
        in.read(reinterpret_cast<char*>(mesh.indices.data()),
                static_cast<std::streamsize>(header.indexCount * sizeof(u32)));
        in.read(reinterpret_cast<char*>(mesh.submeshes.data()),
                static_cast<std::streamsize>(header.submeshCount * sizeof(Submesh)));
        if (!in)
        {
            return std::unexpected(std::format("read failed for '{}'", path));
        }
        return mesh;
    }

    void runGeometrySelfTest(const std::string& modelsDir)
    {
        std::expected<Mesh, std::string> obj = importObj(modelsDir + "/cube.obj");
        if (!obj)
        {
            logError(std::format("geometry self-test: obj import failed: {}", obj.error()));
            return;
        }
        logInfo(std::format("geometry self-test: cube.obj -> {} verts, {} indices, {} submeshes",
                            obj->vertices.size(), obj->indices.size(), obj->submeshes.size()));

        std::expected<Mesh, std::string> gltf = importGltf(modelsDir + "/cube.gltf");
        if (!gltf)
        {
            logError(std::format("geometry self-test: gltf import failed: {}", gltf.error()));
            return;
        }
        logInfo(std::format("geometry self-test: cube.gltf -> {} verts, {} indices, {} submeshes",
                            gltf->vertices.size(), gltf->indices.size(), gltf->submeshes.size()));

        const std::string bakedPath = "/tmp/saffron_cube.smesh";
        if (std::expected<void, std::string> saved = saveMesh(*gltf, bakedPath); !saved)
        {
            logError(std::format("geometry self-test: save failed: {}", saved.error()));
            return;
        }
        std::expected<Mesh, std::string> loaded = loadMesh(bakedPath);
        if (!loaded)
        {
            logError(std::format("geometry self-test: load failed: {}", loaded.error()));
            return;
        }

        const bool roundTrips = loaded->vertices.size() == gltf->vertices.size() &&
                                loaded->indices.size() == gltf->indices.size() &&
                                loaded->submeshes.size() == gltf->submeshes.size() &&
                                loaded->vertices[0].position == gltf->vertices[0].position;
        if (roundTrips)
        {
            logInfo(".smesh round-trip OK");
        }
        else
        {
            logError(".smesh round-trip MISMATCH");
        }
    }
}

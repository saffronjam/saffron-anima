module;

// cgltf + tinyobjloader + glm are header-heavy, so this module uses classic
// includes (no `import std`), like the rendering/scene modules.
#include <cgltf.h>
#include <tiny_obj_loader.h>
#include <stb_image.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_precision.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <array>
#include <cctype>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <optional>
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
    // signed to match vkCmdDrawIndexed; materialSlot indexes the model's material table.
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

    // Per-vertex skin influences, a second stream parallel to Mesh.vertices (empty ==
    // unskinned). Kept out of Vertex so the unskinned layout and v1 .smesh stay intact.
    struct VertexSkin
    {
        glm::u16vec4 joints{ 0 };   // indices into the skin's joint list
        glm::vec4 weights{ 0.0f };  // normalized blend weights
    };

    // One glTF node of the imported scene graph: name, parent index (-1 == root), and
    // the local TRS (rotation as the source quaternion; consumers convert to their
    // own Euler convention).
    struct ImportedNode
    {
        std::string name;
        i32 parent = -1;
        glm::vec3 translation{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 scale{ 1.0f };
    };

    // One glTF skin: the ordered joint node indices (the jointMatrices[] order) and
    // the parallel inverse bind matrices. meshNode is the node carrying the skinned
    // mesh; skeletonRoot is the skin's declared root (-1 == unspecified).
    struct ImportedSkin
    {
        std::vector<i32> joints;
        std::vector<glm::mat4> inverseBind;
        i32 skeletonRoot = -1;
        i32 meshNode = -1;
    };

    // .smesh version 1 is the unskinned three-section layout; version 2 appends a
    // VertexSkin section after the submeshes (same header, same first three sections).
    inline constexpr u32 MeshFormatVersion = 1;
    inline constexpr u32 MeshFormatVersionSkinned = 2;

    // One material extracted from a model: the PBR factors and, if any, the encoded
    // (png/jpg) albedo bytes (read from an external file or embedded). Metallic-roughness,
    // normal, and emissive textures are not imported — the engine material has no slots.
    struct ImportedMaterial
    {
        glm::vec4 baseColor{ 1.0f };
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        f32 emissiveStrength = 1.0f;
        std::vector<u8> albedoBytes;
        std::string albedoExt;  // "png" / "jpg"
        bool hasAlbedo = false;
        // glTF metallic-roughness texture (roughness in G, metalness in B); a linear texture.
        std::vector<u8> metallicRoughnessBytes;
        std::string metallicRoughnessExt;
        bool hasMetallicRoughness = false;
    };

    struct ImportedModel
    {
        Mesh mesh;
        // The material table; each Submesh.materialSlot indexes it. Always at least one
        // entry (a default material when the source declares none).
        std::vector<ImportedMaterial> materials;
        // Skin payload (glTF only): hasSkin gates all three. `skin` parallels
        // mesh.vertices; `nodes` is the source node forest; `skinDesc.joints` indexes
        // into `nodes` in glTF joint order — the single source of jointMatrices order.
        bool hasSkin = false;
        std::vector<VertexSkin> skin;
        std::vector<ImportedNode> nodes;
        ImportedSkin skinDesc;
    };

    // Decoded RGBA8 pixels, tightly packed (width*height*4 bytes).
    struct DecodedImage
    {
        std::vector<u8> rgba;
        u32 width = 0;
        u32 height = 0;
    };

    // Decoded linear float RGBA, tightly packed (width*height*4 floats). From .hdr/.exr-class
    // sources; values are real radiance (may exceed 1.0), never sRGB-encoded.
    struct DecodedImageFloat
    {
        std::vector<f32> rgba;
        u32 width = 0;
        u32 height = 0;
    };

    auto importGltf(const std::string& path) -> Result<Mesh>;
    auto importObj(const std::string& path) -> Result<Mesh>;
    auto importModelFile(const std::string& path) -> Result<Mesh>;  // dispatch by extension

    auto importModelWithMaterial(const std::string& path) -> Result<ImportedModel>;
    auto decodeImage(const std::string& path) -> Result<DecodedImage>;
    auto decodeImageFromMemory(const std::vector<u8>& encoded) -> Result<DecodedImage>;

    auto decodeImageHdr(const std::string& path) -> Result<DecodedImageFloat>;
    auto decodeImageFromMemoryHdr(const std::vector<u8>& encoded) -> Result<DecodedImageFloat>;

    auto saveMesh(const Mesh& mesh, const std::string& path) -> Result<void>;  // baked .smesh
    auto loadMesh(const std::string& path) -> Result<Mesh>;
    // Vertex/index totals read from a .smesh's 64-byte header, without loading the data.
    struct MeshCounts
    {
        u32 vertexCount;
        u32 indexCount;
    };
    auto meshFileCounts(const std::string& path) -> Result<MeshCounts>;
    // Skinned bake: v1 layout plus a VertexSkin section (skin must parallel vertices).
    auto saveMeshSkinned(const Mesh& mesh, const std::vector<VertexSkin>& skin, const std::string& path)
        -> Result<void>;
    // The skin stream of a v2 .smesh; empty (not an error) for a v1 file.
    auto loadMeshSkin(const std::string& path) -> Result<std::vector<VertexSkin>>;

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
    static_assert(sizeof(VertexSkin) == 24, "VertexSkin must stay 24 bytes (the .smesh v2 skin stride)");

    namespace
    {
        // 64-byte fixed header; three contiguous raw arrays follow at the offsets.
        struct SMeshHeader
        {
            char magic[4];  // 'S','M','S','H'
            u32 version;
            u32 flags;         // reserved (0)
            u32 vertexStride;  // == sizeof(Vertex)
            u32 vertexCount;
            u32 indexCount;
            u32 indexWidth;  // bytes per index (4)
            u32 submeshCount;
            u64 verticesOffset;
            u64 indicesOffset;
            u64 submeshesOffset;
            u32 reserved[2];
        };
        static_assert(sizeof(SMeshHeader) == 64, "SMeshHeader must be exactly 64 bytes");

        auto endsWithIgnoreCase(const std::string& text, const std::string& suffix) -> bool
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

        auto anyNormalsPresent(const Mesh& mesh) -> bool
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

        auto directoryOf(const std::string& path) -> std::string
        {
            const std::size_t slash = path.find_last_of("/\\");
            if (slash == std::string::npos)
            {
                return std::string{ "." };
            }
            return path.substr(0, slash);
        }

        auto extensionOf(const std::string& path) -> std::string
        {
            const std::size_t dot = path.find_last_of('.');
            if (dot == std::string::npos)
            {
                return std::string{};
            }
            return path.substr(dot + 1);
        }

        auto extensionFromMime(const std::string& mime) -> std::string
        {
            if (mime == "image/png")
            {
                return std::string{ "png" };
            }
            if (mime == "image/jpeg")
            {
                return std::string{ "jpg" };
            }
            return std::string{ "png" };
        }

        auto readBinaryFile(const std::string& path) -> Result<std::vector<u8>>
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in)
            {
                return Err(std::format("cannot open '{}'", path));
            }
            const std::streamsize size = in.tellg();
            in.seekg(0);
            std::vector<u8> bytes(static_cast<std::size_t>(size));
            in.read(reinterpret_cast<char*>(bytes.data()), size);
            if (!in)
            {
                return Err(std::format("read failed for '{}'", path));
            }
            return bytes;
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
                const glm::vec3 faceNormal = glm::cross(mesh.vertices[b].position - mesh.vertices[a].position,
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

    // Extract one glTF material's PBR factors + albedo bytes into the engine's material.
    // Metallic-roughness/normal/emissive textures are intentionally skipped.
    // Read a glTF texture view's encoded image bytes (embedded buffer view or external file).
    // Returns false (leaving outBytes empty) when the view has no image or the bytes can't be
    // read; a data: URI is logged and skipped. `label` names the slot in any warning.
    auto readGltfTextureBytes(const cgltf_texture_view& view, const std::string& path, const char* label,
                              std::vector<u8>& outBytes, std::string& outExt) -> bool
    {
        if (view.texture == nullptr || view.texture->image == nullptr)
        {
            return false;
        }
        const cgltf_image* image = view.texture->image;
        if (image->buffer_view != nullptr)
        {
            const cgltf_buffer_view* bufferView = image->buffer_view;
            const u8* bytes = static_cast<const u8*>(bufferView->buffer->data) + bufferView->offset;
            outBytes.assign(bytes, bytes + bufferView->size);
            outExt = extensionFromMime(image->mime_type != nullptr ? image->mime_type : "");
            return !outBytes.empty();
        }
        if (image->uri != nullptr && std::strncmp(image->uri, "data:", 5) != 0)
        {
            std::string uri = image->uri;
            uri.resize(cgltf_decode_uri(uri.data()));  // percent-decode (e.g. %20) in place
            const std::string full = directoryOf(path) + "/" + uri;
            if (Result<std::vector<u8>> bytes = readBinaryFile(full); bytes)
            {
                outBytes = std::move(*bytes);
                outExt = extensionOf(uri);
                return true;
            }
            return false;
        }
        if (image->uri != nullptr)
        {
            logWarn(std::format("cgltf: '{}' embeds its {} as a data: URI (not yet supported)", path, label));
        }
        return false;
    }

    auto extractGltfMaterial(const cgltf_material& src, const std::string& path) -> ImportedMaterial
    {
        ImportedMaterial material;
        material.emissive = glm::vec3(src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]);
        material.emissiveStrength = src.has_emissive_strength ? src.emissive_strength.emissive_strength : 1.0f;
        if (!src.has_pbr_metallic_roughness)
        {
            return material;
        }
        const cgltf_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
        material.baseColor = glm::vec4(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2],
                                       pbr.base_color_factor[3]);
        material.metallic = pbr.metallic_factor;
        material.roughness = pbr.roughness_factor;
        material.hasAlbedo =
            readGltfTextureBytes(pbr.base_color_texture, path, "albedo", material.albedoBytes, material.albedoExt);
        material.hasMetallicRoughness =
            readGltfTextureBytes(pbr.metallic_roughness_texture, path, "metallic-roughness",
                                 material.metallicRoughnessBytes, material.metallicRoughnessExt);
        return material;
    }

    auto importGltfModel(const std::string& path) -> Result<ImportedModel>
    {
        cgltf_options options{};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
        {
            return Err(std::format("cgltf: cannot parse '{}'", path));
        }
        if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
        {
            cgltf_free(data);
            return Err(std::format("cgltf: cannot load buffers for '{}'", path));
        }

        Mesh mesh;
        std::vector<VertexSkin> vertexSkins;  // parallel to mesh.vertices when skinned
        bool sawSkinnedPrimitive = false;
        bool sawUnskinnedPrimitive = false;
        // Distinct source materials in first-seen order (keyed on the cgltf material
        // pointer; a null key is a primitive with no material, which gets a default slot).
        std::vector<const cgltf_material*> materialTable;
        std::map<const cgltf_material*, u32> materialSlots;
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
                const cgltf_accessor* jointIndices = nullptr;
                const cgltf_accessor* jointWeights = nullptr;
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
                    else if (attr.type == cgltf_attribute_type_joints && attr.index == 0)
                    {
                        jointIndices = attr.data;
                    }
                    else if (attr.type == cgltf_attribute_type_weights && attr.index == 0)
                    {
                        jointWeights = attr.data;
                    }
                }
                if (positions == nullptr)
                {
                    continue;
                }
                if (jointIndices != nullptr && jointWeights != nullptr)
                {
                    sawSkinnedPrimitive = true;
                }
                else
                {
                    sawUnskinnedPrimitive = true;
                }
                auto [slotIt, inserted] =
                    materialSlots.try_emplace(prim.material, static_cast<u32>(materialTable.size()));
                if (inserted)
                {
                    materialTable.push_back(prim.material);
                }
                const u32 materialSlot = slotIt->second;

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
                    VertexSkin influence;
                    if (jointIndices != nullptr && jointWeights != nullptr)
                    {
                        cgltf_uint joints[4] = { 0, 0, 0, 0 };
                        cgltf_accessor_read_uint(jointIndices, i, joints, 4);
                        cgltf_float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                        cgltf_accessor_read_float(jointWeights, i, weights, 4);
                        influence.joints = glm::u16vec4(joints[0], joints[1], joints[2], joints[3]);
                        influence.weights = glm::vec4(weights[0], weights[1], weights[2], weights[3]);
                    }
                    vertexSkins.push_back(influence);
                }

                if (prim.indices != nullptr)
                {
                    for (cgltf_size i = 0; i < prim.indices->count; i = i + 1)
                    {
                        const cgltf_size index = cgltf_accessor_read_index(prim.indices, i);
                        if (index >= vertexCount)
                        {
                            cgltf_free(data);
                            return Err(std::format("cgltf: '{}' has an out-of-range index", path));
                        }
                        mesh.indices.push_back(static_cast<u32>(index));
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
                submesh.materialSlot = materialSlot;
                mesh.submeshes.push_back(submesh);
            }
        }
        std::vector<ImportedMaterial> materials;
        materials.reserve(materialTable.size());
        for (const cgltf_material* src : materialTable)
        {
            materials.push_back(src != nullptr ? extractGltfMaterial(*src, path) : ImportedMaterial{});
        }
        // Skin payload: only when the FIRST skin covers every triangle primitive (a
        // mixed skinned/unskinned model would deform unweighted vertices to the origin,
        // so it imports as plain geometry instead).
        ImportedModel model;
        if (data->skins_count > 0 && sawSkinnedPrimitive && !sawUnskinnedPrimitive)
        {
            const cgltf_skin& gltfSkin = data->skins[0];
            model.nodes.reserve(data->nodes_count);
            for (cgltf_size n = 0; n < data->nodes_count; n = n + 1)
            {
                const cgltf_node& node = data->nodes[n];
                ImportedNode imported;
                imported.name = node.name != nullptr ? node.name : std::format("Node {}", n);
                imported.parent = node.parent != nullptr ? static_cast<i32>(node.parent - data->nodes) : -1;
                if (node.has_matrix)
                {
                    glm::mat4 local;
                    std::memcpy(&local, node.matrix, sizeof(local));
                    glm::vec3 skew;
                    glm::vec4 perspective;
                    glm::decompose(local, imported.scale, imported.rotation, imported.translation, skew, perspective);
                }
                else
                {
                    if (node.has_translation)
                    {
                        imported.translation = glm::vec3(node.translation[0], node.translation[1], node.translation[2]);
                    }
                    if (node.has_rotation)
                    {
                        // glTF stores (x, y, z, w); glm::quat takes (w, x, y, z).
                        imported.rotation =
                            glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
                    }
                    if (node.has_scale)
                    {
                        imported.scale = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);
                    }
                }
                model.nodes.push_back(std::move(imported));
            }
            model.skinDesc.joints.reserve(gltfSkin.joints_count);
            for (cgltf_size j = 0; j < gltfSkin.joints_count; j = j + 1)
            {
                model.skinDesc.joints.push_back(static_cast<i32>(gltfSkin.joints[j] - data->nodes));
            }
            model.skinDesc.inverseBind.assign(gltfSkin.joints_count, glm::mat4(1.0f));
            if (gltfSkin.inverse_bind_matrices != nullptr)
            {
                for (cgltf_size j = 0; j < gltfSkin.joints_count; j = j + 1)
                {
                    cgltf_float m[16];
                    cgltf_accessor_read_float(gltfSkin.inverse_bind_matrices, j, m, 16);
                    std::memcpy(&model.skinDesc.inverseBind[j], m, sizeof(glm::mat4));
                }
            }
            model.skinDesc.skeletonRoot =
                gltfSkin.skeleton != nullptr ? static_cast<i32>(gltfSkin.skeleton - data->nodes) : -1;
            for (cgltf_size n = 0; n < data->nodes_count; n = n + 1)
            {
                if (data->nodes[n].skin == &gltfSkin && data->nodes[n].mesh != nullptr)
                {
                    model.skinDesc.meshNode = static_cast<i32>(n);
                    break;
                }
            }
            model.skin = std::move(vertexSkins);
            model.hasSkin = true;
        }
        else if (sawSkinnedPrimitive && sawUnskinnedPrimitive)
        {
            logWarn(std::format("cgltf: '{}' mixes skinned and unskinned primitives; importing unskinned", path));
        }
        cgltf_free(data);

        if (mesh.vertices.empty())
        {
            return Err(std::format("cgltf: '{}' has no triangle geometry", path));
        }
        if (!anyNormalsPresent(mesh))
        {
            generateNormals(mesh);
        }
        model.mesh = std::move(mesh);
        model.materials = std::move(materials);
        return model;
    }

    auto importGltf(const std::string& path) -> Result<Mesh>
    {
        auto model = importGltfModel(path);
        if (!model)
        {
            return Err(model.error());
        }
        return std::move(model->mesh);
    }

    auto importObjModel(const std::string& path) -> Result<ImportedModel>
    {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string err;                                // tinyobjloader 1.0.6 combines warnings + errors here
        const std::string baseDir = directoryOf(path);  // resolve .mtl + textures next to the obj
        const bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &err, path.c_str(), baseDir.c_str());
        if (!ok)
        {
            if (err.empty())
            {
                return Err(std::format("tinyobjloader: cannot load '{}'", path));
            }
            return Err(err);
        }

        Mesh mesh;
        // De-duplicate (position, normal, texcoord) triples into unique vertices.
        std::map<std::array<int, 3>, u32> uniqueVertices;
        std::optional<std::string> vertexError;
        const auto resolveVertex = [&](const tinyobj::index_t& index) -> u32
        {
            const std::array<int, 3> key{ index.vertex_index, index.normal_index, index.texcoord_index };
            if (auto it = uniqueVertices.find(key); it != uniqueVertices.end())
            {
                return it->second;
            }
            if (index.vertex_index < 0 ||
                static_cast<std::size_t>(3 * index.vertex_index + 2) >= attrib.vertices.size())
            {
                vertexError = std::format("tinyobjloader: '{}' has an out-of-range vertex index", path);
                return 0;
            }
            Vertex vertex;
            vertex.position =
                glm::vec3(attrib.vertices[3 * index.vertex_index + 0], attrib.vertices[3 * index.vertex_index + 1],
                          attrib.vertices[3 * index.vertex_index + 2]);
            if (index.normal_index >= 0 && static_cast<std::size_t>(3 * index.normal_index + 2) < attrib.normals.size())
            {
                vertex.normal =
                    glm::vec3(attrib.normals[3 * index.normal_index + 0], attrib.normals[3 * index.normal_index + 1],
                              attrib.normals[3 * index.normal_index + 2]);
            }
            if (index.texcoord_index >= 0 &&
                static_cast<std::size_t>(2 * index.texcoord_index + 1) < attrib.texcoords.size())
            {
                // OBJ texture V origin is bottom-left; Vulkan samples top-left.
                vertex.uv0 = glm::vec2(attrib.texcoords[2 * index.texcoord_index + 0],
                                       1.0f - attrib.texcoords[2 * index.texcoord_index + 1]);
            }
            const u32 newIndex = static_cast<u32>(mesh.vertices.size());
            mesh.vertices.push_back(vertex);
            uniqueVertices.emplace(key, newIndex);
            return newIndex;
        };

        // Group faces by material into slots in first-seen order; tinyobj triangulates by
        // default, so each face is three indices and material_ids is one id per face.
        // slotToObjMaterial[slot] is the tinyobj material index (-1 == no material).
        std::vector<int> slotToObjMaterial;
        std::map<int, u32> objMaterialToSlot;
        std::vector<std::vector<u32>> indicesBySlot;
        const auto slotFor = [&](int objMaterial) -> u32
        {
            const int normalized =
                objMaterial >= 0 && static_cast<std::size_t>(objMaterial) < materials.size() ? objMaterial : -1;
            auto [it, inserted] = objMaterialToSlot.try_emplace(normalized, static_cast<u32>(slotToObjMaterial.size()));
            if (inserted)
            {
                slotToObjMaterial.push_back(normalized);
                indicesBySlot.emplace_back();
            }
            return it->second;
        };
        for (const tinyobj::shape_t& shape : shapes)
        {
            const std::size_t faceCount = shape.mesh.indices.size() / 3;
            for (std::size_t f = 0; f < faceCount; f = f + 1)
            {
                const int objMaterial = f < shape.mesh.material_ids.size() ? shape.mesh.material_ids[f] : -1;
                std::vector<u32>& bucket = indicesBySlot[slotFor(objMaterial)];
                for (std::size_t c = 0; c < 3; c = c + 1)
                {
                    bucket.push_back(resolveVertex(shape.mesh.indices[f * 3 + c]));
                }
            }
        }
        if (vertexError)
        {
            return Err(*vertexError);
        }
        for (u32 slot = 0; slot < indicesBySlot.size(); slot = slot + 1)
        {
            if (indicesBySlot[slot].empty())
            {
                continue;
            }
            Submesh submesh;
            submesh.firstIndex = static_cast<u32>(mesh.indices.size());
            submesh.indexCount = static_cast<u32>(indicesBySlot[slot].size());
            submesh.vertexOffset = 0;  // indices already reference the shared vertex array
            submesh.materialSlot = slot;
            mesh.indices.insert(mesh.indices.end(), indicesBySlot[slot].begin(), indicesBySlot[slot].end());
            mesh.submeshes.push_back(submesh);
        }

        if (mesh.vertices.empty())
        {
            return Err(std::format("tinyobjloader: '{}' has no geometry", path));
        }
        if (!anyNormalsPresent(mesh))
        {
            generateNormals(mesh);
        }

        ImportedModel model;
        model.mesh = std::move(mesh);
        model.materials.reserve(slotToObjMaterial.size());
        for (int objMaterial : slotToObjMaterial)
        {
            ImportedMaterial material;
            if (objMaterial >= 0)
            {
                const tinyobj::material_t& mat = materials[static_cast<std::size_t>(objMaterial)];
                material.baseColor = glm::vec4(mat.diffuse[0], mat.diffuse[1], mat.diffuse[2], 1.0f);
                material.metallic = mat.metallic;
                material.roughness = mat.roughness;
                material.emissive = glm::vec3(mat.emission[0], mat.emission[1], mat.emission[2]);
                if (!mat.diffuse_texname.empty())
                {
                    const std::string full = baseDir + "/" + mat.diffuse_texname;
                    if (Result<std::vector<u8>> bytes = readBinaryFile(full); bytes)
                    {
                        material.albedoBytes = std::move(*bytes);
                        material.albedoExt = extensionOf(mat.diffuse_texname);
                        material.hasAlbedo = true;
                    }
                }
            }
            model.materials.push_back(std::move(material));
        }
        return model;
    }

    auto importObj(const std::string& path) -> Result<Mesh>
    {
        auto model = importObjModel(path);
        if (!model)
        {
            return Err(model.error());
        }
        return std::move(model->mesh);
    }

    auto importModelFile(const std::string& path) -> Result<Mesh>
    {
        if (endsWithIgnoreCase(path, ".gltf") || endsWithIgnoreCase(path, ".glb"))
        {
            return importGltf(path);
        }
        if (endsWithIgnoreCase(path, ".obj"))
        {
            return importObj(path);
        }
        return Err(std::format("unsupported model format: '{}' (expected .gltf/.glb/.obj)", path));
    }

    auto importModelWithMaterial(const std::string& path) -> Result<ImportedModel>
    {
        if (endsWithIgnoreCase(path, ".gltf") || endsWithIgnoreCase(path, ".glb"))
        {
            return importGltfModel(path);
        }
        if (endsWithIgnoreCase(path, ".obj"))
        {
            return importObjModel(path);
        }
        return Err(std::format("unsupported model format: '{}' (expected .gltf/.glb/.obj)", path));
    }

    auto decodeImage(const std::string& path) -> Result<DecodedImage>
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr)
        {
            return Err(std::format("cannot decode image '{}'", path));
        }
        DecodedImage image;
        image.width = static_cast<u32>(width);
        image.height = static_cast<u32>(height);
        image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return image;
    }

    auto decodeImageFromMemory(const std::vector<u8>& encoded) -> Result<DecodedImage>
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height,
                                                &channels, STBI_rgb_alpha);
        if (pixels == nullptr)
        {
            return Err(std::string{ "cannot decode image from memory" });
        }
        DecodedImage image;
        image.width = static_cast<u32>(width);
        image.height = static_cast<u32>(height);
        image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return image;
    }

    auto decodeImageHdr(const std::string& path) -> Result<DecodedImageFloat>
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        float* pixels = stbi_loadf(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr)
        {
            return Err(std::format("cannot decode HDR image '{}'", path));
        }
        DecodedImageFloat image;
        image.width = static_cast<u32>(width);
        image.height = static_cast<u32>(height);
        image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return image;
    }

    auto decodeImageFromMemoryHdr(const std::vector<u8>& encoded) -> Result<DecodedImageFloat>
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        float* pixels = stbi_loadf_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height,
                                               &channels, STBI_rgb_alpha);
        if (pixels == nullptr)
        {
            return Err(std::string{ "cannot decode HDR image from memory" });
        }
        DecodedImageFloat image;
        image.width = static_cast<u32>(width);
        image.height = static_cast<u32>(height);
        image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return image;
    }

    auto saveMesh(const Mesh& mesh, const std::string& path) -> Result<void>
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
            return Err(std::format("cannot open '{}' for writing", path));
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
            return Err(std::format("write failed for '{}'", path));
        }
        return {};
    }

    auto loadMesh(const std::string& path) -> Result<Mesh>
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        const std::streamsize fileSize = in.tellg();
        in.seekg(0);
        if (fileSize < static_cast<std::streamsize>(sizeof(SMeshHeader)))
        {
            return Err(std::format("'{}' is too small to be a .smesh", path));
        }

        SMeshHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (std::memcmp(header.magic, "SMSH", 4) != 0)
        {
            return Err(std::format("'{}' is not a .smesh (bad magic)", path));
        }
        if (header.version != MeshFormatVersion && header.version != MeshFormatVersionSkinned)
        {
            return Err(std::format("'{}' has unsupported mesh version {}", path, header.version));
        }
        if (header.vertexStride != sizeof(Vertex) || header.indexWidth != sizeof(u32))
        {
            return Err(std::format("'{}' has an incompatible vertex/index layout", path));
        }
        // Recompute the layout from the counts and require the header offsets to match
        // and the file to be large enough. This rejects a malformed huge count before
        // it reaches resize() (which would otherwise abort on a giant allocation).
        const u64 verticesEnd =
            static_cast<u64>(sizeof(SMeshHeader)) + static_cast<u64>(header.vertexCount) * sizeof(Vertex);
        const u64 indicesEnd = verticesEnd + static_cast<u64>(header.indexCount) * sizeof(u32);
        const u64 submeshesEnd = indicesEnd + static_cast<u64>(header.submeshCount) * sizeof(Submesh);
        if (header.verticesOffset != sizeof(SMeshHeader) || header.indicesOffset != verticesEnd ||
            header.submeshesOffset != indicesEnd || static_cast<u64>(fileSize) < submeshesEnd)
        {
            return Err(std::format("'{}' has an inconsistent or truncated layout", path));
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
            return Err(std::format("read failed for '{}'", path));
        }
        return mesh;
    }

    auto meshFileCounts(const std::string& path) -> Result<MeshCounts>
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        SMeshHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || std::memcmp(header.magic, "SMSH", 4) != 0)
        {
            return Err(std::format("'{}' is not a .smesh (bad magic)", path));
        }
        return MeshCounts{ header.vertexCount, header.indexCount };
    }

    auto saveMeshSkinned(const Mesh& mesh, const std::vector<VertexSkin>& skin, const std::string& path) -> Result<void>
    {
        if (skin.size() != mesh.vertices.size())
        {
            return Err(
                std::format("skin stream ({}) does not parallel the vertices ({})", skin.size(), mesh.vertices.size()));
        }
        SMeshHeader header{};
        std::memcpy(header.magic, "SMSH", 4);
        header.version = MeshFormatVersionSkinned;
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
            return Err(std::format("cannot open '{}' for writing", path));
        }
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(mesh.vertices.data()),
                  static_cast<std::streamsize>(mesh.vertices.size() * sizeof(Vertex)));
        out.write(reinterpret_cast<const char*>(mesh.indices.data()),
                  static_cast<std::streamsize>(mesh.indices.size() * sizeof(u32)));
        out.write(reinterpret_cast<const char*>(mesh.submeshes.data()),
                  static_cast<std::streamsize>(mesh.submeshes.size() * sizeof(Submesh)));
        out.write(reinterpret_cast<const char*>(skin.data()),
                  static_cast<std::streamsize>(skin.size() * sizeof(VertexSkin)));
        if (!out)
        {
            return Err(std::format("write failed for '{}'", path));
        }
        return {};
    }

    auto loadMeshSkin(const std::string& path) -> Result<std::vector<VertexSkin>>
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        const std::streamsize fileSize = in.tellg();
        in.seekg(0);
        if (fileSize < static_cast<std::streamsize>(sizeof(SMeshHeader)))
        {
            return Err(std::format("'{}' is too small to be a .smesh", path));
        }
        SMeshHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (std::memcmp(header.magic, "SMSH", 4) != 0)
        {
            return Err(std::format("'{}' is not a .smesh (bad magic)", path));
        }
        if (header.version != MeshFormatVersionSkinned)
        {
            return std::vector<VertexSkin>{};  // v1: unskinned, empty stream
        }
        const u64 submeshesEnd = header.submeshesOffset + static_cast<u64>(header.submeshCount) * sizeof(Submesh);
        const u64 skinEnd = submeshesEnd + static_cast<u64>(header.vertexCount) * sizeof(VertexSkin);
        if (static_cast<u64>(fileSize) < skinEnd)
        {
            return Err(std::format("'{}' is missing its skin section", path));
        }
        std::vector<VertexSkin> skin(header.vertexCount);
        in.seekg(static_cast<std::streamoff>(submeshesEnd));
        in.read(reinterpret_cast<char*>(skin.data()),
                static_cast<std::streamsize>(header.vertexCount * sizeof(VertexSkin)));
        if (!in)
        {
            return Err(std::format("read failed for '{}'", path));
        }
        return skin;
    }

    void runGeometrySelfTest(const std::string& modelsDir)
    {
        auto obj = importObj(modelsDir + "/cube.obj");
        if (!obj)
        {
            logError(std::format("geometry self-test: obj import failed: {}", obj.error()));
            return;
        }
        logInfo(std::format("geometry self-test: cube.obj -> {} verts, {} indices, {} submeshes", obj->vertices.size(),
                            obj->indices.size(), obj->submeshes.size()));

        auto gltf = importGltf(modelsDir + "/cube.gltf");
        if (!gltf)
        {
            logError(std::format("geometry self-test: gltf import failed: {}", gltf.error()));
            return;
        }
        logInfo(std::format("geometry self-test: cube.gltf -> {} verts, {} indices, {} submeshes",
                            gltf->vertices.size(), gltf->indices.size(), gltf->submeshes.size()));

        const std::string bakedPath = "/tmp/saffron_cube.smesh";
        if (Result<void> saved = saveMesh(*gltf, bakedPath); !saved)
        {
            logError(std::format("geometry self-test: save failed: {}", saved.error()));
            return;
        }
        auto loaded = loadMesh(bakedPath);
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

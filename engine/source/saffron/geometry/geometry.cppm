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

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

    /// One animated joint channel: a sampled curve targeting a joint's translation,
    /// rotation, or scale. A faithful, lossless mirror of a glTF animation channel +
    /// sampler — bound to a joint by stable index plus the durable node name.
    struct AnimTrack
    {
        /// Stable index into SkinnedMeshComponent.bones (resolved at import by name).
        i32 joint = -1;
        /// The glTF node name — the durable binding key (survives reorder/reimport).
        std::string jointName;
        enum class Path : u8
        {
            Translation,
            Rotation,
            Scale,
        } path = Path::Translation;
        enum class Interp : u8
        {
            Step,
            Linear,
            CubicSpline,
        } interp = Interp::Linear;
        std::vector<f32> times;   // sampler.input — strictly increasing, seconds
        std::vector<f32> values;  // sampler.output — flat: vec3 per key (T/S) or quat
                                  // xyzw per key (R); CubicSpline stores 3x
                                  // (in-tangent, value, out-tangent) per key
    };

    /// A named animation clip: a bundle of joint tracks with a total duration. POD-ish
    /// and serializable; the .sanim (SANM) writer/loader lives next to saveMeshSkinned.
    struct AnimClip
    {
        std::string name;
        f32 duration = 0.0f;  // max track end time, seconds
        std::vector<AnimTrack> tracks;
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

    // The unskinned .smesh: a three-section layout (vertices, indices, submeshes).
    inline constexpr u32 MeshFormatVersion = 1;
    // The skinned .smesh: the same header and first three sections, plus a VertexSkin
    // section appended after the submeshes.
    inline constexpr u32 MeshFormatVersionSkinned = 2;

    // One material extracted from a model: the PBR factors and, if any, the encoded
    // (png/jpg) albedo bytes (read from an external file or embedded). Metallic-roughness,
    // normal, and emissive textures are not imported — the engine material has no slots.
    struct ImportedMaterial
    {
        std::string name;  // source material name (the stable key for its baked sub-id)
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
        std::vector<u8> normalBytes;  // tangent-space normal map (linear)
        std::string normalExt;
        bool hasNormal = false;
        std::vector<u8> occlusionBytes;  // ambient occlusion (linear, AO in R)
        std::string occlusionExt;
        bool hasOcclusion = false;
        std::vector<u8> emissiveTexBytes;  // emissive map (sRGB)
        std::string emissiveTexExt;
        bool hasEmissiveTex = false;
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
        // Skeletal clips decoded from the glTF animations (skinned models only); each
        // track binds to a joint by index into skinDesc.joints plus the node name.
        std::vector<AnimClip> animations;
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

    /// A material texture slot's semantic role. The import-options colorspace policy keys on it
    /// (albedo/emissive → sRGB color; the rest → linear data), so one source of truth decides how
    /// a baked or scanned texture is interpreted.
    enum class MaterialMapRole : u8
    {
        Albedo,
        MetallicRoughness,
        Normal,
        Occlusion,
        Emissive,
        Height
    };

    /// Format-neutral translator: parse a source file (.gltf/.glb/.obj) into the in-memory import
    /// graph (`ImportedModel`). Pure — no GPU, no disk writes, no catalog mutation, no spawn. The
    /// read half of the read→decide→build split; the same source bytes always yield the same graph.
    auto translateModel(const std::string& source) -> Result<ImportedModel>;

    /// A stable sub-asset id derived from a source key, NOT from enumeration order or `newUuid`, so
    /// a reimport that reorders meshes/materials still resolves the same sub-id. `dupIndex`
    /// disambiguates duplicate source names (assigned in source declaration order). Folded into the
    /// non-reserved id range (>= 1024) so it never collides with a built-in id.
    auto subIdFor(std::string_view modelKey, std::string_view kind, std::string_view sourceName, u32 dupIndex) -> Uuid;

    auto decodeImage(const std::string& path) -> Result<DecodedImage>;
    auto decodeImageFromMemory(const std::vector<u8>& encoded) -> Result<DecodedImage>;

    auto decodeImageHdr(const std::string& path) -> Result<DecodedImageFloat>;
    auto decodeImageFromMemoryHdr(const std::vector<u8>& encoded) -> Result<DecodedImageFloat>;

    auto loadMesh(const std::string& path) -> Result<Mesh>;
    // Vertex/index totals read from a .smesh's 64-byte header, without loading the data.
    struct MeshCounts
    {
        u32 vertexCount;
        u32 indexCount;
    };
    auto meshFileCounts(const std::string& path) -> Result<MeshCounts>;
    /// The same counts from an in-memory `.smesh` image (a `.smodel` mesh chunk slice or a whole file).
    auto meshCountsFromBytes(std::span<const std::byte> bytes) -> Result<MeshCounts>;
    // Skinned bake: v1 layout plus a VertexSkin section (skin must parallel vertices).
    auto saveMeshSkinned(const Mesh& mesh, const std::vector<VertexSkin>& skin, const std::string& path)
        -> Result<void>;
    // The skin stream of a v2 .smesh; empty (not an error) for a v1 file.
    auto loadMeshSkin(const std::string& path) -> Result<std::vector<VertexSkin>>;

    /// The `.smesh` image as an in-memory buffer, so it can be embedded verbatim as a `.smodel` MESH
    /// chunk or written to a standalone file.
    auto saveMeshToBuffer(const Mesh& mesh) -> std::vector<std::byte>;
    /// The skinned (v2) `.smesh` image as a buffer; errors if the skin does not parallel the vertices.
    auto saveMeshSkinnedToBuffer(const Mesh& mesh, const std::vector<VertexSkin>& skin)
        -> Result<std::vector<std::byte>>;
    /// Parse a `.smesh` image from a memory span (a `.smodel` chunk slice or a whole file's bytes).
    /// Header offsets are validated against the span length, so a chunk reads exactly like a file.
    auto loadMeshFromBytes(std::span<const std::byte> bytes) -> Result<Mesh>;
    /// The v2 skin stream from a `.smesh` image in memory; empty (not an error) for a v1 image.
    auto loadMeshSkinFromBytes(std::span<const std::byte> bytes) -> Result<std::vector<VertexSkin>>;

    // One animation clip baked to a sidecar `.sanim` (magic `SANM`), never folded into
    // the `.smesh`. Little-endian raw, versioned, mirroring the `.smesh` shape.
    auto saveAnimation(const AnimClip& clip, const std::string& path) -> Result<void>;
    auto loadAnimation(const std::string& path) -> Result<AnimClip>;
    /// The `.sanim` image as a buffer, for embedding as a `.smodel` SANM chunk.
    auto saveAnimationToBuffer(const AnimClip& clip) -> std::vector<std::byte>;
    /// Parse a `.sanim` image from a memory span (a `.smodel` SANM chunk slice or a whole file).
    auto loadAnimationFromBytes(std::span<const std::byte> bytes) -> Result<AnimClip>;

    /// Container framing version (the SMDL header layout). Bumped only when the byte
    /// framing changes; independent of the metadata-chunk schema version.
    inline constexpr u32 ContainerFormatVersion = 1;
    /// Metadata-chunk schema version, stamped into the header for cheap gating.
    inline constexpr u32 MetadataSchemaVersion = 1;

    /// Four-character chunk tag packed little-endian into a u32 (tag[0] in the low byte).
    constexpr auto fourcc(const char (&tag)[5]) -> u32
    {
        return u32(static_cast<u8>(tag[0])) | (u32(static_cast<u8>(tag[1])) << 8) |
               (u32(static_cast<u8>(tag[2])) << 16) | (u32(static_cast<u8>(tag[3])) << 24);
    }

    /// The kind of a `.smodel` chunk; the value is its on-disk fourcc tag.
    enum class ChunkKind : u32
    {
        Meta = fourcc("META"),
        Mesh = fourcc("MESH"),
        Texture = fourcc("STEX"),
        Material = fourcc("SMAT"),
        Animation = fourcc("SANM"),
        Thumbnail = fourcc("THMB"),
    };

    /// One `.smodel` container header (64 bytes, little-endian). Mirrors SMeshHeader's
    /// discipline: fixed magic, version gate, 64-bit offsets, file-size validation.
    struct SModelHeader
    {
        char magic[4];         // 'S','M','D','L'
        u32 containerVersion;  // framing version; ContainerFormatVersion
        u32 schemaVersion;     // metadata-chunk schema version
        u32 flags;             // reserved framing flags
        u32 tocCount;          // number of TocEntry records
        u32 reserved0;         // pad to align the following u64s
        u64 tocOffset;         // byte offset of the chunk table
        u64 metaOffset;        // byte offset of the META chunk (front-loaded; 0 if absent)
        u64 metaLength;        // META chunk byte length (0 if absent)
        u64 totalLength;       // total file size, validated on read
        u32 reserved[2];       // pad to 64 bytes
    };
    static_assert(sizeof(SModelHeader) == 64, "SModelHeader must be exactly 64 bytes");

    /// One chunk-table record (32 bytes, fixed stride); offset/length address the payload.
    struct TocEntry
    {
        u32 fourcc;  // ChunkKind value
        u32 flags;   // per-chunk flags (colorspace, hasSkin, ...)
        u64 subId;   // stable sub-asset Uuid (0 for META/THMB)
        u64 offset;  // absolute byte offset of the payload
        u64 length;  // payload byte length
    };
    static_assert(sizeof(TocEntry) == 32, "TocEntry must stay 32 bytes (the .smodel TOC stride)");

    /// A chunk to write. The caller owns the bytes; writeContainer only frames them.
    struct ContainerChunk
    {
        ChunkKind kind;
        u64 subId = 0;
        u32 flags = 0;
        std::span<const std::byte> bytes;
    };

    /// An opened container: its validated header + chunk table, able to slice any chunk's
    /// bytes lazily from disk. Holds the path; drop it before device teardown like a Ref.
    struct ContainerReader
    {
        std::string path;
        SModelHeader header{};
        std::vector<TocEntry> toc;
        /// Read [entry.offset, entry.offset+entry.length) from the file, bounds-checked.
        auto readChunk(const TocEntry& entry) const -> Result<std::vector<std::byte>>;
        /// First TOC entry matching (kind, subId), or nullptr.
        auto find(ChunkKind kind, u64 subId) const -> const TocEntry*;
    };

    /// Write all chunks into one `.smodel`. The META chunk (if any) is placed first after
    /// the TOC and recorded in metaOffset/metaLength; payloads are 16-byte aligned.
    auto writeContainer(const std::string& path, std::span<const ContainerChunk> chunks) -> Result<void>;
    /// Cheap: read + validate only the 64-byte header (magic, version, totalLength vs size).
    auto readContainerHeader(const std::string& path) -> Result<SModelHeader>;
    /// Full: header + chunk table, returning a reader that can slice chunks lazily.
    auto readContainer(const std::string& path) -> Result<ContainerReader>;

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

        // 32-byte fixed header for a sidecar `.sanim` clip. The clip name follows, then
        // per-track: {i32 joint; u8 path; u8 interp; u16 pad; u32 nameLen; u32 timeCount;
        // u32 valueCount} + name + times floats + values floats.
        struct SANimHeader
        {
            char magic[4];  // 'S','A','N','M'
            u32 version;
            u32 trackCount;
            f32 duration;
            u32 nameLen;
            u32 reserved[3];
        };
        static_assert(sizeof(SANimHeader) == 32, "SANimHeader must be exactly 32 bytes");

        // 20-byte per-track record; the joint name, times, then values follow it.
        struct SANimTrackRecord
        {
            i32 joint;
            u8 path;
            u8 interp;
            u16 pad;
            u32 nameLen;
            u32 timeCount;
            u32 valueCount;
        };
        static_assert(sizeof(SANimTrackRecord) == 20, "SANimTrackRecord must be exactly 20 bytes");

        inline constexpr u32 AnimFormatVersion = 1;

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

        // Map a glTF animation target path onto the engine's AnimTrack path enum.
        auto toTrackPath(cgltf_animation_path_type path) -> AnimTrack::Path
        {
            if (path == cgltf_animation_path_type_rotation)
            {
                return AnimTrack::Path::Rotation;
            }
            if (path == cgltf_animation_path_type_scale)
            {
                return AnimTrack::Path::Scale;
            }
            return AnimTrack::Path::Translation;
        }

        // Map a glTF sampler interpolation onto the engine's AnimTrack interpolation enum.
        auto toTrackInterp(cgltf_interpolation_type interp) -> AnimTrack::Interp
        {
            if (interp == cgltf_interpolation_type_step)
            {
                return AnimTrack::Interp::Step;
            }
            if (interp == cgltf_interpolation_type_cubic_spline)
            {
                return AnimTrack::Interp::CubicSpline;
            }
            return AnimTrack::Interp::Linear;
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
            const char* mime = "";
            if (image->mime_type != nullptr)
            {
                mime = image->mime_type;
            }
            outExt = extensionFromMime(mime);
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
        if (src.name != nullptr)
        {
            material.name = src.name;
        }
        material.emissive = glm::vec3(src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]);
        if (src.has_emissive_strength)
        {
            material.emissiveStrength = src.emissive_strength.emissive_strength;
        }
        material.hasNormal =
            readGltfTextureBytes(src.normal_texture, path, "normal", material.normalBytes, material.normalExt);
        material.hasOcclusion = readGltfTextureBytes(src.occlusion_texture, path, "occlusion", material.occlusionBytes,
                                                     material.occlusionExt);
        material.hasEmissiveTex = readGltfTextureBytes(src.emissive_texture, path, "emissive",
                                                       material.emissiveTexBytes, material.emissiveTexExt);
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
        std::optional<std::string> primitiveError;
        auto appendPrimitive = [&](const cgltf_primitive& prim, const glm::mat4& nodeTransform, bool applyNodeTransform)
        {
            if (primitiveError || prim.type != cgltf_primitive_type_triangles)
            {
                return;
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
                return;
            }
            if (jointIndices != nullptr && jointWeights != nullptr)
            {
                sawSkinnedPrimitive = true;
            }
            else
            {
                sawUnskinnedPrimitive = true;
            }
            auto [slotIt, inserted] = materialSlots.try_emplace(prim.material, static_cast<u32>(materialTable.size()));
            if (inserted)
            {
                materialTable.push_back(prim.material);
            }
            const u32 materialSlot = slotIt->second;

            const i32 vertexOffset = static_cast<i32>(mesh.vertices.size());
            const u32 firstIndex = static_cast<u32>(mesh.indices.size());
            const cgltf_size vertexCount = positions->count;
            glm::mat3 normalTransform(1.0f);
            if (applyNodeTransform)
            {
                normalTransform = glm::transpose(glm::inverse(glm::mat3(nodeTransform)));
            }
            for (cgltf_size i = 0; i < vertexCount; i = i + 1)
            {
                Vertex vertex;
                cgltf_float tmp[3] = { 0.0f, 0.0f, 0.0f };
                cgltf_accessor_read_float(positions, i, tmp, 3);
                vertex.position = glm::vec3(tmp[0], tmp[1], tmp[2]);
                if (applyNodeTransform)
                {
                    vertex.position = glm::vec3(nodeTransform * glm::vec4(vertex.position, 1.0f));
                }
                if (normals != nullptr)
                {
                    cgltf_accessor_read_float(normals, i, tmp, 3);
                    vertex.normal = glm::vec3(tmp[0], tmp[1], tmp[2]);
                    if (applyNodeTransform)
                    {
                        vertex.normal = glm::normalize(normalTransform * vertex.normal);
                    }
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
                        primitiveError = std::format("cgltf: '{}' has an out-of-range index", path);
                        return;
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
        };

        if (data->skins_count == 0)
        {
            bool sawMeshNode = false;
            for (cgltf_size n = 0; n < data->nodes_count; n = n + 1)
            {
                const cgltf_node& node = data->nodes[n];
                if (node.mesh == nullptr)
                {
                    continue;
                }
                sawMeshNode = true;
                cgltf_float matrix[16];
                cgltf_node_transform_world(&node, matrix);
                glm::mat4 nodeTransform;
                std::memcpy(&nodeTransform, matrix, sizeof(nodeTransform));
                for (cgltf_size p = 0; p < node.mesh->primitives_count; p = p + 1)
                {
                    appendPrimitive(node.mesh->primitives[p], nodeTransform, true);
                }
            }
            if (!sawMeshNode)
            {
                for (cgltf_size m = 0; m < data->meshes_count; m = m + 1)
                {
                    const cgltf_mesh& gltfMesh = data->meshes[m];
                    for (cgltf_size p = 0; p < gltfMesh.primitives_count; p = p + 1)
                    {
                        appendPrimitive(gltfMesh.primitives[p], glm::mat4(1.0f), false);
                    }
                }
            }
        }
        else
        {
            for (cgltf_size m = 0; m < data->meshes_count; m = m + 1)
            {
                const cgltf_mesh& gltfMesh = data->meshes[m];
                for (cgltf_size p = 0; p < gltfMesh.primitives_count; p = p + 1)
                {
                    appendPrimitive(gltfMesh.primitives[p], glm::mat4(1.0f), false);
                }
            }
        }
        if (primitiveError)
        {
            cgltf_free(data);
            return Err(*primitiveError);
        }
        std::vector<ImportedMaterial> materials;
        materials.reserve(materialTable.size());
        for (const cgltf_material* src : materialTable)
        {
            if (src != nullptr)
            {
                materials.push_back(extractGltfMaterial(*src, path));
            }
            else
            {
                materials.push_back(ImportedMaterial{});
            }
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
                if (node.name != nullptr)
                {
                    imported.name = node.name;
                }
                else
                {
                    imported.name = std::format("Node {}", n);
                }
                if (node.parent != nullptr)
                {
                    imported.parent = static_cast<i32>(node.parent - data->nodes);
                }
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
            if (gltfSkin.skeleton != nullptr)
            {
                model.skinDesc.skeletonRoot = static_cast<i32>(gltfSkin.skeleton - data->nodes);
            }
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

            // Decode skeletal clips. A channel binds to a joint by its position in the
            // skin's joint list (the SkinnedMeshComponent.bones order); channels targeting
            // a non-joint node, morph weights, or sparse samplers are skipped in v1.
            for (cgltf_size a = 0; a < data->animations_count; a = a + 1)
            {
                const cgltf_animation& anim = data->animations[a];
                AnimClip clip;
                if (anim.name != nullptr)
                {
                    clip.name = anim.name;
                }
                else
                {
                    clip.name = std::format("clip_{}", a);
                }
                for (cgltf_size c = 0; c < anim.channels_count; c = c + 1)
                {
                    const cgltf_animation_channel& channel = anim.channels[c];
                    if (channel.target_node == nullptr || channel.sampler == nullptr)
                    {
                        continue;
                    }
                    if (channel.target_path == cgltf_animation_path_type_weights)
                    {
                        logWarn(
                            std::format("cgltf: '{}' clip '{}' has a morph-weights channel; skipped", path, clip.name));
                        continue;
                    }
                    const auto nodeIndex = static_cast<i32>(channel.target_node - data->nodes);
                    i32 joint = -1;
                    for (std::size_t j = 0; j < model.skinDesc.joints.size(); j = j + 1)
                    {
                        if (model.skinDesc.joints[j] == nodeIndex)
                        {
                            joint = static_cast<i32>(j);
                            break;
                        }
                    }
                    if (joint < 0)
                    {
                        logWarn(std::format("cgltf: '{}' clip '{}' targets a non-skin node; channel skipped", path,
                                            clip.name));
                        continue;
                    }
                    const cgltf_animation_sampler& sampler = *channel.sampler;
                    if (sampler.input == nullptr || sampler.output == nullptr || sampler.input->is_sparse ||
                        sampler.output->is_sparse)
                    {
                        logWarn(std::format("cgltf: '{}' clip '{}' has a sparse or empty sampler; channel skipped",
                                            path, clip.name));
                        continue;
                    }

                    AnimTrack track;
                    track.joint = joint;
                    track.jointName = model.nodes[static_cast<std::size_t>(nodeIndex)].name;
                    track.path = toTrackPath(channel.target_path);
                    track.interp = toTrackInterp(sampler.interpolation);
                    cgltf_size componentCount = 3;
                    if (track.path == AnimTrack::Path::Rotation)
                    {
                        componentCount = 4;
                    }
                    const cgltf_size components = componentCount;

                    track.times.resize(sampler.input->count);
                    for (cgltf_size k = 0; k < sampler.input->count; k = k + 1)
                    {
                        cgltf_accessor_read_float(sampler.input, k, &track.times[k], 1);
                    }
                    track.values.resize(sampler.output->count * components);
                    for (cgltf_size e = 0; e < sampler.output->count; e = e + 1)
                    {
                        cgltf_accessor_read_float(sampler.output, e, &track.values[e * components], components);
                    }
                    if (!track.times.empty() && track.times.back() > clip.duration)
                    {
                        clip.duration = track.times.back();
                    }
                    clip.tracks.push_back(std::move(track));
                }
                if (!clip.tracks.empty())
                {
                    model.animations.push_back(std::move(clip));
                }
            }
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
            int normalized = -1;
            if (objMaterial >= 0 && static_cast<std::size_t>(objMaterial) < materials.size())
            {
                normalized = objMaterial;
            }
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
                int objMaterial = -1;
                if (f < shape.mesh.material_ids.size())
                {
                    objMaterial = shape.mesh.material_ids[f];
                }
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

    auto translateModel(const std::string& source) -> Result<ImportedModel>
    {
        if (endsWithIgnoreCase(source, ".gltf") || endsWithIgnoreCase(source, ".glb"))
        {
            return importGltfModel(source);
        }
        if (endsWithIgnoreCase(source, ".obj"))
        {
            return importObjModel(source);
        }
        return Err(std::format("unsupported model format: '{}' (expected .gltf/.glb/.obj)", source));
    }

    auto subIdFor(std::string_view modelKey, std::string_view kind, std::string_view sourceName, u32 dupIndex) -> Uuid
    {
        constexpr u64 fnvOffset = 1469598103934665603ull;
        constexpr u64 fnvPrime = 1099511628211ull;
        u64 hash = fnvOffset;
        auto mix = [&](std::string_view part)
        {
            for (const char ch : part)
            {
                hash = hash ^ static_cast<u8>(ch);
                hash = hash * fnvPrime;
            }
            hash = hash ^ u64{ 0 };
            hash = hash * fnvPrime;  // an extra mix round between fields keeps "ab|c" != "a|bc"
        };
        mix(modelKey);
        mix(kind);
        mix(sourceName);
        for (u32 i = 0; i < 4; i = i + 1)
        {
            hash = hash ^ static_cast<u8>(dupIndex >> (i * 8));
            hash = hash * fnvPrime;
        }
        if (hash < 1024)
        {
            hash = hash + 1024;
        }
        return Uuid{ hash };
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

    namespace
    {
        // The `.smesh` byte image. An empty skin yields a v1 (unskinned) layout; a skin parallel to
        // the vertices appends the VertexSkin section and stamps the v2 version. Header offsets are
        // self-relative so the image is valid both as a file and as a `.smodel` chunk payload.
        auto encodeMeshImage(const Mesh& mesh, const std::vector<VertexSkin>& skin) -> std::vector<std::byte>
        {
            SMeshHeader header{};
            std::memcpy(header.magic, "SMSH", 4);
            header.version = MeshFormatVersion;
            if (!skin.empty())
            {
                header.version = MeshFormatVersionSkinned;
            }
            header.flags = 0;
            header.vertexStride = sizeof(Vertex);
            header.vertexCount = static_cast<u32>(mesh.vertices.size());
            header.indexCount = static_cast<u32>(mesh.indices.size());
            header.indexWidth = sizeof(u32);
            header.submeshCount = static_cast<u32>(mesh.submeshes.size());
            header.verticesOffset = sizeof(SMeshHeader);
            header.indicesOffset = header.verticesOffset + static_cast<u64>(header.vertexCount) * sizeof(Vertex);
            header.submeshesOffset = header.indicesOffset + static_cast<u64>(header.indexCount) * sizeof(u32);
            const u64 submeshesEnd = header.submeshesOffset + static_cast<u64>(header.submeshCount) * sizeof(Submesh);
            u64 total = submeshesEnd;
            if (!skin.empty())
            {
                total = total + static_cast<u64>(skin.size()) * sizeof(VertexSkin);
            }

            std::vector<std::byte> bytes(static_cast<std::size_t>(total));
            auto put = [&](u64 offset, const void* src, std::size_t count)
            {
                if (count != 0)
                {
                    std::memcpy(bytes.data() + offset, src, count);
                }
            };
            put(0, &header, sizeof(header));
            put(header.verticesOffset, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
            put(header.indicesOffset, mesh.indices.data(), mesh.indices.size() * sizeof(u32));
            put(header.submeshesOffset, mesh.submeshes.data(), mesh.submeshes.size() * sizeof(Submesh));
            if (!skin.empty())
            {
                put(submeshesEnd, skin.data(), skin.size() * sizeof(VertexSkin));
            }
            return bytes;
        }

        auto writeBytesToFile(const std::string& path, std::span<const std::byte> bytes) -> Result<void>
        {
            std::ofstream out(path, std::ios::binary);
            if (!out)
            {
                return Err(std::format("cannot open '{}' for writing", path));
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out)
            {
                return Err(std::format("write failed for '{}'", path));
            }
            return {};
        }
    }

    auto saveMeshToBuffer(const Mesh& mesh) -> std::vector<std::byte>
    {
        return encodeMeshImage(mesh, {});
    }

    auto saveMeshSkinnedToBuffer(const Mesh& mesh, const std::vector<VertexSkin>& skin)
        -> Result<std::vector<std::byte>>
    {
        if (skin.size() != mesh.vertices.size())
        {
            return Err(
                std::format("skin stream ({}) does not parallel the vertices ({})", skin.size(), mesh.vertices.size()));
        }
        return encodeMeshImage(mesh, skin);
    }

    auto loadMeshFromBytes(std::span<const std::byte> bytes) -> Result<Mesh>
    {
        if (bytes.size() < sizeof(SMeshHeader))
        {
            return Err(std::string{ "buffer is too small to be a .smesh" });
        }
        SMeshHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (std::memcmp(header.magic, "SMSH", 4) != 0)
        {
            return Err(std::string{ "not a .smesh (bad magic)" });
        }
        if (header.version != MeshFormatVersion && header.version != MeshFormatVersionSkinned)
        {
            return Err(std::format("unsupported mesh version {}", header.version));
        }
        if (header.vertexStride != sizeof(Vertex) || header.indexWidth != sizeof(u32))
        {
            return Err(std::string{ "incompatible vertex/index layout" });
        }
        // Recompute the layout from the counts and require the header offsets to match and the buffer
        // to be large enough. Validated against the span length (the chunk length, not a file size),
        // so an embedded chunk reads identically to a standalone file.
        const u64 verticesEnd =
            static_cast<u64>(sizeof(SMeshHeader)) + static_cast<u64>(header.vertexCount) * sizeof(Vertex);
        const u64 indicesEnd = verticesEnd + static_cast<u64>(header.indexCount) * sizeof(u32);
        const u64 submeshesEnd = indicesEnd + static_cast<u64>(header.submeshCount) * sizeof(Submesh);
        if (header.verticesOffset != sizeof(SMeshHeader) || header.indicesOffset != verticesEnd ||
            header.submeshesOffset != indicesEnd || static_cast<u64>(bytes.size()) < submeshesEnd)
        {
            return Err(std::string{ "inconsistent or truncated .smesh layout" });
        }

        Mesh mesh;
        mesh.vertices.resize(header.vertexCount);
        mesh.indices.resize(header.indexCount);
        mesh.submeshes.resize(header.submeshCount);
        std::memcpy(mesh.vertices.data(), bytes.data() + header.verticesOffset, header.vertexCount * sizeof(Vertex));
        std::memcpy(mesh.indices.data(), bytes.data() + header.indicesOffset, header.indexCount * sizeof(u32));
        std::memcpy(mesh.submeshes.data(), bytes.data() + header.submeshesOffset,
                    header.submeshCount * sizeof(Submesh));
        return mesh;
    }

    auto loadMesh(const std::string& path) -> Result<Mesh>
    {
        auto bytes = readBinaryFile(path);
        if (!bytes)
        {
            return Err(bytes.error());
        }
        auto mesh = loadMeshFromBytes(std::as_bytes(std::span{ *bytes }));
        if (!mesh)
        {
            return Err(std::format("'{}': {}", path, mesh.error()));
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

    auto meshCountsFromBytes(std::span<const std::byte> bytes) -> Result<MeshCounts>
    {
        if (bytes.size() < sizeof(SMeshHeader))
        {
            return Err(std::string{ "buffer is too small to be a .smesh" });
        }
        SMeshHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (std::memcmp(header.magic, "SMSH", 4) != 0)
        {
            return Err(std::string{ "not a .smesh (bad magic)" });
        }
        return MeshCounts{ header.vertexCount, header.indexCount };
    }

    auto saveMeshSkinned(const Mesh& mesh, const std::vector<VertexSkin>& skin, const std::string& path) -> Result<void>
    {
        auto bytes = saveMeshSkinnedToBuffer(mesh, skin);
        if (!bytes)
        {
            return Err(bytes.error());
        }
        return writeBytesToFile(path, *bytes);
    }

    auto loadMeshSkinFromBytes(std::span<const std::byte> bytes) -> Result<std::vector<VertexSkin>>
    {
        if (bytes.size() < sizeof(SMeshHeader))
        {
            return Err(std::string{ "buffer is too small to be a .smesh" });
        }
        SMeshHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (std::memcmp(header.magic, "SMSH", 4) != 0)
        {
            return Err(std::string{ "not a .smesh (bad magic)" });
        }
        if (header.version != MeshFormatVersionSkinned)
        {
            return std::vector<VertexSkin>{};  // v1: unskinned, empty stream
        }
        const u64 submeshesEnd = header.submeshesOffset + static_cast<u64>(header.submeshCount) * sizeof(Submesh);
        const u64 skinEnd = submeshesEnd + static_cast<u64>(header.vertexCount) * sizeof(VertexSkin);
        if (static_cast<u64>(bytes.size()) < skinEnd)
        {
            return Err(std::string{ ".smesh is missing its skin section" });
        }
        std::vector<VertexSkin> skin(header.vertexCount);
        std::memcpy(skin.data(), bytes.data() + submeshesEnd, header.vertexCount * sizeof(VertexSkin));
        return skin;
    }

    auto loadMeshSkin(const std::string& path) -> Result<std::vector<VertexSkin>>
    {
        auto bytes = readBinaryFile(path);
        if (!bytes)
        {
            return Err(bytes.error());
        }
        auto skin = loadMeshSkinFromBytes(std::as_bytes(std::span{ *bytes }));
        if (!skin)
        {
            return Err(std::format("'{}': {}", path, skin.error()));
        }
        return skin;
    }

    auto saveAnimationToBuffer(const AnimClip& clip) -> std::vector<std::byte>
    {
        std::vector<std::byte> bytes;
        auto append = [&](const void* src, std::size_t count)
        {
            const auto* first = reinterpret_cast<const std::byte*>(src);
            bytes.insert(bytes.end(), first, first + count);
        };
        SANimHeader header{};
        std::memcpy(header.magic, "SANM", 4);
        header.version = AnimFormatVersion;
        header.trackCount = static_cast<u32>(clip.tracks.size());
        header.duration = clip.duration;
        header.nameLen = static_cast<u32>(clip.name.size());
        append(&header, sizeof(header));
        append(clip.name.data(), clip.name.size());
        for (const AnimTrack& track : clip.tracks)
        {
            SANimTrackRecord record{};
            record.joint = track.joint;
            record.path = static_cast<u8>(track.path);
            record.interp = static_cast<u8>(track.interp);
            record.nameLen = static_cast<u32>(track.jointName.size());
            record.timeCount = static_cast<u32>(track.times.size());
            record.valueCount = static_cast<u32>(track.values.size());
            append(&record, sizeof(record));
            append(track.jointName.data(), track.jointName.size());
            append(track.times.data(), track.times.size() * sizeof(f32));
            append(track.values.data(), track.values.size() * sizeof(f32));
        }
        return bytes;
    }

    auto saveAnimation(const AnimClip& clip, const std::string& path) -> Result<void>
    {
        return writeBytesToFile(path, saveAnimationToBuffer(clip));
    }

    auto loadAnimationFromBytes(std::span<const std::byte> bytes) -> Result<AnimClip>
    {
        if (bytes.size() < sizeof(SANimHeader))
        {
            return Err(std::string{ "buffer is too small to be a .sanim" });
        }
        SANimHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (std::memcmp(header.magic, "SANM", 4) != 0)
        {
            return Err(std::string{ "not a .sanim (bad magic)" });
        }
        if (header.version != AnimFormatVersion)
        {
            return Err(std::format("unsupported animation version {}", header.version));
        }

        // Cursor over the byte buffer; `take` bounds-checks every field so a malformed
        // count can never drive a giant allocation (same defence as loadMesh).
        std::size_t cursor = sizeof(SANimHeader);
        bool overran = false;
        auto take = [&](std::size_t count) -> const std::byte*
        {
            if (overran || count > bytes.size() - cursor)
            {
                overran = true;
                return nullptr;
            }
            const std::byte* at = bytes.data() + cursor;
            cursor = cursor + count;
            return at;
        };

        AnimClip clip;
        clip.duration = header.duration;
        if (const std::byte* name = take(header.nameLen))
        {
            clip.name.assign(reinterpret_cast<const char*>(name), header.nameLen);
        }
        clip.tracks.reserve(header.trackCount);
        for (u32 t = 0; t < header.trackCount && !overran; t = t + 1)
        {
            const std::byte* recordBytes = take(sizeof(SANimTrackRecord));
            if (recordBytes == nullptr)
            {
                break;
            }
            SANimTrackRecord record{};
            std::memcpy(&record, recordBytes, sizeof(record));
            AnimTrack track;
            track.joint = record.joint;
            track.path = static_cast<AnimTrack::Path>(record.path);
            track.interp = static_cast<AnimTrack::Interp>(record.interp);
            if (const std::byte* name = take(record.nameLen))
            {
                track.jointName.assign(reinterpret_cast<const char*>(name), record.nameLen);
            }
            if (const std::byte* times = take(static_cast<std::size_t>(record.timeCount) * sizeof(f32)))
            {
                track.times.resize(record.timeCount);
                std::memcpy(track.times.data(), times, static_cast<std::size_t>(record.timeCount) * sizeof(f32));
            }
            if (const std::byte* values = take(static_cast<std::size_t>(record.valueCount) * sizeof(f32)))
            {
                track.values.resize(record.valueCount);
                std::memcpy(track.values.data(), values, static_cast<std::size_t>(record.valueCount) * sizeof(f32));
            }
            clip.tracks.push_back(std::move(track));
        }
        if (overran)
        {
            return Err(std::string{ ".sanim is truncated or malformed" });
        }
        return clip;
    }

    auto loadAnimation(const std::string& path) -> Result<AnimClip>
    {
        auto raw = readBinaryFile(path);
        if (!raw)
        {
            return Err(raw.error());
        }
        auto clip = loadAnimationFromBytes(std::as_bytes(std::span{ *raw }));
        if (!clip)
        {
            return Err(std::format("'{}': {}", path, clip.error()));
        }
        return clip;
    }

    namespace
    {
        auto align16(u64 value) -> u64
        {
            return (value + 15) & ~u64{ 15 };
        }
    }

    auto writeContainer(const std::string& path, std::span<const ContainerChunk> chunks) -> Result<void>
    {
        // META is front-loaded (placed first after the TOC) so a prefix read reaches it
        // without scanning payloads; everything else keeps its caller-given order.
        std::vector<const ContainerChunk*> ordered;
        ordered.reserve(chunks.size());
        for (const ContainerChunk& chunk : chunks)
        {
            if (chunk.kind == ChunkKind::Meta)
            {
                ordered.push_back(&chunk);
            }
        }
        for (const ContainerChunk& chunk : chunks)
        {
            if (chunk.kind != ChunkKind::Meta)
            {
                ordered.push_back(&chunk);
            }
        }

        const u64 tocOffset = sizeof(SModelHeader);
        const u64 tocBytes = static_cast<u64>(ordered.size()) * sizeof(TocEntry);

        std::vector<TocEntry> toc(ordered.size());
        u64 cursor = align16(tocOffset + tocBytes);
        u64 metaOffset = 0;
        u64 metaLength = 0;
        for (std::size_t i = 0; i < ordered.size(); i = i + 1)
        {
            const ContainerChunk& chunk = *ordered[i];
            cursor = align16(cursor);
            TocEntry& entry = toc[i];
            entry.fourcc = static_cast<u32>(chunk.kind);
            entry.flags = chunk.flags;
            entry.subId = chunk.subId;
            entry.offset = cursor;
            entry.length = static_cast<u64>(chunk.bytes.size());
            if (chunk.kind == ChunkKind::Meta)
            {
                metaOffset = entry.offset;
                metaLength = entry.length;
            }
            cursor = cursor + entry.length;
        }
        const u64 totalLength = cursor;

        std::vector<std::byte> buffer(static_cast<std::size_t>(totalLength), std::byte{ 0 });
        SModelHeader header{};
        std::memcpy(header.magic, "SMDL", 4);
        header.containerVersion = ContainerFormatVersion;
        header.schemaVersion = MetadataSchemaVersion;
        header.flags = 0;
        header.tocCount = static_cast<u32>(ordered.size());
        header.tocOffset = tocOffset;
        header.metaOffset = metaOffset;
        header.metaLength = metaLength;
        header.totalLength = totalLength;
        std::memcpy(buffer.data(), &header, sizeof(header));
        if (!toc.empty())
        {
            std::memcpy(buffer.data() + tocOffset, toc.data(), static_cast<std::size_t>(tocBytes));
        }
        for (std::size_t i = 0; i < ordered.size(); i = i + 1)
        {
            const ContainerChunk& chunk = *ordered[i];
            if (!chunk.bytes.empty())
            {
                std::memcpy(buffer.data() + toc[i].offset, chunk.bytes.data(), chunk.bytes.size());
            }
        }

        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return Err(std::format("cannot open '{}' for writing", path));
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!out)
        {
            return Err(std::format("write failed for '{}'", path));
        }
        return {};
    }

    auto readContainerHeader(const std::string& path) -> Result<SModelHeader>
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        const std::streamsize fileSize = in.tellg();
        in.seekg(0);
        if (fileSize < static_cast<std::streamsize>(sizeof(SModelHeader)))
        {
            return Err(std::format("'{}' is too small to be a .smodel", path));
        }
        SModelHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in)
        {
            return Err(std::format("read failed for '{}'", path));
        }
        if (std::memcmp(header.magic, "SMDL", 4) != 0)
        {
            return Err(std::format("'{}' is not a .smodel (bad magic)", path));
        }
        if (header.containerVersion != ContainerFormatVersion)
        {
            return Err(std::format("'{}' has unsupported container version {}", path, header.containerVersion));
        }
        if (header.totalLength != static_cast<u64>(fileSize))
        {
            return Err(std::format("'{}' totalLength {} disagrees with file size {}", path, header.totalLength,
                                   static_cast<u64>(fileSize)));
        }
        return header;
    }

    auto readContainer(const std::string& path) -> Result<ContainerReader>
    {
        auto header = readContainerHeader(path);
        if (!header)
        {
            return Err(header.error());
        }
        const u64 tocBytes = static_cast<u64>(header->tocCount) * sizeof(TocEntry);
        if (header->tocOffset < sizeof(SModelHeader) || header->tocOffset + tocBytes > header->totalLength)
        {
            return Err(std::format("'{}' has an out-of-bounds chunk table", path));
        }

        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        std::vector<TocEntry> toc(header->tocCount);
        if (header->tocCount != 0)
        {
            in.seekg(static_cast<std::streamoff>(header->tocOffset));
            in.read(reinterpret_cast<char*>(toc.data()), static_cast<std::streamsize>(tocBytes));
            if (!in)
            {
                return Err(std::format("read failed for the chunk table of '{}'", path));
            }
        }

        // Bounds + overlap validation: every payload sits past the header and inside the file,
        // and no two payloads cover the same bytes (sort a copy by offset and check gaps).
        for (const TocEntry& entry : toc)
        {
            if (entry.length == 0)
            {
                continue;
            }
            if (entry.offset < sizeof(SModelHeader) || entry.offset + entry.length > header->totalLength)
            {
                return Err(std::format("'{}' has a chunk that spills outside the file", path));
            }
        }
        std::vector<std::pair<u64, u64>> ranges;  // (offset, length), payload chunks only
        ranges.reserve(toc.size());
        for (const TocEntry& entry : toc)
        {
            if (entry.length != 0)
            {
                ranges.emplace_back(entry.offset, entry.length);
            }
        }
        std::ranges::sort(ranges);
        for (std::size_t i = 1; i < ranges.size(); i = i + 1)
        {
            if (ranges[i].first < ranges[i - 1].first + ranges[i - 1].second)
            {
                return Err(std::format("'{}' has overlapping chunk payloads", path));
            }
        }

        ContainerReader reader;
        reader.path = path;
        reader.header = *header;
        reader.toc = std::move(toc);
        return reader;
    }

    auto ContainerReader::readChunk(const TocEntry& entry) const -> Result<std::vector<std::byte>>
    {
        if (entry.offset + entry.length > header.totalLength)
        {
            return Err(std::format("chunk in '{}' spills outside the file", path));
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(entry.length));
        if (entry.length != 0)
        {
            in.seekg(static_cast<std::streamoff>(entry.offset));
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(entry.length));
            if (!in)
            {
                return Err(std::format("read failed for a chunk of '{}'", path));
            }
        }
        return bytes;
    }

    auto ContainerReader::find(ChunkKind kind, u64 subId) const -> const TocEntry*
    {
        for (const TocEntry& entry : toc)
        {
            if (entry.fourcc == static_cast<u32>(kind) && entry.subId == subId)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    namespace
    {
        void runTranslateDeterminismSelfTest(const std::string& modelsDir)
        {
            auto first = translateModel(modelsDir + "/cube.gltf");
            auto second = translateModel(modelsDir + "/cube.gltf");
            if (!first || !second)
            {
                logError("translate determinism self-test: cube.gltf translate failed");
                return;
            }
            bool sameGraph = first->mesh.vertices.size() == second->mesh.vertices.size() &&
                             first->mesh.indices.size() == second->mesh.indices.size() &&
                             first->mesh.submeshes.size() == second->mesh.submeshes.size() &&
                             first->materials.size() == second->materials.size() &&
                             first->nodes.size() == second->nodes.size() && first->hasSkin == second->hasSkin;
            if (sameGraph && !first->mesh.vertices.empty())
            {
                sameGraph = first->mesh.vertices.front().position == second->mesh.vertices.front().position &&
                            first->mesh.vertices.back().position == second->mesh.vertices.back().position;
            }
            for (std::size_t i = 0; sameGraph && i < first->nodes.size(); i = i + 1)
            {
                sameGraph = first->nodes[i].name == second->nodes[i].name;
            }

            const Uuid stone = subIdFor("town", "material", "stone", 0);
            const Uuid stoneAgain = subIdFor("town", "material", "stone", 0);
            const Uuid stoneDup = subIdFor("town", "material", "stone", 1);
            const Uuid stoneMesh = subIdFor("town", "mesh", "stone", 0);
            const Uuid marble = subIdFor("town", "material", "marble", 0);
            const bool stableIds = stone.value == stoneAgain.value && stone.value != stoneDup.value &&
                                   stone.value != stoneMesh.value && stone.value != marble.value && stone.value >= 1024;

            if (sameGraph && stableIds)
            {
                logInfo("translate determinism + stable sub-ids OK");
            }
            else
            {
                logError(
                    std::format("translate determinism self-test FAILED (graph={}, ids={})", sameGraph, stableIds));
            }
        }

        void runContainerSelfTest()
        {
            std::array<std::byte, 12> metaBytes{};
            for (std::size_t i = 0; i < metaBytes.size(); i = i + 1)
            {
                metaBytes[i] = static_cast<std::byte>(0xA0 + i);
            }
            std::vector<std::byte> meshBytes(40);
            for (std::size_t i = 0; i < meshBytes.size(); i = i + 1)
            {
                meshBytes[i] = static_cast<std::byte>(i * 3 + 1);
            }
            std::vector<std::byte> texBytes(33);
            for (std::size_t i = 0; i < texBytes.size(); i = i + 1)
            {
                texBytes[i] = static_cast<std::byte>(i * 7 + 2);
            }

            // Mesh first in caller order so the META front-loading is actually exercised.
            const std::array<ContainerChunk, 3> chunks{
                ContainerChunk{ .kind = ChunkKind::Mesh, .subId = 111, .flags = 0, .bytes = meshBytes },
                ContainerChunk{ .kind = ChunkKind::Meta, .subId = 0, .flags = 0, .bytes = metaBytes },
                ContainerChunk{ .kind = ChunkKind::Texture, .subId = 222, .flags = 1, .bytes = texBytes },
            };

            const std::string containerPath = "/tmp/saffron_container.smodel";
            if (auto wrote = writeContainer(containerPath, chunks); !wrote)
            {
                logError(std::format("container self-test: write failed: {}", wrote.error()));
                return;
            }

            auto reader = readContainer(containerPath);
            if (!reader)
            {
                logError(std::format("container self-test: read failed: {}", reader.error()));
                return;
            }

            auto sameBytes = [](const std::vector<std::byte>& got, std::span<const std::byte> want)
            { return got.size() == want.size() && std::memcmp(got.data(), want.data(), want.size()) == 0; };

            bool ok = reader->toc.size() == 3 && reader->header.metaLength == metaBytes.size() &&
                      reader->header.metaOffset != 0;
            const TocEntry* metaEntry = reader->find(ChunkKind::Meta, 0);
            const TocEntry* meshEntry = reader->find(ChunkKind::Mesh, 111);
            const TocEntry* texEntry = reader->find(ChunkKind::Texture, 222);
            ok = ok && metaEntry != nullptr && meshEntry != nullptr && texEntry != nullptr;
            if (metaEntry != nullptr && meshEntry != nullptr && texEntry != nullptr)
            {
                ok = ok && metaEntry->offset < meshEntry->offset && metaEntry->offset < texEntry->offset;
            }
            if (meshEntry != nullptr)
            {
                ok = ok && (meshEntry->offset % 16 == 0);
                auto got = reader->readChunk(*meshEntry);
                ok = ok && got && sameBytes(*got, meshBytes);
            }
            if (texEntry != nullptr)
            {
                ok = ok && (texEntry->offset % 16 == 0) && texEntry->flags == 1;
                auto got = reader->readChunk(*texEntry);
                ok = ok && got && sameBytes(*got, texBytes);
            }
            if (ok)
            {
                logInfo(".smodel container round-trip OK");
            }
            else
            {
                logError(".smodel container round-trip MISMATCH");
            }

            // Rejection cases: a corrupted magic and a lying totalLength must both error, not crash.
            auto raw = readBinaryFile(containerPath);
            if (!raw)
            {
                return;
            }
            std::vector<u8> badMagic = *raw;
            badMagic[0] = static_cast<u8>('X');
            const std::string badMagicPath = "/tmp/saffron_container_badmagic.smodel";
            if (std::ofstream out(badMagicPath, std::ios::binary); out)
            {
                out.write(reinterpret_cast<const char*>(badMagic.data()),
                          static_cast<std::streamsize>(badMagic.size()));
            }

            std::vector<u8> badLength = *raw;
            const u64 wrongLength = static_cast<u64>(badLength.size()) + 4096;
            std::memcpy(badLength.data() + offsetof(SModelHeader, totalLength), &wrongLength, sizeof(wrongLength));
            const std::string badLengthPath = "/tmp/saffron_container_badlen.smodel";
            if (std::ofstream out(badLengthPath, std::ios::binary); out)
            {
                out.write(reinterpret_cast<const char*>(badLength.data()),
                          static_cast<std::streamsize>(badLength.size()));
            }

            const bool magicRejected = !readContainerHeader(badMagicPath).has_value();
            const bool lengthRejected = !readContainerHeader(badLengthPath).has_value();
            if (magicRejected && lengthRejected)
            {
                logInfo(".smodel rejects corrupted magic + totalLength");
            }
            else
            {
                logError(
                    std::format(".smodel rejection check FAILED (magic={}, length={})", magicRejected, lengthRejected));
            }
        }
    }

    void runGeometrySelfTest(const std::string& modelsDir)
    {
        auto obj = translateModel(modelsDir + "/cube.obj");
        if (!obj)
        {
            logError(std::format("geometry self-test: obj import failed: {}", obj.error()));
            return;
        }
        logInfo(std::format("geometry self-test: cube.obj -> {} verts, {} indices, {} submeshes",
                            obj->mesh.vertices.size(), obj->mesh.indices.size(), obj->mesh.submeshes.size()));

        auto gltf = translateModel(modelsDir + "/cube.gltf");
        if (!gltf)
        {
            logError(std::format("geometry self-test: gltf import failed: {}", gltf.error()));
            return;
        }
        const Mesh& gltfMesh = gltf->mesh;
        logInfo(std::format("geometry self-test: cube.gltf -> {} verts, {} indices, {} submeshes",
                            gltfMesh.vertices.size(), gltfMesh.indices.size(), gltfMesh.submeshes.size()));

        // Drive the live bake/load path: encode the .smesh image as the asset importer does
        // (saveMeshToBuffer), then read it back through loadMeshFromBytes.
        const std::vector<std::byte> baked = saveMeshToBuffer(gltfMesh);
        auto loaded = loadMeshFromBytes(std::span<const std::byte>{ baked });
        if (!loaded)
        {
            logError(std::format("geometry self-test: load failed: {}", loaded.error()));
            return;
        }

        const bool roundTrips = loaded->vertices.size() == gltfMesh.vertices.size() &&
                                loaded->indices.size() == gltfMesh.indices.size() &&
                                loaded->submeshes.size() == gltfMesh.submeshes.size() &&
                                loaded->vertices[0].position == gltfMesh.vertices[0].position;
        if (roundTrips)
        {
            logInfo(".smesh round-trip OK");
        }
        else
        {
            logError(".smesh round-trip MISMATCH");
        }

        // Skeletal clip import (Phase 2): the rigged+animated fixture yields a skin plus at
        // least one decoded clip, and that clip survives a .sanim round-trip.
        if (auto rigged = translateModel(modelsDir + "/animated-strip.gltf"); !rigged)
        {
            logError(std::format("geometry self-test: animated-strip import failed: {}", rigged.error()));
        }
        else if (!rigged->hasSkin || rigged->animations.empty())
        {
            logError(std::format("geometry self-test: animated-strip missing skin/clips (skin={}, clips={})",
                                 rigged->hasSkin, rigged->animations.size()));
        }
        else
        {
            const AnimClip& clip = rigged->animations.front();
            logInfo(std::format("geometry self-test: animated-strip -> clip '{}', {} track(s), {:.2f}s", clip.name,
                                clip.tracks.size(), clip.duration));
            const std::string animPath = "/tmp/saffron_strip.sanim";
            if (auto savedAnim = saveAnimation(clip, animPath); !savedAnim)
            {
                logError(std::format("geometry self-test: .sanim save failed: {}", savedAnim.error()));
            }
            else if (auto loadedAnim = loadAnimation(animPath); !loadedAnim)
            {
                logError(std::format("geometry self-test: .sanim load failed: {}", loadedAnim.error()));
            }
            else if (loadedAnim->name == clip.name && loadedAnim->tracks.size() == clip.tracks.size())
            {
                logInfo(".sanim round-trip OK");
            }
            else
            {
                logError(".sanim round-trip MISMATCH");
            }
        }

        runTranslateDeterminismSelfTest(modelsDir);
        runContainerSelfTest();
    }
}

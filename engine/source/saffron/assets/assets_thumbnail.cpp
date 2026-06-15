module;

// Bridges Scene + Geometry + Rendering, so (like those) it uses classic includes.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

module Saffron.Assets;

import Saffron.Core;
import Saffron.Json;
import Saffron.Geometry;
import Saffron.Rendering;
import Saffron.Scene;

namespace sa
{
    auto thumbnailCacheDir(const AssetServer& assets) -> std::filesystem::path
    {
        if (assets.root.empty())
        {
            return {};
        }
        return std::filesystem::path(assets.root).parent_path() / "cache" / "thumbnails";
    }

    // Folds version + size + mtime (FNV-1a) into a compact hex token — the <stamp> filename field.
    auto foldThumbnailStamp(u64 version, u64 fileSize, u64 mtimeTicks) -> std::string
    {
        u64 h = 1469598103934665603ull;
        const auto mix = [&](u64 v)
        {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(version);
        mix(fileSize);
        mix(mtimeTicks);
        return std::format("{:016x}", h);
    }

    // A stamp of the asset's source file (its catalog `path` under assets/): size + mtime folded with
    // the cache version. Empty when the file is missing/unstattable — the entry is then never cached.
    auto thumbnailSourceStamp(const AssetServer& assets, const AssetEntry& entry) -> std::string
    {
        if (assets.root.empty() || entry.path.empty())
        {
            return {};
        }
        std::error_code ec;
        const std::filesystem::path src = std::filesystem::path(assets.root) / entry.path;
        const auto size = std::filesystem::file_size(src, ec);
        if (ec)
        {
            return {};
        }
        const auto mtime = std::filesystem::last_write_time(src, ec);
        if (ec)
        {
            return {};
        }
        return foldThumbnailStamp(ThumbnailCacheVersion, static_cast<u64>(size),
                                  static_cast<u64>(mtime.time_since_epoch().count()));
    }

    auto thumbnailCachePath(const AssetServer& assets, Uuid id, u32 size, const std::string& stamp)
        -> std::filesystem::path
    {
        return thumbnailCacheDir(assets) / std::format("{}-{}-{}.png", id.value, size, stamp);
    }

    // A cached thumbnail's bytes + the dimensions read from its PNG header (so a hit reports truthful
    // width/height without a decode). nullopt if the file is absent or not a readable PNG.
    struct CachedThumbnail
    {
        std::vector<u8> bytes;
        u32 width = 0;
        u32 height = 0;
    };

    auto readThumbnailCache(const std::filesystem::path& path) -> std::optional<CachedThumbnail>
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return std::nullopt;
        }
        const std::streamsize size = in.tellg();
        if (size < 24)  // 8-byte signature + IHDR length/type + the width/height fields
        {
            return std::nullopt;
        }
        in.seekg(0);
        CachedThumbnail hit;
        hit.bytes.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(hit.bytes.data()), size);
        if (!in)
        {
            return std::nullopt;
        }
        static constexpr std::array<u8, 8> pngSig{ 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
        if (!std::equal(pngSig.begin(), pngSig.end(), hit.bytes.begin()))
        {
            return std::nullopt;
        }
        const auto be32 = [&](std::size_t at) -> u32
        {
            return (static_cast<u32>(hit.bytes[at]) << 24) | (static_cast<u32>(hit.bytes[at + 1]) << 16) |
                   (static_cast<u32>(hit.bytes[at + 2]) << 8) | static_cast<u32>(hit.bytes[at + 3]);
        };
        hit.width = be32(16);   // IHDR width
        hit.height = be32(20);  // IHDR height
        return hit;
    }

    auto writeThumbnailCache(const std::filesystem::path& path, const std::vector<u8>& bytes) -> Result<void>
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return Err(std::format("cannot open thumbnail cache '{}'", path.string()));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            return Err(std::format("write failed for thumbnail cache '{}'", path.string()));
        }
        return {};
    }

    // 1 when the flag is set, else 0 — for folding a bool into a numeric hash.
    auto boolFlag(bool value) -> u32
    {
        if (value)
        {
            return 1u;
        }
        return 0u;
    }

    // A material thumbnail keys on its *resolved* state, not a file stamp: editing a parent material
    // reflows every instance without touching the child .smat, so the key must be a content hash of
    // the resolved params + texture uuids. Folded with the cache version like the source stamp.
    auto thumbnailMaterialStamp(const MaterialAsset& m) -> std::string
    {
        u64 h = 1469598103934665603ull;
        const auto mix = [&](u64 v)
        {
            h ^= v;
            h *= 1099511628211ull;
        };
        const auto mixF = [&](f32 f) { mix(std::bit_cast<u32>(f)); };
        mix(ThumbnailCacheVersion);
        mixF(m.baseColor.r);
        mixF(m.baseColor.g);
        mixF(m.baseColor.b);
        mixF(m.baseColor.a);
        mixF(m.metallic);
        mixF(m.roughness);
        mixF(m.emissive.r);
        mixF(m.emissive.g);
        mixF(m.emissive.b);
        mixF(m.emissiveStrength);
        mixF(m.normalStrength);
        mixF(m.alphaCutoff);
        mixF(m.heightScale);
        mixF(m.uvTiling.x);
        mixF(m.uvTiling.y);
        mixF(m.uvOffset.x);
        mixF(m.uvOffset.y);
        mix(m.albedoTexture.value);
        mix(m.ormTexture.value);
        mix(m.normalTexture.value);
        mix(m.emissiveTexture.value);
        mix(m.heightTexture.value);
        mix(boolFlag(m.unlit));
        mix(boolFlag(m.doubleSided));
        for (char c : m.shader)
        {
            mix(static_cast<u8>(c));
        }
        for (char c : m.blend)
        {
            mix(static_cast<u8>(c));
        }
        return std::format("{:016x}", h);
    }

    // The leading uuid of a cache filename (<uuid>-<size>-<stamp>.png); 0 if it doesn't parse.
    auto thumbnailCacheFileUuid(const std::string& filename) -> u64
    {
        const std::size_t dash = filename.find('-');
        if (dash == std::string::npos)
        {
            return 0;
        }
        return std::strtoull(filename.substr(0, dash).c_str(), nullptr, 10);
    }

    // Removes every cached thumbnail for one asset uuid (all sizes/stamps) — on delete + reimport.
    void removeThumbnailCacheForAsset(const AssetServer& assets, Uuid id)
    {
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (dir.empty() || !std::filesystem::exists(dir, ec))
        {
            return;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (thumbnailCacheFileUuid(entry.path().filename().string()) == id.value)
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    // Deletes cache files whose uuid is no longer in the catalog (re-import mints new uuids, orphaning
    // the old PNGs). Run on project load to keep the dir bounded without a background task.
    void sweepThumbnailCacheOrphans(const AssetServer& assets)
    {
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (dir.empty() || !std::filesystem::exists(dir, ec))
        {
            return;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
        {
            const u64 uuid = thumbnailCacheFileUuid(entry.path().filename().string());
            if (uuid == 0 || !assets.catalog.byId.contains(uuid))
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    auto thumbnailCacheStats(const AssetServer& assets) -> ThumbnailCacheStats
    {
        ThumbnailCacheStats stats;
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (dir.empty() || !std::filesystem::exists(dir, ec))
        {
            return stats;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec))
            {
                continue;
            }
            stats.entries += 1;
            stats.bytes += static_cast<u64>(entry.file_size(ec));
        }
        return stats;
    }

    // Empties the project's cache dir; returns what was removed (count + bytes).
    auto clearThumbnailCacheDir(const AssetServer& assets) -> ThumbnailCacheStats
    {
        const ThumbnailCacheStats removed = thumbnailCacheStats(assets);
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (!dir.empty() && std::filesystem::exists(dir, ec))
        {
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
        return removed;
    }

    // One texture the worker must decode + upload, resolved from the catalog at enqueue.
    struct ThumbnailTextureSource
    {
        Uuid id;
        std::string path;       // absolute source file under assets/ (empty when bytes is set)
        bool hdr = false;       // .hdr float source -> uploadTextureFloat + tonemapped preview
        bool srgb = true;       // LDR colorspace: albedo/emissive sRGB, data maps linear
        std::vector<u8> bytes;  // embedded chunk image bytes (decoded from memory when non-empty)
    };

    struct ThumbnailJob
    {
        Uuid id;
        u32 size = 0;
        AssetType type = AssetType::Texture;
        std::string cachePath;                                 // <projectRoot>/cache/thumbnails/<…>.png
        ThumbnailTextureSource texture;                        // type == Texture
        std::string meshPath;                                  // type == Mesh (standalone .smesh, absolute)
        std::vector<std::byte> meshBytes;                      // type == Mesh/Model, embedded: the .smesh chunk image
        MaterialAsset material;                                // type == Material (parent-resolved)
        std::vector<ThumbnailTextureSource> materialTextures;  // the material's (or model's) referenced textures
        std::vector<MaterialAsset> modelMaterials;             // type == Model: one per material slot, in slot order
    };

    struct ThumbnailWorker
    {
        std::thread thread;
        std::mutex mutex;  // guards queue + inFlight + failed + the handback vectors
        std::condition_variable cv;
        std::deque<ThumbnailJob> queue;
        std::unordered_set<std::string> inFlight;  // cachePaths queued/running (dedup retries)
        std::unordered_set<std::string> failed;    // cachePaths that failed (settle to no-thumbnail)
        std::vector<std::pair<Uuid, Ref<GpuTexture>>> textureHandback;  // -> main inserts into the cache
        std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshHandback;
        bool stop = false;
    };

    // Decode + upload one texture (worker or sync path); records (id, Ref) for cache handback.
    auto thumbnailUploadTexture(Renderer& renderer, const ThumbnailTextureSource& src,
                                std::vector<std::pair<Uuid, Ref<GpuTexture>>>& handback) -> Ref<GpuTexture>
    {
        if (src.hdr)
        {
            auto decoded = decodeImageFromMemoryHdr(src.bytes);
            if (src.bytes.empty())
            {
                decoded = decodeImageHdr(src.path);
            }
            if (!decoded)
            {
                logWarn(decoded.error());
                return nullptr;
            }
            auto tex = uploadTextureFloat(renderer, decoded->rgba.data(), decoded->width, decoded->height);
            if (!tex)
            {
                logWarn(tex.error());
                return nullptr;
            }
            handback.emplace_back(src.id, *tex);
            return *tex;
        }
        auto decoded = decodeImageFromMemory(src.bytes);
        if (src.bytes.empty())
        {
            decoded = decodeImage(src.path);
        }
        if (!decoded)
        {
            logWarn(decoded.error());
            return nullptr;
        }
        auto tex = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, src.srgb);
        if (!tex)
        {
            logWarn(tex.error());
            return nullptr;
        }
        handback.emplace_back(src.id, *tex);
        return *tex;
    }

    // Generates the PNG for a resolved job (no cache write, no catalog/map access). Uploaded GPU
    // resources are appended to texOut/meshOut for the caller to cache. Runs on the worker thread
    // (worker command pool, queue/bindless mutexes) or, with no worker, inline on the main thread.
    auto generateThumbnail(Renderer& renderer, const ThumbnailJob& job,
                           std::vector<std::pair<Uuid, Ref<GpuTexture>>>& texOut,
                           std::vector<std::pair<Uuid, Ref<GpuMesh>>>& meshOut) -> Result<ThumbnailPng>
    {
        if (job.type == AssetType::Texture)
        {
            auto tex = thumbnailUploadTexture(renderer, job.texture, texOut);
            if (!tex)
            {
                return Err(std::string{ "texture failed to load" });
            }
            PngTransfer transfer = PngTransfer::Clamp;
            if (job.texture.hdr)
            {
                transfer = PngTransfer::Tonemap;
            }
            return encodeTextureThumbnailPng(renderer, tex, job.size, transfer);
        }
        if (job.type == AssetType::Mesh)
        {
            // Embedded sub-assets ship the .smesh chunk image in meshBytes (resolved on the main thread,
            // since the worker has no AssetServer); standalone meshes load from their file path.
            auto mesh = loadMeshFromBytes(std::span<const std::byte>{ job.meshBytes });
            if (job.meshBytes.empty())
            {
                mesh = loadMesh(job.meshPath);
            }
            if (!mesh)
            {
                return Err(mesh.error());
            }
            auto meshRef = uploadMesh(renderer, *mesh);
            if (!meshRef)
            {
                return Err(meshRef.error());
            }
            meshOut.emplace_back(job.id, *meshRef);
            return encodeAssetThumbnailPng(renderer, *meshRef, job.size);
        }
        if (job.type == AssetType::Material)
        {
            std::unordered_map<u64, Ref<GpuTexture>> local;
            for (const ThumbnailTextureSource& src : job.materialTextures)
            {
                if (auto tex = thumbnailUploadTexture(renderer, src, texOut))
                {
                    local[src.id.value] = tex;
                }
            }
            const SubmeshMaterial sm = buildSubmeshMaterial(job.material,
                                                            [&](Uuid tid) -> Ref<GpuTexture>
                                                            {
                                                                auto it = local.find(tid.value);
                                                                if (it != local.end())
                                                                {
                                                                    return it->second;
                                                                }
                                                                return Ref<GpuTexture>{};
                                                            });
            auto tex = renderMaterialPreview(renderer, sm, job.size);
            if (!tex)
            {
                return Err(tex.error());
            }
            return encodeTextureThumbnailPng(renderer, *tex, job.size);
        }
        if (job.type == AssetType::Model)
        {
            std::unordered_map<u64, Ref<GpuTexture>> local;
            for (const ThumbnailTextureSource& src : job.materialTextures)
            {
                if (auto tex = thumbnailUploadTexture(renderer, src, texOut))
                {
                    local[src.id.value] = tex;
                }
            }
            const auto resolveTex = [&](Uuid tid) -> Ref<GpuTexture>
            {
                auto it = local.find(tid.value);
                if (it != local.end())
                {
                    return it->second;
                }
                return Ref<GpuTexture>{};
            };
            std::vector<SubmeshMaterial> submeshMaterials;
            submeshMaterials.reserve(job.modelMaterials.size());
            for (const MaterialAsset& mat : job.modelMaterials)
            {
                submeshMaterials.push_back(buildSubmeshMaterial(mat, resolveTex));
            }
            auto mesh = loadMeshFromBytes(std::span<const std::byte>{ job.meshBytes });
            if (!mesh)
            {
                return Err(mesh.error());
            }
            auto meshRef = uploadMesh(renderer, *mesh);
            if (!meshRef)
            {
                return Err(meshRef.error());
            }
            return encodeModelThumbnailPng(renderer, *meshRef, submeshMaterials, job.size);
        }
        return Err(std::string{ "asset has no thumbnail" });
    }

    // Insert handed-back GPU resources into the caches (skipping uuids already cached) — main thread.
    void insertThumbnailHandback(AssetServer& assets, std::vector<std::pair<Uuid, Ref<GpuTexture>>>& textures,
                                 std::vector<std::pair<Uuid, Ref<GpuMesh>>>& meshes)
    {
        for (auto& [id, tex] : textures)
        {
            if (tex && !assets.textureRefByUuid.contains(id.value))
            {
                assets.textureRefByUuid[id.value] = tex;
            }
        }
        for (auto& [id, mesh] : meshes)
        {
            if (mesh && !assets.meshRefByUuid.contains(id.value))
            {
                assets.meshRefByUuid[id.value] = mesh;
            }
        }
    }

    void thumbnailWorkerLoop(ThumbnailWorker* worker, Renderer* renderer)
    {
        bindThumbnailWorkerThread(*renderer);  // this thread allocates from the dedicated worker pool
        for (;;)
        {
            ThumbnailJob job;
            {
                std::unique_lock<std::mutex> lock(worker->mutex);
                worker->cv.wait(lock, [&] { return worker->stop || !worker->queue.empty(); });
                if (worker->stop)
                {
                    return;  // teardown / project switch: abandon any queued jobs
                }
                job = std::move(worker->queue.front());
                worker->queue.pop_front();
            }

            std::vector<std::pair<Uuid, Ref<GpuTexture>>> texOut;
            std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshOut;
            auto png = generateThumbnail(*renderer, job, texOut, meshOut);

            const std::lock_guard<std::mutex> lock(worker->mutex);
            worker->inFlight.erase(job.cachePath);
            if (png)
            {
                if (auto written = writeThumbnailCache(job.cachePath, png->bytes); !written)
                {
                    logWarn(written.error());
                }
                for (auto& h : texOut)
                {
                    worker->textureHandback.push_back(std::move(h));
                }
                for (auto& h : meshOut)
                {
                    worker->meshHandback.push_back(std::move(h));
                }
            }
            else
            {
                logWarn(std::format("thumbnail {}: {}", job.id.value, png.error()));
                worker->failed.insert(job.cachePath);  // a missing thumbnail settles to the type icon
            }
        }
    }

    void startThumbnailWorker(AssetServer& assets, Renderer& renderer)
    {
        if (assets.thumbnailWorker)
        {
            return;
        }
        // Build the shared lazy pipelines/sphere on the main thread first; the worker must never be
        // the one to initialize them (that would race a concurrent main-thread read).
        if (auto warmed = prewarmThumbnailResources(renderer); !warmed)
        {
            logWarn(std::format("thumbnail worker not started: {}", warmed.error()));
            return;
        }
        auto worker = std::make_shared<ThumbnailWorker>();
        assets.thumbnailWorker = worker;
        worker->thread = std::thread(thumbnailWorkerLoop, worker.get(), &renderer);
    }

    // Drains the queue and joins the worker, then drops it. Called before waitGpuIdle/renderer
    // teardown: the worker's last submit has completed (its fences), and its un-handed-back textures
    // are referenced by no frame, so dropping them here frees their GPU resources safely.
    void stopThumbnailWorker(AssetServer& assets)
    {
        if (!assets.thumbnailWorker)
        {
            return;
        }
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        {
            const std::lock_guard<std::mutex> lock(worker.mutex);
            worker.stop = true;
        }
        worker.cv.notify_all();
        if (worker.thread.joinable())
        {
            worker.thread.join();
        }
        assets.thumbnailWorker.reset();
    }

    // Abandon queued jobs + dedup/failed state + un-drained handbacks (a project switch; the GPU is
    // idle at the clearAssetCaches call site, so dropping the handback Refs frees them safely). An
    // already-running job finishes harmlessly and its single handback is dropped on the next switch.
    void clearThumbnailQueue(AssetServer& assets)
    {
        if (!assets.thumbnailWorker)
        {
            return;
        }
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        const std::lock_guard<std::mutex> lock(worker.mutex);
        worker.queue.clear();
        worker.inFlight.clear();
        worker.failed.clear();
        worker.textureHandback.clear();
        worker.meshHandback.clear();
    }

    // Main thread: move the worker's finished uploads into the GPU caches. Call once per frame.
    void drainThumbnailCompletions(AssetServer& assets)
    {
        if (!assets.thumbnailWorker)
        {
            return;
        }
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        std::vector<std::pair<Uuid, Ref<GpuTexture>>> textures;
        std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshes;
        {
            const std::lock_guard<std::mutex> lock(worker.mutex);
            textures.swap(worker.textureHandback);
            meshes.swap(worker.meshHandback);
        }
        insertThumbnailHandback(assets, textures, meshes);
    }

    // Resolves {asset, size} to a thumbnail: a disk-cache hit returns the PNG; a miss generates it
    // (synchronously when there is no worker, else enqueued with a `pending` reply for the editor to
    // retry). Materials key on resolved state, mesh/texture on the source-file stat (phase 3/4).
    auto requestThumbnail(AssetServer& assets, Renderer& renderer, Uuid id, u32 size) -> Result<ThumbnailReply>
    {
        const AssetEntry* match = findAsset(assets.catalog, id);
        if (match == nullptr)
        {
            return Err(std::format("no asset {}", id.value));
        }

        ThumbnailJob job;
        job.id = id;
        job.size = size;
        job.type = match->type;
        std::string stamp;
        if (match->type == AssetType::Material)
        {
            auto loaded = loadMaterialAsset(assets, id);
            if (!loaded)
            {
                return Err(loaded.error());
            }
            job.material = *loaded;
            stamp = thumbnailMaterialStamp(*loaded);
            for (Uuid tid : { loaded->albedoTexture, loaded->ormTexture, loaded->normalTexture, loaded->emissiveTexture,
                              loaded->heightTexture })
            {
                if (tid.value == 0)
                {
                    continue;
                }
                const AssetEntry* te = findAsset(assets.catalog, tid);
                if (te != nullptr && te->type == AssetType::Texture)
                {
                    job.materialTextures.push_back(
                        ThumbnailTextureSource{ tid, assets.root + "/" + te->path, te->hdr, !te->linear });
                }
            }
        }
        else if (match->type == AssetType::Texture)
        {
            stamp = thumbnailSourceStamp(assets, *match);
            job.texture = ThumbnailTextureSource{ id, assets.root + "/" + match->path, match->hdr, !match->linear };
        }
        else if (match->type == AssetType::Mesh && match->container.value == 0)
        {
            stamp = thumbnailSourceStamp(assets, *match);
            job.meshPath = assets.root + "/" + match->path;
        }
        else if (match->type == AssetType::Mesh || match->type == AssetType::Model)
        {
            // An embedded mesh, or a model (previewed as its primary mesh sub-asset): resolve the
            // .smesh chunk bytes from the container on this (main) thread — the worker has no
            // AssetServer, so it parses the bytes we hand it rather than touching the catalog.
            Uuid containerId = match->container;
            Uuid meshSubId = id;
            if (match->type == AssetType::Model)
            {
                containerId = id;
                meshSubId = Uuid{ 0 };
                if (auto model = loadModelAsset(assets, id))
                {
                    for (const ContainerMetadata::SubAsset& sub : model->meta.subAssets)
                    {
                        if (sub.type == AssetType::Mesh)
                        {
                            meshSubId = sub.subId;
                            break;
                        }
                    }
                }
                if (meshSubId.value == 0)
                {
                    return Err(std::format("model {} has no mesh to preview", id.value));
                }
            }
            auto container = loadModelAsset(assets, containerId);
            if (!container)
            {
                return Err(std::format("model {} is not loadable", containerId.value));
            }
            const ByteSource source = chunkSourceFor(assets, *container, ChunkKind::Mesh, meshSubId);
            if (source.path.empty())
            {
                return Err(std::format("no mesh chunk for sub-asset {}", meshSubId.value));
            }
            auto bytes = source.read();
            if (!bytes)
            {
                return Err(bytes.error());
            }
            stamp = thumbnailSourceStamp(assets, *match);
            job.meshBytes = std::move(*bytes);
            if (match->type == AssetType::Model)
            {
                // Textured preview: resolve each material slot (subAsset order matches Submesh.materialSlot)
                // and hand the worker each referenced texture's embedded chunk bytes + colorspace.
                std::unordered_set<u64> addedTextures;
                const auto addTexture = [&](Uuid tid)
                {
                    if (tid.value == 0 || addedTextures.contains(tid.value))
                    {
                        return;
                    }
                    const AssetEntry* te = findAsset(assets.catalog, tid);
                    if (te == nullptr || te->type != AssetType::Texture)
                    {
                        return;
                    }
                    ThumbnailTextureSource ts;
                    ts.id = tid;
                    if (te->container.value != 0)
                    {
                        auto tc = loadModelAsset(assets, te->container);
                        if (!tc)
                        {
                            return;
                        }
                        Colorspace space = Colorspace::Srgb;
                        if (const TocEntry* toc = tc->reader.find(ChunkKind::Texture, tid.value); toc != nullptr)
                        {
                            space = static_cast<Colorspace>(toc->flags);
                        }
                        const ByteSource tsrc = chunkSourceFor(assets, *tc, ChunkKind::Texture, tid);
                        if (tsrc.path.empty())
                        {
                            return;
                        }
                        auto tb = tsrc.read();
                        if (!tb)
                        {
                            return;
                        }
                        ts.bytes.resize(tb->size());
                        if (!tb->empty())
                        {
                            std::memcpy(ts.bytes.data(), tb->data(), tb->size());
                        }
                        ts.hdr = space == Colorspace::Hdr;
                        ts.srgb = space != Colorspace::Linear && space != Colorspace::Hdr;
                    }
                    else
                    {
                        ts.path = assets.root + "/" + te->path;
                        ts.hdr = te->hdr;
                        ts.srgb = !te->linear;
                    }
                    addedTextures.insert(tid.value);
                    job.materialTextures.push_back(std::move(ts));
                };
                for (const ContainerMetadata::SubAsset& sub : container->meta.subAssets)
                {
                    if (sub.type != AssetType::Material)
                    {
                        continue;
                    }
                    MaterialAsset mat;
                    if (auto resolved = resolveMaterial(assets, id, sub.subId))
                    {
                        mat = std::move(*resolved);
                    }
                    else
                    {
                        logWarn(std::format("model {}: material {} unresolved: {}", id.value, sub.subId.value,
                                            resolved.error()));
                    }
                    for (Uuid tid : { mat.albedoTexture, mat.ormTexture, mat.normalTexture, mat.emissiveTexture,
                                      mat.heightTexture })
                    {
                        addTexture(tid);
                    }
                    job.modelMaterials.push_back(std::move(mat));
                }
                job.type = AssetType::Model;
            }
            else
            {
                job.type = AssetType::Mesh;
            }
        }
        else
        {
            return Err(std::string{ "asset has no thumbnail" });
        }
        if (!stamp.empty())
        {
            job.cachePath = thumbnailCachePath(assets, id, size, stamp).string();
        }

        if (!job.cachePath.empty())
        {
            if (auto hit = readThumbnailCache(job.cachePath))
            {
                return ThumbnailReply{ std::move(hit->bytes), hit->width, hit->height, false };
            }
        }

        // No worker, or no cache key to dedup/persist against: generate inline on the calling thread.
        if (!assets.thumbnailWorker || job.cachePath.empty())
        {
            std::vector<std::pair<Uuid, Ref<GpuTexture>>> texOut;
            std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshOut;
            auto png = generateThumbnail(renderer, job, texOut, meshOut);
            if (!png)
            {
                return Err(png.error());
            }
            insertThumbnailHandback(assets, texOut, meshOut);
            if (!job.cachePath.empty())
            {
                if (auto written = writeThumbnailCache(job.cachePath, png->bytes); !written)
                {
                    logWarn(written.error());
                }
            }
            return ThumbnailReply{ std::move(png->bytes), png->width, png->height, false };
        }

        // Worker path: dedup on cachePath, enqueue once, reply pending.
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        const std::lock_guard<std::mutex> lock(worker.mutex);
        if (worker.failed.contains(job.cachePath))
        {
            return Err(std::string{ "thumbnail generation failed" });
        }
        if (!worker.inFlight.contains(job.cachePath))
        {
            worker.inFlight.insert(job.cachePath);
            worker.queue.push_back(std::move(job));
            worker.cv.notify_one();
        }
        return ThumbnailReply{ {}, 0, 0, true };
    }
}

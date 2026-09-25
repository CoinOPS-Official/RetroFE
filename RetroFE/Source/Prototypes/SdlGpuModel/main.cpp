#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include "Ibl.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace DirectX;
static constexpr SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

static void check(bool ok, const char* action) {
    if (!ok) throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
}
static std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static uint32_t u32(const uint8_t* data) {
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
static Uint32 mipLevels(int w, int h) {
    Uint32 levels = 1;
    while (w > 1 || h > 1) {
        w = std::max(1,w / 2);
        h = std::max(1,h / 2);
        ++levels;
    }
    return levels;
}
static size_t mipBytes(int w, int h) {
    size_t bytes = 0;
    while (true) {
        bytes += size_t(w) * h * 4;
        if (w == 1 && h == 1) break;
        w = std::max(1,w / 2);
        h = std::max(1,h / 2);
    }
    return bytes;
}
static SDL_GPUTexture* texture(SDL_GPUDevice* gpu, int w, int h, SDL_GPUTextureFormat format,
                               SDL_GPUTextureUsageFlags usage,
                               SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1,
                               Uint32 levels = 1) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = static_cast<Uint32>(w);
    info.height = static_cast<Uint32>(h);
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    info.sample_count = samples;
    auto* result = SDL_CreateGPUTexture(gpu, &info);
    check(result != nullptr, "SDL_CreateGPUTexture");
    return result;
}

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
    XMFLOAT4 tangent;
};
struct Material {
    std::string name;
    XMFLOAT4 base{1, 1, 1, 1};
    XMFLOAT3 emissive{0, 0, 0};
    float emissiveStrength = 1;
    float metallic = 0;
    float roughness = 0.75f;
    float normalScale = 1;
    float alphaCutoff = 0.5f;
    float transmission = 0;
    float specularFactor = 1;
    float occlusionStrength = 1;
    XMFLOAT3 specularColor{1,1,1};
    int alphaMode = 0; // 0 opaque, 1 mask, 2 blend
    int baseTexture = -1, normalTexture = -1, mrTexture = -1, emissiveTexture = -1,
        occlusionTexture = -1;
    bool screen = false, marquee = false;
};
struct Primitive {
    uint32_t firstIndex = 0, indexCount = 0;
    int material = 0;
    XMFLOAT4X4 transform{};
};
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    SDL_GPUTexture* colorGpu = nullptr;
    SDL_GPUTexture* dataGpu = nullptr;
};
struct EmissiveLightSource {
    XMFLOAT3 position{0,0,0};
    XMFLOAT3 direction{0,0,-1};
    float receiverMinY = 0;
    bool present = false;
};

struct ModelResource {
    SDL_GPUDevice* gpu = nullptr;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Material> materials;
    std::vector<Primitive> primitives;
    std::vector<Image> images;
    XMFLOAT4X4 normalization{};
    EmissiveLightSource screenLight, marqueeLight;
    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUBuffer* indexBuffer = nullptr;
    ~ModelResource() {
        if (vertexBuffer) SDL_ReleaseGPUBuffer(gpu, vertexBuffer);
        if (indexBuffer) SDL_ReleaseGPUBuffer(gpu, indexBuffer);
        for (auto& image : images) {
            if (image.colorGpu) SDL_ReleaseGPUTexture(gpu, image.colorGpu);
            if (image.dataGpu) SDL_ReleaseGPUTexture(gpu, image.dataGpu);
        }
    }
    ModelResource(const ModelResource&) = delete;
    ModelResource& operator=(const ModelResource&) = delete;
    explicit ModelResource(SDL_GPUDevice* device) : gpu(device) { XMStoreFloat4x4(&normalization, XMMatrixIdentity()); }
};

static void deriveEmissiveLights(ModelResource& model) {
    float highestMarqueeY = -FLT_MAX;
    for (const auto& primitive : model.primitives) {
        const auto& material = model.materials[primitive.material];
        const bool screen = material.screen;
        std::string lowerName = material.name;
        std::transform(lowerName.begin(),lowerName.end(),lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool marquee = material.marquee ||
            (lowerName.find("marquee") != std::string::npos &&
             (material.emissive.x > 0 || material.emissive.y > 0 || material.emissive.z > 0));
        if (!screen && !marquee) continue;
        const XMMATRIX transform = XMLoadFloat4x4(&primitive.transform) *
                                   XMLoadFloat4x4(&model.normalization);
        XMFLOAT3 minimum{FLT_MAX,FLT_MAX,FLT_MAX}, maximum{-FLT_MAX,-FLT_MAX,-FLT_MAX};
        XMVECTOR normalSum = XMVectorZero();
        for (uint32_t i = 0; i < primitive.indexCount; ++i) {
            const auto& vertex = model.vertices[model.indices[primitive.firstIndex+i]];
            XMFLOAT3 point;
            XMStoreFloat3(&point,XMVector3TransformCoord(XMLoadFloat3(&vertex.position),transform));
            minimum.x = std::min(minimum.x,point.x); minimum.y = std::min(minimum.y,point.y);
            minimum.z = std::min(minimum.z,point.z);
            maximum.x = std::max(maximum.x,point.x); maximum.y = std::max(maximum.y,point.y);
            maximum.z = std::max(maximum.z,point.z);
            normalSum += XMVector3TransformNormal(XMLoadFloat3(&vertex.normal),transform);
        }
        if (primitive.indexCount == 0) continue;
        const float centerY = (minimum.y+maximum.y)*0.5f;
        if (marquee && !screen && centerY <= highestMarqueeY) continue;
        XMVECTOR direction = XMVectorGetX(XMVector3LengthSq(normalSum)) > 1e-8f
            ? XMVector3Normalize(normalSum) : XMVectorSet(0,0,-1,0);
        if (XMVectorGetX(XMVector3Dot(direction,XMVectorSet(0,0,-1,0))) < 0)
            direction = -direction;
        XMFLOAT3 normal;
        XMStoreFloat3(&normal,direction);
        EmissiveLightSource source;
        source.position = {(minimum.x+maximum.x)*0.5f + normal.x*0.03f,
                           centerY + normal.y*0.03f,
                           (minimum.z+maximum.z)*0.5f + normal.z*0.03f};
        source.direction = normal;
        source.receiverMinY = minimum.y - (screen ? 0.18f : 0.22f);
        source.present = true;
        if (screen) model.screenLight = source;
        if (marquee) {
            model.marqueeLight = source;
            highestMarqueeY = centerY;
        }
    }
}

static void uploadTexture(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* destination,
                          const void* pixels, int w, int h, Uint32 mip = 0,
                          Uint32 bytesPerPixel = 4) {
    const Uint32 bytes = static_cast<Uint32>(w * h * bytesPerPixel);
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    auto* transfer = SDL_CreateGPUTransferBuffer(gpu, &info);
    check(transfer != nullptr, "SDL_CreateGPUTransferBuffer(texture)");
    auto* mapped = SDL_MapGPUTransferBuffer(gpu, transfer, false);
    check(mapped != nullptr, "SDL_MapGPUTransferBuffer(texture)");
    std::memcpy(mapped, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(gpu, transfer);
    auto* pass = SDL_BeginGPUCopyPass(cmd);
    check(pass != nullptr, "SDL_BeginGPUCopyPass(texture)");
    SDL_GPUTextureTransferInfo src{transfer, 0, static_cast<Uint32>(w), static_cast<Uint32>(h)};
    SDL_GPUTextureRegion dst{destination, mip, 0, 0, 0, 0,
                             static_cast<Uint32>(w), static_cast<Uint32>(h), 1};
    SDL_UploadToGPUTexture(pass, &src, &dst, false);
    SDL_EndGPUCopyPass(pass);
    SDL_ReleaseGPUTransferBuffer(gpu, transfer);
}
static void uploadGeometry(ModelResource& model) {
    check(!model.vertices.empty() && !model.indices.empty(), "Model has no triangles");
    const Uint32 vertexBytes = static_cast<Uint32>(model.vertices.size() * sizeof(Vertex));
    const Uint32 indexBytes = static_cast<Uint32>(model.indices.size() * sizeof(uint32_t));
    SDL_GPUBufferCreateInfo vi{SDL_GPU_BUFFERUSAGE_VERTEX, vertexBytes, 0};
    SDL_GPUBufferCreateInfo ii{SDL_GPU_BUFFERUSAGE_INDEX, indexBytes, 0};
    model.vertexBuffer = SDL_CreateGPUBuffer(model.gpu, &vi);
    model.indexBuffer = SDL_CreateGPUBuffer(model.gpu, &ii);
    check(model.vertexBuffer && model.indexBuffer, "Create model GPU buffers");
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vertexBytes + indexBytes, 0};
    auto* transfer = SDL_CreateGPUTransferBuffer(model.gpu, &ti);
    check(transfer != nullptr, "Create model transfer buffer");
    auto* data = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(model.gpu, transfer, false));
    check(data != nullptr, "Map model transfer buffer");
    std::memcpy(data, model.vertices.data(), vertexBytes);
    std::memcpy(data + vertexBytes, model.indices.data(), indexBytes);
    SDL_UnmapGPUTransferBuffer(model.gpu, transfer);
    auto* cmd = SDL_AcquireGPUCommandBuffer(model.gpu);
    check(cmd != nullptr, "Acquire geometry command buffer");
    auto* pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation source{transfer, 0};
    SDL_GPUBufferRegion destination{model.vertexBuffer, 0, vertexBytes};
    SDL_UploadToGPUBuffer(pass, &source, &destination, false);
    source.offset = vertexBytes;
    destination = {model.indexBuffer, 0, indexBytes};
    SDL_UploadToGPUBuffer(pass, &source, &destination, false);
    SDL_EndGPUCopyPass(pass);
    check(SDL_SubmitGPUCommandBuffer(cmd), "Submit geometry upload");
    SDL_ReleaseGPUTransferBuffer(model.gpu, transfer);
}
static Image decodeImage(const uint8_t* bytes, size_t length) {
    auto* io = SDL_IOFromConstMem(bytes, length);
    check(io != nullptr, "SDL_IOFromConstMem(image)");
    SDL_Surface* source = IMG_Load_IO(io, true);
    check(source != nullptr, "IMG_Load_IO");
    SDL_Surface* rgba = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(source);
    check(rgba != nullptr, "SDL_ConvertSurface(image)");
    Image out;
    out.width = rgba->w;
    out.height = rgba->h;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.rgba.data() + size_t(y) * out.width * 4,
                    static_cast<uint8_t*>(rgba->pixels) + size_t(y) * rgba->pitch,
                    size_t(out.width) * 4);
    }
    SDL_DestroySurface(rgba);
    return out;
}
static void uploadImages(ModelResource& model) {
    if (model.images.empty()) return;
    auto* cmd = SDL_AcquireGPUCommandBuffer(model.gpu);
    check(cmd != nullptr, "Acquire image upload command buffer");
    for (size_t index = 0; index < model.images.size(); ++index) {
        auto& image = model.images[index];
        bool color = false, data = false;
        for (const auto& material : model.materials) {
            color |= material.baseTexture == static_cast<int>(index) ||
                     material.emissiveTexture == static_cast<int>(index);
            data |= material.normalTexture == static_cast<int>(index) ||
                    material.mrTexture == static_cast<int>(index) ||
                    material.occlusionTexture == static_cast<int>(index);
        }
        const auto create = [&](SDL_GPUTextureFormat format) {
            auto* gpuTexture = texture(model.gpu, image.width, image.height, format,
                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                SDL_GPU_SAMPLECOUNT_1, mipLevels(image.width,image.height));
            uploadTexture(model.gpu, cmd, gpuTexture, image.rgba.data(), image.width, image.height);
            SDL_GenerateMipmapsForGPUTexture(cmd,gpuTexture);
            return gpuTexture;
        };
        if (color) image.colorGpu = create(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB);
        if (data) image.dataGpu = create(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        image.rgba.clear();
    }
    check(SDL_SubmitGPUCommandBuffer(cmd), "Submit image uploads");
}

static void addBox(ModelResource& model, XMFLOAT3 min, XMFLOAT3 max, int material) {
    static constexpr float faces[6][4][3] = {
        {{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1},{ 1,-1,-1}},
        {{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},{-1,-1, 1}},
        {{-1,-1, 1},{-1, 1, 1},{-1, 1,-1},{-1,-1,-1}},
        {{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1},{ 1,-1, 1}},
        {{-1, 1,-1},{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1}},
        {{-1,-1, 1},{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1}}
    };
    static constexpr XMFLOAT3 normals[6] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    Primitive p;
    p.firstIndex = static_cast<uint32_t>(model.indices.size());
    p.material = material;
    XMStoreFloat4x4(&p.transform, XMMatrixIdentity());
    for (int face = 0; face < 6; ++face) {
        const auto start = static_cast<uint32_t>(model.vertices.size());
        for (int corner = 0; corner < 4; ++corner) {
            const auto& f = faces[face][corner];
            const XMFLOAT3 position{
                f[0] < 0 ? min.x : max.x,
                f[1] < 0 ? min.y : max.y,
                f[2] < 0 ? min.z : max.z};
            const XMFLOAT2 uv{corner == 2 || corner == 3 ? 1.0f : 0.0f,
                              corner == 0 || corner == 3 ? 1.0f : 0.0f};
            model.vertices.push_back({position, normals[face], uv, {1,0,0,1}});
        }
        for (uint32_t i : {0u,1u,2u,0u,2u,3u}) model.indices.push_back(start + i);
    }
    p.indexCount = static_cast<uint32_t>(model.indices.size()) - p.firstIndex;
    model.primitives.push_back(p);
}
static std::unique_ptr<ModelResource> proceduralCabinet(SDL_GPUDevice* gpu) {
    auto model = std::make_unique<ModelResource>(gpu);
    Material body; body.name = "body"; body.base = {0.055f,0.085f,0.15f,1}; body.metallic = 0.25f; body.roughness = 0.35f;
    Material trim; trim.name = "trim"; trim.base = {0.6f,0.63f,0.68f,1}; trim.metallic = 0.85f; trim.roughness = 0.22f;
    Material screen; screen.name = "retrofe.screen"; screen.screen = true; screen.emissiveStrength = 2.8f;
    Material marquee; marquee.name = "retrofe.marquee"; marquee.marquee = true; marquee.base = {0.85f,0.2f,0.08f,1}; marquee.emissiveStrength = 2.6f;
    Material glass; glass.name = "ScreenGlass"; glass.base = {0.3f,0.48f,0.65f,0.18f}; glass.roughness = 0.08f; glass.alphaMode = 2;
    model->materials = {body, trim, screen, marquee, glass};
    addBox(*model, {-0.50f,0,-0.37f}, {0.50f,2.12f,0.37f}, 0);
    addBox(*model, {-0.46f,0.98f,-0.395f}, {0.46f,1.73f,-0.37f}, 1);
    addBox(*model, {-0.385f,1.08f,-0.423f}, {0.385f,1.62f,-0.395f}, 2);
    addBox(*model, {-0.39f,1.075f,-0.429f}, {0.39f,1.625f,-0.426f}, 4);
    addBox(*model, {-0.45f,1.82f,-0.422f}, {0.45f,2.05f,-0.37f}, 3);
    addBox(*model, {-0.48f,0.73f,-0.52f}, {0.48f,0.91f,-0.37f}, 1);
    addBox(*model, {-0.17f,0.26f,-0.385f}, {0.17f,0.45f,-0.37f}, 1);
    deriveEmissiveLights(*model);
    uploadGeometry(*model);
    return model;
}

struct GlbData {
    json document;
    std::vector<uint8_t> binary;
    fs::path directory;
};
static GlbData readGlb(const fs::path& path) {
    auto bytes = readBytes(path);
    if (bytes.size() < 20 || u32(bytes.data()) != 0x46546C67 || u32(bytes.data() + 4) != 2 ||
        u32(bytes.data() + 8) != bytes.size()) throw std::runtime_error("Expected a valid glTF 2.0 GLB");
    GlbData out;
    out.directory = path.parent_path();
    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const auto length = u32(bytes.data() + offset);
        const auto type = u32(bytes.data() + offset + 4);
        offset += 8;
        if (length > bytes.size() - offset) throw std::runtime_error("Invalid GLB chunk length");
        if (type == 0x4E4F534A) out.document = json::parse(bytes.begin() + offset, bytes.begin() + offset + length);
        if (type == 0x004E4942) out.binary.assign(bytes.begin() + offset, bytes.begin() + offset + length);
        offset += length;
    }
    if (!out.document.is_object() || out.binary.empty()) throw std::runtime_error("GLB needs JSON and BIN chunks");
    if (out.document.value("asset", json::object()).value("version", std::string{}) != "2.0")
        throw std::runtime_error("GLB asset version must be 2.0");
    return out;
}
static int componentSize(int type) {
    switch (type) { case 5120: case 5121: return 1; case 5122: case 5123: return 2;
                    case 5125: case 5126: return 4; default: throw std::runtime_error("Unsupported glTF component type"); }
}
struct Accessor {
    const uint8_t* bytes = nullptr;
    size_t stride = 0, count = 0;
    int component = 0, components = 0;
    bool normalized = false;
};
static Accessor accessor(const GlbData& glb, int index) {
    const auto& a = glb.document.at("accessors").at(index);
    if (a.contains("sparse")) throw std::runtime_error("Sparse glTF accessors are not supported");
    const auto& v = glb.document.at("bufferViews").at(a.at("bufferView").get<size_t>());
    if (v.value("buffer", 0) != 0) throw std::runtime_error("GLB references another buffer");
    const auto type = a.at("type").get<std::string>();
    const int components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 :
        type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
    if (!components) throw std::runtime_error("Unsupported glTF accessor shape");
    Accessor result;
    result.component = a.at("componentType").get<int>();
    result.components = components;
    result.count = a.at("count").get<size_t>();
    result.normalized = a.value("normalized", false);
    const size_t element = size_t(componentSize(result.component)) * components;
    result.stride = v.value("byteStride", element);
    if (result.stride < element) throw std::runtime_error("Invalid glTF accessor stride");
    const size_t start = v.value("byteOffset", size_t{}) + a.value("byteOffset", size_t{});
    const size_t viewEnd = v.value("byteOffset", size_t{}) + v.at("byteLength").get<size_t>();
    const size_t end = result.count ? start + (result.count - 1) * result.stride + element : start;
    if (end > viewEnd || viewEnd > glb.binary.size()) throw std::runtime_error("glTF accessor outside BIN chunk");
    result.bytes = glb.binary.data() + start;
    return result;
}
static float accessFloat(const Accessor& a, size_t row, int component) {
    if (row >= a.count || component >= a.components) throw std::runtime_error("glTF accessor index out of range");
    const uint8_t* p = a.bytes + row * a.stride + component * componentSize(a.component);
    switch (a.component) {
    case 5126: { float x; std::memcpy(&x, p, 4); return x; }
    case 5121: return a.normalized ? p[0] / 255.0f : float(p[0]);
    case 5120: return a.normalized ? std::max(-1.0f, int8_t(p[0]) / 127.0f) : float(int8_t(p[0]));
    case 5123: { uint16_t x; std::memcpy(&x,p,2); return a.normalized ? x / 65535.0f : float(x); }
    case 5122: { int16_t x; std::memcpy(&x,p,2); return a.normalized ? std::max(-1.0f, x / 32767.0f) : float(x); }
    case 5125: { uint32_t x; std::memcpy(&x,p,4); return float(x); }
    default: throw std::runtime_error("Unsupported glTF accessor component");
    }
}
static uint32_t accessIndex(const Accessor& a, size_t row) {
    if (a.components != 1) throw std::runtime_error("glTF indices must be scalar");
    const uint8_t* p = a.bytes + row * a.stride;
    if (a.component == 5121) return p[0];
    if (a.component == 5123) { uint16_t x; std::memcpy(&x,p,2); return x; }
    if (a.component == 5125) { uint32_t x; std::memcpy(&x,p,4); return x; }
    throw std::runtime_error("Unsupported glTF index format");
}
static int imageForTexture(const json& doc, const json& property) {
    if (!property.is_object() || !property.contains("index")) return -1;
    const auto& entry = doc.at("textures").at(property.at("index").get<size_t>());
    return entry.value("source", -1);
}
static XMMATRIX nodeMatrix(const json& node) {
    if (node.contains("matrix")) {
        const auto& a = node.at("matrix");
        if (!a.is_array() || a.size() != 16) throw std::runtime_error("Invalid glTF node matrix");
        XMFLOAT4X4 m;
        for (size_t i = 0; i < 16; ++i) reinterpret_cast<float*>(&m)[i] = a[i].get<float>();
        return XMLoadFloat4x4(&m);
    }
    const auto t = node.value("translation", std::vector<float>{0,0,0});
    const auto r = node.value("rotation", std::vector<float>{0,0,0,1});
    const auto s = node.value("scale", std::vector<float>{1,1,1});
    if (t.size() != 3 || r.size() != 4 || s.size() != 3) throw std::runtime_error("Invalid glTF node transform");
    return XMMatrixScaling(s[0],s[1],s[2]) *
           XMMatrixRotationQuaternion(XMVectorSet(r[0],r[1],r[2],r[3])) *
           XMMatrixTranslation(t[0],t[1],t[2]);
}
static std::unique_ptr<ModelResource> loadGlb(SDL_GPUDevice* gpu, const fs::path& path,
                                              const std::string& screenMaterial) {
    const GlbData glb = readGlb(path);
    const auto& doc = glb.document;
    auto model = std::make_unique<ModelResource>(gpu);
    if (doc.contains("images")) for (const auto& image : doc.at("images")) {
        if (image.contains("bufferView")) {
            const auto& view = doc.at("bufferViews").at(image.at("bufferView").get<size_t>());
            const size_t start = view.value("byteOffset", size_t{}), size = view.at("byteLength").get<size_t>();
            if (start + size > glb.binary.size()) throw std::runtime_error("GLB image outside BIN chunk");
            model->images.push_back(decodeImage(glb.binary.data() + start, size));
        } else if (image.contains("uri")) {
            const std::string uri = image.at("uri").get<std::string>();
            if (uri.starts_with("data:")) throw std::runtime_error("Data URI images are not supported; embed images in GLB");
            const auto bytes = readBytes(glb.directory / fs::path(uri));
            model->images.push_back(decodeImage(bytes.data(), bytes.size()));
        } else throw std::runtime_error("GLB image lacks a source");
    }
    if (doc.contains("materials")) for (const auto& entry : doc.at("materials")) {
        Material m;
        m.name = entry.value("name", std::string{});
        m.screen = m.name == screenMaterial;
        m.marquee = m.name == "retrofe.marquee";
        const auto pbr = entry.value("pbrMetallicRoughness", json::object());
        const auto base = pbr.value("baseColorFactor", std::vector<float>{1,1,1,1});
        if (base.size() == 4) m.base = {base[0],base[1],base[2],base[3]};
        m.metallic = pbr.value("metallicFactor", 1.0f);
        m.roughness = pbr.value("roughnessFactor", 1.0f);
        m.baseTexture = imageForTexture(doc, pbr.value("baseColorTexture", json::object()));
        m.mrTexture = imageForTexture(doc, pbr.value("metallicRoughnessTexture", json::object()));
        m.normalTexture = imageForTexture(doc, entry.value("normalTexture", json::object()));
        m.emissiveTexture = imageForTexture(doc, entry.value("emissiveTexture", json::object()));
        m.occlusionTexture = imageForTexture(doc, entry.value("occlusionTexture", json::object()));
        m.occlusionStrength = entry.value("occlusionTexture", json::object()).value("strength", 1.0f);
        m.normalScale = entry.value("normalTexture", json::object()).value("scale", 1.0f);
        const auto emissive = entry.value("emissiveFactor", std::vector<float>{0,0,0});
        if (emissive.size() == 3) m.emissive = {emissive[0],emissive[1],emissive[2]};
        const auto extensions = entry.value("extensions", json::object());
        if (extensions.contains("KHR_materials_specular")) {
            const auto& specular = extensions.at("KHR_materials_specular");
            m.specularFactor = specular.value("specularFactor",1.0f);
            const auto color = specular.value("specularColorFactor",std::vector<float>{1,1,1});
            if (color.size()==3) m.specularColor = {color[0],color[1],color[2]};
        }
        if (extensions.contains("KHR_materials_emissive_strength"))
            m.emissiveStrength = extensions.at("KHR_materials_emissive_strength").value("emissiveStrength", 1.0f);
        if (extensions.contains("KHR_materials_transmission"))
            m.transmission = std::clamp(
                extensions.at("KHR_materials_transmission").value("transmissionFactor", 0.0f),0.0f,1.0f);
        m.alphaCutoff = entry.value("alphaCutoff", 0.5f);
        const std::string alpha = entry.value("alphaMode", std::string("OPAQUE"));
        m.alphaMode = alpha == "BLEND" ? 2 : alpha == "MASK" ? 1 : 0;
        if (m.transmission > 0) m.alphaMode = 2;
        model->materials.push_back(m);
    }
    if (model->materials.empty()) model->materials.emplace_back();
    const auto& nodes = doc.at("nodes");
    const auto& meshes = doc.at("meshes");
    auto visit = [&](auto&& self, size_t nodeIndex, XMMATRIX parent, int depth) -> void {
        if (depth > 64) throw std::runtime_error("glTF node hierarchy too deep");
        const auto& node = nodes.at(nodeIndex);
        const XMMATRIX world = nodeMatrix(node) * parent;
        if (node.contains("mesh")) {
            const auto& mesh = meshes.at(node.at("mesh").get<size_t>());
            for (const auto& source : mesh.at("primitives")) {
                if (source.value("mode", 4) != 4) continue;
                const auto& attributes = source.at("attributes");
                const auto positions = accessor(glb, attributes.at("POSITION").get<int>());
                const bool hasNormals = attributes.contains("NORMAL");
                const bool hasUVs = attributes.contains("TEXCOORD_0");
                const bool hasTangents = attributes.contains("TANGENT");
                Accessor normals{}, uvs{}, tangents{};
                if (hasNormals) normals = accessor(glb, attributes.at("NORMAL").get<int>());
                if (hasUVs) uvs = accessor(glb, attributes.at("TEXCOORD_0").get<int>());
                if (hasTangents) tangents = accessor(glb, attributes.at("TANGENT").get<int>());
                const auto baseVertex = static_cast<uint32_t>(model->vertices.size());
                for (size_t i = 0; i < positions.count; ++i) {
                    Vertex vertex{};
                    vertex.position = {accessFloat(positions,i,0),accessFloat(positions,i,1),accessFloat(positions,i,2)};
                    vertex.normal = hasNormals ? XMFLOAT3{accessFloat(normals,i,0),accessFloat(normals,i,1),accessFloat(normals,i,2)} : XMFLOAT3{0,0,0};
                    vertex.uv = hasUVs ? XMFLOAT2{accessFloat(uvs,i,0),accessFloat(uvs,i,1)} : XMFLOAT2{0,0};
                    vertex.tangent = hasTangents ? XMFLOAT4{accessFloat(tangents,i,0),accessFloat(tangents,i,1),accessFloat(tangents,i,2),accessFloat(tangents,i,3)} : XMFLOAT4{0,0,0,1};
                    model->vertices.push_back(vertex);
                }
                Primitive primitive;
                primitive.firstIndex = static_cast<uint32_t>(model->indices.size());
                primitive.material = source.value("material", 0);
                if (primitive.material < 0 || primitive.material >= static_cast<int>(model->materials.size()))
                    throw std::runtime_error("GLB primitive has invalid material");
                XMStoreFloat4x4(&primitive.transform, world);
                if (source.contains("indices")) {
                    const auto indices = accessor(glb, source.at("indices").get<int>());
                    for (size_t i = 0; i < indices.count; ++i) {
                        const auto index = accessIndex(indices, i);
                        if (index >= positions.count) throw std::runtime_error("GLB index exceeds vertex count");
                        model->indices.push_back(baseVertex + index);
                    }
                } else for (size_t i = 0; i < positions.count; ++i)
                    model->indices.push_back(baseVertex + static_cast<uint32_t>(i));
                primitive.indexCount = static_cast<uint32_t>(model->indices.size()) - primitive.firstIndex;
                if (!hasNormals || !hasTangents) {
                    std::vector<XMVECTOR> normalSum(positions.count, XMVectorZero());
                    std::vector<XMVECTOR> tangentSum(positions.count, XMVectorZero());
                    std::vector<XMVECTOR> bitangentSum(positions.count, XMVectorZero());
                    for (uint32_t k = 0; k + 2 < primitive.indexCount; k += 3) {
                        const auto a = model->indices[primitive.firstIndex+k] - baseVertex;
                        const auto b = model->indices[primitive.firstIndex+k+1] - baseVertex;
                        const auto c = model->indices[primitive.firstIndex+k+2] - baseVertex;
                        const auto& va = model->vertices[baseVertex+a];
                        const auto& vb = model->vertices[baseVertex+b];
                        const auto& vc = model->vertices[baseVertex+c];
                        const XMVECTOR edge1 = XMLoadFloat3(&vb.position) - XMLoadFloat3(&va.position);
                        const XMVECTOR edge2 = XMLoadFloat3(&vc.position) - XMLoadFloat3(&va.position);
                        const XMVECTOR n = XMVector3Cross(edge1,edge2);
                        for (auto index : {a,b,c}) normalSum[index] += n;
                        const float du1 = vb.uv.x - va.uv.x, dv1 = vb.uv.y - va.uv.y;
                        const float du2 = vc.uv.x - va.uv.x, dv2 = vc.uv.y - va.uv.y;
                        const float determinant = du1*dv2 - dv1*du2;
                        if (std::abs(determinant) > 1e-8f) {
                            const XMVECTOR tangent = (edge1 * dv2 - edge2 * dv1) / determinant;
                            const XMVECTOR bitangent = (edge2 * du1 - edge1 * du2) / determinant;
                            for (auto index : {a,b,c}) {
                                tangentSum[index] += tangent;
                                bitangentSum[index] += bitangent;
                            }
                        }
                    }
                    for (size_t v = 0; v < positions.count; ++v) {
                        auto& vertex = model->vertices[baseVertex+v];
                        if (!hasNormals) {
                            XMVECTOR n = normalSum[v];
                            if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-10f) n = XMVectorSet(0,0,-1,0);
                            XMStoreFloat3(&vertex.normal,XMVector3Normalize(n));
                        }
                        if (!hasTangents) {
                            const XMVECTOR n = XMLoadFloat3(&vertex.normal);
                            XMVECTOR t = tangentSum[v] - n * XMVector3Dot(n,tangentSum[v]);
                            if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-10f)
                                t = XMVector3Cross(XMVectorSet(0,1,0,0),n);
                            if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-10f)
                                t = XMVector3Cross(XMVectorSet(1,0,0,0),n);
                            t = XMVector3Normalize(t);
                            const float sign = XMVectorGetX(XMVector3Dot(XMVector3Cross(n,t),bitangentSum[v])) < 0 ? -1.0f : 1.0f;
                            XMFLOAT3 result;
                            XMStoreFloat3(&result,t);
                            vertex.tangent = {result.x,result.y,result.z,sign};
                        }
                    }
                }
                model->primitives.push_back(primitive);
            }
        }
        if (node.contains("children")) for (const auto child : node.at("children"))
            self(self, child.get<size_t>(), world, depth + 1);
    };
    const auto sceneIndex = doc.value("scene", size_t{});
    for (const auto root : doc.at("scenes").at(sceneIndex).at("nodes"))
        visit(visit, root.get<size_t>(), XMMatrixIdentity(), 0);
    if (model->primitives.empty()) throw std::runtime_error("GLB has no triangle primitives in its default scene");
    XMFLOAT3 minimum{FLT_MAX,FLT_MAX,FLT_MAX}, maximum{-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for (const auto& primitive : model->primitives) {
        const XMMATRIX transform = XMLoadFloat4x4(&primitive.transform);
        for (uint32_t i = 0; i < primitive.indexCount; ++i) {
            const XMVECTOR p = XMVector3TransformCoord(
                XMLoadFloat3(&model->vertices[model->indices[primitive.firstIndex+i]].position),transform);
            XMFLOAT3 point; XMStoreFloat3(&point,p);
            minimum.x = std::min(minimum.x,point.x); minimum.y = std::min(minimum.y,point.y);
            minimum.z = std::min(minimum.z,point.z);
            maximum.x = std::max(maximum.x,point.x); maximum.y = std::max(maximum.y,point.y);
            maximum.z = std::max(maximum.z,point.z);
        }
    }
    const float height = std::max(0.001f,maximum.y-minimum.y);
    const float scale = 2.12f / height;
    // glTF is right handed while the SDL_GPU scene uses a left handed camera.
    // Reflect Z once at the asset boundary so its front (+Z) faces our camera
    // (-Z), without reversing the artwork or every instance's controls.
    XMStoreFloat4x4(&model->normalization,
        XMMatrixTranslation(-(minimum.x+maximum.x)*0.5f,-minimum.y,-(minimum.z+maximum.z)*0.5f) *
        XMMatrixScaling(scale,scale,-scale));
    deriveEmissiveLights(*model);
    uploadGeometry(*model);
    uploadImages(*model);
    return model;
}

class VideoSource {
    GstElement* pipeline_ = nullptr;
    GstElement* sink_ = nullptr;
    GstBus* bus_ = nullptr;
    std::vector<uint8_t> frame_;
    XMFLOAT3 average_{0.3f,0.3f,0.5f};
    uint64_t decodedFrames_ = 0;
    static void onPad(GstElement*, GstPad* pad, gpointer target) {
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        if (caps) {
            const GstStructure* structure = gst_caps_get_structure(caps, 0);
            const char* name = gst_structure_get_name(structure);
            if (name && g_str_has_prefix(name, "video/")) {
                GstPad* sinkPad = gst_element_get_static_pad(static_cast<GstElement*>(target), "sink");
                if (sinkPad) {
                    if (!gst_pad_is_linked(sinkPad)) gst_pad_link(pad, sinkPad);
                    gst_object_unref(sinkPad);
                }
            }
            gst_caps_unref(caps);
        }
    }
public:
    VideoSource(const fs::path& path, int width, int height) {
        pipeline_ = gst_pipeline_new("cabinet-video");
        auto* file = gst_element_factory_make("filesrc", nullptr);
        auto* demux = gst_element_factory_make("qtdemux", nullptr);
        auto* parser = gst_element_factory_make("h264parse", nullptr);
        auto* decoder = gst_element_factory_make("avdec_h264", nullptr);
        auto* convert = gst_element_factory_make("videoconvert", nullptr);
        auto* scale = gst_element_factory_make("videoscale", nullptr);
        auto* filter = gst_element_factory_make("capsfilter", nullptr);
        sink_ = gst_element_factory_make("appsink", nullptr);
        if (!pipeline_ || !file || !demux || !parser || !decoder || !convert || !scale || !filter || !sink_)
            throw std::runtime_error("GStreamer requires qtdemux, h264parse, avdec_h264, videoconvert, videoscale and appsink");
        const auto native = path.string();
        g_object_set(file, "location", native.c_str(), nullptr);
        g_object_set(scale, "add-borders", TRUE, nullptr);
        GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA",
            "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
            "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
        g_object_set(filter, "caps", caps, nullptr);
        gst_caps_unref(caps);
        g_object_set(sink_, "max-buffers", 1, "drop", TRUE, "sync", TRUE,
                     "enable-last-sample", FALSE, nullptr);
        gst_bin_add_many(GST_BIN(pipeline_), file, demux, parser, decoder, convert, scale, filter, sink_, nullptr);
        if (!gst_element_link(file, demux) ||
            !gst_element_link_many(parser, decoder, convert, scale, filter, sink_, nullptr))
            throw std::runtime_error("Cannot link GStreamer software decode pipeline");
        g_signal_connect(demux, "pad-added", G_CALLBACK(onPad), parser);
        bus_ = gst_element_get_bus(pipeline_);
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("GStreamer could not play " + path.string());
        frame_.resize(size_t(width) * height * 4);
    }
    ~VideoSource() {
        if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (bus_) gst_object_unref(bus_);
        if (pipeline_) gst_object_unref(pipeline_);
    }
    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;
    bool update(int width, int height) {
        while (GstMessage* msg = gst_bus_pop_filtered(bus_, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                gst_element_seek_simple(pipeline_, GST_FORMAT_TIME,
                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
            } else {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(msg, &error, &debug);
                std::string description = error ? error->message : "unknown GStreamer error";
                if (error) g_error_free(error);
                g_free(debug);
                gst_message_unref(msg);
                throw std::runtime_error(description);
            }
            gst_message_unref(msg);
        }
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0);
        if (!sample) return false;
        GstVideoInfo info;
        GstVideoFrame video{};
        bool changed = false;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
            static_cast<int>(info.width) == width && static_cast<int>(info.height) == height &&
            GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_RGBA &&
            gst_video_frame_map(&video, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
            const auto* pixels = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&video, 0));
            const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video, 0);
            uint64_t r = 0, g = 0, b = 0, count = 0;
            for (int y = 0; y < height; ++y) {
                std::memcpy(frame_.data() + size_t(y) * width * 4, pixels + ptrdiff_t(y) * stride,
                            size_t(width) * 4);
                if (y % 16 == 0) for (int x = 0; x < width; x += 16) {
                    const auto* pixel = pixels + ptrdiff_t(y) * stride + x * 4;
                    r += pixel[0]; g += pixel[1]; b += pixel[2]; ++count;
                }
            }
            if (count) average_ = {float(r) / (255 * count),float(g) / (255 * count),float(b) / (255 * count)};
            gst_video_frame_unmap(&video);
            ++decodedFrames_;
            changed = true;
        }
        gst_sample_unref(sample);
        return changed;
    }
    const std::vector<uint8_t>& frame() const { return frame_; }
    XMFLOAT3 average() const { return average_; }
    uint64_t decodedFrames() const { return decodedFrames_; }
};

static std::vector<uint8_t> animatedPattern(int w, int h, double time, XMFLOAT3& average) {
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    const float wave = float(0.5 + 0.5 * std::sin(time * 1.7));
    average = {0.2f + 0.5f * wave, 0.2f, 0.6f - 0.35f * wave};
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const auto i = (size_t(y) * w + x) * 4;
        const int grid = ((x / 40 + y / 40 + int(time * 4)) & 1) ? 45 : 0;
        pixels[i] = static_cast<uint8_t>(std::clamp(int(255 * average.x + grid), 0, 255));
        pixels[i+1] = static_cast<uint8_t>(std::clamp(int(255 * average.y + grid), 0, 255));
        pixels[i+2] = static_cast<uint8_t>(std::clamp(int(255 * average.z + grid), 0, 255));
        pixels[i+3] = 255;
    }
    return pixels;
}

struct SceneUniform {
    XMFLOAT4X4 model, viewProjection, cabinetModel, normalModel;
};
struct ShadowSceneUniform { XMFLOAT4X4 model, lightViewProjection; };
struct ShadowMaterialUniform { XMFLOAT4 alphaOptions; };
struct AoSceneUniform { XMFLOAT4X4 model, viewProjection, normalModel; };
struct AoUniform { XMFLOAT4 texelAndRadius; };
struct MaterialUniform {
    XMFLOAT4 base, emissive, surface, flags, specular, occlusion;
};
struct LightingUniform {
    XMFLOAT4 cameraAndTime;
    XMFLOAT4 keyPositionIntensity;
    XMFLOAT4 fillPositionIntensity;
    XMFLOAT4 screenPositionIntensity;
    XMFLOAT4 screenColor;
    XMFLOAT4 screenDirectionCone;
    XMFLOAT4 marqueePositionIntensity;
    XMFLOAT4 marqueeDirectionCone;
    XMFLOAT4 spillReceiverHeights;
    XMFLOAT4 ambientAndMatcap;
    XMFLOAT4 cameraRight;
    XMFLOAT4 cameraUp;
    XMFLOAT4 pbrOptions;
    std::array<XMFLOAT4X4,4> shadowViewProjection;
    XMFLOAT4 shadowOptions; // enabled, inverse map size, inverse target width/height
    XMFLOAT4 shadowActive;
};
struct PostUniform { XMFLOAT4 options, texel; };
struct Controls {
    float rotateX = 0, rotateY = 0, rotateZ = 0;
    float scale = 1;
    XMFLOAT3 camera{0,1.28f,-3.65f};
    XMFLOAT3 target{0,1.05f,0};
    float fov = XMConvertToRadians(48);
    float screenEmissive = 2.8f;
    float marqueeEmissive = 2.5f;
    XMFLOAT3 keyPosition{-1.8f,2.9f,-2.3f};
    XMFLOAT3 fillPosition{1.6f,1.7f,-1.2f};
    float keyLight = 4.0f;
    float fillLight = 1.0f;
    float ambientLight = 0.6f;
    float environmentRotation = 0.0f;
    bool specularAa = true;
    bool multiscatter = true;
    bool shadows = true;
    bool ssao = true;
    float screenLight = 1.5f;
    float marqueeLight = 1.0f;
    float matcapStrength = 0.0f;
    float exposure = 1.0f;
    bool filmicTonemap = true;
};
struct RenderTarget {
    int width = 0, height = 0;
    SDL_GPUTexture *msaaColor = nullptr, *hdr = nullptr, *depth = nullptr, *bright = nullptr,
                   *blurA = nullptr, *blurB = nullptr, *final = nullptr;
    SDL_Texture* wrapped = nullptr;
    std::array<SDL_GPUTexture*,4> shadowMaps{};
    SDL_GPUTexture *aoWorld = nullptr, *aoNormal = nullptr,
                   *aoDepth = nullptr, *ao = nullptr;
};

class ModelRenderer {
    SDL_GPUDevice* gpu_;
    SDL_Renderer* renderer_;
    SDL_GPUSampleCount sampleCount_ = SDL_GPU_SAMPLECOUNT_1;
    int sampleCountValue_ = 1;
    SDL_GPUShader *pbrVertex_ = nullptr, *pbrFragment_ = nullptr,
                  *shadowVertex_ = nullptr, *shadowFragment_ = nullptr,
                  *aoGeometryVertex_ = nullptr, *aoGeometryFragment_ = nullptr,
                  *aoFragment_ = nullptr,
                  *postVertex_ = nullptr, *postFragment_ = nullptr;
    SDL_GPUGraphicsPipeline *opaque_ = nullptr, *blend_ = nullptr,
                            *shadowPipeline_ = nullptr,
                            *aoGeometryPipeline_ = nullptr, *aoPipeline_ = nullptr,
                            *hdrPost_ = nullptr, *ldrPost_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUSampler* postSampler_ = nullptr;
    SDL_GPUSampler* environmentSampler_ = nullptr;
    SDL_GPUSampler* shadowSampler_ = nullptr;
    SDL_GPUTexture *white_ = nullptr, *normal_ = nullptr, *neutralMr_ = nullptr,
                   *black_ = nullptr, *environment_ = nullptr, *matcap_ = nullptr,
                   *screen_ = nullptr, *irradiance_ = nullptr, *dfg_ = nullptr;
    std::array<RenderTarget, 3> targets_{};
    XMFLOAT3 screenAverage_{0.2f,0.2f,0.4f};
    static constexpr int screenWidth_ = 640, screenHeight_ = 480;
    static constexpr int shadowSize_ = 512;

    SDL_GPUShader* shader(const fs::path& path, SDL_GPUShaderStage stage, Uint32 samplers, Uint32 uniforms) {
        const auto bytes = readBytes(path);
        SDL_GPUShaderCreateInfo info{};
        info.code_size = bytes.size();
        info.code = bytes.data();
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        auto* result = SDL_CreateGPUShader(gpu_, &info);
        check(result != nullptr, "SDL_CreateGPUShader");
        return result;
    }
    SDL_GPUTexture* solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        auto* result = texture(gpu_, 1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        const uint8_t pixel[4] = {r,g,b,a};
        auto* cmd = SDL_AcquireGPUCommandBuffer(gpu_);
        check(cmd != nullptr, "Acquire solid texture command buffer");
        uploadTexture(gpu_, cmd, result, pixel, 1, 1);
        check(SDL_SubmitGPUCommandBuffer(cmd), "Submit solid texture upload");
        return result;
    }
    SDL_GPUGraphicsPipeline* pbrPipeline(bool alphaBlend) {
        SDL_GPUVertexBufferDescription buffer{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        SDL_GPUVertexAttribute attributes[4] = {
            {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,position)},
            {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,normal)},
            {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(Vertex,uv)},
            {3,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(Vertex,tangent)}
        };
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        if (alphaBlend) {
            color.blend_state.enable_blend = true;
            color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = pbrVertex_;
        info.fragment_shader = pbrFragment_;
        info.vertex_input_state = {&buffer,1,attributes,4};
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = sampleCount_;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = !alphaBlend;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info = {&color,1,depthFormat,true};
        auto* result = SDL_CreateGPUGraphicsPipeline(gpu_, &info);
        check(result != nullptr, "Create PBR pipeline");
        return result;
    }
    SDL_GPUGraphicsPipeline* shadowPipeline() {
        SDL_GPUVertexBufferDescription buffer{0,sizeof(Vertex),SDL_GPU_VERTEXINPUTRATE_VERTEX,0};
        SDL_GPUVertexAttribute attributes[2] = {
            {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,position)},
            {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(Vertex,uv)}
        };
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = shadowVertex_;
        info.fragment_shader = shadowFragment_;
        info.vertex_input_state = {&buffer,1,attributes,2};
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info = {nullptr,0,depthFormat,true};
        auto* result = SDL_CreateGPUGraphicsPipeline(gpu_,&info);
        check(result != nullptr,"Create shadow pipeline");
        return result;
    }
    SDL_GPUGraphicsPipeline* aoGeometryPipeline() {
        SDL_GPUVertexBufferDescription buffer{0,sizeof(Vertex),SDL_GPU_VERTEXINPUTRATE_VERTEX,0};
        SDL_GPUVertexAttribute attributes[3] = {
            {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,position)},
            {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,normal)},
            {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(Vertex,uv)}
        };
        SDL_GPUColorTargetDescription colors[2]{};
        colors[0].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        colors[1].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = aoGeometryVertex_;
        info.fragment_shader = aoGeometryFragment_;
        info.vertex_input_state = {&buffer,1,attributes,3};
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info = {colors,2,depthFormat,true};
        auto* result = SDL_CreateGPUGraphicsPipeline(gpu_,&info);
        check(result != nullptr,"Create AO geometry pipeline");
        return result;
    }
    SDL_GPUGraphicsPipeline* aoPipeline() {
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = postVertex_;
        info.fragment_shader = aoFragment_;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info = {&color,1,SDL_GPU_TEXTUREFORMAT_INVALID,false};
        auto* result = SDL_CreateGPUGraphicsPipeline(gpu_,&info);
        check(result != nullptr,"Create AO pipeline");
        return result;
    }
    SDL_GPUGraphicsPipeline* postPipeline(SDL_GPUTextureFormat format) {
        SDL_GPUColorTargetDescription color{};
        color.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = postVertex_;
        info.fragment_shader = postFragment_;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info = {&color,1,SDL_GPU_TEXTUREFORMAT_INVALID,false};
        auto* result = SDL_CreateGPUGraphicsPipeline(gpu_, &info);
        check(result != nullptr, "Create post pipeline");
        return result;
    }
    RenderTarget createTarget(int width, int height) {
        RenderTarget target;
        target.width = width;
        target.height = height;
        const auto hdrUsage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        target.hdr = texture(gpu_, width, height, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, hdrUsage);
        if (sampleCountValue_ > 1)
            target.msaaColor = texture(gpu_, width, height, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                       SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, sampleCount_);
        target.depth = texture(gpu_, width, height, depthFormat,
                               SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, sampleCount_);
        for (auto& map : target.shadowMaps)
            map = texture(gpu_,shadowSize_,shadowSize_,depthFormat,
                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        const auto aoUsage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        target.aoWorld = texture(gpu_,width,height,SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,aoUsage);
        target.aoNormal = texture(gpu_,width,height,SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,aoUsage);
        target.aoDepth = texture(gpu_,width,height,depthFormat,SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        target.ao = texture(gpu_,width,height,SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,aoUsage);
        const int halfW = std::max(1,width / 2), halfH = std::max(1,height / 2);
        target.bright = texture(gpu_, halfW, halfH, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, hdrUsage);
        target.blurA = texture(gpu_, halfW, halfH, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, hdrUsage);
        target.blurB = texture(gpu_, halfW, halfH, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, hdrUsage);
        target.final = texture(gpu_, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, hdrUsage);
        const auto props = SDL_CreateProperties();
        check(props != 0, "SDL_CreateProperties(texture)");
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
        SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, target.final);
        target.wrapped = SDL_CreateTextureWithProperties(renderer_, props);
        SDL_DestroyProperties(props);
        check(target.wrapped != nullptr, "Wrap SDL_GPUTexture as SDL_Texture");
        SDL_SetTextureScaleMode(target.wrapped, SDL_SCALEMODE_LINEAR);
        // The final shader stores premultiplied color so bilinear scaling keeps
        // silhouette coverage and bloom free of dark fringes.
        check(SDL_SetTextureBlendMode(target.wrapped, SDL_BLENDMODE_BLEND_PREMULTIPLIED),
              "Set premultiplied cabinet texture blending");
        return target;
    }
    void destroyTarget(RenderTarget& target) {
        if (target.wrapped) SDL_DestroyTexture(target.wrapped);
        for (auto* image : {target.msaaColor,target.hdr,target.depth,target.bright,
                            target.blurA,target.blurB,target.final})
            if (image) SDL_ReleaseGPUTexture(gpu_,image);
        for (auto* map : target.shadowMaps) if (map) SDL_ReleaseGPUTexture(gpu_,map);
        for (auto* image : {target.aoWorld,target.aoNormal,target.aoDepth,target.ao})
            if (image) SDL_ReleaseGPUTexture(gpu_,image);
        target = {};
    }
    SDL_GPUTexture* materialTexture(const ModelResource& model, int index,
                                    SDL_GPUTexture* fallback, bool color) const {
        if (index < 0 || index >= static_cast<int>(model.images.size())) return fallback;
        return color ? model.images[index].colorGpu : model.images[index].dataGpu;
    }
    void post(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* source, SDL_GPUTexture* bloom,
              SDL_GPUTexture* destination, SDL_GPUGraphicsPipeline* pipeline,
              float mode, int sourceWidth, int sourceHeight,
              float exposure = 1.0f, bool filmicTonemap = false) {
        SDL_GPUColorTargetInfo color{};
        color.texture = destination;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        color.clear_color = {0,0,0,1};
        auto* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
        check(pass != nullptr, "Begin post pass");
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        const SDL_GPUTextureSamplerBinding bindings[2] = {{source,postSampler_},{bloom,postSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
        const PostUniform data{{mode,exposure,filmicTonemap ? 1.0f : 0.0f,0},
                               {1.0f/sourceWidth,1.0f/sourceHeight,0,0}};
        SDL_PushGPUFragmentUniformData(cmd, 0, &data, sizeof(data));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    struct ShadowSetup {
        std::array<XMFLOAT4X4,4> viewProjection{};
        std::array<bool,4> active{};
    };
    static XMMATRIX cabinetInstance(float angle, const Controls& controls) {
        return XMMatrixScaling(controls.scale,controls.scale,controls.scale) *
            XMMatrixRotationX(controls.rotateX) * XMMatrixRotationY(angle + controls.rotateY) *
            XMMatrixRotationZ(controls.rotateZ);
    }
    ShadowSetup shadowSetup(const ModelResource& model, float angle,
                            const Controls& controls) const {
        ShadowSetup result;
        const XMMATRIX instance = cabinetInstance(angle,controls);
        const auto matrix = [&](int index, XMVECTOR eye, XMVECTOR target,
                                float fov, float nearPlane, float farPlane) {
            const XMMATRIX view = XMMatrixLookAtLH(eye,target,XMVectorSet(0,1,0,0));
            const XMMATRIX projection = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(fov),1,nearPlane,farPlane);
            XMStoreFloat4x4(&result.viewProjection[index],view*projection);
        };
        result.active[0] = controls.keyLight > 0;
        result.active[1] = controls.fillLight > 0;
        const XMVECTOR center = XMVectorSet(0,1.05f,0,1);
        matrix(0,XMLoadFloat3(&controls.keyPosition),center,110,0.05f,12);
        matrix(1,XMLoadFloat3(&controls.fillPosition),center,120,0.05f,12);
        const auto localLight = [&](int index, const EmissiveLightSource& source,
                                    float intensity) {
            result.active[index] = source.present && intensity > 0;
            XMVECTOR eye = XMVector3TransformCoord(XMLoadFloat3(&source.position),instance);
            XMVECTOR direction = XMVector3Normalize(
                XMVector3TransformNormal(XMLoadFloat3(&source.direction),instance));
            matrix(index,eye,eye+direction,145,0.015f,5);
        };
        localLight(2,model.screenLight,controls.screenLight);
        localLight(3,model.marqueeLight,controls.marqueeLight);
        return result;
    }
    void renderShadowMaps(SDL_GPUCommandBuffer* cmd, const ModelResource& model,
                          float angle, const Controls& controls,
                          const ShadowSetup& setup, const RenderTarget& target) {
        const XMMATRIX instance = cabinetInstance(angle,controls);
        const SDL_GPUBufferBinding vertex{model.vertexBuffer,0};
        const SDL_GPUBufferBinding index{model.indexBuffer,0};
        for (int light=0; light<4; ++light) {
            if (!setup.active[light]) continue;
            SDL_GPUDepthStencilTargetInfo depth{};
            depth.texture = target.shadowMaps[light];
            depth.clear_depth = 1;
            depth.load_op = SDL_GPU_LOADOP_CLEAR;
            depth.store_op = SDL_GPU_STOREOP_STORE;
            auto* pass = SDL_BeginGPURenderPass(cmd,nullptr,0,&depth);
            check(pass != nullptr,"Begin shadow render pass");
            SDL_BindGPUGraphicsPipeline(pass,shadowPipeline_);
            SDL_BindGPUVertexBuffers(pass,0,&vertex,1);
            SDL_BindGPUIndexBuffer(pass,&index,SDL_GPU_INDEXELEMENTSIZE_32BIT);
            for (const auto& primitive : model.primitives) {
                const auto& material = model.materials[primitive.material];
                if (material.alphaMode == 2) continue;
                const XMMATRIX cabinet = XMLoadFloat4x4(&primitive.transform) *
                    XMLoadFloat4x4(&model.normalization);
                ShadowSceneUniform scene{};
                XMStoreFloat4x4(&scene.model,cabinet*instance);
                scene.lightViewProjection = setup.viewProjection[light];
                SDL_PushGPUVertexUniformData(cmd,0,&scene,sizeof(scene));
                const ShadowMaterialUniform properties{{material.base.w,material.alphaCutoff,
                    material.alphaMode == 1 ? 1.0f : 0.0f,0}};
                SDL_PushGPUFragmentUniformData(cmd,0,&properties,sizeof(properties));
                const SDL_GPUTextureSamplerBinding binding{
                    material.screen ? screen_ : materialTexture(model,material.baseTexture,white_,true),
                    sampler_};
                SDL_BindGPUFragmentSamplers(pass,0,&binding,1);
                SDL_DrawGPUIndexedPrimitives(pass,primitive.indexCount,1,primitive.firstIndex,0,0);
            }
            SDL_EndGPURenderPass(pass);
        }
    }
    void renderAo(SDL_GPUCommandBuffer* cmd, const ModelResource& model, float angle,
                  const Controls& controls, const RenderTarget& target) {
        const XMMATRIX instance = cabinetInstance(angle,controls);
        const XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&controls.camera),
            XMLoadFloat3(&controls.target),XMVectorSet(0,1,0,0));
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(controls.fov,
            float(target.width)/target.height,0.1f,40.0f);
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp,view*projection);
        SDL_GPUColorTargetInfo colors[2]{};
        colors[0].texture = target.aoWorld;
        colors[1].texture = target.aoNormal;
        for (auto& color : colors) {
            color.clear_color = {0,0,0,0};
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
        }
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = target.aoDepth;
        depth.clear_depth = 1;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        auto* pass = SDL_BeginGPURenderPass(cmd,colors,2,&depth);
        check(pass != nullptr,"Begin AO geometry pass");
        SDL_BindGPUGraphicsPipeline(pass,aoGeometryPipeline_);
        const SDL_GPUBufferBinding vertex{model.vertexBuffer,0};
        const SDL_GPUBufferBinding index{model.indexBuffer,0};
        SDL_BindGPUVertexBuffers(pass,0,&vertex,1);
        SDL_BindGPUIndexBuffer(pass,&index,SDL_GPU_INDEXELEMENTSIZE_32BIT);
        for (const auto& primitive : model.primitives) {
            const auto& material = model.materials[primitive.material];
            if (material.alphaMode == 2) continue;
            const XMMATRIX cabinet = XMLoadFloat4x4(&primitive.transform) *
                XMLoadFloat4x4(&model.normalization);
            const XMMATRIX local = cabinet*instance;
            AoSceneUniform scene{};
            XMStoreFloat4x4(&scene.model,local);
            scene.viewProjection = vp;
            XMStoreFloat4x4(&scene.normalModel,XMMatrixTranspose(XMMatrixInverse(nullptr,local)));
            SDL_PushGPUVertexUniformData(cmd,0,&scene,sizeof(scene));
            const ShadowMaterialUniform properties{{material.base.w,material.alphaCutoff,
                material.alphaMode == 1 ? 1.0f : 0.0f,0}};
            SDL_PushGPUFragmentUniformData(cmd,0,&properties,sizeof(properties));
            const SDL_GPUTextureSamplerBinding binding{
                material.screen ? screen_ : materialTexture(model,material.baseTexture,white_,true),
                sampler_};
            SDL_BindGPUFragmentSamplers(pass,0,&binding,1);
            SDL_DrawGPUIndexedPrimitives(pass,primitive.indexCount,1,primitive.firstIndex,0,0);
        }
        SDL_EndGPURenderPass(pass);
        SDL_GPUColorTargetInfo aoColor{};
        aoColor.texture = target.ao;
        aoColor.clear_color = {1,1,1,1};
        aoColor.load_op = SDL_GPU_LOADOP_CLEAR;
        aoColor.store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(cmd,&aoColor,1,nullptr);
        check(pass != nullptr,"Begin AO evaluation pass");
        SDL_BindGPUGraphicsPipeline(pass,aoPipeline_);
        const SDL_GPUTextureSamplerBinding bindings[2] = {
            {target.aoWorld,shadowSampler_},{target.aoNormal,shadowSampler_}};
        SDL_BindGPUFragmentSamplers(pass,0,bindings,2);
        const AoUniform options{{1.0f/target.width,1.0f/target.height,0.10f,2.0f}};
        SDL_PushGPUFragmentUniformData(cmd,0,&options,sizeof(options));
        SDL_DrawGPUPrimitives(pass,3,1,0,0);
        SDL_EndGPURenderPass(pass);
    }
    void drawModel(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass,
                   const ModelResource& model, float angle, float time, const Controls& controls,
                   const ShadowSetup& shadows, const RenderTarget& target) {
        const XMVECTOR eye = XMVectorSet(controls.camera.x,controls.camera.y,controls.camera.z,1);
        const XMMATRIX view = XMMatrixLookAtLH(eye,
            XMVectorSet(controls.target.x,controls.target.y,controls.target.z,1), XMVectorSet(0,1,0,0));
        const XMVECTOR forward = XMVector3Normalize(
            XMVectorSet(controls.target.x,controls.target.y,controls.target.z,1) - eye);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),forward));
        const XMVECTOR up = XMVector3Cross(forward,right);
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(controls.fov,
            float(target.width)/target.height, 0.1f, 40.0f);
        const XMMATRIX instance = cabinetInstance(angle,controls);
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, view * projection);
        const float averageLuma = 0.2126f * screenAverage_.x + 0.7152f * screenAverage_.y + 0.0722f * screenAverage_.z;
        const auto placeLight = [&](const EmissiveLightSource& source) {
            XMFLOAT3 position{}, direction{};
            XMStoreFloat3(&position,XMVector3TransformCoord(XMLoadFloat3(&source.position),instance));
            XMStoreFloat3(&direction,XMVector3Normalize(
                XMVector3TransformNormal(XMLoadFloat3(&source.direction),instance)));
            return std::pair{position,direction};
        };
        const auto [screenPosition,screenDirection] = placeLight(model.screenLight);
        const auto [marqueePosition,marqueeDirection] = placeLight(model.marqueeLight);
        const LightingUniform light{
            {controls.camera.x,controls.camera.y,controls.camera.z,time},
            {controls.keyPosition.x,controls.keyPosition.y,controls.keyPosition.z,controls.keyLight},
            {controls.fillPosition.x,controls.fillPosition.y,controls.fillPosition.z,controls.fillLight},
            {screenPosition.x,screenPosition.y,screenPosition.z,
             model.screenLight.present ? controls.screenLight * (0.65f + averageLuma * 2.0f) : 0},
            {screenAverage_.x,screenAverage_.y,screenAverage_.z,0},
            {screenDirection.x,screenDirection.y,screenDirection.z,0.42f},
            {marqueePosition.x,marqueePosition.y,marqueePosition.z,
             model.marqueeLight.present ? controls.marqueeLight : 0},
            {marqueeDirection.x,marqueeDirection.y,marqueeDirection.z,0.20f},
            {model.screenLight.receiverMinY,model.marqueeLight.receiverMinY,0,0},
            {controls.ambientLight,controls.matcapStrength,controls.environmentRotation,
             controls.specularAa ? 1.0f : 0.0f},
            {XMVectorGetX(right),XMVectorGetY(right),XMVectorGetZ(right),0},
            {XMVectorGetX(up),XMVectorGetY(up),XMVectorGetZ(up),0},
            {controls.multiscatter ? 1.0f : 0.0f,0,0,0},
            shadows.viewProjection,
            {controls.shadows ? 1.0f : 0.0f,1.0f/shadowSize_,
             1.0f/target.width,1.0f/target.height},
            {shadows.active[0] ? 1.0f : 0.0f,shadows.active[1] ? 1.0f : 0.0f,
             shadows.active[2] ? 1.0f : 0.0f,shadows.active[3] ? 1.0f : 0.0f}
        };
        SDL_PushGPUFragmentUniformData(cmd, 1, &light, sizeof(light));
        const SDL_GPUBufferBinding vertex{model.vertexBuffer,0};
        const SDL_GPUBufferBinding index{model.indexBuffer,0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
        SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        for (int alphaPass = 0; alphaPass < 2; ++alphaPass) {
            SDL_BindGPUGraphicsPipeline(pass, alphaPass ? blend_ : opaque_);
            for (const auto& primitive : model.primitives) {
                const auto& material = model.materials[primitive.material];
                if ((material.alphaMode == 2) != (alphaPass != 0)) continue;
                const XMMATRIX cabinet = XMLoadFloat4x4(&primitive.transform) *
                    XMLoadFloat4x4(&model.normalization);
                const XMMATRIX local = cabinet * instance;
                SceneUniform scene{};
                XMStoreFloat4x4(&scene.model, local);
                scene.viewProjection = vp;
                XMStoreFloat4x4(&scene.cabinetModel,cabinet);
                XMStoreFloat4x4(&scene.normalModel,XMMatrixTranspose(XMMatrixInverse(nullptr,local)));
                SDL_PushGPUVertexUniformData(cmd, 0, &scene, sizeof(scene));
                MaterialUniform properties{};
                // A video override owns the screen's color and emission. An
                // authored CRT material may have a black base factor and an
                // old still image in its emissive slot, so replace both.
                properties.base = material.screen ? XMFLOAT4{1,1,1,material.base.w} : material.base;
                properties.emissive = material.screen
                    ? XMFLOAT4{0,0,0,controls.screenEmissive}
                    : XMFLOAT4{material.emissive.x,material.emissive.y,material.emissive.z,
                               material.marquee ? controls.marqueeEmissive : material.emissiveStrength};
                properties.surface = {material.metallic,material.roughness,material.normalScale,material.alphaCutoff};
                properties.flags = {material.screen ? 1.0f : 0.0f,material.marquee ? 1.0f : 0.0f,
                                    float(material.alphaMode),material.transmission};
                properties.specular = {material.specularColor.x,material.specularColor.y,
                                       material.specularColor.z,material.specularFactor};
                properties.occlusion = {material.occlusionStrength,0,0,0};
                SDL_PushGPUFragmentUniformData(cmd, 0, &properties, sizeof(properties));
                const SDL_GPUTextureSamplerBinding bindings[14] = {
                    {material.screen ? screen_ : materialTexture(model,material.baseTexture,white_,true),sampler_},
                    {materialTexture(model,material.normalTexture,normal_,false),sampler_},
                    {materialTexture(model,material.mrTexture,neutralMr_,false),sampler_},
                    {material.screen ? black_ : materialTexture(model,material.emissiveTexture,black_,true),sampler_},
                    {environment_,environmentSampler_},
                    {matcap_,sampler_},
                    {irradiance_,environmentSampler_},
                    {dfg_,sampler_},
                    {materialTexture(model,material.occlusionTexture,white_,false),sampler_},
                    {target.shadowMaps[0],shadowSampler_},
                    {target.shadowMaps[1],shadowSampler_},
                    {target.shadowMaps[2],shadowSampler_},
                    {target.shadowMaps[3],shadowSampler_},
                    {controls.ssao ? target.ao : white_,postSampler_}
                };
                SDL_BindGPUFragmentSamplers(pass, 0, bindings, 14);
                SDL_DrawGPUIndexedPrimitives(pass, primitive.indexCount, 1, primitive.firstIndex, 0, 0);
            }
        }
    }
public:
    ModelRenderer(SDL_GPUDevice* gpu, SDL_Renderer* renderer, int requestedSamples, int anisotropy)
        : gpu_(gpu), renderer_(renderer) {
        for (const auto [count, value] : {
                 std::pair{8, SDL_GPU_SAMPLECOUNT_8}, std::pair{4, SDL_GPU_SAMPLECOUNT_4},
                 std::pair{2, SDL_GPU_SAMPLECOUNT_2}}) {
            if (count <= requestedSamples &&
                SDL_GPUTextureSupportsSampleCount(gpu_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, value) &&
                SDL_GPUTextureSupportsSampleCount(gpu_, depthFormat, value)) {
                sampleCount_ = value;
                sampleCountValue_ = count;
                break;
            }
        }
        const fs::path root = fs::path(SDL_GetBasePath()) / "shaders";
        pbrVertex_ = shader(root / "pbr.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
        pbrFragment_ = shader(root / "pbr.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 14, 2);
        shadowVertex_ = shader(root / "shadow.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
        shadowFragment_ = shader(root / "shadow.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        aoGeometryVertex_ = shader(root / "ao_geometry.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
        aoGeometryFragment_ = shader(root / "ao_geometry.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        aoFragment_ = shader(root / "ao.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        postVertex_ = shader(root / "post.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        postFragment_ = shader(root / "post.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        opaque_ = pbrPipeline(false);
        blend_ = pbrPipeline(true);
        shadowPipeline_ = shadowPipeline();
        aoGeometryPipeline_ = aoGeometryPipeline();
        aoPipeline_ = aoPipeline();
        hdrPost_ = postPipeline(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        ldrPost_ = postPipeline(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter = SDL_GPU_FILTER_LINEAR;
        info.mag_filter = SDL_GPU_FILTER_LINEAR;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.max_lod = 16;
        info.enable_anisotropy = anisotropy > 1;
        info.max_anisotropy = static_cast<float>(anisotropy);
        sampler_ = SDL_CreateGPUSampler(gpu_, &info);
        check(sampler_ != nullptr, "Create GPU sampler");
        info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        info.enable_anisotropy = false;
        environmentSampler_ = SDL_CreateGPUSampler(gpu_, &info);
        check(environmentSampler_ != nullptr, "Create environment sampler");
        info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.enable_anisotropy = false;
        postSampler_ = SDL_CreateGPUSampler(gpu_, &info);
        check(postSampler_ != nullptr, "Create postprocess sampler");
        info.min_filter = SDL_GPU_FILTER_NEAREST;
        info.mag_filter = SDL_GPU_FILTER_NEAREST;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        info.max_lod = 0;
        shadowSampler_ = SDL_CreateGPUSampler(gpu_,&info);
        check(shadowSampler_ != nullptr,"Create shadow sampler");
        white_ = solid(255,255,255,255);
        normal_ = solid(128,128,255,255);
        neutralMr_ = solid(255,255,255,255);
        black_ = solid(0,0,0,255);
        // An HDR studio probe: broad dark surroundings and a few bright,
        // differently shaped panels give polished metal something to reflect.
        // Longitude 0.25 faces the default camera (negative world Z).
        std::vector<ibl::Vec3> environment(128 * 64);
        const auto panel = [](float u, float v, float cx, float cy, float sx, float sy) {
            const float dx = std::min(std::abs(u-cx),1.0f-std::abs(u-cx))/sx;
            const float dy = (v-cy)/sy;
            return std::exp(-(dx*dx*dx*dx + dy*dy*dy*dy));
        };
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 128; ++x) {
            const float u = (x+0.5f)/128.0f, v = (y+0.5f)/64.0f;
            const float horizon = std::max(0.0f,1.0f-std::abs(v-0.48f)*1.4f);
            const float key = panel(u,v,0.19f,0.34f,0.055f,0.18f);
            const float fill = panel(u,v,0.39f,0.43f,0.033f,0.22f);
            const float rim = panel(u,v,0.73f,0.29f,0.055f,0.14f);
            const float ceiling = panel(u,v,0.50f,0.09f,0.20f,0.035f);
            environment[size_t(y)*128+x] = {
                0.018f+0.035f*horizon+6.0f*key+2.6f*fill+4.0f*rim+1.2f*ceiling,
                0.023f+0.041f*horizon+5.6f*key+3.0f*fill+2.9f*rim+1.2f*ceiling,
                0.035f+0.055f*horizon+4.9f*key+3.9f*fill+1.8f*rim+1.2f*ceiling};
        }
        const auto iblMaps = ibl::generate(environment,128,64);
        environment_ = texture(gpu_,128,64,SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
            SDL_GPU_TEXTUREUSAGE_SAMPLER,
            SDL_GPU_SAMPLECOUNT_1,mipLevels(128,64));
        irradiance_ = texture(gpu_,ibl::Maps::irradianceWidth,ibl::Maps::irradianceHeight,
            SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,SDL_GPU_TEXTUREUSAGE_SAMPLER);
        dfg_ = texture(gpu_,ibl::Maps::dfgSize,ibl::Maps::dfgSize,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,SDL_GPU_TEXTUREUSAGE_SAMPLER);
        auto* cmd = SDL_AcquireGPUCommandBuffer(gpu_);
        for (size_t mip=0; mip<iblMaps.specular.size(); ++mip)
            uploadTexture(gpu_,cmd,environment_,iblMaps.specular[mip].data(),
                std::max(1,128>>mip),std::max(1,64>>mip),static_cast<Uint32>(mip),16);
        uploadTexture(gpu_,cmd,irradiance_,iblMaps.irradiance.data(),
            ibl::Maps::irradianceWidth,ibl::Maps::irradianceHeight,0,16);
        uploadTexture(gpu_,cmd,dfg_,iblMaps.dfg.data(),ibl::Maps::dfgSize,ibl::Maps::dfgSize);
        std::vector<uint8_t> matcap(128 * 128 * 4);
        for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
            const float u = (x + 0.5f) / 64.0f - 1.0f;
            const float v = (y + 0.5f) / 64.0f - 1.0f;
            const auto softbox = [](float u, float v, float cx, float cy, float sx, float sy) {
                const float dx = (u-cx)/sx, dy = (v-cy)/sy;
                return std::exp(-(dx*dx*dx*dx + dy*dy*dy*dy));
            };
            const float key = softbox(u,v,-0.38f,-0.38f,0.24f,0.40f);
            const float fill = softbox(u,v,0.47f,-0.12f,0.16f,0.32f);
            const float rim = 0.10f * std::pow(std::min(1.0f,std::sqrt(u*u+v*v)),4.0f);
            const size_t p = (size_t(y)*128+x)*4;
            matcap[p] = static_cast<uint8_t>(255 * std::min(1.0f,key + 0.36f*fill + rim));
            matcap[p+1] = static_cast<uint8_t>(255 * std::min(1.0f,0.90f*key + 0.54f*fill + rim));
            matcap[p+2] = static_cast<uint8_t>(255 * std::min(1.0f,0.78f*key + 0.95f*fill + rim));
            matcap[p+3] = 255;
        }
        matcap_ = texture(gpu_,128,128,SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            SDL_GPU_SAMPLECOUNT_1,mipLevels(128,128));
        uploadTexture(gpu_,cmd,matcap_,matcap.data(),128,128);
        SDL_GenerateMipmapsForGPUTexture(cmd,matcap_);
        check(SDL_SubmitGPUCommandBuffer(cmd), "Submit lighting textures");
        screen_ = texture(gpu_,screenWidth_,screenHeight_,SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
                          SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                          SDL_GPU_SAMPLECOUNT_1,mipLevels(screenWidth_,screenHeight_));
        XMFLOAT3 average{};
        const auto pattern = animatedPattern(screenWidth_,screenHeight_,0,average);
        setScreen(nullptr,pattern,average);
    }
    ~ModelRenderer() {
        for (auto& target : targets_) destroyTarget(target);
        for (auto* image : {white_,normal_,neutralMr_,black_,environment_,matcap_,screen_,irradiance_,dfg_})
            if (image) SDL_ReleaseGPUTexture(gpu_,image);
        if (sampler_) SDL_ReleaseGPUSampler(gpu_,sampler_);
        if (environmentSampler_) SDL_ReleaseGPUSampler(gpu_,environmentSampler_);
        if (postSampler_) SDL_ReleaseGPUSampler(gpu_,postSampler_);
        if (shadowSampler_) SDL_ReleaseGPUSampler(gpu_,shadowSampler_);
        for (auto* pipeline : {opaque_,blend_,shadowPipeline_,aoGeometryPipeline_,aoPipeline_,
                               hdrPost_,ldrPost_})
            if (pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu_,pipeline);
        for (auto* shader : {pbrVertex_,pbrFragment_,shadowVertex_,shadowFragment_,
                             aoGeometryVertex_,aoGeometryFragment_,aoFragment_,
                             postVertex_,postFragment_})
            if (shader) SDL_ReleaseGPUShader(gpu_,shader);
    }
    void setScreen(SDL_GPUCommandBuffer* cmd, const std::vector<uint8_t>& pixels, XMFLOAT3 average) {
        if (pixels.size() != size_t(screenWidth_) * screenHeight_ * 4)
            throw std::runtime_error("Screen frame must be 640x480 RGBA");
        screenAverage_ = average;
        const bool own = cmd == nullptr;
        if (own) cmd = SDL_AcquireGPUCommandBuffer(gpu_);
        check(cmd != nullptr, "Acquire screen upload command buffer");
        uploadTexture(gpu_,cmd,screen_,pixels.data(),screenWidth_,screenHeight_);
        SDL_GenerateMipmapsForGPUTexture(cmd,screen_);
        if (own) check(SDL_SubmitGPUCommandBuffer(cmd), "Submit initial screen upload");
    }
    bool resizeTargets(int outputWidth, int outputHeight, bool single, float renderScale) {
        const float centerDisplayHeight = single
            ? std::min(outputHeight * 0.87f,outputWidth * 0.63f * 1.5f)
            : std::min(outputHeight * 0.86f,outputWidth * 0.56f * 1.5f);
        const auto targetHeight = [renderScale](float displayHeight) {
            return std::clamp(int(std::ceil(displayHeight * renderScale / 32.0f)) * 32,32,2048);
        };
        const int centerHeight = targetHeight(centerDisplayHeight);
        const int sideHeight = single ? 0 : targetHeight(centerDisplayHeight * 0.64f);
        const std::array<int,3> heights{sideHeight,centerHeight,sideHeight};
        bool changed = false;
        for (size_t i = 0; i < targets_.size(); ++i)
            changed |= targets_[i].height != heights[i];
        if (!changed) return false;
        bool hasExisting = false;
        for (const auto& target : targets_) hasExisting |= target.width != 0;
        if (hasExisting) check(SDL_WaitForGPUIdle(gpu_),"Wait before resizing cabinet targets");
        for (size_t i = 0; i < targets_.size(); ++i) {
            if (targets_[i].height == heights[i]) continue;
            destroyTarget(targets_[i]);
            if (heights[i] > 0)
                targets_[i] = createTarget(int(std::lround(heights[i] * (2.0 / 3.0))),heights[i]);
        }
        std::cout << "Cabinet targets: center " << targets_[1].width << 'x' << targets_[1].height;
        if (!single) std::cout << ", sides " << targets_[0].width << 'x' << targets_[0].height;
        std::cout << '\n';
        return true;
    }
    size_t targetPixels() const {
        size_t pixels = 0;
        for (const auto& target : targets_) pixels += size_t(target.width) * target.height;
        return pixels;
    }
    void render(SDL_GPUCommandBuffer* cmd, const ModelResource& model, float time, const Controls& controls) {
        for (size_t i = 0; i < targets_.size(); ++i) {
            auto& target = targets_[i];
            if (!target.width) continue;
            const float angle = i == 0 ? -0.45f : i == 2 ? 0.45f : 0.15f * std::sin(time * 0.7f);
            const ShadowSetup shadows = shadowSetup(model,angle,controls);
            if (controls.shadows) renderShadowMaps(cmd,model,angle,controls,shadows,target);
            if (controls.ssao) renderAo(cmd,model,angle,controls,target);
            SDL_GPUColorTargetInfo color{};
            color.texture = target.msaaColor ? target.msaaColor : target.hdr;
            color.clear_color = {0,0,0,0};
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = target.msaaColor ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE;
            color.resolve_texture = target.msaaColor ? target.hdr : nullptr;
            SDL_GPUDepthStencilTargetInfo depth{};
            depth.texture = target.depth;
            depth.clear_depth = 1;
            depth.load_op = SDL_GPU_LOADOP_CLEAR;
            depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
            auto* pass = SDL_BeginGPURenderPass(cmd,&color,1,&depth);
            check(pass != nullptr, "Begin cabinet HDR render pass");
            drawModel(cmd,pass,model,angle,time,controls,shadows,target);
            SDL_EndGPURenderPass(pass);
            post(cmd,target.hdr,target.hdr,target.bright,hdrPost_,0,target.width,target.height);
            post(cmd,target.bright,target.bright,target.blurA,hdrPost_,1,target.width/2,target.height/2);
            post(cmd,target.blurA,target.blurA,target.blurB,hdrPost_,2,target.width/2,target.height/2);
            post(cmd,target.hdr,target.blurB,target.final,ldrPost_,3,target.width,target.height,
                 controls.exposure,controls.filmicTonemap);
        }
    }
    SDL_Texture* output(size_t index) const { return targets_.at(index).wrapped; }
    int sampleCount() const { return sampleCountValue_; }
};

struct Options {
    fs::path model, video, screenshot;
    std::string screenMaterial = "retrofe.screen";
    double duration = 0;
    Controls controls;
    bool single = false;
    bool uncapped = false;
    bool checkerBackground = false;
    int msaa = 4;
    int anisotropy = 8;
    float renderScale = 1.2f;
    int windowWidth = 1280, windowHeight = 720;
    std::vector<std::pair<std::string,float>> metallicOverrides, roughnessOverrides;
};
static std::pair<std::string,float> parseMaterialOverride(const std::string& value) {
    const size_t separator = value.rfind('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 == value.size())
        throw std::runtime_error("Material override must be name=value");
    const std::string number = value.substr(separator + 1);
    size_t consumed = 0;
    const float amount = std::stof(number,&consumed);
    if (consumed != number.size() || !std::isfinite(amount) || amount < 0 || amount > 1)
        throw std::runtime_error("Material override value must be between 0 and 1");
    return {value.substr(0,separator),amount};
}
static Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) options.model = argv[++i];
        else if (arg == "--material-metallic" && i + 1 < argc)
            options.metallicOverrides.push_back(parseMaterialOverride(argv[++i]));
        else if (arg == "--material-roughness" && i + 1 < argc)
            options.roughnessOverrides.push_back(parseMaterialOverride(argv[++i]));
        else if (arg == "--screen-material" && i + 1 < argc) options.screenMaterial = argv[++i];
        else if (arg == "--video" && i + 1 < argc) options.video = argv[++i];
        else if (arg == "--screenshot" && i + 1 < argc) options.screenshot = argv[++i];
        else if (arg == "--duration" && i + 1 < argc) options.duration = std::stod(argv[++i]);
        else if (arg == "--fov" && i + 1 < argc) options.controls.fov = XMConvertToRadians(std::stof(argv[++i]));
        else if (arg == "--rotate-x" && i + 1 < argc) options.controls.rotateX = XMConvertToRadians(std::stof(argv[++i]));
        else if (arg == "--rotate-y" && i + 1 < argc) options.controls.rotateY = XMConvertToRadians(std::stof(argv[++i]));
        else if (arg == "--rotate-z" && i + 1 < argc) options.controls.rotateZ = XMConvertToRadians(std::stof(argv[++i]));
        else if (arg == "--scale" && i + 1 < argc) options.controls.scale = std::stof(argv[++i]);
        else if (arg == "--camera-x" && i + 1 < argc) options.controls.camera.x = std::stof(argv[++i]);
        else if (arg == "--camera-y" && i + 1 < argc) options.controls.camera.y = std::stof(argv[++i]);
        else if (arg == "--camera-z" && i + 1 < argc) options.controls.camera.z = std::stof(argv[++i]);
        else if (arg == "--target-x" && i + 1 < argc) options.controls.target.x = std::stof(argv[++i]);
        else if (arg == "--target-y" && i + 1 < argc) options.controls.target.y = std::stof(argv[++i]);
        else if (arg == "--target-z" && i + 1 < argc) options.controls.target.z = std::stof(argv[++i]);
        else if (arg == "--screen-emissive" && i + 1 < argc) options.controls.screenEmissive = std::stof(argv[++i]);
        else if (arg == "--marquee-emissive" && i + 1 < argc) options.controls.marqueeEmissive = std::stof(argv[++i]);
        else if (arg == "--key-x" && i + 1 < argc) options.controls.keyPosition.x = std::stof(argv[++i]);
        else if (arg == "--key-y" && i + 1 < argc) options.controls.keyPosition.y = std::stof(argv[++i]);
        else if (arg == "--key-z" && i + 1 < argc) options.controls.keyPosition.z = std::stof(argv[++i]);
        else if (arg == "--fill-x" && i + 1 < argc) options.controls.fillPosition.x = std::stof(argv[++i]);
        else if (arg == "--fill-y" && i + 1 < argc) options.controls.fillPosition.y = std::stof(argv[++i]);
        else if (arg == "--fill-z" && i + 1 < argc) options.controls.fillPosition.z = std::stof(argv[++i]);
        else if (arg == "--key-light" && i + 1 < argc) options.controls.keyLight = std::stof(argv[++i]);
        else if (arg == "--fill-light" && i + 1 < argc) options.controls.fillLight = std::stof(argv[++i]);
        else if (arg == "--ambient-light" && i + 1 < argc) options.controls.ambientLight = std::stof(argv[++i]);
        else if (arg == "--environment-intensity" && i + 1 < argc)
            options.controls.ambientLight = std::stof(argv[++i]);
        else if (arg == "--environment-rotation" && i + 1 < argc)
            options.controls.environmentRotation = XMConvertToRadians(std::stof(argv[++i]));
        else if (arg == "--no-specular-aa") options.controls.specularAa = false;
        else if (arg == "--no-multiscatter") options.controls.multiscatter = false;
        else if (arg == "--no-shadows") options.controls.shadows = false;
        else if (arg == "--no-ssao") options.controls.ssao = false;
        else if (arg == "--screen-light" && i + 1 < argc) options.controls.screenLight = std::stof(argv[++i]);
        else if (arg == "--marquee-light" && i + 1 < argc) options.controls.marqueeLight = std::stof(argv[++i]);
        else if (arg == "--matcap-strength" && i + 1 < argc) options.controls.matcapStrength = std::stof(argv[++i]);
        else if (arg == "--exposure" && i + 1 < argc) options.controls.exposure = std::stof(argv[++i]);
        else if (arg == "--tonemap" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode != "reinhard" && mode != "filmic")
                throw std::runtime_error("--tonemap must be reinhard or filmic");
            options.controls.filmicTonemap = mode == "filmic";
        }
        else if (arg == "--single") options.single = true;
        else if (arg == "--uncapped") options.uncapped = true;
        else if (arg == "--checker-background") options.checkerBackground = true;
        else if (arg == "--msaa" && i + 1 < argc) options.msaa = std::stoi(argv[++i]);
        else if (arg == "--anisotropy" && i + 1 < argc) options.anisotropy = std::stoi(argv[++i]);
        else if (arg == "--render-scale" && i + 1 < argc) options.renderScale = std::stof(argv[++i]);
        else if (arg == "--window-width" && i + 1 < argc) options.windowWidth = std::stoi(argv[++i]);
        else if (arg == "--window-height" && i + 1 < argc) options.windowHeight = std::stoi(argv[++i]);
        else if (arg == "--help") {
            std::cout << "retrofe_sdl_gpu_model [--model cabinet.glb] [--screen-material name] [--video h264.mp4] "
                "[--single] [--fov degrees] [--duration seconds] [--screenshot image.bmp] "
                "[--uncapped] [--checker-background] [--msaa 1|2|4|8] [--anisotropy 1|2|4|8|16] "
                "[--render-scale 0.5..2] [--window-width pixels] [--window-height pixels] "
                "[--rotate-x/y/z degrees] [--scale factor] "
                "[--camera-x/y/z value] [--target-x/y/z value] "
                "[--screen-emissive value] [--marquee-emissive value] "
                "[--key-x/y/z value] [--fill-x/y/z value] "
                "[--key-light value] [--fill-light value] [--ambient-light value] "
                "[--environment-intensity value] [--environment-rotation degrees] "
                "[--no-specular-aa] [--no-multiscatter] [--no-shadows] [--no-ssao] "
                "[--material-metallic name=0..1] [--material-roughness name=0..1] "
                "[--screen-light value] [--marquee-light value] [--matcap-strength value] "
                "[--tonemap reinhard|filmic] [--exposure value]\n";
            std::exit(0);
        } else throw std::runtime_error("Unknown or incomplete option: " + arg);
    }
    if (options.controls.fov < XMConvertToRadians(15) || options.controls.fov > XMConvertToRadians(100))
        throw std::runtime_error("--fov must be between 15 and 100 degrees");
    if (options.controls.scale <= 0) throw std::runtime_error("--scale must be positive");
    if (!std::isfinite(options.controls.environmentRotation))
        throw std::runtime_error("--environment-rotation must be finite");
    for (const auto [name, intensity] : std::array<std::pair<const char*,float>,6>{
             {{"--key-light",options.controls.keyLight}, {"--fill-light",options.controls.fillLight},
              {"--ambient-light",options.controls.ambientLight}, {"--screen-light",options.controls.screenLight},
              {"--marquee-light",options.controls.marqueeLight}, {"--matcap-strength",options.controls.matcapStrength}}})
        if (!std::isfinite(intensity) || intensity < 0)
            throw std::runtime_error(std::string(name) + " must be finite and nonnegative");
    if (options.msaa != 1 && options.msaa != 2 && options.msaa != 4 && options.msaa != 8)
        throw std::runtime_error("--msaa must be 1, 2, 4 or 8");
    if (options.anisotropy != 1 && options.anisotropy != 2 && options.anisotropy != 4 &&
        options.anisotropy != 8 && options.anisotropy != 16)
        throw std::runtime_error("--anisotropy must be 1, 2, 4, 8 or 16");
    if (!std::isfinite(options.renderScale) || options.renderScale < 0.5f || options.renderScale > 2.0f)
        throw std::runtime_error("--render-scale must be between 0.5 and 2");
    if (!std::isfinite(options.controls.exposure) || options.controls.exposure <= 0 ||
        options.controls.exposure > 16)
        throw std::runtime_error("--exposure must be greater than 0 and no more than 16");
    if (options.windowWidth < 320 || options.windowWidth > 3840 ||
        options.windowHeight < 240 || options.windowHeight > 2160)
        throw std::runtime_error("Window dimensions must be between 320x240 and 3840x2160");
    return options;
}
struct Application {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* gpu = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<ModelRenderer> modelRenderer;
    std::unique_ptr<ModelResource> model;
    std::unique_ptr<VideoSource> video;
    ~Application() {
        video.reset();
        model.reset();
        modelRenderer.reset();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (gpu) SDL_DestroyGPUDevice(gpu);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};
static double secondsNow() { return SDL_GetTicksNS() / 1e9; }
static uint64_t processCpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) return 0;
    const auto ticks = [](FILETIME time) {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}
static void compose(SDL_Renderer* renderer, const ModelRenderer& models,
                    bool single, bool checkerBackground) {
    int width = 0, height = 0;
    check(SDL_GetRenderOutputSize(renderer,&width,&height), "SDL_GetRenderOutputSize");
    check(SDL_SetRenderDrawColor(renderer,12,16,28,255), "Set background color");
    check(SDL_RenderClear(renderer), "Clear 2D renderer");
    if (checkerBackground) {
        constexpr int tile = 64;
        for (int y = 0; y < height; y += tile) for (int x = 0; x < width; x += tile) {
            const bool light = ((x / tile + y / tile) & 1) == 0;
            check(SDL_SetRenderDrawColor(renderer, light ? 228 : 54,
                                         light ? 234 : 83, light ? 240 : 142, 255),
                  "Set checker background color");
            const SDL_FRect rect{float(x),float(y),float(tile),float(tile)};
            check(SDL_RenderFillRect(renderer,&rect), "Draw checker background");
        }
    } else {
        const SDL_FRect stripe{0,float(height)*0.79f,float(width),float(height)*0.21f};
        check(SDL_SetRenderDrawColor(renderer,23,30,45,255), "Set stripe color");
        check(SDL_RenderFillRect(renderer,&stripe), "Render 2D stripe");
    }
    if (single) {
        const float h = std::min(float(height)*0.87f,float(width)*0.63f*1.5f);
        const SDL_FRect dst{(width-h*2/3)/2,(height-h)/2,h*2/3,h};
        check(SDL_RenderTexture(renderer,models.output(1),nullptr,&dst), "Compose central cabinet");
    } else {
        const float centerH = std::min(float(height)*0.86f,float(width)*0.56f*1.5f);
        const float centerW = centerH * 2/3;
        const float sideH = centerH * 0.64f;
        const float sideW = sideH * 2/3;
        const float centerX = (width-centerW)/2;
        const float centerY = (height-centerH)/2;
        const SDL_FRect left{centerX-sideW*0.9f,centerY+(centerH-sideH)/2,sideW,sideH};
        const SDL_FRect center{centerX,centerY,centerW,centerH};
        const SDL_FRect right{centerX+centerW-sideW*0.1f,centerY+(centerH-sideH)/2,sideW,sideH};
        check(SDL_RenderTexture(renderer,models.output(0),nullptr,&left), "Compose left cabinet");
        check(SDL_RenderTexture(renderer,models.output(2),nullptr,&right), "Compose right cabinet");
        check(SDL_RenderTexture(renderer,models.output(1),nullptr,&center), "Compose center cabinet");
    }
    if (!checkerBackground) {
        const SDL_FRect accent{float(width)*0.08f,float(height)*0.08f,float(width)*0.84f,3};
        check(SDL_SetRenderDrawColor(renderer,120,156,195,255), "Set accent color");
        check(SDL_RenderFillRect(renderer,&accent), "Render 2D accent");
    }
}
static int run(const Options& options) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL_Init");
    Application app;
    app.window = SDL_CreateWindow("RetroFE SDL_GPU Model Test",options.windowWidth,
                                  options.windowHeight,SDL_WINDOW_RESIZABLE);
    check(app.window != nullptr, "SDL_CreateWindow");
    app.gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL,false,nullptr);
    check(app.gpu != nullptr, "SDL_CreateGPUDevice(DXIL)");
    app.renderer = SDL_CreateGPURenderer(app.gpu,app.window);
    check(app.renderer != nullptr, "SDL_CreateGPURenderer(shared device)");
    check(SDL_GetGPURendererDevice(app.renderer) == app.gpu, "GPU renderer uses different device");
    check(SDL_SetRenderVSync(app.renderer,options.uncapped ? 0 : 1), "Set render VSync");
    std::cout << "SDL " << SDL_GetVersion() << ", GPU driver " << SDL_GetGPUDeviceDriver(app.gpu)
              << ", shared device " << app.gpu << '\n';
    app.modelRenderer = std::make_unique<ModelRenderer>(app.gpu,app.renderer,options.msaa,
                                                         options.anisotropy);
    int outputWidth = 0, outputHeight = 0;
    check(SDL_GetRenderOutputSize(app.renderer,&outputWidth,&outputHeight),
          "Get initial renderer output size");
    app.modelRenderer->resizeTargets(outputWidth,outputHeight,options.single,options.renderScale);
    std::cout << "Cabinet MSAA: " << app.modelRenderer->sampleCount() << "x"
              << (app.modelRenderer->sampleCount() < options.msaa ? " (device fallback)" : "") << '\n';
    std::cout << "Depth: D32_FLOAT; render scale: " << options.renderScale << "x\n";
    std::cout << "Texture filtering: trilinear mipmaps, " << options.anisotropy << "x anisotropy\n";
    app.model = options.model.empty() ? proceduralCabinet(app.gpu) :
        loadGlb(app.gpu,options.model,options.screenMaterial);
    const auto applyOverrides = [&](const std::vector<std::pair<std::string,float>>& overrides,
                                    bool metallic) {
        for (const auto& [name,value] : overrides) {
            bool found = false;
            for (auto& material : app.model->materials) if (material.name == name) {
                (metallic ? material.metallic : material.roughness) = value;
                found = true;
            }
            if (!found) throw std::runtime_error("No material named '" + name + "'");
            std::cout << "Material " << name << ' ' << (metallic ? "metallic" : "roughness")
                      << " = " << value << '\n';
        }
    };
    applyOverrides(options.metallicOverrides,true);
    applyOverrides(options.roughnessOverrides,false);
    std::cout << "Model: " << app.model->vertices.size() << " vertices, "
              << app.model->primitives.size() << " primitives, "
              << app.model->materials.size() << " materials, "
              << app.model->images.size() << " images\n";
    size_t imageBytes = 0;
    for (const auto& image : app.model->images) imageBytes += mipBytes(image.width,image.height);
    const int samples = app.modelRenderer->sampleCount();
    const size_t targetBytesPerPixel = 22 + (samples > 1 ? 8*samples + 4*(samples-1) : 0);
    const size_t sharedBytes = mipBytes(640,480) +
        mipBytes(128,64) + mipBytes(128,128) + imageBytes +
        app.model->vertices.size()*sizeof(Vertex) + app.model->indices.size()*sizeof(uint32_t);
    const auto reportMemory = [&] {
        const size_t shadowBytes = size_t(options.single ? 1 : 3) * 4 * 512 * 512 * 4;
        const size_t approximateBytes = app.modelRenderer->targetPixels() *
                                        (targetBytesPerPixel + 8 + 8 + 4 + 4) +
                                        sharedBytes + shadowBytes;
        std::cout << "Approximate GPU resource payload: " << approximateBytes / (1024.0*1024.0)
                  << " MiB (excludes driver allocation overhead)\n";
    };
    reportMemory();
    if (!options.video.empty()) {
        gst_init(nullptr,nullptr);
        app.video = std::make_unique<VideoSource>(options.video,640,480);
        std::cout << "Software H.264 decoder: avdec_h264, output 640x480 RGBA\n";
    }
    const double start = secondsNow();
    double lastReport = start, uploadMillis = 0;
    uint64_t lastCpu = processCpuTicks();
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    uint64_t frames = 0, uploads = 0;
    bool running = true, screenshotSaved = false;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) running = false;
        }
        if (!running) break;
        const double now = secondsNow(), elapsed = now - start;
        check(SDL_FlushRenderer(app.renderer), "Flush SDL renderer before custom GPU work");
        check(SDL_GetRenderOutputSize(app.renderer,&outputWidth,&outputHeight),
              "Get renderer output size");
        if (app.modelRenderer->resizeTargets(outputWidth,outputHeight,options.single,options.renderScale))
            reportMemory();
        auto* cmd = SDL_AcquireGPUCommandBuffer(app.gpu);
        check(cmd != nullptr, "Acquire frame command buffer");
        XMFLOAT3 average{};
        const std::vector<uint8_t>* frame = nullptr;
        std::vector<uint8_t> pattern;
        if (app.video) {
            if (app.video->update(640,480)) frame = &app.video->frame();
        } else {
            pattern = animatedPattern(640,480,elapsed,average);
            frame = &pattern;
        }
        if (frame) {
            if (app.video) average = app.video->average();
            const auto before = std::chrono::steady_clock::now();
            app.modelRenderer->setScreen(cmd,*frame,average);
            uploadMillis += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            ++uploads;
        }
        app.modelRenderer->render(cmd,*app.model,float(elapsed),options.controls);
        check(SDL_SubmitGPUCommandBuffer(cmd), "Submit model rendering");
        compose(app.renderer,*app.modelRenderer,options.single,options.checkerBackground);
        if (!options.screenshot.empty() && options.duration > 0 &&
            elapsed >= options.duration && !screenshotSaved) {
            SDL_Surface* screenshot = SDL_RenderReadPixels(app.renderer,nullptr);
            check(screenshot != nullptr, "Read composited screenshot");
            const auto filename = options.screenshot.string();
            check(SDL_SaveBMP(screenshot,filename.c_str()), "Save screenshot");
            SDL_DestroySurface(screenshot);
            screenshotSaved = true;
        }
        check(SDL_RenderPresent(app.renderer), "SDL_RenderPresent");
        if (!options.uncapped) {
            const double remaining = 1.0 / 60.0 - (secondsNow() - now);
            if (remaining > 0) SDL_DelayPrecise(static_cast<Uint64>(remaining * 1e9));
        }
        ++frames;
        if (now - lastReport >= 5.0 || (options.duration > 0 && elapsed >= options.duration)) {
            const double interval = now - lastReport;
            const auto cpu = processCpuTicks();
            const double cpuPercent = 100.0 * double(cpu - lastCpu) /
                (1e7 * interval * std::max<DWORD>(1,systemInfo.dwNumberOfProcessors));
            std::cout << "FPS " << frames / interval << ", uploads " << uploads
                      << ", upload CPU ms " << (uploads ? uploadMillis/uploads : 0)
                      << ", process CPU " << cpuPercent << "%"
                      << ", decoded " << (app.video ? app.video->decodedFrames() : 0) << '\n';
            lastReport = now; lastCpu = cpu; frames = uploads = 0; uploadMillis = 0;
        }
        if (options.duration > 0 && elapsed >= options.duration) running = false;
    }
    check(SDL_WaitForGPUIdle(app.gpu), "Wait for GPU idle at shutdown");
    return 0;
}
int main(int argc, char** argv) {
    try { return run(parseOptions(argc,argv)); }
    catch (const std::exception& error) { std::cerr << "Model test failed: " << error.what() << '\n'; return 1; }
}

#ifndef RETROFE_IMAGE_H
#define RETROFE_IMAGE_H

#include "Component.h"
#include "../../Utility/ThreadPool.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <future>
#include <memory>
#include <limits>
#include <SDL3_image/SDL_image.h>

struct SDL_Texture;
struct SDL_Surface;
struct SDL_IOStream;

class Image : public Component {
public:
    Image(const std::string& file, const std::string& altFile, Page& p,
        int monitor = 0, bool additive = false, bool useTextureCaching = true);
    ~Image() override;
    bool update(float dt) override;
    void draw() override;
    void allocateGraphicsMemory() override;
    void freeGraphicsMemory() override;
    void pumpGraphicsPreparation() override;
    bool isGraphicsReadyForFirstRender() const override;
    std::string_view filePath();

    static void cleanupTextureCache();

    bool recycleAsImage(const std::string& newFilePath, const std::string& newAltPath = "") override;


private:
    enum class LoadStatus { Unloaded, Loading, Ready, Error };

    struct SurfaceDeleter {
        void operator()(SDL_Surface* s) const { if (s) SDL_DestroySurface(s); }
    };
    using SharedSurface = std::shared_ptr<SDL_Surface>;

    struct AsyncLoadResult {
        SharedSurface staticSurface;
        std::vector<SharedSurface> animatedSurfaces;
        std::vector<int> frameDelays;
        int w = 0, h = 0;
        bool success = false;
    };

    struct AsyncLoadTask {
        std::shared_future<AsyncLoadResult> future;
    };

    struct CachedImage {
        SDL_Texture* texture = nullptr;
        std::vector<SharedSurface> animatedSurfaces;
        std::vector<int> frameDelays;
        int w = 0;
        int h = 0;
    };

    struct PathCache {
        struct CacheKey {
            std::string_view fullPath;
            int monitor;
            bool operator==(const CacheKey& other) const {
                return monitor == other.monitor && fullPath == other.fullPath;
            }
        };
        struct CacheKeyHash {
            size_t operator()(const CacheKey& k) const {
                return std::hash<std::string_view>{}(k.fullPath) ^ (std::hash<int>{}(k.monitor) << 1);
            }
        };
        std::unordered_set<std::string> fullPaths_;
        CacheKey getKey(const std::string& filePath, int monitor);
    };

    // Internal Logic
    bool startAsyncLoad(const std::string& path);
    void finalizeLoad();
    bool loadFromCache(const std::string& filePath);
    bool applyCachedImage(const CachedImage& cached);
    void releaseLocalImageAssets();
    void releaseLoadTask();
    static void pruneExpiredLoadTask(const std::string& path);

    void resetAnimationState();
    bool createAnimatedStreamingTexture(int width, int height);
    void primeAnimatedTextureIfNeeded();
    static void ensureCacheReserved();

    std::string file_;
    std::string altFile_;
    std::string currentLoadingPath_; // NEW: Tracks which file is in-flight

    LoadStatus status_ = LoadStatus::Unloaded;
    std::shared_ptr<AsyncLoadTask> loadTask_;

    SDL_Texture* texture_ = nullptr;
    SDL_Texture* animatedTexture_ = nullptr;
    std::vector<SharedSurface> animatedSurfaces_;
    std::vector<int> frameDelays_;

    size_t currentFrame_ = 0;
    Uint64 animationStartTime_ = 0;
    size_t lastRenderedFrame_ = std::numeric_limits<size_t>::max();

    bool useTextureCaching_;
    bool isUsingCachedStaticTexture_ = false;
    bool isUsingCachedSurfaces_ = false;

    static PathCache pathCache_;
    static std::unordered_map<PathCache::CacheKey, CachedImage, PathCache::CacheKeyHash> textureCache_;
    static std::unordered_map<std::string, std::weak_ptr<AsyncLoadTask>> loadingTasks_;
};

#endif

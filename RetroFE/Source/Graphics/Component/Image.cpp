#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <SDL3/SDL_asyncio.h>

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::unordered_map<std::string, std::weak_ptr<Image::AsyncLoadTask>> Image::loadingTasks_;

static std::mutex g_ImageTextureCacheMutex;
static std::mutex g_ImageLoadTaskMutex;

// Own the complete read/decode pipeline. Pending requests retain only metadata;
// at most three files can be reading or decoding at once. All Image/renderer
// operations, including startup and shutdown, remain on the main thread.
struct Image::AsyncIOState {
    struct Request {
        std::string path;
        std::shared_ptr<std::promise<AsyncLoadResult>> promise;
        std::weak_ptr<AsyncLoadTask> consumer;
    };

    SDL_AsyncIOQueue* queue = nullptr;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Request>> pending;
    std::unordered_map<void*, std::shared_ptr<Request>> reading;
    size_t inFlight = 0;
    bool stopping = false;
    static constexpr size_t maxInFlight = 3;

    void finish(const std::shared_ptr<Request>& request, AsyncLoadResult result = {}) {
        request->promise->set_value(std::move(result));
        std::lock_guard<std::mutex> lock(mutex);
        --inFlight;
        changed.notify_all();
        if (queue) SDL_SignalAsyncIOQueue(queue);
    }

    void decode(const std::shared_ptr<Request>& request, void* buffer = nullptr, Uint64 bytes = 0) {
        // Retain file memory with the job, including enqueue failure paths.
        auto data = std::shared_ptr<void>(buffer, SDL_free);
        try {
            (void)ThreadPool::getInstance().enqueue([this, request, data, bytes] {
                AsyncLoadResult result;
                bool abandoned;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    abandoned = stopping || request->consumer.expired();
                }
                if (!abandoned) {
                    SDL_IOStream* stream = data
                        ? SDL_IOFromConstMem(data.get(), static_cast<size_t>(bytes))
                        : SDL_IOFromFile(request->path.c_str(), "rb");
                    result = Image::decodeImage(stream);
                }
                finish(request, std::move(result));
            });
        } catch (...) {
            finish(request);
        }
    }

    void run() {
        for (;;) {
            std::shared_ptr<Request> next;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (stopping) {
                    for (auto& request : pending) request->promise->set_value({});
                    pending.clear();
                }
                if (pending.empty() && inFlight == 0) {
                    changed.notify_all();
                    if (stopping) return;
                    changed.wait(lock, [this] { return stopping || !pending.empty(); });
                    continue;
                }
                if (!pending.empty() && inFlight < maxInFlight) {
                    next = std::move(pending.front());
                    pending.pop_front();
                    ++inFlight;
                }
            }
            if (next) {
                if (next->consumer.expired()) {
                    finish(next);
                } else if (!queue) {
                    decode(next); // Portable fallback if queue creation failed.
                } else {
                    reading.emplace(next.get(), next);
                    if (!SDL_LoadFileAsync(next->path.c_str(), queue, next.get())) {
                        reading.erase(next.get());
                        decode(next); // Submission failure need not discard valid artwork.
                    }
                }
                continue;
            }
            if (queue) {
                SDL_AsyncIOOutcome outcome{};
                if (SDL_WaitAsyncIOResult(queue, &outcome, 50)) {
                    auto it = reading.find(outcome.userdata);
                    if (it != reading.end()) {
                        auto request = std::move(it->second);
                        reading.erase(it);
                        if (outcome.result == SDL_ASYNCIO_COMPLETE &&
                            outcome.buffer && outcome.bytes_transferred > 0) {
                            decode(request, outcome.buffer, outcome.bytes_transferred);
                        } else {
                            SDL_free(outcome.buffer);
                            finish(request);
                        }
                    } else {
                        SDL_free(outcome.buffer);
                    }
                }
            } else {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return inFlight == 0 || (!pending.empty() && inFlight < maxInFlight); });
            }
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            changed.notify_all();
            if (queue) SDL_SignalAsyncIOQueue(queue);
        }
        // The worker exits only after reads AND dispatched decodes finish.
        if (worker.joinable()) worker.join();
        if (queue) SDL_DestroyAsyncIOQueue(queue);
        queue = nullptr;
    }
    ~AsyncIOState() { stop(); }
};

Image::AsyncIOState& Image::asyncIOState() {
    // Ensure the pool outlives this service during static destruction.
    (void)ThreadPool::getInstance();
    static AsyncIOState state;
    return state;
}

Image::AsyncLoadResult Image::decodeImage(SDL_IOStream* stream) {
    AsyncLoadResult result;
    if (!stream) return result;
    std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> input(stream, SDL_CloseIO);
    try {
        if (IMG_isGIF(stream) || IMG_isWEBP(stream)) {
            std::unique_ptr<IMG_Animation, decltype(&IMG_FreeAnimation)> animation(
                IMG_LoadAnimation_IO(stream, false), IMG_FreeAnimation);
            if (!animation) return result;
            result.w = animation->w;
            result.h = animation->h;
            for (int i = 0; i < animation->count; ++i) {
                SharedSurface frame(SDL_ConvertSurface(animation->frames[i], SDL_PIXELFORMAT_RGBA32), SurfaceDeleter());
                if (!frame) return {}; // Never silently drop animation frames.
                result.animatedSurfaces.push_back(std::move(frame));
                result.frameDelays.push_back(animation->delays && animation->delays[i] > 0
                    ? animation->delays[i] : 100);
            }
            result.success = !result.animatedSurfaces.empty();
        } else {
            result.staticSurface = SharedSurface(IMG_Load_IO(stream, false), SurfaceDeleter());
            if (result.staticSurface) {
                result.w = result.staticSurface->w;
                result.h = result.staticSurface->h;
                result.success = true;
            }
        }
    } catch (...) {
        return {};
    }
    return result;
}

void Image::ensureAsyncIO() {
    auto& state = asyncIOState();
    if (!state.worker.joinable()) {
        state.stopping = false;
        state.queue = SDL_CreateAsyncIOQueue();
        try {
            state.worker = std::thread([&state] { state.run(); });
        } catch (...) {
            if (state.queue) SDL_DestroyAsyncIOQueue(state.queue);
            state.queue = nullptr;
            throw;
        }
    }
}

void Image::waitForAsyncLoads() {
    auto& state = asyncIOState();
    std::unique_lock<std::mutex> lock(state.mutex);
    state.changed.wait(lock, [&state] { return state.pending.empty() && state.inFlight == 0; });
}

void Image::shutdownAsyncIO() {
    asyncIOState().stop();
}

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
	// Callers protect path interning with g_ImageTextureCacheMutex.
	const auto& interned = *fullPaths_.emplace(filePath).first;
	return { interned, monitor };
}

void Image::ensureCacheReserved() {
	static std::once_flag reserveOnce;
	std::call_once(reserveOnce, []() {
		{
			std::lock_guard<std::mutex> lock(g_ImageTextureCacheMutex);
			textureCache_.reserve(4096);
		}
		{
			std::lock_guard<std::mutex> lock(g_ImageLoadTaskMutex);
			loadingTasks_.reserve(512);
		}
	});
}

// -------------------- Lifecycle --------------------
Image::Image(const std::string& file, const std::string& altFile, Page& p, int monitor, bool additive, bool useTextureCaching)
	: Component(p), file_(file), altFile_(altFile), useTextureCaching_(useTextureCaching) {
	baseViewInfo.Monitor = monitor;
	baseViewInfo.Additive = additive;
	baseViewInfo.Layout = page.getCurrentLayout();
}

Image::~Image() {
	freeGraphicsMemory();
}

// -------------------- Async Logic --------------------
void Image::allocateGraphicsMemory() {
	// If we are already loading or ready, don't restart unless recycleAsImage set us to Unloaded
	if (status_ != LoadStatus::Unloaded) return;
	ensureCacheReserved();

	if (!file_.empty() && startAsyncLoad(file_)) return;
	if (!altFile_.empty() && startAsyncLoad(altFile_)) return;

	status_ = LoadStatus::Error;
}

void Image::pumpGraphicsPreparation() {
	if (status_ == LoadStatus::Loading) {
		if (loadTask_ &&
			loadTask_->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			finalizeLoad();
		}
	}
}

bool Image::isGraphicsReadyForFirstRender() const {
	if (status_ == LoadStatus::Error) {
		return true;
	}

	if (status_ == LoadStatus::Ready) {
		return texture_ || animatedTexture_ || isUsingCachedStaticTexture_ || isUsingCachedSurfaces_;
	}

	return false;
}

bool Image::startAsyncLoad(const std::string& path) {
	// 1. Check Monitor-Specific VRAM Cache
	if (useTextureCaching_ && loadFromCache(path)) {
		releaseLoadTask();
		status_ = LoadStatus::Ready;
		return true;
	}

	std::lock_guard<std::mutex> lock(g_ImageLoadTaskMutex);

	// 2. Check for an already in-flight task for this path
	auto it = loadingTasks_.find(path);
	if (it != loadingTasks_.end()) {
		if (auto existing = it->second.lock()) {
			loadTask_ = std::move(existing);
			currentLoadingPath_ = path;
			status_ = LoadStatus::Loading;
			return true;
		}
		loadingTasks_.erase(it);
	}

	// 3. Start a new decompression task
	auto promise = std::make_shared<std::promise<AsyncLoadResult>>();
	auto task = std::make_shared<AsyncLoadTask>();
	task->future = promise->get_future().share();

	currentLoadingPath_ = path;
	status_ = LoadStatus::Loading;
	loadTask_ = task;
	loadingTasks_[path] = task;

    try {
        ensureAsyncIO();
        auto request = std::make_shared<AsyncIOState::Request>();
        request->path = path;
        request->promise = promise;
        request->consumer = task;
        auto& state = asyncIOState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending.push_back(std::move(request));
        state.changed.notify_all();
        if (state.queue) SDL_SignalAsyncIOQueue(state.queue);
    } catch (...) {
        loadingTasks_.erase(path);
        loadTask_.reset();
        currentLoadingPath_.clear();
        status_ = LoadStatus::Error;
        return false;
    }

	return true;
}

void Image::finalizeLoad() {
	if (!loadTask_) {
		status_ = LoadStatus::Error;
		return;
	}

	std::string path = currentLoadingPath_;
	AsyncLoadResult res = loadTask_->future.get();

	if (!res.success) {
		releaseLoadTask();
		// Fallback to alt file logic
		if (path == file_ && !altFile_.empty()) {
			startAsyncLoad(altFile_);
			return;
		}
		status_ = LoadStatus::Error;
		return;
	}

	// A second consumer may have populated the cache while this shared decode
	// was waiting to be finalized.
	if (useTextureCaching_ && loadFromCache(path)) {
		releaseLoadTask();
		status_ = LoadStatus::Ready;
		return;
	}

	// Prepare new assets on the main thread (Renderer calls must be main thread)
	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	SDL_Texture* newTexture = nullptr;
	if (res.staticSurface) {
		newTexture = SDL_CreateTextureFromSurface(renderer, res.staticSurface.get());
		if (!newTexture) {
			releaseLoadTask();
			status_ = LoadStatus::Error;
			return;
		}
	}

	// --- THE ATOMIC SWAP ---
	// Cleanup old local assets only now that we have the new ones
	releaseLocalImageAssets();

	// Assign new data
	texture_ = newTexture;
	animatedSurfaces_ = res.animatedSurfaces;
	frameDelays_ = res.frameDelays;
	baseViewInfo.ImageWidth = (float)res.w;
	baseViewInfo.ImageHeight = (float)res.h;

	if (!animatedSurfaces_.empty()) {
		if (!createAnimatedStreamingTexture(res.w, res.h)) {
			releaseLocalImageAssets();
			releaseLoadTask();
			status_ = LoadStatus::Error;
			return;
		}
		primeAnimatedTextureIfNeeded();
	}

	// Update Cache Status
	if (useTextureCaching_) {
		CachedImage ci = { texture_, animatedSurfaces_, frameDelays_, res.w, res.h };
		CachedImage existing;
		bool inserted = false;

		{
			std::lock_guard<std::mutex> lock(g_ImageTextureCacheMutex);
			auto [entry, didInsert] = textureCache_.try_emplace(
				pathCache_.getKey(path, baseViewInfo.Monitor), ci);
			inserted = didInsert;
			if (!inserted) {
				existing = entry->second;
			}
		}

		if (inserted) {
			isUsingCachedStaticTexture_ = (texture_ != nullptr);
			isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
		}
		else if (!applyCachedImage(existing)) {
			releaseLoadTask();
			status_ = LoadStatus::Error;
			return;
		}
	}
	else {
		isUsingCachedStaticTexture_ = false;
		isUsingCachedSurfaces_ = false;
	}

	releaseLoadTask();
	resetAnimationState();
	status_ = LoadStatus::Ready;
}

// -------------------- Render Logic --------------------
bool Image::loadFromCache(const std::string& filePath) {
	CachedImage cached;
	{
		std::lock_guard<std::mutex> lock(g_ImageTextureCacheMutex);
		auto it = textureCache_.find(pathCache_.getKey(filePath, baseViewInfo.Monitor));
		if (it == textureCache_.end()) return false;
		cached = it->second;
	}

	return applyCachedImage(cached);
}

bool Image::applyCachedImage(const CachedImage& cached) {
	releaseLocalImageAssets();

	baseViewInfo.ImageWidth = (float)cached.w;
	baseViewInfo.ImageHeight = (float)cached.h;

	if (cached.texture) {
		texture_ = cached.texture;
		isUsingCachedStaticTexture_ = true;
	}
	else if (!cached.animatedSurfaces.empty()) {
		animatedSurfaces_ = cached.animatedSurfaces;
		frameDelays_ = cached.frameDelays;
		isUsingCachedSurfaces_ = true;
		if (!createAnimatedStreamingTexture(cached.w, cached.h)) {
			releaseLocalImageAssets();
			return false;
		}
	}

	if (!texture_ && !animatedTexture_) return false;

	primeAnimatedTextureIfNeeded();
	resetAnimationState();
	return true;
}

bool Image::update(float dt) {
	bool done = Component::update(dt);

	// Check background asset status in the logic pass, BEFORE drawing starts
	if (status_ == LoadStatus::Loading) {
		if (loadTask_ &&
			loadTask_->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			finalizeLoad();
		}
	}

	return done;
}

void Image::draw() {
	Component::draw();

	if (status_ == LoadStatus::Error || (status_ == LoadStatus::Unloaded && !texture_ && !animatedTexture_)) {
		return;
	}

	if (animatedTexture_ && !animatedSurfaces_.empty() && !frameDelays_.empty()) {
		Uint64 now = SDL_GetTicks();

		/* Initialize a stable timeline anchor once */
		if (animationStartTime_ == 0) {
			animationStartTime_ = now;
			lastRenderedFrame_ = std::numeric_limits<size_t>::max(); // force upload
		}

		/* Compute total cycle time */
		Uint64 totalCycleTime = 0;
		for (int d : frameDelays_) totalCycleTime += (Uint64)d;

		if (totalCycleTime > 0) {
			/* Resolve current phase in cycle */
			Uint64 t = (now - animationStartTime_) % totalCycleTime;

			/* Map phase -> frame index */
			Uint64 accum = 0;
			size_t frameIndex = 0;

			for (size_t i = 0; i < frameDelays_.size(); ++i) {
				accum += (Uint64)frameDelays_[i];
				if (t < accum) {
					frameIndex = i;
					break;
				}
			}

			currentFrame_ = frameIndex;

			/* Upload only if frame changed */
			if (currentFrame_ != lastRenderedFrame_) {
				SDL_Surface* s = animatedSurfaces_[currentFrame_].get();
				if (s && s->pixels) {
					SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
				}
				lastRenderedFrame_ = currentFrame_;
			}
		}
	}

	SDL_Texture* target = animatedTexture_ ? animatedTexture_ : texture_;
	if (!target) return;

	SDL_SetTextureBlendMode(target, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);

	SDL_FRect rect{
		baseViewInfo.XRelativeToOrigin(),
		baseViewInfo.YRelativeToOrigin(),
		baseViewInfo.ScaledWidth(),
		baseViewInfo.ScaledHeight()
	};

	SDL::renderCopyF(
		target,
		baseViewInfo.Alpha,
		nullptr,
		&rect,
		baseViewInfo,
		page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
		page.getLayoutHeightByMonitor(baseViewInfo.Monitor)
	);
}

// -------------------- Helpers --------------------
void Image::freeGraphicsMemory() {
	releaseLocalImageAssets();
	status_ = LoadStatus::Unloaded;
	releaseLoadTask();
}

void Image::releaseLocalImageAssets() {
	if (animatedTexture_) SDL_DestroyTexture(animatedTexture_);
	if (texture_ && !isUsingCachedStaticTexture_) SDL_DestroyTexture(texture_);

	texture_ = nullptr;
	animatedTexture_ = nullptr;
	animatedSurfaces_.clear();
	frameDelays_.clear();
	isUsingCachedStaticTexture_ = isUsingCachedSurfaces_ = false;
	resetAnimationState();
}

void Image::releaseLoadTask() {
	const std::string path = currentLoadingPath_;
	loadTask_.reset();
	currentLoadingPath_.clear();
	pruneExpiredLoadTask(path);
}

void Image::pruneExpiredLoadTask(const std::string& path) {
	if (path.empty()) return;

	std::lock_guard<std::mutex> lock(g_ImageLoadTaskMutex);
	auto it = loadingTasks_.find(path);
	if (it != loadingTasks_.end() && it->second.expired()) {
		loadingTasks_.erase(it);
	}
}

void Image::resetAnimationState() {
	currentFrame_ = 0;
	animationStartTime_ = 0;
	lastRenderedFrame_ = std::numeric_limits<size_t>::max();
}

bool Image::createAnimatedStreamingTexture(int width, int height) {
	if (width <= 0 || height <= 0) return false;

	// Ensure we aren't leaking a previous streaming texture
	if (animatedTexture_) {
		SDL_DestroyTexture(animatedTexture_);
	}

	animatedTexture_ = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor),
		SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STREAMING,
		width, height);
	return animatedTexture_ != nullptr;
}

void Image::primeAnimatedTextureIfNeeded() {
	if (animatedTexture_ && !animatedSurfaces_.empty()) {
		SDL_Surface* s = animatedSurfaces_[0].get();
		if (s) SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
	}
}

void Image::cleanupTextureCache() {
	std::scoped_lock lock(g_ImageTextureCacheMutex, g_ImageLoadTaskMutex);
	for (auto& [key, entry] : textureCache_) {
		if (entry.texture) SDL_DestroyTexture(entry.texture);
	}
	textureCache_.clear();
	loadingTasks_.clear();
	pathCache_.fullPaths_.clear();
}

bool Image::recycleAsImage(const std::string& newFilePath, const std::string& newAltPath) {
	if (newFilePath.empty() && newAltPath.empty()) return false;

	// 1. Exit if already showing this file
	if (file_ == newFilePath && altFile_ == newAltPath && status_ == LoadStatus::Ready) {
		return true;
	}

	this->Component::freeGraphicsMemory();
	releaseLoadTask();

	// Keep drawing the old image while a cache miss is decoded.
	file_ = newFilePath;
	altFile_ = newAltPath;

	const std::string& pathToLoad = file_.empty() ? altFile_ : file_;
	if (loadFromCache(pathToLoad)) {
		status_ = LoadStatus::Ready;
		return true;
	}

	if (!startAsyncLoad(pathToLoad)) {
		status_ = LoadStatus::Error;
	}

	return true;
}

std::string_view Image::filePath() { return file_; }

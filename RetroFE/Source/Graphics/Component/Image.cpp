#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include <mutex>

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::unordered_map<std::string, std::weak_ptr<Image::AsyncLoadTask>> Image::loadingTasks_;

static std::mutex g_ImageTextureCacheMutex;
static std::mutex g_ImageLoadTaskMutex;

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
		(void)ThreadPool::getInstance().enqueue([path, promise, task]() mutable {
			AsyncLoadResult res;

			try {
				SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
				if (rw) {
					if (IMG_isGIF(rw) || IMG_isWEBP(rw)) {
						IMG_Animation* anim = IMG_LoadAnimation_RW(rw, 1);
						if (anim) {
							res.w = anim->w;
							res.h = anim->h;
							for (int i = 0; i < anim->count; ++i) {
								SDL_Surface* conv = SDL_ConvertSurfaceFormat(
									anim->frames[i], SDL_PIXELFORMAT_RGBA32, 0);
								if (conv) {
									res.animatedSurfaces.emplace_back(conv, SurfaceDeleter());
									res.frameDelays.push_back(
										(anim->delays && anim->delays[i] > 0)
										? anim->delays[i] : 100);
								}
							}
							IMG_FreeAnimation(anim);
							res.success = !res.animatedSurfaces.empty();
						}
					}
					else {
						SDL_Surface* s = IMG_Load_RW(rw, 1);
						if (s) {
							res.staticSurface = SharedSurface(s, SurfaceDeleter());
							res.w = s->w;
							res.h = s->h;
							res.success = true;
						}
					}
				}
			}
			catch (...) {
				res = {};
			}

			promise->set_value(std::move(res));
			task.reset();
			pruneExpiredLoadTask(path);
		});
	}
	catch (...) {
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
		Uint32 now = SDL_GetTicks();

		/* Initialize a stable timeline anchor once */
		if (animationStartTime_ == 0) {
			animationStartTime_ = now;
			lastRenderedFrame_ = std::numeric_limits<size_t>::max(); // force upload
		}

		/* Compute total cycle time */
		Uint32 totalCycleTime = 0;
		for (int d : frameDelays_) totalCycleTime += (Uint32)d;

		if (totalCycleTime > 0) {
			/* Resolve current phase in cycle */
			Uint32 t = (now - animationStartTime_) % totalCycleTime;

			/* Map phase -> frame index */
			Uint32 accum = 0;
			size_t frameIndex = 0;

			for (size_t i = 0; i < frameDelays_.size(); ++i) {
				accum += (Uint32)frameDelays_[i];
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

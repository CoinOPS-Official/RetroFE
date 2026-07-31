#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include <algorithm>
#include <mutex>

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::unordered_map<std::string, std::weak_ptr<Image::AsyncLoadTask>> Image::loadingTasks_;

// Renderer-owned cache/path state and worker-owned load deduplication have
// different contention domains. Keeping them separate prevents a completed
// decode task from blocking a scrolling-list texture-cache hit.
static std::mutex g_ImageTextureCacheMutex;
static std::mutex g_ImageLoadTaskMutex;

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
	// All callers hold the texture-cache mutex while interning cache keys.
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
	if (useTextureCaching_) ensureCacheReserved();

	if (!file_.empty() && startAsyncLoad(file_)) return;
	if (!altFile_.empty() && startAsyncLoad(altFile_)) return;

	status_ = LoadStatus::Error;
}

void Image::pumpGraphicsPreparation() {
	if (status_ == LoadStatus::Loading) {
		if (loadTask_ &&
			loadTask_->future.wait_for(std::chrono::milliseconds(0)) ==
				std::future_status::ready) {
			finalizeLoad();
		}
	}
}

void Image::waitForGraphicsPreparation() {
	if (status_ == LoadStatus::Loading && loadTask_) {
		loadTask_->future.wait();
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

bool Image::isDecodeReadyForFinalization() const {
	return status_ == LoadStatus::Loading &&
		loadTask_ &&
		loadTask_->future.wait_for(std::chrono::milliseconds(0)) ==
			std::future_status::ready;
}

bool Image::startAsyncLoad(const std::string& path) {
	// 1. Adopt any renderer-specific texture already retained by another
	// component or by presentation warming. useTextureCaching_ controls
	// whether this instance inserts new entries, not whether it can reuse one.
	if (loadFromCache(path)) {
		loadSource_ = LoadSource::TextureCache;
		releaseLoadTask();
		currentLoadingPath_.clear();
		status_ = LoadStatus::Ready;
		return true;
	}

	std::lock_guard<std::mutex> lock(g_ImageLoadTaskMutex);

	// 2. Check for an already in-flight task for this path
	auto it = loadingTasks_.find(path);
	if (it != loadingTasks_.end()) {
		if (auto existing = it->second.lock()) {
			loadSource_ = LoadSource::SharedInFlight;
			loadTask_ = std::move(existing);
			currentLoadingPath_ = path;
			status_ = LoadStatus::Loading;
			return true;
		}
		loadingTasks_.erase(it);
	}

	// 3. Start a new decompression task
	auto promise =
		std::make_shared<std::promise<AsyncLoadResult>>();
	auto task = std::make_shared<AsyncLoadTask>();
	task->future = promise->get_future().share();

	loadSource_ = LoadSource::NewDecode;
	currentLoadingPath_ = path;
	status_ = LoadStatus::Loading;
	loadTask_ = task;
	loadingTasks_[path] = task;

	try {
		(void)ThreadPool::getInstance().enqueue(
			[path, promise, task]() mutable {
				AsyncLoadResult res;

				try {
					SDL_RWops* rw =
						SDL_RWFromFile(path.c_str(), "rb");
					if (rw) {
						if (IMG_isGIF(rw) || IMG_isWEBP(rw)) {
							IMG_Animation* anim =
								IMG_LoadAnimation_RW(rw, 1);
							if (anim) {
								res.w = anim->w;
								res.h = anim->h;
								for (int i = 0; i < anim->count; ++i) {
									SDL_Surface* conv =
										SDL_ConvertSurfaceFormat(
											anim->frames[i],
											SDL_PIXELFORMAT_RGBA32,
											0
										);
									res.animatedSurfaces.emplace_back(
										conv,
										SurfaceDeleter()
									);
									res.frameDelays.push_back(
										(anim->delays &&
											anim->delays[i] > 0)
										? anim->delays[i]
										: 100
									);
								}
								IMG_FreeAnimation(anim);
								res.success =
									!res.animatedSurfaces.empty();
							}
						}
						else {
							SDL_Surface* surface =
								IMG_Load_RW(rw, 1);
							if (surface) {
								res.staticSurface = SharedSurface(
									surface,
									SurfaceDeleter()
								);
								res.w = surface->w;
								res.h = surface->h;
								res.success = true;
							}
						}
					}
				}
				catch (...) {
					// Treat decoder/library exceptions as a normal failed
					// load so waiters always receive a usable result.
					res = {};
				}

				// Publish the completed surfaces before dropping the worker's
				// registry lifetime. New consumers can join this ready result
				// until the final main-thread consumer completes its handoff.
				promise->set_value(std::move(res));
				task.reset();
				pruneExpiredLoadTask(path);
			}
		);
	}
	catch (...) {
		loadingTasks_.erase(path);
		loadTask_.reset();
		currentLoadingPath_.clear();
		loadSource_ = LoadSource::None;
		status_ = LoadStatus::Error;
		return false;
	}

	return true;
}

void Image::finalizeLoad() {
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

	// Another consumer of the same shared load may already have finalized
	// and cached the renderer-specific texture. Adopt it instead of
	// creating and then orphaning a duplicate texture.
	if (loadFromCache(path)) {
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
	resetAnimationState();

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
				pathCache_.getKey(path, baseViewInfo.Monitor),
				ci
			);
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
	status_ = LoadStatus::Ready;
}

// -------------------- Render Logic --------------------
bool Image::loadFromCache(const std::string& filePath) {
	CachedImage cached;

	{
		std::lock_guard<std::mutex> lock(g_ImageTextureCacheMutex);
		auto it = textureCache_.find(
			pathCache_.getKey(filePath, baseViewInfo.Monitor)
		);
		if (it == textureCache_.end()) {
			return false;
		}
		cached = it->second;
	}

	return applyCachedImage(cached);
}

bool Image::isTextureCached(
	const std::string& filePath,
	int monitor)
{
	std::lock_guard<std::mutex> lock(g_ImageTextureCacheMutex);
	return textureCache_.find({ filePath, monitor }) !=
		textureCache_.end();
}

bool Image::applyCachedImage(const CachedImage& cached) {
	releaseLocalImageAssets();

	baseViewInfo.ImageWidth = static_cast<float>(cached.w);
	baseViewInfo.ImageHeight = static_cast<float>(cached.h);

	texture_ = cached.texture;
	animatedSurfaces_ = cached.animatedSurfaces;
	frameDelays_ = cached.frameDelays;
	isUsingCachedStaticTexture_ = texture_ != nullptr;
	isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
	resetAnimationState();

	if (isUsingCachedSurfaces_) {
		if (!createAnimatedStreamingTexture(cached.w, cached.h)) {
			releaseLocalImageAssets();
			return false;
		}
		primeAnimatedTextureIfNeeded();
	}

	return texture_ || animatedTexture_;
}

void Image::releaseLocalImageAssets() {
	if (animatedTexture_) {
		SDL_DestroyTexture(animatedTexture_);
	}
	if (texture_ && !isUsingCachedStaticTexture_) {
		SDL_DestroyTexture(texture_);
	}

	texture_ = nullptr;
	animatedTexture_ = nullptr;
	animatedSurfaces_.clear();
	frameDelays_.clear();
	isUsingCachedStaticTexture_ = false;
	isUsingCachedSurfaces_ = false;
	resetAnimationState();
}

bool Image::update(float dt) {
	bool done = Component::update(dt);

	// Check background asset status in the logic pass, BEFORE drawing starts
	if (isDecodeReadyForFinalization()) {
		finalizeLoad();
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
		}

		if (totalAnimationDuration_ > 0) {
			/* Resolve current phase in cycle */
			const std::uint64_t t =
				static_cast<std::uint64_t>(now - animationStartTime_) %
				totalAnimationDuration_;

			const auto frame = std::upper_bound(
				cumulativeFrameDelays_.begin(),
				cumulativeFrameDelays_.end(),
				t
			);
			const size_t frameIndex = static_cast<size_t>(
				std::distance(cumulativeFrameDelays_.begin(), frame)
			);

			currentFrame_ = std::min(
				frameIndex,
				animatedSurfaces_.size() - 1
			);

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
	loadSource_ = LoadSource::None;

	releaseLoadTask();
}

void Image::releaseLoadTask() {
	const std::string path = currentLoadingPath_;
	loadTask_.reset();
	currentLoadingPath_.clear();
	pruneExpiredLoadTask(path);
}

void Image::pruneExpiredLoadTask(const std::string& path) {
	if (path.empty()) {
		return;
	}

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
	cumulativeFrameDelays_.clear();
	cumulativeFrameDelays_.reserve(frameDelays_.size());
	totalAnimationDuration_ = 0;

	for (int delay : frameDelays_) {
		totalAnimationDuration_ += static_cast<std::uint64_t>(
			std::max(delay, 0)
		);
		cumulativeFrameDelays_.push_back(totalAnimationDuration_);
	}
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
		if (s && SDL_UpdateTexture(
			animatedTexture_,
			nullptr,
			s->pixels,
			s->pitch) == 0)
		{
			currentFrame_ = 0;
			lastRenderedFrame_ = 0;
		}
	}
}

void Image::cleanupTextureCache() {
	std::scoped_lock lock(
		g_ImageTextureCacheMutex,
		g_ImageLoadTaskMutex);
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

	file_ = newFilePath;
	altFile_ = newAltPath;

	const std::string& pathToLoad =
		file_.empty() ? altFile_ : file_;

	// Keep drawing the old image on a miss. startAsyncLoad() also owns the
	// single cache-hit path, avoiding a duplicate lookup during recycling.
	if (!startAsyncLoad(pathToLoad)) {
		status_ = LoadStatus::Error;
	}

	return true;
}

std::string_view Image::filePath() { return file_; }

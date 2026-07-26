#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include <algorithm>
#include <mutex>

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::unordered_map<std::string, std::shared_future<Image::AsyncLoadResult>> Image::loadingTasks_;

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
		if (loadTask_.valid() &&
			loadTask_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			finalizeLoad();
		}
	}
}

void Image::waitForGraphicsPreparation() {
	if (status_ == LoadStatus::Loading && loadTask_.valid()) {
		loadTask_.wait();
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
		loadTask_ = {};
		currentLoadingPath_.clear();
		status_ = LoadStatus::Ready;
		return true;
	}

	std::lock_guard<std::mutex> lock(g_ImageLoadTaskMutex);

	// 2. Check for an already in-flight task for this path
	auto it = loadingTasks_.find(path);
	if (it != loadingTasks_.end()) {
		loadTask_ = it->second;
		currentLoadingPath_ = path;
		status_ = LoadStatus::Loading;
		return true;
	}

	// 3. Start a new decompression task
	currentLoadingPath_ = path;
	status_ = LoadStatus::Loading;
	loadTask_ = ThreadPool::getInstance().enqueue([path]() -> AsyncLoadResult {
		AsyncLoadResult res;

		auto cleanup = [&]() {
			std::lock_guard<std::mutex> lock(g_ImageLoadTaskMutex);
			loadingTasks_.erase(path);
			};

		SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
		if (!rw) {
			cleanup();
			return res;
		}

		if (IMG_isGIF(rw) || IMG_isWEBP(rw)) {
			IMG_Animation* anim = IMG_LoadAnimation_RW(rw, 1);
			if (anim) {
				res.w = anim->w; res.h = anim->h;
				for (int i = 0; i < anim->count; ++i) {
					SDL_Surface* conv = SDL_ConvertSurfaceFormat(anim->frames[i], SDL_PIXELFORMAT_RGBA32, 0);
					res.animatedSurfaces.emplace_back(conv, SurfaceDeleter());
					res.frameDelays.push_back((anim->delays && anim->delays[i] > 0) ? anim->delays[i] : 100);
				}
				IMG_FreeAnimation(anim);
				res.success = !res.animatedSurfaces.empty();
			}
		}
		else {
			SDL_Surface* s = IMG_Load_RW(rw, 1);
			if (s) {
				res.staticSurface = SharedSurface(s, SurfaceDeleter());
				res.w = s->w; res.h = s->h;
				res.success = true;
			}
		}

		cleanup();
		return res;
		}).share();

	loadingTasks_[path] = loadTask_;
	return true;
}

void Image::finalizeLoad() {
	std::string path = currentLoadingPath_;
	AsyncLoadResult res = loadTask_.get();
	loadTask_ = {};
	currentLoadingPath_.clear();

	if (!res.success) {
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
	if (useTextureCaching_ && loadFromCache(path)) {
		status_ = LoadStatus::Ready;
		return;
	}

	// Prepare new assets on the main thread (Renderer calls must be main thread)
	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	SDL_Texture* newTexture = nullptr;
	if (res.staticSurface) {
		newTexture = SDL_CreateTextureFromSurface(renderer, res.staticSurface.get());
		if (!newTexture) {
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
			status_ = LoadStatus::Error;
			return;
		}
	}
	else {
		isUsingCachedStaticTexture_ = false;
		isUsingCachedSurfaces_ = false;
	}

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
	if (status_ == LoadStatus::Loading) {
		if (loadTask_.valid() && loadTask_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
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

	loadTask_ = {};

	// Reset our local path so we don't accidentally check it later
	currentLoadingPath_.clear();
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

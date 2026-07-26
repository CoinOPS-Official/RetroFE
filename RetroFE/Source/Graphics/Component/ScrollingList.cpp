/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "ScrollingList.h"
#include "../Animate/Tween.h"
#include "../Animate/TweenSet.h"
#include "../Animate/Animation.h"
#include "../Animate/AnimationEvents.h"
#include "../Animate/TweenTypes.h"
#include "../Font.h"
#include "ImageBuilder.h"
#include "VideoBuilder.h"
#include "VideoComponent.h"
#include "ReloadableMedia.h"
#include "Text.h"
#include "../../Video/VideoPool.h"
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Collection/Item.h"
#include "../../Utility/Utils.h"
#include "../../Utility/ThreadPool.h"
#include "../../Utility/Log.h"
#include "../../SDL.h"
#include "../ViewInfo.h"
#include <math.h>
#if __has_include(<SDL_image.h>)
#include <SDL_image.h>
#elif __has_include(<SDL2_image/SDL_image.h>)
#include <SDL2_image/SDL_image.h>
#else
#error "Cannot find SDL_image header"
#endif
#include <sstream>
#include <cctype>
#include <iomanip>
#include <algorithm>
#include <cmath>

int ScrollingList::nextListId = 0;
size_t ScrollingList::activeImagePreloads_ = 0;
std::unordered_map<std::string, ScrollingList::SharedResolvedMedia>
    ScrollingList::sharedMediaCache_;

ScrollingList::ScrollingList( Configuration &c,
                              Page          &p,
                              bool           layoutMode,
                              bool           commonMode,
                              bool          playlistType,
                              bool          selectedImage,
                              FontManager          *font,
                              const std::string    &layoutKey,
                              const std::string    &imageType,
                              const std::string    &videoType,
                              bool          useTextureCaching)
    : Component( p )
    , layoutMode_( layoutMode )
    , commonMode_( commonMode )
    , playlistType_( playlistType )
    , selectedImage_( selectedImage)
    , config_( c )
    , fontInst_( font )
    , layoutKey_( layoutKey )
    , imageType_( imageType )
    , videoType_( videoType )
    , components_()
    , useTextureCaching_(useTextureCaching)
{
    listId_ = nextListId++;
    config_.getProperty(OPTION_LAYOUT, layoutName_);
    imageTypeLC_ = Utils::toLower(imageType_);

    // Pre-build the base paths so they are only calculated ONCE
    layoutCollectionsBase_ = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName_, "collections");
    commonCollectionsBase_ = Utils::combinePath(Configuration::absolutePath, "collections", "_common");
}

ScrollingList::ScrollingList(const ScrollingList& other)
    : Component(other)
    , listId_(nextListId++)
    , layoutMode_(other.layoutMode_)
    , commonMode_(other.commonMode_)
    , playlistType_(other.playlistType_)
    , selectedImage_(other.selectedImage_)
    , textFallback_(other.textFallback_)
    , tweenPoints_(other.tweenPoints_)
    , itemIndex_(other.itemIndex_)
    , selectedOffsetIndex_(other.selectedOffsetIndex_)
    , scrollAcceleration_(other.scrollAcceleration_)
    , startScrollTime_(other.startScrollTime_)
    , minScrollTime_(other.minScrollTime_)
    , scrollPeriod_(other.scrollPeriod_)
    , letterSkipTimer_(other.letterSkipTimer_)
    , coastFriction_(other.coastFriction_)
    , currentDt_(other.currentDt_)
    , coasting_(other.coasting_)
    , coastElapsed_(other.coastElapsed_)
    , coastStartPeriod_(other.coastStartPeriod_)
    , config_(other.config_)
    , fontInst_(other.fontInst_)
    , layoutKey_(other.layoutKey_)
    , imageType_(other.imageType_)
    , videoType_(other.videoType_)
    , layoutName_(other.layoutName_)
    , imageTypeLC_(other.imageTypeLC_)
    , layoutCollectionsBase_(other.layoutCollectionsBase_)
    , commonCollectionsBase_(other.commonCollectionsBase_)
    , items_(other.items_)
    , components_(other.components_)
    , useTextureCaching_(other.useTextureCaching_)
    , mediaCache_(other.mediaCache_)
    , letterAnchors_(other.letterAnchors_)
    , perspectiveCornersInitialized_(other.perspectiveCornersInitialized_)
    , cachedIdle_(other.cachedIdle_)
    , cachedAttractIdle_(other.cachedAttractIdle_)
{
    horizontalScroll = other.horizontalScroll;
    std::fill(
        components_.raw().begin(),
        components_.raw().end(),
        nullptr);
    std::copy(
        std::begin(other.perspectiveCorners_),
        std::end(other.perspectiveCorners_),
        std::begin(perspectiveCorners_));

    if (other.scrollPoints_) {
        scrollPoints_ = new std::vector<ViewInfo*>();
        scrollPoints_->reserve(other.scrollPoints_->size());
        try {
            for (const ViewInfo* point : *other.scrollPoints_) {
                scrollPoints_->push_back(point ? new ViewInfo(*point) : nullptr);
            }
        }
        catch (...) {
            clearPoints();
            throw;
        }
    }

    // Retain the circular view's logical head, but never share its owned
    // component pointers with the source list.
    rebuildSlotTopology_(false);
}


ScrollingList::~ScrollingList() {
    ScrollingList::freeGraphicsMemory();
    clearPoints();
    destroyItems();
}

int ScrollingList::getListId() const {
    return listId_;
}

void ScrollingList::clearPoints() {
    if (scrollPoints_) {
        while (!scrollPoints_->empty()) {
            ViewInfo* scrollPoint = scrollPoints_->back();
            delete scrollPoint;
            scrollPoints_->pop_back();
        }
        delete scrollPoints_;
        scrollPoints_ = nullptr;
    }
}

void ScrollingList::clearTweenPoints() {
    tweenPoints_.reset(); // Using reset to clear the shared pointer
}

const std::vector<Item*>& ScrollingList::getItems() const
{
    return *items_;
}

void ScrollingList::setItems(std::vector<Item*>* items) {
    resetImagePreload_();
    items_ = items;
    mediaCache_.clear();
    if (!items_) return;

    const size_t size = items_->size();
    itemIndex_ = size == 0
        ? 0
        : loopDecrement(size, selectedOffsetIndex_, size);
    mediaCache_.resize(size);

    resetScrollPeriod();
    rebuildLetterAnchors_();
    refreshImagePreloadQueue_();
}

void ScrollingList::clearSharedMediaCache() {
    sharedMediaCache_.clear();
}

void ScrollingList::rebuildLetterAnchors_() {
    letterAnchors_.clear();
    if (!items_ || items_->empty()) {
        return;
    }

    bool haveGroup = false;
    int previousGroup = 0;

    for (size_t i = 0; i < items_->size(); ++i) {
        const Item* item = (*items_)[i];
        if (!item || item->fullTitle.empty()) {
            continue;
        }

        const unsigned char first =
            static_cast<unsigned char>(item->fullTitle.front());
        const int group = std::isalpha(first)
            ? 1 + std::tolower(first)
            : 0;

        if (!haveGroup || group != previousGroup) {
            letterAnchors_.push_back(i);
            previousGroup = group;
            haveGroup = true;
        }
    }
}

void ScrollingList::refreshImagePreloadQueue_(
    bool directionKnown,
    bool forward)
{
    constexpr size_t MAX_PRELOAD_CANDIDATES = 192;
    constexpr size_t LETTER_ANCHORS_EACH_WAY = 2;

    imagePreloadQueue_.clear();

    if (!useTextureCaching_ || !items_ || items_->empty() ||
        !scrollPoints_ || scrollPoints_->empty() ||
        imageType_.empty()) {
        return;
    }

    const size_t itemCount = items_->size();
    const size_t slotCount = scrollPoints_->size();
    const size_t selected = getSelectedIndex();
    imagePreloadQueued_.clear();
    if (imagePreloadQueued_.bucket_count() <
        MAX_PRELOAD_CANDIDATES) {
        imagePreloadQueued_.reserve(MAX_PRELOAD_CANDIDATES);
    }

    auto addCandidate = [&](size_t index, bool idleOnly) {
        if (imagePreloadQueue_.size() >= MAX_PRELOAD_CANDIDATES ||
            imagePreloadAttempted_.find(index) !=
                imagePreloadAttempted_.end() ||
            !imagePreloadQueued_.insert(index).second) {
            return;
        }

        imagePreloadQueue_.push_back({ index, idleOnly });
    };

    size_t highPriorityRadius = 0;

    if (directionKnown) {
        const size_t ahead = std::max(slotCount * 4, size_t{ 24 });
        const size_t behind = std::max(slotCount * 2, size_t{ 12 });
        highPriorityRadius = std::max(ahead, behind);

        for (size_t distance = 1;
            distance <= highPriorityRadius;
            ++distance)
        {
            if (distance <= ahead) {
                addCandidate(
                    forward
                        ? loopIncrement(selected, distance, itemCount)
                        : loopDecrement(selected, distance, itemCount),
                    false
                );
            }
            if (distance <= behind) {
                addCandidate(
                    forward
                        ? loopDecrement(selected, distance, itemCount)
                        : loopIncrement(selected, distance, itemCount),
                    false
                );
            }
        }
    }
    else {
        highPriorityRadius =
            std::max(slotCount * 3, size_t{ 16 });

        for (size_t distance = 1;
            distance <= highPriorityRadius;
            ++distance)
        {
            addCandidate(
                loopIncrement(selected, distance, itemCount),
                false
            );
            addCandidate(
                loopDecrement(selected, distance, itemCount),
                false
            );
        }
    }

    // Warm the complete slot windows for the next and previous letter
    // landings. This predicts sequential letter navigation without
    // touching the items that would be skipped over.
    if (letterAnchors_.size() > 1) {
        const auto afterCurrent = std::upper_bound(
            letterAnchors_.begin(),
            letterAnchors_.end(),
            selected
        );
        const size_t currentGroup =
            afterCurrent == letterAnchors_.begin()
            ? letterAnchors_.size() - 1
            : static_cast<size_t>(
                std::distance(
                    letterAnchors_.begin(),
                    afterCurrent
                ) - 1
            );

        auto addLandingWindow = [&](size_t anchor) {
            const size_t firstSlot = loopDecrement(
                anchor,
                selectedOffsetIndex_,
                itemCount
            );
            for (size_t slot = 0; slot < slotCount; ++slot) {
                addCandidate(
                    loopIncrement(firstSlot, slot, itemCount),
                    false
                );
            }
        };

        for (size_t step = 1;
            step <= LETTER_ANCHORS_EACH_WAY;
            ++step)
        {
            addLandingWindow(
                letterAnchors_[
                    loopIncrement(
                        currentGroup,
                        step,
                        letterAnchors_.size()
                    )
                ]
            );
            addLandingWindow(
                letterAnchors_[
                    loopDecrement(
                        currentGroup,
                        step,
                        letterAnchors_.size()
                    )
                ]
            );
        }
    }

    // Use true idle time to expand the warm neighborhood, while keeping
    // active-navigation work limited to the candidates above.
    for (size_t distance = 1;
        distance < itemCount &&
        imagePreloadQueue_.size() < MAX_PRELOAD_CANDIDATES;
        ++distance)
    {
        addCandidate(
            loopIncrement(selected, distance, itemCount),
            true
        );
        addCandidate(
            loopDecrement(selected, distance, itemCount),
            true
        );
    }
}

void ScrollingList::pumpImagePreload_() {
    constexpr unsigned int MAX_CACHE_HITS_PER_FRAME = 8;

    if (!useTextureCaching_ || !items_ || items_->empty()) {
        return;
    }

    for (unsigned int pass = 0;
        pass < MAX_CACHE_HITS_PER_FRAME;
        ++pass)
    {
        const bool allowIdleWork =
            cachedIdle_ && letterSkipTimer_ <= 0.0f;

        if (imagePreload_) {
            imagePreload_->pumpGraphicsPreparation();
            if (!imagePreload_->isGraphicsReadyForFirstRender()) {
                return;
            }

            releaseImagePreload_();
            return;
        }

        if (imagePreloadQueue_.empty()) {
            return;
        }

        const ImagePreloadCandidate candidate =
            imagePreloadQueue_.front();
        if (candidate.idleOnly && !allowIdleWork) {
            return;
        }
        imagePreloadQueue_.pop_front();

        if (!imagePreloadAttempted_.insert(
            candidate.itemIndex).second) {
            continue;
        }

        const ResolvedMedia& media =
            resolveMediaAt_(candidate.itemIndex);

        // Video-backed entries already have their own pool and preroll
        // policy. Only warm the normal, non-selected image path here.
        if (!media.videoPath.empty() ||
            media.idleImagePath.empty()) {
            continue;
        }

        if (activeImagePreloads_ >= 1) {
            imagePreloadQueue_.push_front(candidate);
            return;
        }

        ++activeImagePreloads_;
        ownsImagePreloadSlot_ = true;
        imagePreloadIdleOnly_ = candidate.idleOnly;
        imagePreload_ = std::make_shared<Image>(
            media.idleImagePath,
            "",
            page,
            baseViewInfo.Monitor,
            baseViewInfo.Additive,
            true
        );
        imagePreload_->allocateGraphicsMemory();
        imagePreload_->pumpGraphicsPreparation();

        if (!imagePreload_->isGraphicsReadyForFirstRender()) {
            return;
        }

        releaseImagePreload_();
        return;
    }
}

void ScrollingList::releaseImagePreload_() {
    imagePreload_.reset();
    imagePreloadIdleOnly_ = false;

    if (ownsImagePreloadSlot_) {
        if (activeImagePreloads_ > 0) {
            --activeImagePreloads_;
        }
        ownsImagePreloadSlot_ = false;
    }
}

void ScrollingList::resetImagePreload_() {
    releaseImagePreload_();
    imagePreloadQueue_.clear();
    imagePreloadAttempted_.clear();
    imagePreloadQueued_.clear();
    letterAnchors_.clear();
}

std::string ScrollingList::mediaCacheKey_(const Item& item) const {
    const auto& names = item.baseNameCandidates(imageTypeLC_);
    const std::string itemCollection =
        item.collectionInfo ? item.collectionInfo->name : "";

    std::string key;
    key.reserve(
        layoutCollectionsBase_.size() +
        commonCollectionsBase_.size() +
        collectionName.size() +
        imageType_.size() +
        videoType_.size() +
        itemCollection.size() +
        item.filepath.size() +
        item.name.size() +
        64
    );

    auto appendField = [&key](std::string_view value) {
        key.append(std::to_string(value.size()));
        key.push_back(':');
        key.append(value);
        key.push_back('|');
    };

    key.push_back(layoutMode_ ? 'L' : 'M');
    key.push_back(commonMode_ ? 'C' : 'S');
    key.push_back(selectedImage_ ? 'X' : 'I');
    key.push_back('|');
    appendField(layoutCollectionsBase_);
    appendField(commonCollectionsBase_);
    appendField(collectionName);
    appendField(imageType_);
    appendField(videoType_);
    appendField(itemCollection);
    appendField(item.filepath);
    appendField(item.name);

    for (std::string_view name : names) {
        appendField(name);
    }

    return key;
}

ScrollingList::ResolvedMedia ScrollingList::resolveMediaForItem_(
    const Item& item) const
{
    ResolvedMedia media;
    const std::vector<std::string_view>& cachedNames =
        item.baseNameCandidates(imageTypeLC_);
    const std::string itemCollection =
        item.collectionInfo ? item.collectionInfo->name : "";

    for (size_t iter = 0; iter <= cachedNames.size(); ++iter) {
        std::string name =
            iter < cachedNames.size()
            ? std::string(cachedNames[iter])
            : "default";
        std::string imagePath;
        std::string videoPath;

        if (layoutMode_) {
            const std::string subPath =
                commonMode_ ? "_common" : collectionName;
            buildPaths(
                imagePath,
                videoPath,
                layoutCollectionsBase_,
                subPath,
                imageType_,
                videoType_
            );
        }
        else {
            if (commonMode_) {
                buildPaths(
                    imagePath,
                    videoPath,
                    commonCollectionsBase_,
                    "",
                    imageType_,
                    videoType_
                );
            }
            else {
                config_.getMediaPropertyAbsolutePath(
                    collectionName, imageType_, false, imagePath);
                config_.getMediaPropertyAbsolutePath(
                    collectionName, videoType_, false, videoPath);
            }
        }

        if (media.videoPath.empty() && videoType_ != "null") {
            VideoBuilder::resolveVideoPath(
                videoPath, name, media.videoPath);
        }
        if (media.idleImagePath.empty() && !imageType_.empty()) {
            ImageBuilder::resolveImagePath(
                imagePath, name, media.idleImagePath);
            if (selectedImage_) {
                ImageBuilder::resolveImagePath(
                    imagePath,
                    name + "-selected",
                    media.selectedImagePath
                );
            }
        }

        if (!media.videoPath.empty() && !media.idleImagePath.empty()) {
            break;
        }

        std::string itemImagePath;
        std::string itemVideoPath;
        if (layoutMode_) {
            buildPaths(
                itemImagePath,
                itemVideoPath,
                layoutCollectionsBase_,
                itemCollection,
                imageType_,
                videoType_
            );
        }
        else {
            if (commonMode_) {
                buildPaths(
                    itemImagePath,
                    itemVideoPath,
                    commonCollectionsBase_,
                    "",
                    imageType_,
                    videoType_
                );
            }
            else {
                config_.getMediaPropertyAbsolutePath(
                    itemCollection, imageType_, false, itemImagePath);
                config_.getMediaPropertyAbsolutePath(
                    itemCollection, videoType_, false, itemVideoPath);
            }
        }

        if (media.videoPath.empty() && videoType_ != "null") {
            VideoBuilder::resolveVideoPath(
                itemVideoPath, name, media.videoPath);
        }
        if (media.idleImagePath.empty() && !imageType_.empty()) {
            ImageBuilder::resolveImagePath(
                itemImagePath, name, media.idleImagePath);
            if (selectedImage_) {
                ImageBuilder::resolveImagePath(
                    itemImagePath,
                    name + "-selected",
                    media.selectedImagePath
                );
            }
        }

        if (!media.videoPath.empty() || !media.idleImagePath.empty()) {
            break;
        }
    }

    if (media.videoPath.empty() && media.idleImagePath.empty()) {
        std::string sysImgPath;
        std::string sysVidPath;
        if (layoutMode_) {
            sysImgPath = Utils::combinePath(
                layoutCollectionsBase_,
                commonMode_ ? "_common" : item.name,
                "system_artwork"
            );
            sysVidPath = sysImgPath;
        }
        else {
            if (commonMode_) {
                sysImgPath = Utils::combinePath(
                    commonCollectionsBase_, "system_artwork");
                sysVidPath = sysImgPath;
            }
            else {
                config_.getMediaPropertyAbsolutePath(
                    item.name, imageType_, true, sysImgPath);
                config_.getMediaPropertyAbsolutePath(
                    item.name, videoType_, true, sysVidPath);
            }
        }

        if (videoType_ != "null") {
            VideoBuilder::resolveVideoPath(
                sysVidPath, videoType_, media.videoPath);
        }
        if (!imageType_.empty()) {
            ImageBuilder::resolveImagePath(
                sysImgPath, imageType_, media.idleImagePath);
            if (selectedImage_) {
                ImageBuilder::resolveImagePath(
                    sysImgPath,
                    imageType_ + "-selected",
                    media.selectedImagePath
                );
            }
        }
    }

    if (media.videoPath.empty() && media.idleImagePath.empty()) {
        if (videoType_ != "null") {
            VideoBuilder::resolveVideoPath(
                item.filepath, videoType_, media.videoPath);
        }
        if (!imageType_.empty()) {
            ImageBuilder::resolveImagePath(
                item.filepath, imageType_, media.idleImagePath);
            if (selectedImage_) {
                ImageBuilder::resolveImagePath(
                    item.filepath,
                    imageType_ + "-selected",
                    media.selectedImagePath
                );
            }
        }
    }

    return media;
}

const ScrollingList::ResolvedMedia& ScrollingList::resolveMediaAt_(
    size_t fullListIndex)
{
    static const ResolvedMedia emptyMedia;

    if (!items_ || fullListIndex >= items_->size() ||
        fullListIndex >= mediaCache_.size()) {
        return emptyMedia;
    }

    if (mediaCache_[fullListIndex]) {
        return *mediaCache_[fullListIndex];
    }

    const Item* item = (*items_)[fullListIndex];
    if (!item) {
        return emptyMedia;
    }

    const std::string key = mediaCacheKey_(*item);
    auto shared = sharedMediaCache_.find(key);

    if (shared == sharedMediaCache_.end()) {
        if (sharedMediaCache_.size() >= 16384) {
            sharedMediaCache_.clear();
        }

        auto resolved =
            std::make_shared<const ResolvedMedia>(
                resolveMediaForItem_(*item)
            );
        shared = sharedMediaCache_.emplace(key, resolved).first;
    }

    mediaCache_[fullListIndex] = shared->second;
    return *mediaCache_[fullListIndex];
}

void ScrollingList::selectItemByName(std::string_view name)
{
    if (!items_ || items_->empty())
        return;
    
    size_t size = items_->size();
    size_t index = 0;

    for (size_t i = 0; i < size; ++i) {
        index = loopDecrement(itemIndex_, i, size);

        // Since items_ is likely storing std::string, using std::string_view for comparison is fine.
        if ((*items_)[(index + selectedOffsetIndex_) % size]->name == name) {
            itemIndex_ = index;
            break;
        }
    }
}

void ScrollingList::restartByMonitor(int monitor) const {
    for (Component* c : getComponents()) {
        if (c && c->baseViewInfo.Monitor == monitor)
            c->restart();
    }
}

const std::string& ScrollingList::getSelectedItemName() {
    if (!items_ || items_->empty()) {
        static const std::string empty = "";
        return empty;
    }

    size_t idx = loopIncrement(itemIndex_, selectedOffsetIndex_, items_->size());
    return (*items_)[idx]->name;
}

void ScrollingList::setScrollAcceleration( float value )
{
    scrollAcceleration_ = value;
}

void ScrollingList::setStartScrollTime( float value )
{
    startScrollTime_ = value;
}

void ScrollingList::setMinScrollTime( float value )
{
    minScrollTime_ = value;
}

void ScrollingList::enableTextFallback(bool value)
{
    textFallback_ = value;
}

void ScrollingList::deallocateSpritePoints() {
    if (components_.empty())
        return;

    int monitor = baseViewInfo.Monitor;
    std::vector<std::shared_ptr<IVideo>> pooledVideos;

    // Extract videos first, before deleting components
    for (Component* comp : components_.raw()) {
        if (comp) {
            auto video = comp->extractVideo();   // nullptr for Image/Text, valid for VideoComponent
            if (video)
                pooledVideos.push_back(std::move(video));
        }
    }

    // Delete components
    for (size_t i = 0; i < components_.size(); ++i) {
        deallocateTexture(i);  // This sets components_[i] = nullptr internally
    }

    // Batch release videos
    if (!pooledVideos.empty()) {
        VideoPool::releaseVideoBatch(pooledVideos, monitor, listId_);
    }
}

void ScrollingList::allocateSpritePoints() {
    if (!items_ || items_->empty()) return;
    if (!scrollPoints_ || scrollPoints_->empty()) return;
    if (components_.empty()) return;

    size_t itemsSize = items_->size();

    for (size_t i : visualPriorityOrder_()) {
        const size_t index = loopIncrement(itemIndex_, i, itemsSize);

        allocateTexture(i, index);
        if (Component* c = components_[i]) {
            c->allocateGraphicsMemory();
            ViewInfo* view = (*scrollPoints_)[i];
            resetTweens(c, (*tweenPoints_)[i], view, view, 0);
        }
    }

    refreshPreparationPriorityHints_();
}

void ScrollingList::reallocateSpritePoints() {
    if (!items_ || items_->empty()) return;
    if (!scrollPoints_ || scrollPoints_->empty()) return;
    if (components_.empty()) return;

    size_t scrollPointsSize = scrollPoints_->size();
    size_t itemsSize = items_->size();
    int monitor = baseViewInfo.Monitor;

    // --- NEW: Reset the pool before we start releasing the current batch ---
    // 1. Clears all idle/cached videos from VRAM instantly.
    // 2. Increments the generation ID, ensuring the videos we are about to 
    //    extract are marked as "obsolete".
    VideoPool::reset(baseViewInfo.Monitor, listId_);

    // --- Step 1: Extract video instances for batch release ---
    std::vector<VideoPool::VideoPtr> pooledVideos;

    for (size_t i = 0; i < scrollPointsSize; ++i) {
        Component* comp = components_[i];
        if (!comp) continue;

        auto video = comp->extractVideo();   // nullptr for Image/Text, valid for VideoComponent
        if (video)
            pooledVideos.push_back(std::move(video));
    }

    // --- Step 2: Batch release to pool ---
    if (!pooledVideos.empty()) {
        // Because we called reset() above, releaseVideo will now detect 
        // the generation mismatch and physically destroy these instances 
        // instead of caching them.
        VideoPool::releaseVideoBatch(pooledVideos, monitor, listId_);
    }

    // --- Step 4: Reallocate components and assign tweens ---
    for (size_t i : visualPriorityOrder_()) {
        size_t index = loopIncrement(itemIndex_, i, itemsSize);

        // allocateTexture will now call VideoPool::acquireVideo, which 
        // will find an empty pool and create brand-new instances.
        allocateTexture(i, index);

        Component* c = components_[i];
        if (c) {
            c->allocateGraphicsMemory();
            ViewInfo* view = (*scrollPoints_)[i];
            resetTweens(c, (*tweenPoints_)[i], view, view, 0);
        }
    }

    refreshPreparationPriorityHints_();
}

void ScrollingList::destroyItems() {
    // Pool cleanup is handled by freeGraphicsMemory() ? deallocateSpritePoints().
    // By the time destroyItems() is called from the destructor, all videos
    // have already been extracted and released. Just delete the component shells.
    auto& data = components_.raw();
    for (size_t i = 0; i < data.size(); ++i) {
        if (Component* component = data[i]) {
            delete component;
            data[i] = nullptr;
        }
    }
}


void ScrollingList::setPoints(std::vector<ViewInfo*>* scrollPoints,
    std::shared_ptr<std::vector<std::shared_ptr<AnimationEvents>>> tweenPoints) {
    
    resetImagePreload_();
    deallocateSpritePoints();

    clearPoints();
    clearTweenPoints();

    scrollPoints_ = scrollPoints;
    tweenPoints_ = std::move(tweenPoints);

    if (items_ && !items_->empty()) {
        itemIndex_ = loopDecrement(0, selectedOffsetIndex_, items_->size());
    }
    else {
        itemIndex_ = 0;
    }

    rebuildSlotTopology_();

    // Allocate and initialize components (existing behavior)
    allocateSpritePoints();
    rebuildLetterAnchors_();
    refreshImagePreloadQueue_();
}

void ScrollingList::rebuildSlotTopology_(bool initializeComponents) {
    const size_t N = (scrollPoints_ ? scrollPoints_->size() : 0);

    if (initializeComponents || components_.size() != N) {
        // Reset circular buffer (fills with nullptrs)
        components_.initialize(N);
    }

    // --- Precompute neighbor index maps ---
    forwardMap_.resize(N);
    backwardMap_.resize(N);

    for (size_t i = 0; i < N; ++i) {
        forwardMap_[i] = (N <= 1) ? 0 : (i == 0 ? N - 1 : i - 1);
        backwardMap_[i] = (N <= 1) ? 0 : (i + 1 == N ? 0 : i + 1);
    }

    // --- Precompute tween/view tuples (so scroll() is just lookups) ---
    forwardTween_.resize(N);
    backwardTween_.resize(N);

    if (N > 0 &&
        tweenPoints_ &&
        tweenPoints_->size() >= N &&
        scrollPoints_) {
        for (size_t i = 0; i < N; ++i) {
            const size_t jF = forwardMap_[i];
            const size_t jB = backwardMap_[i];

            forwardTween_[i] = TweenNeighbor{
                (*tweenPoints_)[jF],
                (*scrollPoints_)[i],
                (*scrollPoints_)[jF],
                buildTweenTemplate_(
                    *(*scrollPoints_)[i],
                    *(*scrollPoints_)[jF]
                ),
                (*scrollPoints_)[i]->Restart !=
                    (*scrollPoints_)[jF]->Restart
            };
            backwardTween_[i] = TweenNeighbor{
                (*tweenPoints_)[jB],
                (*scrollPoints_)[i],
                (*scrollPoints_)[jB],
                buildTweenTemplate_(
                    *(*scrollPoints_)[i],
                    *(*scrollPoints_)[jB]
                ),
                (*scrollPoints_)[i]->Restart !=
                    (*scrollPoints_)[jB]->Restart
            };
        }
    }
    else {
        forwardTween_.clear();
        backwardTween_.clear();
    }

    if (listId_ != -1 && N > 0) {
        const size_t desiredTotal = N + VideoPool::POOL_BUFFER_INSTANCES;  // or N + POOL_BUFFER_INSTANCES
        VideoPool::reserveCapacity(baseViewInfo.Monitor, listId_, desiredTotal);
    }
}


size_t ScrollingList::getScrollOffsetIndex( ) const
{
    return loopIncrement( itemIndex_, selectedOffsetIndex_, items_->size());
}

void ScrollingList::setScrollOffsetIndex( size_t index )
{
    itemIndex_ = loopDecrement( index, selectedOffsetIndex_, items_->size());
    refreshImagePreloadQueue_();
}

void ScrollingList::setSelectedIndex( int selectedIndex )
{
    selectedOffsetIndex_ = selectedIndex;
    refreshPreparationPriorityHints_();
    refreshImagePreloadQueue_();
}

Item *ScrollingList::getItemByOffset(int offset)
{
    // First, check if items_ is nullptr or empty
    if (!items_ || items_->empty()) return nullptr;
    
    size_t itemSize = items_->size();
    size_t index = getSelectedIndex();
    if (offset >= 0) {
        index = loopIncrement(index, offset, itemSize);
    }
    else {
        index = loopDecrement(index, offset, itemSize);
    }
    
    return (*items_)[index];
}

Item* ScrollingList::getSelectedItem()
{
    // First, check if items_ is nullptr or empty
    if (!items_ || items_->empty()) return nullptr;
    size_t itemSize = items_->size();
    
    return (*items_)[loopIncrement(itemIndex_, selectedOffsetIndex_, itemSize)];
}

void ScrollingList::pageUp()
{
    if (components_.empty()) return; // More idiomatic
    itemIndex_ = loopDecrement(itemIndex_, components_.size(), items_->size());
    refreshImagePreloadQueue_();
}

void ScrollingList::pageDown()
{
    if (components_.empty()) return; // More idiomatic
    itemIndex_ = loopIncrement(itemIndex_, components_.size(), items_->size());
    refreshImagePreloadQueue_();
}

void ScrollingList::random( )
{
    if (!items_ || items_->empty()) return;
    size_t itemSize = items_->size();
    
    itemIndex_ = rand( ) % itemSize;
    refreshImagePreloadQueue_();
}

void ScrollingList::letterUp( )
{
    letterChange( true );
}

void ScrollingList::letterDown( )
{
    letterChange( false );
}

void ScrollingList::letterChange(bool increment) {
    if (!items_ || items_->empty()) {
        return;
    }
    
    letterSkipTimer_ = 0.25f;

    const size_t itemSize = items_->size();

    // 2. Get the starting character for comparison (case-insensitively)
    const size_t startItemOriginalIndex = getSelectedIndex();
    std::string_view startTitle = (*items_)[startItemOriginalIndex]->fullTitle;

    // If the current item's title is empty, we can't do a letter jump.
    if (startTitle.empty()) {
        return;
    }
    const char startChar = startTitle[0];

    // Helper lambda to check if two characters represent a "letter change"
    auto isLetterBoundary = [](char charA, char charB) -> bool {
        // Cast to unsigned char is crucial for correctness with tolower/isalpha
        const unsigned char ucA = static_cast<unsigned char>(charA);
        const unsigned char ucB = static_cast<unsigned char>(charB);

        const bool isAlphaA = isalpha(ucA);
        const bool isAlphaB = isalpha(ucB);

        // Boundary is crossed if one is a letter and the other isn't...
        if (isAlphaA != isAlphaB) {
            return true;
        }

        // ...or if both are letters but they are different (case-insensitive).
        if (isAlphaA && (tolower(ucA) != tolower(ucB))) {
            return true;
        }

        return false;
        };


    // 3. Main loop: Find the first boundary in the specified direction.
    size_t newIndex = itemIndex_; // Start with current index
    for (size_t i = 1; i < itemSize; ++i) {
        // Get the next index to check, wrapping around the list
        const size_t loopIndex = increment ? loopIncrement(itemIndex_, i, itemSize)
            : loopDecrement(itemIndex_, i, itemSize);

        const size_t itemLookupIndex = loopIncrement(loopIndex, selectedOffsetIndex_, itemSize);
        std::string_view endTitle = (*items_)[itemLookupIndex]->fullTitle;

        if (endTitle.empty()) {
            continue; // Skip items with no title
        }

        if (isLetterBoundary(startChar, endTitle[0])) {
            newIndex = loopIndex; // We found the start of the next group
            break;
        }
    }

    // 4. Handle the special case for decrementing ("Previous Letter")
    // This logic aims to jump to the *start* of the previous letter group.
    if (!increment) {
        bool prevLetterSubToCurrent = false;
        config_.getProperty(OPTION_PREVLETTERSUBTOCURRENT, prevLetterSubToCurrent);

        // If the found item is the one right before our original start, the logic might need adjustment.
        const size_t foundItemOriginalIndex = loopIncrement(newIndex, selectedOffsetIndex_, itemSize);
        if ((*items_)[foundItemOriginalIndex] == (*items_)[startItemOriginalIndex]) {
            // This can happen if the list is very small or has only one letter group.
            // We didn't actually move, so no need to adjust.
        }
        // This complex condition checks if we need to do the "find the start of the previous group" logic.
        else if (!prevLetterSubToCurrent || loopDecrement(itemIndex_, 1, itemSize) == newIndex) {

            // We've jumped into a new group. Now, find the beginning of that group by searching backwards again.
            // The character we are looking for is the one at the `newIndex` we just found.
            std::string_view newGroupTitle = (*items_)[foundItemOriginalIndex]->fullTitle;
            if (!newGroupTitle.empty())
            {
                const char newGroupChar = newGroupTitle[0];

                // Scan backwards from our new position to find where this group started.
                for (size_t i = 1; i < itemSize; ++i) {
                    size_t prevIndexInGroup = loopDecrement(newIndex, i, itemSize);
                    size_t prevItemLookupIndex = loopIncrement(prevIndexInGroup, selectedOffsetIndex_, itemSize);
                    std::string_view prevTitle = (*items_)[prevItemLookupIndex]->fullTitle;

                    if (prevTitle.empty()) continue;

                    // If we find a *different* letter, it means the item *after* it was the true start.
                    if (isLetterBoundary(newGroupChar, prevTitle[0])) {
                        newIndex = loopIncrement(prevIndexInGroup, 1, itemSize); // The one after the boundary
                        break;
                    }
                }
            }
        }
        else {
            // The config is set to "jump to previous item" and we are not at the boundary,
            // so just move one step forward from the found boundary.
            newIndex = loopIncrement(newIndex, 1, itemSize);
        }
    }

    // 5. Update the list's index
    itemIndex_ = newIndex;
    refreshImagePreloadQueue_();
}

size_t ScrollingList::loopIncrement(size_t currentIndex, size_t incrementAmount, size_t N) const {
    if (N == 0) return 0;

    if (currentIndex >= N) currentIndex -= N;

    if (incrementAmount >= N) incrementAmount %= N;

    size_t next = currentIndex + incrementAmount;
    if (next >= N) next -= N;
    return next;
}

size_t ScrollingList::loopDecrement(size_t currentIndex, size_t decrementAmount, size_t N) const {
    if (N == 0) return 0;

    if (currentIndex >= N) currentIndex -= N;

    if (decrementAmount >= N) decrementAmount %= N;

    const size_t add = N - decrementAmount;
    size_t next = currentIndex + add;
    if (next >= N) next -= N;
    return next;
}

void ScrollingList::metaUp(const std::string& attribute)
{
    metaChange(true, attribute);
}

void ScrollingList::metaDown(const std::string& attribute)
{
    metaChange(false, attribute);
}

void ScrollingList::metaChange(bool increment, const std::string& attribute)
{
    if (!items_ || items_->empty()) return;
    
    letterSkipTimer_ = 0.15f;
    
    size_t itemSize = items_->size();

    const Item* startItem = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize];
    std::string startValue = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->getMetaAttribute(attribute);

    for (size_t i = 0; i < itemSize; ++i) {
        size_t index = increment ? loopIncrement(itemIndex_, i, itemSize) : loopDecrement(itemIndex_, i, itemSize);
        std::string endValue = (*items_)[(index + selectedOffsetIndex_) % itemSize]->getMetaAttribute(attribute);

        if (startValue != endValue) {
            itemIndex_ = index;
            break;
        }
    }

    if (!increment) {
        bool prevLetterSubToCurrent = false;
        config_.getProperty(OPTION_PREVLETTERSUBTOCURRENT, prevLetterSubToCurrent);
        if (!prevLetterSubToCurrent || (*items_)[(itemIndex_ + 1 + selectedOffsetIndex_) % itemSize] == startItem) {
            startValue = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->getMetaAttribute(attribute);

            for (size_t i = 0; i < itemSize; ++i) {
                size_t index = loopDecrement(itemIndex_, i, itemSize);
                std::string endValue = (*items_)[(index + selectedOffsetIndex_) % itemSize]->getMetaAttribute(attribute);

                if (startValue != endValue) {
                    itemIndex_ = loopIncrement(index, 1, itemSize);
                    break;
                }
            }
        }
        else
        {
            itemIndex_ = loopIncrement(itemIndex_, 1, itemSize);
        }
    }

    refreshImagePreloadQueue_();
}

void ScrollingList::subChange(bool increment)
{
    if (!items_ || items_->empty()) return;
    size_t itemSize = items_->size();

    const Item* startItem = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize];
    std::string startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

    for (size_t i = 0; i < itemSize; ++i) {
        size_t index = increment ? loopIncrement(itemIndex_, i, itemSize) : loopDecrement(itemIndex_, i, itemSize);
        std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

        if (startname != endname) {
            itemIndex_ = index;
            break;
        }
    }

    if (!increment) // For decrement, find the first game of the new sub
    {
        bool prevLetterSubToCurrent = false;
        config_.getProperty(OPTION_PREVLETTERSUBTOCURRENT, prevLetterSubToCurrent);
        if (!prevLetterSubToCurrent || (*items_)[(itemIndex_ + 1 + selectedOffsetIndex_) % itemSize] == startItem) {
            startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

            for (size_t i = 0; i < itemSize; ++i) {
                size_t index = loopDecrement(itemIndex_, i, itemSize);
                std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

                if (startname != endname) {
                    itemIndex_ = loopIncrement(index, 1, itemSize);
                    break;
                }
            }
        }
        else {
            itemIndex_ = loopIncrement(itemIndex_, 1, itemSize);
        }
    }

    refreshImagePreloadQueue_();
}

void ScrollingList::cfwLetterSubUp()
{
    if (!items_ || items_->empty())
        return;

    if (Utils::toLower(collectionName) != (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName())
        subChange(true);
    else
        letterChange(true);
}

void ScrollingList::cfwLetterSubDown()
{
    if (!items_ || items_->empty())
        return;
    
    if (Utils::toLower(collectionName) != (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName()) {
        subChange(false);
        if (Utils::toLower(collectionName) == (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName()) {
            subChange(true);
            letterChange(false);
        }
    }
    else {
        letterChange(false);
        if (Utils::toLower(collectionName) != (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName()) {
            letterChange(true);
            subChange(false);
        }
    }
}

void ScrollingList::allocateGraphicsMemory( )
{
    Component::allocateGraphicsMemory( );
    scrollPeriod_ = startScrollTime_;

    allocateSpritePoints( );
    rebuildLetterAnchors_();
    refreshImagePreloadQueue_();
}

void ScrollingList::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    resetScrollPeriod();
    resetImagePreload_();

    deallocateSpritePoints();

    if (listId_ != -1) {
        VideoPool::cleanup(baseViewInfo.Monitor, listId_);
    }
}

void ScrollingList::pumpGraphicsPreparation() {
    for (Component* c : components_.raw()) {
        if (c) {
            c->pumpGraphicsPreparation();
        }
    }
}

void ScrollingList::waitForGraphicsPreparation() {
    const size_t count = std::min(
        components_.size(),
        scrollPoints_ ? scrollPoints_->size() : size_t{ 0 }
    );

    for (size_t i = 0; i < count; ++i) {
        Component* component = components_[i];
        if (component && preparationTierForIndex_(i) <= 1) {
            component->waitForGraphicsPreparation();
        }
    }
}

bool ScrollingList::isGraphicsReadyForFirstRender() const {
    const size_t count = std::min(
        components_.size(),
        scrollPoints_ ? scrollPoints_->size() : size_t{ 0 }
    );

    for (size_t i = 0; i < count; ++i) {
        Component* component = components_[i];
        if (component &&
            preparationTierForIndex_(i) <= 1 &&
            !component->isGraphicsReadyForFirstRender())
        {
            return false;
        }
    }
    return true;
}

void ScrollingList::triggerEnterEvent( )
{
    triggerEventOnAll("enter", 0);
}

void ScrollingList::triggerExitEvent( )
{
    triggerEventOnAll("exit", 0);
}

void ScrollingList::triggerMenuEnterEvent( int menuIndex )
{
    triggerEventOnAll("menuEnter", menuIndex);
}

void ScrollingList::triggerMenuExitEvent( int menuIndex )
{
    triggerEventOnAll("menuExit", menuIndex);
}

void ScrollingList::triggerGameEnterEvent( int menuIndex )
{
    triggerEventOnAll("gameEnter", menuIndex);
}

void ScrollingList::triggerTrackChangeEvent(int menuIndex) {
    triggerEventOnAll("trackChange", menuIndex);
}

void ScrollingList::triggerGameExitEvent( int menuIndex )
{
    triggerEventOnAll("gameExit", menuIndex);
}

void ScrollingList::triggerHighlightEnterEvent( int menuIndex )
{
    triggerEventOnAll("highlightEnter", menuIndex);
}

void ScrollingList::triggerHighlightExitEvent( int menuIndex )
{
    triggerEventOnAll("highlightExit", menuIndex);
}

void ScrollingList::triggerPlaylistEnterEvent( int menuIndex )
{
    triggerEventOnAll("playlistEnter", menuIndex);
}

void ScrollingList::triggerPlaylistExitEvent( int menuIndex )
{
    triggerEventOnAll("playlistExit", menuIndex);
}

void ScrollingList::triggerMenuJumpEnterEvent( int menuIndex )
{
    triggerEventOnAll("menuJumpEnter", menuIndex);
}

void ScrollingList::triggerMenuJumpExitEvent( int menuIndex )
{
    triggerEventOnAll("menuJumpExit", menuIndex);
}

void ScrollingList::triggerAttractEnterEvent( int menuIndex )
{
    triggerEventOnAll("attractEnter", menuIndex);
}

void ScrollingList::triggerAttractEvent( int menuIndex )
{
    triggerEventOnAll("attract", menuIndex);
}

void ScrollingList::triggerAttractExitEvent( int menuIndex )
{
    triggerEventOnAll("attractExit", menuIndex);
}

void ScrollingList::triggerGameInfoEnter(int menuIndex)
{
    triggerEventOnAll("gameInfoEnter", menuIndex);
}
void ScrollingList::triggerGameInfoExit(int menuIndex)
{
    triggerEventOnAll("gameInfoExit", menuIndex);
}

void ScrollingList::triggerCollectionInfoEnter(int menuIndex)
{
    triggerEventOnAll("collectionInfoEnter", menuIndex);
}
void ScrollingList::triggerCollectionInfoExit(int menuIndex)
{
    triggerEventOnAll("collectionInfoExit", menuIndex);
}

void ScrollingList::triggerBuildInfoEnter(int menuIndex)
{
    triggerEventOnAll("buildInfoEnter", menuIndex);
}
void ScrollingList::triggerBuildInfoExit(int menuIndex)
{
    triggerEventOnAll("buildInfoExit", menuIndex);
}

void ScrollingList::triggerJukeboxJumpEvent( int menuIndex )
{
    triggerEventOnAll("jukeboxJump", menuIndex);
}

void ScrollingList::triggerEventOnAll(const std::string& event, int menuIndex) {
    size_t componentSize = components_.size();
    for (size_t i = 0; i < componentSize; ++i) {
        Component* c = components_[i];
        if (c) c->triggerEvent(event, menuIndex);
    }

    // Animations triggered; we are no longer idle
    cachedIdle_ = false;
    cachedAttractIdle_ = false;
}

bool ScrollingList::update(float dt) {
    currentDt_ = dt;
    bool done = Component::update(dt);

    // 1. Handle Letter Skip Timer
    if (letterSkipTimer_ > 0.0f) {
        letterSkipTimer_ -= dt;
        if (letterSkipTimer_ <= 0.0f) letterSkipTimer_ = 0.0f;
    }

    // 2. Child Component Updates (Evaluate idle state)
    const size_t scrollPointsSize =
        scrollPoints_ ? scrollPoints_->size() : 0;

    bool allIdle = Component::isIdle();
    bool allAttractIdle = Component::isAttractIdle();

    for (size_t i = 0; i < scrollPointsSize; ++i) {
        Component* c = components_[i];
        if (c) {
            done &= c->update(dt);
            if (!c->isIdle()) allIdle = false;
            if (!c->isAttractIdle()) allAttractIdle = false;
        }
    }

    cachedIdle_ = allIdle;
    cachedAttractIdle_ = allAttractIdle;
    pumpImagePreload_();

    // Notice: The movement logic is gone from here! It lives in Page.cpp now.
    return done;
}

size_t ScrollingList::getSelectedIndex( ) const
{
    if ( !items_ ) return 0;
    return loopIncrement( itemIndex_, selectedOffsetIndex_, items_->size( ) );
}

void ScrollingList::setItemIndex( unsigned int index )
{
     if ( !items_ ) return;
     itemIndex_ = loopDecrement( index, selectedOffsetIndex_, items_->size( ) );
     refreshImagePreloadQueue_();
}

size_t ScrollingList::getSize() const
{
    if ( !items_ ) return 0;
    return items_->size();
}

TweenSet ScrollingList::buildTweenTemplate_(
    const ViewInfo& current,
    const ViewInfo& next) const
{
    constexpr float EPSILON_FLOAT = 0.0001f;
    TweenSet set;

    auto addFloat = [&set](
        TweenProperty property,
        float start,
        float end)
    {
        set.push(Tween(property, LINEAR, start, end, 0.0f));
    };

    if (std::abs(current.Height - next.Height) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_HEIGHT, current.Height, next.Height);
    if (std::abs(current.Width - next.Width) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_WIDTH, current.Width, next.Width);
    if (std::abs(current.Angle - next.Angle) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_ANGLE, current.Angle, next.Angle);
    if (std::abs(current.Alpha - next.Alpha) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_ALPHA, current.Alpha, next.Alpha);
    if (std::abs(current.X - next.X) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_X, current.X, next.X);
    if (std::abs(current.Y - next.Y) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_Y, current.Y, next.Y);
    if (std::abs(current.XOrigin - next.XOrigin) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_X_ORIGIN, current.XOrigin, next.XOrigin);
    if (std::abs(current.YOrigin - next.YOrigin) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_Y_ORIGIN, current.YOrigin, next.YOrigin);
    if (std::abs(current.XOffset - next.XOffset) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_X_OFFSET, current.XOffset, next.XOffset);
    if (std::abs(current.YOffset - next.YOffset) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_Y_OFFSET, current.YOffset, next.YOffset);
    if (std::abs(current.FontSize - next.FontSize) > EPSILON_FLOAT)
        addFloat(
            TWEEN_PROPERTY_FONT_SIZE,
            current.FontSize,
            next.FontSize
        );
    if (std::abs(current.MaxWidth - next.MaxWidth) > EPSILON_FLOAT)
        addFloat(
            TWEEN_PROPERTY_MAX_WIDTH,
            current.MaxWidth,
            next.MaxWidth
        );
    if (std::abs(current.MaxHeight - next.MaxHeight) > EPSILON_FLOAT)
        addFloat(
            TWEEN_PROPERTY_MAX_HEIGHT,
            current.MaxHeight,
            next.MaxHeight
        );
    if (current.Layer != next.Layer)
        addFloat(
            TWEEN_PROPERTY_LAYER,
            static_cast<float>(current.Layer),
            static_cast<float>(next.Layer)
        );
    if (std::abs(current.Volume - next.Volume) > EPSILON_FLOAT)
        addFloat(TWEEN_PROPERTY_VOLUME, current.Volume, next.Volume);
    if (current.Monitor != next.Monitor)
        addFloat(
            TWEEN_PROPERTY_MONITOR,
            static_cast<float>(current.Monitor),
            static_cast<float>(next.Monitor)
        );

    return set;
}

void ScrollingList::resetTweens(
    Component* c,
    const std::shared_ptr<AnimationEvents>& sets,
    ViewInfo* currentViewInfo,
    ViewInfo* nextViewInfo,
    float scrollTime,
    const TweenSet* transitionTemplate,
    bool restartChanges) const
{
    if (!c || !sets || !currentViewInfo || !nextViewInfo) return;

    c->setTweens(sets);

    // Fetch a pointer to the specific Animation object in the map
    Animation* scrollAnimation = sets->getAnimation("menuScroll");
    if (!scrollAnimation)
        return;
    scrollAnimation->Clear();

    // Backup only the fields this function temporarily patches.
    const float oldCurImageHeight = currentViewInfo->ImageHeight;
    const float oldCurImageWidth = currentViewInfo->ImageWidth;

    const float oldNextImageHeight = nextViewInfo->ImageHeight;
    const float oldNextImageWidth = nextViewInfo->ImageWidth;
    const float oldNextBackgroundAlpha = nextViewInfo->BackgroundAlpha;
    const float componentBackgroundAlpha =
        c->baseViewInfo.BackgroundAlpha;

    // Temporary runtime patch for correct image rendering during this tween setup.
    currentViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    currentViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;

    nextViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    nextViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;
    nextViewInfo->BackgroundAlpha = c->baseViewInfo.BackgroundAlpha;

    // Reset baseViewInfo to the scroll start position.
    // This now sees the temporarily patched ImageWidth/ImageHeight values.
    c->baseViewInfo = *currentViewInfo;


    TweenSet set = transitionTemplate
        ? *transitionTemplate
        : buildTweenTemplate_(*currentViewInfo, *nextViewInfo);
    const float EPSILON_FLOAT = 0.0001f;

    for (size_t i = 0; i < set.size(); ++i) {
        if (Tween* tween = set.getTween(static_cast<unsigned int>(i))) {
            tween->duration = scrollTime;
        }
    }

    if ((restartChanges ||
        currentViewInfo->Restart != nextViewInfo->Restart) &&
        scrollTime > 0.0f)
    {
        set.push(Tween(
            TWEEN_PROPERTY_RESTART,
            LINEAR,
            static_cast<float>(currentViewInfo->Restart),
            static_cast<float>(nextViewInfo->Restart),
            0.0f
        ));
    }

    if (std::abs(
        currentViewInfo->BackgroundAlpha -
        componentBackgroundAlpha) > EPSILON_FLOAT)
    {
        set.push(Tween(
            TWEEN_PROPERTY_BACKGROUND_ALPHA,
            LINEAR,
            currentViewInfo->BackgroundAlpha,
            componentBackgroundAlpha,
            scrollTime
        ));
    }

    // C++20: Use std::move to trigger the rvalue overload and avoid deep-copying the vector
    if (set.size() > 0) {
        scrollAnimation->Push(std::move(set));
    }

    // Restore layout slot definitions so stale image dimensions do not leak
// into future playlist/menu states.
    currentViewInfo->ImageHeight = oldCurImageHeight;
    currentViewInfo->ImageWidth = oldCurImageWidth;

    nextViewInfo->ImageHeight = oldNextImageHeight;
    nextViewInfo->ImageWidth = oldNextImageWidth;
    nextViewInfo->BackgroundAlpha = oldNextBackgroundAlpha;
}

bool ScrollingList::allocateTexture(size_t componentIndex, size_t fullListIndex) {
    if (!items_ || componentIndex >= components_.size() ||
        fullListIndex >= items_->size()) return false;

    Component* existingComponent = components_[componentIndex];
    components_[componentIndex] = nullptr;

    const ResolvedMedia& media = resolveMediaAt_(fullListIndex);
    const Item* item = (*items_)[fullListIndex];
    Component* t = nullptr;

    ImageBuilder imageBuild;
    VideoBuilder videoBuild;

    const std::string& selectedItemName = getSelectedItemName();
    const bool isSelectedItem = (selectedImage_ && item->name == selectedItemName);
    bool loadedNormalImage = false;

    // 1. Instantly load Video from exact resolved path
    if (!media.videoPath.empty()) {
        std::string logicalName = item->name; // Basic fallback name
        t = videoBuild.createVideoFromResolved(media.videoPath, logicalName, page, baseViewInfo.Monitor, -1, false, listId_, perspectiveCornersInitialized_ ? perspectiveCorners_ : nullptr, existingComponent);
    }

    // 2. Instantly load Image from exact resolved path
    if (!t) {
        std::string imageToLoad = (isSelectedItem && !media.selectedImagePath.empty()) ? media.selectedImagePath : media.idleImagePath;
        if (!imageToLoad.empty()) {
            t = imageBuild.CreateImageFromResolved(imageToLoad, page, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_, existingComponent);
            loadedNormalImage =
                imageToLoad == media.idleImagePath;
        }
    }

    // 3. Fallback to Text
    if (!t && textFallback_) {
        if (existingComponent && existingComponent->recycleAsText(item->title)) {
            t = existingComponent;
        }
        else {
            t = new Text(item->title, page, fontInst_, baseViewInfo.Monitor);
        }
    }

    // Finalize
    if (t) {
        t->playlistName = playlistName;
        t->setHighPriority(componentIndex == selectedOffsetIndex_);

        // CLEAN FIX: Instantly arm the component with its slot's baseline position and tween definitions
        if (scrollPoints_ && componentIndex < scrollPoints_->size()) {
            t->baseViewInfo = *(*scrollPoints_)[componentIndex];
        }
        if (tweenPoints_ && componentIndex < tweenPoints_->size()) {
            t->setTweens((*tweenPoints_)[componentIndex]);
        }

        components_[componentIndex] = t;

        if (media.videoPath.empty() && loadedNormalImage) {
            imagePreloadAttempted_.insert(fullListIndex);
        }
    }

    if (existingComponent != nullptr && existingComponent != t) {
        delete existingComponent;
    }

    return true;
}

void ScrollingList::buildPaths(std::string& imagePath, std::string& videoPath, const std::string& base, const std::string& subPath, const std::string& mediaType, const std::string& videoType) const {
    imagePath = Utils::combinePath(base, subPath, "medium_artwork", mediaType);
    videoPath = Utils::combinePath(base, subPath, "medium_artwork", videoType);
}

void ScrollingList::deallocateTexture(size_t index) {
    if (components_.size() <= index) return;

    Component* s = components_[index];

    if (s) {
        //s->freeGraphicsMemory();
        delete s;
        components_[index] = nullptr;
    }
}


const std::vector<Component*>& ScrollingList::getComponents() const {
    return components_.raw();
}

bool ScrollingList::isScrollingListIdle() {
    return cachedIdle_;
}

bool ScrollingList::isScrollingListAttractIdle() {
    return cachedAttractIdle_;
}

void ScrollingList::resetScrollPeriod() {
    scrollPeriod_ = startScrollTime_;
    coasting_ = false;
    coastElapsed_ = 0.0f;
    coastStartPeriod_ = 0.0f;
}

void ScrollingList::updateScrollPeriod() {
    if (scrollPeriod_ <= 0.0f) {
        scrollPeriod_ = startScrollTime_;
    }

    scrollPeriod_ -= scrollAcceleration_;

    if (scrollPeriod_ < minScrollTime_) {
        scrollPeriod_ = minScrollTime_;
    }
}

void ScrollingList::setCoastFriction(float value) {
    coastFriction_ = std::max(0.0f, value);
}

bool ScrollingList::canBeginCoast() const {
    constexpr float EPSILON = 0.0001f;
    return coastFriction_ > 0.0f &&
        scrollPeriod_ <= minScrollTime_ + EPSILON;
}

void ScrollingList::beginCoast() {
    if (!canBeginCoast()) {
        coasting_ = false;
        coastElapsed_ = 0.0f;
        coastStartPeriod_ = 0.0f;
        return;
    }

    coasting_ = true;
    coastElapsed_ = 0.0f;

    // Do not start coast at minScrollTime_; that produces the steppy burst.
    const float coastEntryPeriod =
        minScrollTime_ + (startScrollTime_ - minScrollTime_) * 0.45f;

    coastStartPeriod_ = std::clamp(coastEntryPeriod, minScrollTime_, startScrollTime_);
    scrollPeriod_ = coastStartPeriod_;
}

void ScrollingList::decelerateScrollPeriod(float dt) {
    if (!coasting_) {
        scrollPeriod_ = startScrollTime_;
        return;
    }

    const float coastDurationSeconds =
        std::clamp(coastFriction_ * 0.20f, 0.10f, 0.75f);

    coastElapsed_ += dt;

    float t = coastElapsed_ / coastDurationSeconds;
    if (t >= 1.0f) {
        t = 1.0f;
        coasting_ = false;
    }

    const float eased = t * t * (3.0f - 2.0f * t);

    scrollPeriod_ =
        coastStartPeriod_ +
        (startScrollTime_ - coastStartPeriod_) * eased;
}

bool ScrollingList::canCoast() const {
    return coasting_;
}

bool ScrollingList::isFastScrolling() const {
    if (letterSkipTimer_ > 0.0f)
        return true;

    constexpr float EPSILON = 0.0001f;
    return scrollPeriod_ <= minScrollTime_ + EPSILON;
}

void ScrollingList::syncToSelectedIndex(size_t selectedIndex) {
    if (!items_ || items_->empty()) {
        return;
    }

    itemIndex_ = loopDecrement(selectedIndex, selectedOffsetIndex_, items_->size());
    refreshImagePreloadQueue_();
}

float ScrollingList::getScrollPeriod() const {
    return scrollPeriod_ > 0.0f ? scrollPeriod_ : startScrollTime_;
}

void ScrollingList::setScrollPeriod(float value) {
    scrollPeriod_ = value > 0.0f ? value : startScrollTime_;
}

size_t ScrollingList::nextSelectedIndex(bool forward) const {
    if (!items_ || items_->empty())
        return 0;

    const size_t current = getSelectedIndex();
    const size_t size = items_->size();

    return forward
        ? loopIncrement(current, 1, size)
        : loopDecrement(current, 1, size);
}

void ScrollingList::scrollToSelectedIndex(size_t newSelectedIndex, bool forward, float scrollTime) {
    if (!items_ || items_->empty() || !scrollPoints_ || scrollPoints_->empty())
        return;

    if (components_.empty())
        return;

    const size_t itemsSize = items_->size();
    const size_t N = scrollPoints_->size();

    if (N == 0 || components_.size() < N)
        return;

    if (scrollTime <= 0.0f) {
        scrollTime = startScrollTime_;
    }

    const size_t oldItemIndex = itemIndex_;
    const size_t newItemIndex =
        loopDecrement(newSelectedIndex, selectedOffsetIndex_, itemsSize);

    const size_t exitIndex =
        (N <= 1) ? 0 : (N - 1) * (1 - static_cast<size_t>(forward));

    const size_t fullListIndex = forward
        ? loopIncrement(oldItemIndex, N, itemsSize)
        : loopDecrement(oldItemIndex, 1, itemsSize);

    allocateTexture(exitIndex, fullListIndex);

    if (components_[exitIndex]) {
        components_[exitIndex]->allocateGraphicsMemory();
    }

    const auto& T = forward ? forwardTween_ : backwardTween_;

    if (T.size() == N) {
        for (size_t index = 0; index < N; ++index) {
            Component* component = components_[index];
            if (!component)
                continue;

            const TweenNeighbor& t = T[index];

            resetTweens(
                component,
                t.tween,
                t.cur,
                t.next,
                scrollTime,
                &t.transitionTemplate,
                t.restartChanges
            );

            if (component->baseViewInfo.font != t.next->font) {
                component->baseViewInfo.font = t.next->font;
            }

            component->triggerEvent("menuScroll");
        }
    }

    components_.rotate(forward);
    refreshPreparationPriorityHints_();

    itemIndex_ = newItemIndex;
    refreshImagePreloadQueue_(true, forward);

    cachedIdle_ = false;
    cachedAttractIdle_ = false;

    LOG_DEBUG("ScrollingList",
        "scrollToSelectedIndex() itemIndex=" + std::to_string(itemIndex_) +
        " selected=" + std::to_string(getSelectedIndex()) +
        " target=" + std::to_string(newSelectedIndex) +
        " period=" + std::to_string(scrollTime));
}

void ScrollingList::scroll(bool forward) {
    if (!items_ || items_->empty() || !scrollPoints_ || scrollPoints_->empty())
        return;

    if (components_.empty())
        return;

    if (scrollPeriod_ <= 0.0f) {
        scrollPeriod_ = startScrollTime_;
    }

    const size_t itemsSize = items_->size();
    const size_t N = scrollPoints_->size();

    if (N == 0 || components_.size() < N)
        return;

    const size_t exitIndex =
        (N <= 1) ? 0 : (N - 1) * (1 - static_cast<size_t>(forward));

    size_t fullListIndex = 0;

    if (forward) {
        fullListIndex = loopIncrement(itemIndex_, N, itemsSize);
        itemIndex_ = loopIncrement(itemIndex_, 1, itemsSize);
    }
    else {
        fullListIndex = loopDecrement(itemIndex_, 1, itemsSize);
        itemIndex_ = loopDecrement(itemIndex_, 1, itemsSize);
    }

    allocateTexture(exitIndex, fullListIndex);

    if (components_[exitIndex]) {
        components_[exitIndex]->allocateGraphicsMemory();
    }

    const auto& T = forward ? forwardTween_ : backwardTween_;

    if (T.size() == N) {
        for (size_t index = 0; index < N; ++index) {
            Component* component = components_[index];
            if (!component)
                continue;

            const TweenNeighbor& t = T[index];

            resetTweens(
                component,
                t.tween,
                t.cur,
                t.next,
                scrollPeriod_,
                &t.transitionTemplate,
                t.restartChanges
            );

            if (component->baseViewInfo.font != t.next->font) {
                component->baseViewInfo.font = t.next->font;
            }

            component->triggerEvent("menuScroll");
        }
    }

    components_.rotate(forward);
    refreshPreparationPriorityHints_();
    refreshImagePreloadQueue_(true, forward);

    cachedIdle_ = false;
    cachedAttractIdle_ = false;

    LOG_DEBUG("ScrollingList",
        "scroll() after itemIndex=" + std::to_string(itemIndex_) +
        " selected=" + std::to_string(getSelectedIndex()) +
        " period=" + std::to_string(scrollPeriod_));
}

bool ScrollingList::isPlaylist() const
{
    return playlistType_;
}

unsigned int ScrollingList::getVisualPriorityLayer() const {
    const unsigned int bestTier = getVisualPriorityTier();
    unsigned int highestLayer = 0;
    bool found = false;

    if (!scrollPoints_) {
        return baseViewInfo.Layer;
    }

    const size_t count =
        std::min(scrollPoints_->size(), components_.size());

    for (size_t i = 0; i < count; ++i) {
        if (preparationTierForIndex_(i) == bestTier) {
            highestLayer = std::max(highestLayer, layerForIndex_(i));
            found = true;
        }
    }

    return found ? highestLayer : baseViewInfo.Layer;
}

unsigned int ScrollingList::getVisualPriorityTier() const {
    constexpr unsigned int DORMANT_TIER = 2;
    unsigned int bestTier = DORMANT_TIER;

    if (!scrollPoints_) {
        return bestTier;
    }

    const size_t count =
        std::min(scrollPoints_->size(), components_.size());

    for (size_t i = 0; i < count; ++i) {
        bestTier = std::min(bestTier, preparationTierForIndex_(i));
    }

    return bestTier;
}

bool ScrollingList::isSlotVisible_(size_t index) const {
    if (!scrollPoints_ || index >= scrollPoints_->size()) {
        return false;
    }

    if (index < components_.size()) {
        const Component* component = components_[index];
        if (component) {
            return component->isVisibleForGraphicsPreparation();
        }
    }

    const ViewInfo* view = (*scrollPoints_)[index];
    if (!view || view->Alpha <= 0.0f) {
        return false;
    }

    const float width = view->ScaledWidth();
    const float height = view->ScaledHeight();

    if (!std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0f || height <= 0.0f) {
        return true;
    }

    const float viewportWidth =
        static_cast<float>(page.getLayoutWidthByMonitor(view->Monitor));
    const float viewportHeight =
        static_cast<float>(page.getLayoutHeightByMonitor(view->Monitor));

    if (viewportWidth <= 0.0f || viewportHeight <= 0.0f) {
        return true;
    }

    const float x = view->XRelativeToOrigin();
    const float y = view->YRelativeToOrigin();

    return x + width > 0.0f && x < viewportWidth &&
        y + height > 0.0f && y < viewportHeight;
}

unsigned int ScrollingList::preparationTierForIndex_(size_t index) const {
    if (isSlotVisible_(index)) {
        return 0;
    }

    if (index < components_.size()) {
        const Component* component = components_[index];
        if (component && !component->isIdle()) {
            return 0;
        }
    }

    return index == selectedOffsetIndex_ ? 1 : 2;
}

unsigned int ScrollingList::layerForIndex_(size_t index) const {
    if (index < components_.size()) {
        const Component* component = components_[index];
        if (component) {
            return component->baseViewInfo.Layer;
        }
    }

    if (scrollPoints_ && index < scrollPoints_->size() &&
        (*scrollPoints_)[index]) {
        return (*scrollPoints_)[index]->Layer;
    }

    return baseViewInfo.Layer;
}

void ScrollingList::refreshPreparationPriorityHints_() {
    for (size_t i = 0; i < components_.size(); ++i) {
        Component* component = components_[i];
        if (component) {
            component->setHighPriority(i == selectedOffsetIndex_);
        }
    }
}

std::vector<size_t> ScrollingList::visualPriorityOrder_() const {
    std::vector<size_t> order;

    if (!scrollPoints_) {
        return order;
    }

    const size_t count =
        std::min(scrollPoints_->size(), components_.size());

    order.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        order.push_back(i);
    }

    std::stable_sort(
        order.begin(),
        order.end(),
        [this](size_t lhs, size_t rhs) {
            const unsigned int lhsTier =
                preparationTierForIndex_(lhs);
            const unsigned int rhsTier =
                preparationTierForIndex_(rhs);

            if (lhsTier != rhsTier) {
                return lhsTier < rhsTier;
            }

            const unsigned int lhsLayer = layerForIndex_(lhs);
            const unsigned int rhsLayer = layerForIndex_(rhs);

            if (lhsLayer != rhsLayer) {
                return lhsLayer > rhsLayer;
            }

            const bool lhsVisible = isSlotVisible_(lhs);
            const bool rhsVisible = isSlotVisible_(rhs);

            if (lhsVisible != rhsVisible) {
                return lhsVisible;
            }

            const bool lhsSelected = lhs == selectedOffsetIndex_;
            const bool rhsSelected = rhs == selectedOffsetIndex_;
            return lhsSelected && !rhsSelected;
        }
    );

    return order;
}

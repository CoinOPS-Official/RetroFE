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
#include <SDL3_image/SDL_image.h>
#include <sstream>
#include <cctype>
#include <iomanip>
#include <algorithm>

int ScrollingList::nextListId = 0;

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
    items_ = items;
    if (!items_) return;

    const size_t size = items_->size();
    itemIndex_ = loopDecrement(size, selectedOffsetIndex_, size);

    resetScrollPeriod();

    // ---- warm name-candidate caches for the types this list actually uses ----
    const std::string imageTypeLC = Utils::toLower(imageType_);
    const std::string videoTypeLC = Utils::toLower(videoType_);

    std::vector<std::string> types;
    types.reserve(2);
    if (!imageTypeLC.empty()) types.push_back(imageTypeLC);
    if (!videoTypeLC.empty() && videoTypeLC != "null" && videoTypeLC != imageTypeLC)
        types.push_back(videoTypeLC);

    if (!types.empty()) {
        for (Item* it : *items_) {
            if (!it) continue;
            it->precomputeNameCandidates(types);   // builds once; selection-agnostic
        }
    }
    precalculateMediaPaths();

}

void ScrollingList::precalculateMediaPaths() {
    if (!items_ || items_->empty()) return;

    mediaCache_.clear();
    mediaCache_.resize(items_->size());

    const std::string& selectedItemName = getSelectedItemName();

    for (size_t i = 0; i < items_->size(); ++i) {
        Item* item = (*items_)[i];
        ResolvedMedia media;

        const bool isSelectedItem = (selectedImage_ && item->name == selectedItemName);
        const std::vector<std::string_view>& cachedNames = item->baseNameCandidates(imageTypeLC_);

        for (size_t iter = 0; iter <= cachedNames.size(); ++iter) {
            std::string name = (iter < cachedNames.size()) ? std::string(cachedNames[iter]) : "default";
            std::string imagePath, videoPath;

            if (layoutMode_) {
                std::string subPath = commonMode_ ? "_common" : collectionName;
                buildPaths(imagePath, videoPath, layoutCollectionsBase_, subPath, imageType_, videoType_);
            }
            else {
                if (commonMode_) {
                    buildPaths(imagePath, videoPath, commonCollectionsBase_, "", imageType_, videoType_);
                }
                else {
                    config_.getMediaPropertyAbsolutePath(collectionName, imageType_, false, imagePath);
                    config_.getMediaPropertyAbsolutePath(collectionName, videoType_, false, videoPath);
                }
            }

            // Try resolving primary paths
            if (media.videoPath.empty() && videoType_ != "null") {
                VideoBuilder::resolveVideoPath(videoPath, name, media.videoPath);
            }
            if (media.idleImagePath.empty() && !imageType_.empty()) {
                ImageBuilder::resolveImagePath(imagePath, name, media.idleImagePath);
                if (isSelectedItem) {
                    ImageBuilder::resolveImagePath(imagePath, name + "-selected", media.selectedImagePath);
                }
            }

            // If we found both, stop searching
            if (!media.videoPath.empty() && !media.idleImagePath.empty()) break;

            // Try Item-specific paths
            std::string itemImagePath, itemVideoPath;
            if (layoutMode_) {
                buildPaths(itemImagePath, itemVideoPath, layoutCollectionsBase_, item->collectionInfo->name, imageType_, videoType_);
            }
            else {
                config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, imageType_, false, itemImagePath);
                config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, videoType_, false, itemVideoPath);
            }

            if (media.videoPath.empty() && videoType_ != "null") {
                VideoBuilder::resolveVideoPath(itemVideoPath, name, media.videoPath);
            }
            if (media.idleImagePath.empty() && !imageType_.empty()) {
                ImageBuilder::resolveImagePath(itemImagePath, name, media.idleImagePath);
                if (isSelectedItem) {
                    ImageBuilder::resolveImagePath(itemImagePath, name + "-selected", media.selectedImagePath);
                }
            }

            if (!media.videoPath.empty() || !media.idleImagePath.empty()) break;
        }

        // Fallback: System Artwork
        if (media.videoPath.empty() && media.idleImagePath.empty()) {
            std::string sysImgPath, sysVidPath;
            if (layoutMode_) {
                sysImgPath = Utils::combinePath(layoutCollectionsBase_, commonMode_ ? "_common" : item->name, "system_artwork");
                sysVidPath = sysImgPath;
            }
            else {
                if (commonMode_) {
                    sysImgPath = Utils::combinePath(commonCollectionsBase_, "system_artwork");
                    sysVidPath = sysImgPath;
                }
                else {
                    config_.getMediaPropertyAbsolutePath(item->name, imageType_, true, sysImgPath);
                    config_.getMediaPropertyAbsolutePath(item->name, videoType_, true, sysVidPath);
                }
            }

            if (videoType_ != "null") VideoBuilder::resolveVideoPath(sysVidPath, videoType_, media.videoPath);
            if (!imageType_.empty()) {
                ImageBuilder::resolveImagePath(sysImgPath, imageType_, media.idleImagePath);
                if (isSelectedItem) ImageBuilder::resolveImagePath(sysImgPath, imageType_ + "-selected", media.selectedImagePath);
            }
        }

        // Fallback: ROM Path
        if (media.videoPath.empty() && media.idleImagePath.empty()) {
            if (videoType_ != "null") VideoBuilder::resolveVideoPath(item->filepath, videoType_, media.videoPath);
            if (!imageType_.empty()) {
                ImageBuilder::resolveImagePath(item->filepath, imageType_, media.idleImagePath);
                if (isSelectedItem) ImageBuilder::resolveImagePath(item->filepath, imageType_ + "-selected", media.selectedImagePath);
            }
        }

        mediaCache_[i] = media;
    }
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
    size_t scrollPointsSize = scrollPoints_->size();

    for (size_t i = 0; i < scrollPointsSize; ++i) {
        const size_t index = loopIncrement(itemIndex_, i, itemsSize);

        allocateTexture(i, index);
        if (Component* c = components_[i]) {
            c->allocateGraphicsMemory();
            ViewInfo* view = (*scrollPoints_)[i];
            resetTweens(c, (*tweenPoints_)[i], view, view, 0);
        }
    }
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
    for (size_t i = 0; i < scrollPointsSize; ++i) {
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
    
    deallocateSpritePoints();

    clearPoints();
    clearTweenPoints();

    scrollPoints_ = scrollPoints;
    tweenPoints_ = std::move(tweenPoints);

    const size_t N = (scrollPoints_ ? scrollPoints_->size() : 0);

    // Reset circular buffer (fills with nullptrs)
    components_.initialize(N);

    if (items_) {
        itemIndex_ = loopDecrement(0, selectedOffsetIndex_, items_->size());
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

    if (N > 0 && tweenPoints_ && scrollPoints_) {
        for (size_t i = 0; i < N; ++i) {
            const size_t jF = forwardMap_[i];
            const size_t jB = backwardMap_[i];

            forwardTween_[i] = TweenNeighbor{
                (*tweenPoints_)[jF],
                (*scrollPoints_)[i],
                (*scrollPoints_)[jF]
            };
            backwardTween_[i] = TweenNeighbor{
                (*tweenPoints_)[jB],
                (*scrollPoints_)[i],
                (*scrollPoints_)[jB]
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

    // Allocate and initialize components (your existing behavior)
    allocateSpritePoints();
}


size_t ScrollingList::getScrollOffsetIndex( ) const
{
    return loopIncrement( itemIndex_, selectedOffsetIndex_, items_->size());
}

void ScrollingList::setScrollOffsetIndex( size_t index )
{
    itemIndex_ = loopDecrement( index, selectedOffsetIndex_, items_->size());
}

void ScrollingList::setSelectedIndex( int selectedIndex )
{
    selectedOffsetIndex_ = selectedIndex;
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
}

void ScrollingList::pageDown()
{
    if (components_.empty()) return; // More idiomatic
    itemIndex_ = loopIncrement(itemIndex_, components_.size(), items_->size());
}

void ScrollingList::random( )
{
    if (!items_ || items_->empty()) return;
    size_t itemSize = items_->size();
    
    itemIndex_ = rand( ) % itemSize;
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
}

void ScrollingList::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    resetScrollPeriod();

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

bool ScrollingList::isGraphicsReadyForFirstRender() const {
    for (Component* c : components_.raw()) {
        if (c && !c->isGraphicsReadyForFirstRender()) {
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
    size_t scrollPointsSize = scrollPoints_ ? scrollPoints_->size() : 0;
    bool allIdle = Component::isIdle();
    bool allAttractIdle = Component::isAttractIdle();

    for (unsigned int i = 0; i < scrollPointsSize; i++) {
        Component* c = components_[i];
        if (c) {
            done &= c->update(dt);
            if (!c->isIdle()) allIdle = false;
            if (!c->isAttractIdle()) allAttractIdle = false;
        }
    }

    cachedIdle_ = allIdle;
    cachedAttractIdle_ = allAttractIdle;

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
}

size_t ScrollingList::getSize() const
{
    if ( !items_ ) return 0;
    return items_->size();
}

void ScrollingList::resetTweens(Component* c, const std::shared_ptr<AnimationEvents>& sets, ViewInfo* currentViewInfo, ViewInfo* nextViewInfo, float scrollTime) const {
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

    // Temporary runtime patch for correct image rendering during this tween setup.
    currentViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    currentViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;

    nextViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    nextViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;
    nextViewInfo->BackgroundAlpha = c->baseViewInfo.BackgroundAlpha;

    // Reset baseViewInfo to the scroll start position.
    // This now sees the temporarily patched ImageWidth/ImageHeight values.
    c->baseViewInfo = *currentViewInfo;


    // Allocate the TweenSet on the stack for cache efficiency
    TweenSet set;
    const float EPSILON_FLOAT = 0.0001f;

    // Conditionally add Tweens only if properties differ
    if (currentViewInfo->Restart != nextViewInfo->Restart && scrollTime > 0.0f) {
        set.push(Tween(TWEEN_PROPERTY_RESTART, LINEAR, static_cast<float>(currentViewInfo->Restart), static_cast<float>(nextViewInfo->Restart), 0.0f));
    }
    if (std::abs(currentViewInfo->Height - nextViewInfo->Height) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_HEIGHT, LINEAR, static_cast<float>(currentViewInfo->Height), static_cast<float>(nextViewInfo->Height), scrollTime));
    }
    if (std::abs(currentViewInfo->Width - nextViewInfo->Width) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_WIDTH, LINEAR, static_cast<float>(currentViewInfo->Width), static_cast<float>(nextViewInfo->Width), scrollTime));
    }
    if (std::abs(currentViewInfo->Angle - nextViewInfo->Angle) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_ANGLE, LINEAR, static_cast<float>(currentViewInfo->Angle), static_cast<float>(nextViewInfo->Angle), scrollTime));
    }
    if (std::abs(currentViewInfo->Alpha - nextViewInfo->Alpha) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_ALPHA, LINEAR, static_cast<float>(currentViewInfo->Alpha), static_cast<float>(nextViewInfo->Alpha), scrollTime));
    }
    if (std::abs(currentViewInfo->X - nextViewInfo->X) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_X, LINEAR, static_cast<float>(currentViewInfo->X), static_cast<float>(nextViewInfo->X), scrollTime));
    }
    if (std::abs(currentViewInfo->Y - nextViewInfo->Y) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_Y, LINEAR, static_cast<float>(currentViewInfo->Y), static_cast<float>(nextViewInfo->Y), scrollTime));
    }
    if (std::abs(currentViewInfo->XOrigin - nextViewInfo->XOrigin) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_X_ORIGIN, LINEAR, static_cast<float>(currentViewInfo->XOrigin), static_cast<float>(nextViewInfo->XOrigin), scrollTime));
    }
    if (std::abs(currentViewInfo->YOrigin - nextViewInfo->YOrigin) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_Y_ORIGIN, LINEAR, static_cast<float>(currentViewInfo->YOrigin), static_cast<float>(nextViewInfo->YOrigin), scrollTime));
    }
    if (std::abs(currentViewInfo->XOffset - nextViewInfo->XOffset) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_X_OFFSET, LINEAR, static_cast<float>(currentViewInfo->XOffset), static_cast<float>(nextViewInfo->XOffset), scrollTime));
    }
    if (std::abs(currentViewInfo->YOffset - nextViewInfo->YOffset) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_Y_OFFSET, LINEAR, static_cast<float>(currentViewInfo->YOffset), static_cast<float>(nextViewInfo->YOffset), scrollTime));
    }
    if (std::abs(currentViewInfo->FontSize - nextViewInfo->FontSize) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_FONT_SIZE, LINEAR, static_cast<float>(currentViewInfo->FontSize), static_cast<float>(nextViewInfo->FontSize), scrollTime));
    }
    if (std::abs(currentViewInfo->BackgroundAlpha - nextViewInfo->BackgroundAlpha) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_BACKGROUND_ALPHA, LINEAR, static_cast<float>(currentViewInfo->BackgroundAlpha), static_cast<float>(nextViewInfo->BackgroundAlpha), scrollTime));
    }
    if (std::abs(currentViewInfo->MaxWidth - nextViewInfo->MaxWidth) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_MAX_WIDTH, LINEAR, static_cast<float>(currentViewInfo->MaxWidth), static_cast<float>(nextViewInfo->MaxWidth), scrollTime));
    }
    if (std::abs(currentViewInfo->MaxHeight - nextViewInfo->MaxHeight) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_MAX_HEIGHT, LINEAR, static_cast<float>(currentViewInfo->MaxHeight), static_cast<float>(nextViewInfo->MaxHeight), scrollTime));
    }
    if (currentViewInfo->Layer != nextViewInfo->Layer) {
        set.push(Tween(TWEEN_PROPERTY_LAYER, LINEAR, static_cast<float>(currentViewInfo->Layer), static_cast<float>(nextViewInfo->Layer), scrollTime));
    }
    if (std::abs(currentViewInfo->Volume - nextViewInfo->Volume) > EPSILON_FLOAT) {
        set.push(Tween(TWEEN_PROPERTY_VOLUME, LINEAR, static_cast<float>(currentViewInfo->Volume), static_cast<float>(nextViewInfo->Volume), scrollTime));
    }
    if (currentViewInfo->Monitor != nextViewInfo->Monitor) {
        set.push(Tween(TWEEN_PROPERTY_MONITOR, LINEAR, static_cast<float>(currentViewInfo->Monitor), static_cast<float>(nextViewInfo->Monitor), scrollTime));
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
    if (componentIndex >= components_.size() || fullListIndex >= mediaCache_.size()) return false;

    Component* existingComponent = components_[componentIndex];
    components_[componentIndex] = nullptr;

    const ResolvedMedia& media = mediaCache_[fullListIndex];
    const Item* item = (*items_)[fullListIndex];
    Component* t = nullptr;

    ImageBuilder imageBuild;
    VideoBuilder videoBuild;

    const std::string& selectedItemName = getSelectedItemName();
    const bool isSelectedItem = (selectedImage_ && item->name == selectedItemName);

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
    }

    if (existingComponent != nullptr && existingComponent != t) {
        delete existingComponent;
    }

    return true;
}

void ScrollingList::buildPaths(std::string& imagePath, std::string& videoPath, const std::string& base, const std::string& subPath, const std::string& mediaType, const std::string& videoType) {
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

            resetTweens(component, t.tween, t.cur, t.next, scrollTime);

            if (component->baseViewInfo.font != t.next->font) {
                component->baseViewInfo.font = t.next->font;
            }

            component->triggerEvent("menuScroll");
        }
    }

    components_.rotate(forward);

    itemIndex_ = newItemIndex;

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

            resetTweens(component, t.tween, t.cur, t.next, scrollPeriod_);

            if (component->baseViewInfo.font != t.next->font) {
                component->baseViewInfo.font = t.next->font;
            }

            component->triggerEvent("menuScroll");
        }
    }

    components_.rotate(forward);

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

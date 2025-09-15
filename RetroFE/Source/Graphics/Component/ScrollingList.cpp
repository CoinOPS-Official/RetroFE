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
#if __has_include(<SDL2/SDL_image.h>)
#include <SDL2/SDL_image.h>
#elif __has_include(<SDL2_image/SDL_image.h>)
#include <SDL2_image/SDL_image.h>
#else
#error "Cannot find SDL_image header"
#endif
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

void ScrollingList::setItems( std::vector<Item *> *items )
{
    items_ = items;
    if (items_) {
        size_t size = items_->size();
        itemIndex_ = loopDecrement(size, selectedOffsetIndex_, size);
    }
}

void ScrollingList::selectItemByName(std::string_view name)
{
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

void ScrollingList::restartByMonitor(int monitor) {
    for (Component* c : getComponents()) {
        if (c && c->baseViewInfo.Monitor == monitor)
            c->restart();
    }
}

std::string ScrollingList::getSelectedItemName()
{
    size_t size = items_->size();
    if (!size)
        return "";
    
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
    std::vector<std::unique_ptr<IVideo>> pooledVideos;

    // Extract videos first, before deleting components
    for (Component* comp : components_.raw()) {
        if (comp) {
            if (auto* videoComp = dynamic_cast<VideoComponent*>(comp)) {
                auto video = videoComp->extractVideo();
                if (video)
                    pooledVideos.push_back(std::move(video));
            }
        }
    }

    // Delete components
    for (size_t i = 0; i < components_.size(); ++i) {
        deallocateTexture(i);  // This sets components_[i] = nullptr internally
    }

    // Batch release videos
    if (!pooledVideos.empty()) {
        VideoPool::releaseVideoBatch(std::move(pooledVideos), monitor, listId_);
        ThreadPool::getInstance().wait();  // Ensure they're cleaned up now
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
        const Item* item = (*items_)[index];

        allocateTexture(i, item);
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

    // --- Step 1: Extract video instances for batch release ---
    std::vector<std::unique_ptr<IVideo>> pooledVideos;

    for (size_t i = 0; i < scrollPointsSize; ++i) {
        Component* comp = components_[i];
        if (!comp) continue;

        if (auto* videoComp = dynamic_cast<VideoComponent*>(comp)) {
            auto video = videoComp->extractVideo();  // move videoInst_ out
            if (video)
                pooledVideos.push_back(std::move(video));
        }
    }

    // --- Step 2: Batch release to pool ---
    if (!pooledVideos.empty()) {
        VideoPool::releaseVideoBatch(std::move(pooledVideos), monitor, listId_);
    }

    // --- Step 3: Destroy all components ---
    for (size_t i = 0; i < scrollPointsSize; ++i) {
        deallocateTexture(i);  // deletes VideoComponent or others
    }

    // --- Step 4: Wait for releases to complete ---
    ThreadPool::getInstance().wait();

    // --- Step 5: Reallocate components and assign tweens ---
    for (size_t i = 0; i < scrollPointsSize; ++i) {
        size_t index = loopIncrement(itemIndex_, i, itemsSize);
        Item const* item = (*items_)[index];

        allocateTexture(i, item);  // creates VideoComponent or others

        Component* c = components_[i];
        if (c) {
            c->allocateGraphicsMemory();

            ViewInfo* view = (*scrollPoints_)[i];
            resetTweens(c, (*tweenPoints_)[i], view, view, 0);
        }
    }
}

void ScrollingList::destroyItems()
{
    auto& data = components_.raw();
    size_t componentSize = data.size();

    // Clean up the pool - listId_ will always be valid
    if (listId_ == -1) {
        LOG_ERROR("ScrollingList", "Attempting to clean up video pool with invalid listId_ (-1).");
        return;
	}
    LOG_DEBUG("ScrollingList", "Cleaning up video pool for list: " + std::to_string(listId_));
    VideoPool::cleanup(baseViewInfo.Monitor, listId_);
    ThreadPool::getInstance().wait();

    // Delete all components
    for (unsigned int i = 0; i < componentSize; ++i) {
        if (Component* component = data[i]) {
            //component->freeGraphicsMemory();
            delete component;
            data[i] = nullptr;
        }
    }
}


void ScrollingList::setPoints(std::vector<ViewInfo*>* scrollPoints,
    std::shared_ptr<std::vector<std::shared_ptr<AnimationEvents>>> tweenPoints) {
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
        else if (!prevLetterSubToCurrent || loopIncrement(itemIndex_, 1, itemSize) == newIndex) {

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
    if (Utils::toLower(collectionName) != (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName())
        subChange(true);
    else
        letterChange(true);
}

void ScrollingList::cfwLetterSubDown()
{
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

void ScrollingList::freeGraphicsMemory()
{
    Component::freeGraphicsMemory();
    scrollPeriod_ = 0;
    // Clean up components
    deallocateSpritePoints();
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

void ScrollingList::triggerEventOnAll(const std::string& event, int menuIndex)
{
    size_t componentSize = components_.size();
    for (size_t i = 0; i < componentSize; ++i) {
        Component* c = components_[i];
        if (c) c->triggerEvent(event, menuIndex);
    }
}

bool ScrollingList::update(float dt)
{
    bool done = Component::update(dt);

    if (components_.empty()) 
        return done;
    if (!items_) 
        return done;

    size_t scrollPointsSize = scrollPoints_->size();
    
    for (unsigned int i = 0; i < scrollPointsSize; i++) {
        Component *c = components_[i];
        if (c) {
            c->playlistName = playlistName;
            done &= c->update(dt);
        }
    }

    return done;
}

size_t ScrollingList::getSelectedIndex( ) const
{
    if ( !items_ ) return 0;
    return loopIncrement( itemIndex_, selectedOffsetIndex_, items_->size( ) );
}

void ScrollingList::setSelectedIndex( unsigned int index )
{
     if ( !items_ ) return;
     itemIndex_ = loopDecrement( index, selectedOffsetIndex_, items_->size( ) );
}

size_t ScrollingList::getSize() const
{
    if ( !items_ ) return 0;
    return items_->size();
}

// Change the type of scrollTime here to float, as it's only used to create tweens that need floats.
void ScrollingList::resetTweens(Component* c, std::shared_ptr<AnimationEvents> sets, ViewInfo* currentViewInfo, ViewInfo* nextViewInfo, float scrollTime) const {
    if (!c || !sets || !currentViewInfo || !nextViewInfo) return;

    // These adjustments are still necessary outside of tweening logic
    currentViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    currentViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;
    nextViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    nextViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;
    nextViewInfo->BackgroundAlpha = c->baseViewInfo.BackgroundAlpha;

    c->setTweens(sets);

    std::shared_ptr<Animation> scrollTween = sets->getAnimation("menuScroll");
    scrollTween->Clear();

    // Start with a fresh baseViewInfo
    c->baseViewInfo = *currentViewInfo;

    auto set = std::make_unique<TweenSet>();

    // Define a small epsilon for floating-point comparisons
    const float EPSILON_FLOAT = 0.0001f;

    // NOTE: Using static_cast<float> on all numeric start/end values to explicitly
    // convert them to the type expected by the Tween constructor. This resolves all C4244 warnings.

    // Apply conditional push for each Tween property
    if (currentViewInfo->Restart != nextViewInfo->Restart && scrollPeriod_ > minScrollTime_) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_RESTART, LINEAR, static_cast<float>(currentViewInfo->Restart), static_cast<float>(nextViewInfo->Restart), 0.0f));
    }
    if (std::abs(currentViewInfo->Height - nextViewInfo->Height) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_HEIGHT, LINEAR, static_cast<float>(currentViewInfo->Height), static_cast<float>(nextViewInfo->Height), scrollTime));
    }
    if (std::abs(currentViewInfo->Width - nextViewInfo->Width) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_WIDTH, LINEAR, static_cast<float>(currentViewInfo->Width), static_cast<float>(nextViewInfo->Width), scrollTime));
    }
    if (std::abs(currentViewInfo->Angle - nextViewInfo->Angle) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_ANGLE, LINEAR, static_cast<float>(currentViewInfo->Angle), static_cast<float>(nextViewInfo->Angle), scrollTime));
    }
    if (std::abs(currentViewInfo->Alpha - nextViewInfo->Alpha) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_ALPHA, LINEAR, static_cast<float>(currentViewInfo->Alpha), static_cast<float>(nextViewInfo->Alpha), scrollTime));
    }
    if (std::abs(currentViewInfo->X - nextViewInfo->X) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_X, LINEAR, static_cast<float>(currentViewInfo->X), static_cast<float>(nextViewInfo->X), scrollTime));
    }
    if (std::abs(currentViewInfo->Y - nextViewInfo->Y) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_Y, LINEAR, static_cast<float>(currentViewInfo->Y), static_cast<float>(nextViewInfo->Y), scrollTime));
    }
    if (std::abs(currentViewInfo->XOrigin - nextViewInfo->XOrigin) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_X_ORIGIN, LINEAR, static_cast<float>(currentViewInfo->XOrigin), static_cast<float>(nextViewInfo->XOrigin), scrollTime));
    }
    if (std::abs(currentViewInfo->YOrigin - nextViewInfo->YOrigin) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_Y_ORIGIN, LINEAR, static_cast<float>(currentViewInfo->YOrigin), static_cast<float>(nextViewInfo->YOrigin), scrollTime));
    }
    if (std::abs(currentViewInfo->XOffset - nextViewInfo->XOffset) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_X_OFFSET, LINEAR, static_cast<float>(currentViewInfo->XOffset), static_cast<float>(nextViewInfo->XOffset), scrollTime));
    }
    if (std::abs(currentViewInfo->YOffset - nextViewInfo->YOffset) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_Y_OFFSET, LINEAR, static_cast<float>(currentViewInfo->YOffset), static_cast<float>(nextViewInfo->YOffset), scrollTime));
    }
    if (std::abs(currentViewInfo->FontSize - nextViewInfo->FontSize) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_FONT_SIZE, LINEAR, static_cast<float>(currentViewInfo->FontSize), static_cast<float>(nextViewInfo->FontSize), scrollTime));
    }
    if (std::abs(currentViewInfo->BackgroundAlpha - nextViewInfo->BackgroundAlpha) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_BACKGROUND_ALPHA, LINEAR, static_cast<float>(currentViewInfo->BackgroundAlpha), static_cast<float>(nextViewInfo->BackgroundAlpha), scrollTime));
    }
    if (std::abs(currentViewInfo->MaxWidth - nextViewInfo->MaxWidth) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_MAX_WIDTH, LINEAR, static_cast<float>(currentViewInfo->MaxWidth), static_cast<float>(nextViewInfo->MaxWidth), scrollTime));
    }
    if (std::abs(currentViewInfo->MaxHeight - nextViewInfo->MaxHeight) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_MAX_HEIGHT, LINEAR, static_cast<float>(currentViewInfo->MaxHeight), static_cast<float>(nextViewInfo->MaxHeight), scrollTime));
    }
    if (currentViewInfo->Layer != nextViewInfo->Layer) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_LAYER, LINEAR, static_cast<float>(currentViewInfo->Layer), static_cast<float>(nextViewInfo->Layer), scrollTime));
    }
    if (std::abs(currentViewInfo->Volume - nextViewInfo->Volume) > EPSILON_FLOAT) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_VOLUME, LINEAR, static_cast<float>(currentViewInfo->Volume), static_cast<float>(nextViewInfo->Volume), scrollTime));
    }
    if (currentViewInfo->Monitor != nextViewInfo->Monitor) {
        set->push(std::make_unique<Tween>(TWEEN_PROPERTY_MONITOR, LINEAR, static_cast<float>(currentViewInfo->Monitor), static_cast<float>(nextViewInfo->Monitor), scrollTime));
    }

    // Only push the set if there are any tweens in it
    if (set->size() > 0) {
        scrollTween->Push(std::move(set));
    }
}

bool ScrollingList::allocateTexture(size_t index, const Item* item) {
    if (index >= components_.size()) return false;

    Component* t = nullptr;
    std::string layoutName;
    config_.getProperty(OPTION_LAYOUT, layoutName);
    std::string typeLC = Utils::toLower(imageType_);
    std::string selectedItemName = getSelectedItemName();

    // Compose name candidates once
    std::vector<std::string> names = {
        item->name, item->fullTitle
    };
    if (!item->cloneof.empty())        names.push_back(item->cloneof);
    if (typeLC == "numberbuttons")     names.push_back(item->numberButtons);
    if (typeLC == "numberplayers")     names.push_back(item->numberPlayers);
    if (typeLC == "year")              names.push_back(item->year);
    if (typeLC == "title")             names.push_back(item->title);
    if (typeLC == "developer")         names.push_back(item->developer.empty() ? item->manufacturer : item->developer);
    if (typeLC == "manufacturer")      names.push_back(item->manufacturer);
    if (typeLC == "genre")             names.push_back(item->genre);
    if (typeLC == "ctrltype")          names.push_back(item->ctrlType);
    if (typeLC == "joyways")           names.push_back(item->joyWays);
    if (typeLC == "rating")            names.push_back(item->rating);
    if (typeLC == "score")             names.push_back(item->score);
    if (typeLC.rfind("playlist", 0) == 0)
        names.push_back(item->name);
    names.emplace_back("default");

    ImageBuilder imageBuild;
    VideoBuilder videoBuild;

    // -------- Main media search loop --------
    for (const auto& name : names) {
        std::string imagePath, videoPath;

        // Precompute base/subPath for this iteration
        if (layoutMode_) {
            std::string base = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections");
            std::string subPath = commonMode_ ? "_common" : collectionName;
            buildPaths(imagePath, videoPath, base, subPath, imageType_, videoType_);
        }
        else {
            if (commonMode_) {
                buildPaths(imagePath, videoPath, Configuration::absolutePath, "collections/_common", imageType_, videoType_);
            }
            else {
                config_.getMediaPropertyAbsolutePath(collectionName, imageType_, false, imagePath);
                config_.getMediaPropertyAbsolutePath(collectionName, videoType_, false, videoPath);
            }
        }

        if (!t) {
            if (videoType_ != "null") {
                t = videoBuild.createVideo(videoPath, page, name, baseViewInfo.Monitor, -1, false, listId_, perspectiveCornersInitialized_ ? perspectiveCorners_ : nullptr);
            }
            else {
                std::string imageName = (selectedImage_ && item->name == selectedItemName) ? name + "-selected" : name;
                t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
            }
        }
        if (t) break;

        // --------- Sub-collection fallback ---------
        if (!commonMode_) {
            if (layoutMode_) {
                std::string base = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", item->collectionInfo->name);
                buildPaths(imagePath, videoPath, base, "", imageType_, videoType_);
            }
            else {
                config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, imageType_, false, imagePath);
                config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, videoType_, false, videoPath);
            }

            if (!t) {
                if (videoType_ != "null") {
                    t = videoBuild.createVideo(videoPath, page, name, baseViewInfo.Monitor, -1, false, listId_, perspectiveCornersInitialized_ ? perspectiveCorners_ : nullptr);
                }
                else {
                    std::string imageName = (selectedImage_ && item->name == selectedItemName) ? name + "-selected" : name;
                    t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
                }
            }
        }
        if (t) break;
    }

    // -------- System collection fallback --------
    if (!t) {
        std::string imagePath, videoPath;
        if (layoutMode_) {
            imagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", commonMode_ ? "_common" : item->name);
            imagePath = Utils::combinePath(imagePath, "system_artwork");
            videoPath = imagePath;
        }
        else {
            if (commonMode_) {
                imagePath = Utils::combinePath(Configuration::absolutePath, "collections", "_common");
                imagePath = Utils::combinePath(imagePath, "system_artwork");
                videoPath = imagePath;
            }
            else {
                config_.getMediaPropertyAbsolutePath(item->name, imageType_, true, imagePath);
                config_.getMediaPropertyAbsolutePath(item->name, videoType_, true, videoPath);
            }
        }
        if (videoType_ != "null") {
            t = videoBuild.createVideo(videoPath, page, videoType_, baseViewInfo.Monitor, -1, false, listId_, perspectiveCornersInitialized_ ? perspectiveCorners_ : nullptr);
        }
        else {
            std::string fallbackName = imageType_;
            if (selectedImage_ && item->name == selectedItemName) {
                t = imageBuild.CreateImage(imagePath, page, fallbackName + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
            }
            if (!t) {
                t = imageBuild.CreateImage(imagePath, page, fallbackName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
            }
        }
    }

    // -------- ROM directory fallback --------
    if (!t) {
        if (videoType_ != "null") {
            t = videoBuild.createVideo(item->filepath, page, videoType_, baseViewInfo.Monitor, -1, false, listId_, perspectiveCornersInitialized_ ? perspectiveCorners_ : nullptr);
        }
        else {
            std::string fallbackName = imageType_;
            if (selectedImage_ && item->name == selectedItemName) {
                t = imageBuild.CreateImage(item->filepath, page, fallbackName + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
            }
            if (!t) {
                t = imageBuild.CreateImage(item->filepath, page, fallbackName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
            }
        }
    }

    // -------- Video fallback: fallback to image for each name --------
    if (videoType_ != "null" && !t) {
        for (const auto& name : names) {
            if (t) break;
            std::string imagePath, videoPath;

            if (layoutMode_) {
                std::string base = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections");
                std::string subPath = commonMode_ ? "_common" : collectionName;
                buildPaths(imagePath, videoPath, base, subPath, imageType_, videoType_);
            }
            else {
                if (commonMode_) {
                    buildPaths(imagePath, videoPath, Configuration::absolutePath, "collections/_common", imageType_, videoType_);
                }
                else {
                    config_.getMediaPropertyAbsolutePath(collectionName, imageType_, false, imagePath);
                    config_.getMediaPropertyAbsolutePath(collectionName, videoType_, false, videoPath);
                }
            }
            std::string imageName = (selectedImage_ && item->name == selectedItemName) ? name + "-selected" : name;
            t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);

            if (!t && !commonMode_) {
                if (layoutMode_) {
                    std::string base = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", item->collectionInfo->name);
                    buildPaths(imagePath, videoPath, base, "", imageType_, videoType_);
                }
                else {
                    config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, imageType_, false, imagePath);
                    config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, videoType_, false, videoPath);
                }
                imageName = (selectedImage_ && item->name == selectedItemName) ? name + "-selected" : name;
                t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
            }
        }
        // -------- System collection fallback --------
        if (!t) {
            std::string imagePath, videoPath;
            if (layoutMode_) {
                imagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", commonMode_ ? "_common" : item->name);
                imagePath = Utils::combinePath(imagePath, "system_artwork");
                videoPath = imagePath;
            }
            else {
                if (commonMode_) {
                    imagePath = Utils::combinePath(Configuration::absolutePath, "collections", "_common");
                    imagePath = Utils::combinePath(imagePath, "system_artwork");
                    videoPath = imagePath;
                }
                else {
                    config_.getMediaPropertyAbsolutePath(item->name, imageType_, true, imagePath);
                    config_.getMediaPropertyAbsolutePath(item->name, videoType_, true, videoPath);
                }
            }
            if (videoType_ != "null") {
                t = videoBuild.createVideo(videoPath, page, videoType_, baseViewInfo.Monitor, -1, false, listId_, perspectiveCornersInitialized_ ? perspectiveCorners_ : nullptr);
            }
            else {
                std::string fallbackName = imageType_;
                if (selectedImage_ && item->name == selectedItemName) {
                    t = imageBuild.CreateImage(imagePath, page, fallbackName + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
                }
                if (!t) {
                    t = imageBuild.CreateImage(imagePath, page, fallbackName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
                }
            }
        }

        // -------- ROM directory fallback --------
        if (!t) {
            if (videoType_ != "null") {
                t = videoBuild.createVideo(item->filepath, page, videoType_, baseViewInfo.Monitor, -1, false, listId_, perspectiveCornersInitialized_ ? perspectiveCorners_ : nullptr);
            }
            else {
                std::string fallbackName = imageType_;
                if (selectedImage_ && item->name == selectedItemName) {
                    t = imageBuild.CreateImage(item->filepath, page, fallbackName + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
                }
                if (!t) {
                    t = imageBuild.CreateImage(item->filepath, page, fallbackName, baseViewInfo.Monitor, baseViewInfo.Additive, useTextureCaching_);
                }
            }
        }
    }

    if (!t && textFallback_) {
        t = new Text(item->title, page, fontInst_, baseViewInfo.Monitor);
    }

    if (t) {
        components_[index] = t;
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

bool ScrollingList::isScrollingListIdle()
{
    size_t componentSize = components_.size();
    if ( !Component::isIdle(  ) ) return false;

    for ( unsigned int i = 0; i < componentSize; ++i ) {
        Component const *c = components_[i];
        if ( c && !c->isIdle(  ) ) return false;
    }

    return true;
}

bool ScrollingList::isScrollingListAttractIdle()
{
    size_t componentSize = components_.size();
    if ( !Component::isAttractIdle(  ) ) return false;

    for ( unsigned int i = 0; i < componentSize; ++i ) {
        Component const *c = components_[i];
        if ( c && !c->isAttractIdle(  ) ) return false;
    }

    return true;
}

void ScrollingList::resetScrollPeriod(  )
{
    scrollPeriod_ = startScrollTime_;
    return;
}

void ScrollingList::updateScrollPeriod(  )
{
    scrollPeriod_ -= scrollAcceleration_;
    if ( scrollPeriod_ < minScrollTime_ )
    {
        scrollPeriod_ = minScrollTime_;
    }
}

bool ScrollingList::isFastScrolling() const
{
    return scrollPeriod_ == minScrollTime_;
}

void ScrollingList::scroll(bool forward) {
    // Exit conditions
    if (!items_ || items_->empty() || !scrollPoints_ || scrollPoints_->empty())
        return;

    // Clamp scroll speed
    if (scrollPeriod_ < minScrollTime_)
        scrollPeriod_ = minScrollTime_;

    const size_t itemsSize = items_->size();
    const size_t N = scrollPoints_->size();
    const size_t f = static_cast<size_t>(forward); // 0 or 1

    // Decide which slot exits (was: forward ? 0 : N-1)
    const size_t exitIndex = (N <= 1) ? 0 : (N - 1) * (1 - f);

    // Determine the new item to load into the exiting slot, and update itemIndex_
    const Item* itemToScroll = nullptr;
    if (forward) {
        itemToScroll = (*items_)[loopIncrement(itemIndex_, N, itemsSize)];
        itemIndex_ = loopIncrement(itemIndex_, 1, itemsSize);
    }
    else {
        itemToScroll = (*items_)[loopDecrement(itemIndex_, 1, itemsSize)];
        itemIndex_ = loopDecrement(itemIndex_, 1, itemsSize);
    }

    // Rebuild only the exiting slot (consider switching to recycle/retarget later)
    deallocateTexture(exitIndex);
    allocateTexture(exitIndex, itemToScroll);

    // --- Use precomputed tuples ---
    const auto& T = forward ? forwardTween_ : backwardTween_;
    // Guard (paranoia) in case N changed without setPoints being called:
    if (T.size() != N) {
        // fallback to maps if ever mismatched
        const auto& neighbor = forward ? forwardMap_ : backwardMap_;
        for (size_t index = 0; index < N; ++index) {
            const size_t nextIndex = neighbor[index];
            Component* component = components_[index];
            if (!component) continue;

            auto& nextTweenPoint = (*tweenPoints_)[nextIndex];
            auto* currentScrollPoint = (*scrollPoints_)[index];
            auto* nextScrollPoint = (*scrollPoints_)[nextIndex];

            component->allocateGraphicsMemory();
            resetTweens(component, nextTweenPoint, currentScrollPoint, nextScrollPoint, scrollPeriod_);
            component->baseViewInfo.font = nextScrollPoint->font;
            component->triggerEvent("menuScroll");
        }
    }
    else {
        for (size_t index = 0; index < N; ++index) {
            Component* component = components_[index];
            if (!component) continue;

            const TweenNeighbor& t = T[index];

            component->allocateGraphicsMemory(); // consider removing per-scroll alloc later
            resetTweens(component, t.tween, t.cur, t.next, scrollPeriod_);
            if (component->baseViewInfo.font != t.next->font)
                component->baseViewInfo.font = t.next->font;
            component->triggerEvent("menuScroll");
        }
    }

    // Rotate logical order
    components_.rotate(forward);
}

bool ScrollingList::isPlaylist() const
{
    return playlistType_;
}

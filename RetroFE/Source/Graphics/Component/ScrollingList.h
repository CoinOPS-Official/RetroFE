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
#pragma once


#include <vector>
#include <memory>
#include <unordered_map>
#include "Component.h"
#include "../PresentationPreload.h"
#include "../Animate/Tween.h"
#include "../Animate/TweenSet.h"
#include "../Page.h"
#include "../ViewInfo.h"
#include "../../Database/Configuration.h"
#include <SDL.h>

template<typename T>
class RotatableView {
private:
    std::vector<T> data_;
    size_t head_ = 0;
    size_t size_ = 0;
    size_t capacity_;

public:
    // Default constructor
    RotatableView() : data_(0), capacity_(0) {}

    // Parameterized constructor
    explicit RotatableView(size_t capacity) : data_(capacity, T()), capacity_(capacity) {}

    // Initialize or reset the buffer
    void initialize(size_t capacity) {
        data_.clear();
        data_.resize(capacity, T());
        head_ = 0;
        capacity_ = capacity;
    }

    // Rotate the buffer forward or backward
    void rotate(bool forward) {
        if (capacity_ == 0) return;
        if (forward) {
            if (++head_ == capacity_) head_ = 0;
        }
        else {
            head_ = (head_ == 0) ? (capacity_ - 1) : (head_ - 1);
        }
    }

    T& operator[](size_t index) {
        size_t i = head_ + index;
        if (i >= capacity_) i -= capacity_;
        return data_[i];
    }
    const T& operator[](size_t index) const {
        size_t i = head_ + index;
        if (i >= capacity_) i -= capacity_;
        return data_[i];
    }


    // Get the raw underlying vector for iteration if needed
    std::vector<T>& raw() { return data_; }
    const std::vector<T>& raw() const { return data_; }

    // Size and capacity methods
    size_t size() const { return capacity_; }
    bool empty() const { return capacity_ == 0; }

    // Direct access to current head
    T& head() { return data_[head_]; }
    const T& head() const { return data_[head_]; }
};


class Configuration;
class FontManager;
class Image;

class ScrollingList : public Component
{

public:

    ScrollingList(Configuration& c,
        Page& p,
        bool          layoutMode,
        bool          commonMode,
        bool          playlistType,
        bool          selectedImage,
        FontManager* font,
        const std::string& layoutKey,
        const std::string& imageType,
        const std::string& videoType,
        bool useTextureCaching);

    ScrollingList(const ScrollingList& other);
    ScrollingList& operator=(const ScrollingList&) = delete;
    ~ScrollingList() override;
    const std::vector<Item*>& getItems() const;
    
    int getListId() const;

    void syncToSelectedIndex(size_t selectedIndex);

    float getScrollPeriod() const;
    void setScrollPeriod(float value);

    void triggerEnterEvent();
    void triggerExitEvent();
    void triggerMenuEnterEvent(int menuIndex = -1);
    void triggerMenuExitEvent(int menuIndex = -1);
    void triggerGameEnterEvent(int menuIndex = -1);
    void triggerTrackChangeEvent(int menuIndex);
    void triggerGameExitEvent(int menuIndex = -1);
    void triggerHighlightEnterEvent(int menuIndex = -1);
    void triggerHighlightExitEvent(int menuIndex = -1);
    void triggerPlaylistEnterEvent(int menuIndex = -1);
    void triggerPlaylistExitEvent(int menuIndex = -1);
    void triggerMenuJumpEnterEvent(int menuIndex = -1);
    void triggerMenuJumpExitEvent(int menuIndex = -1);
    void triggerAttractEnterEvent(int menuIndex = -1);
    void triggerAttractEvent(int menuIndex = -1);
    void triggerAttractExitEvent(int menuIndex = -1);
    void triggerGameInfoEnter(int menuIndex = -1);
    void triggerGameInfoExit(int menuIndex = -1);
    void triggerCollectionInfoEnter(int menuIndex = -1);
    void triggerCollectionInfoExit(int menuIndex = -1);
    void triggerBuildInfoEnter(int menuIndex = -1);
    void triggerBuildInfoExit(int menuIndex = -1);
    void triggerJukeboxJumpEvent(int menuIndex = -1);
    void triggerEventOnAll(const std::string& event, int menuIndex);;
    bool allocateTexture(size_t componentIndex, size_t fullListIndex);
    void buildPaths(std::string& imagePath, std::string& videoPath, const std::string& base, const std::string& subPath, const std::string& mediaType, const std::string& videoType) const;
    void deallocateTexture(size_t index);
    void setItems(std::vector<Item*>* items);
    void selectItemByName(std::string_view name);
    void restartByMonitor(int monitor) const;
    const std::string& getSelectedItemName();
    void destroyItems();
    void setPoints(std::vector<ViewInfo*>* points, std::shared_ptr<std::vector<std::shared_ptr<AnimationEvents>>> tweenPoints);
    size_t getSelectedIndex() const;
    size_t nextSelectedIndex(bool forward) const;
    void setItemIndex(unsigned int index);
    size_t getSize() const;
    void pageUp();
    void pageDown();
    void letterUp();
    void letterDown();
    void letterChange(bool increment);
    void metaUp(const std::string& attribute);
    void metaDown(const std::string& attribute);
    void metaChange(bool increment, const std::string& attribute);
    void subChange(bool increment);
    void cfwLetterSubUp();
    void cfwLetterSubDown();
    void random();
    bool isScrollingListIdle();
    bool isScrollingListAttractIdle();
    size_t getScrollOffsetIndex() const;
    void setScrollOffsetIndex(size_t index);
    void setSelectedIndex(int selectedIndex);
    Item* getItemByOffset(int offset);
    Item* getSelectedItem();
    void allocateGraphicsMemory() override;
    void freeGraphicsMemory() override;
    void pumpGraphicsPreparation() override;
    void waitForGraphicsPreparation() override;
    bool isGraphicsReadyForFirstRender() const override;
    bool update(float dt) override;
    const std::vector<Component*>& getComponents() const;
    void setScrollAcceleration(float value);
    void setStartScrollTime(float value);
    void setMinScrollTime(float value);
    void setCoastFriction(float value);
    bool canBeginCoast() const;
    void enableTextFallback(bool value);
    bool horizontalScroll{ false };
    void deallocateSpritePoints();
    void allocateSpritePoints();
    void reallocateSpritePoints();
    void resetScrollPeriod();
    void updateScrollPeriod();

    void beginCoast();
    void decelerateScrollPeriod(float dt);
    bool canCoast() const;
    bool isFastScrolling() const;
    void scroll(bool forward);
    void scrollToSelectedIndex(size_t newSelectedIndex, bool forward, float scrollTime);
    bool isPlaylist() const;
    unsigned int getVisualPriorityTier() const;
    unsigned int getVisualPriorityLayer() const;
    bool enablesPresentationPreload() const {
        return useTextureCaching_ && !playlistType_;
    }
    bool presentsItems(
        const std::vector<Item*>& items) const
    {
        return items_ == &items;
    }
    size_t getSlotCount() const {
        return scrollPoints_ ? scrollPoints_->size() : 0;
    }
    PresentationPreloadContribution
        collectPresentationPreloadsForSelection(
            size_t selectedIndex,
            PresentationPreloadCollector& collector);
    static void clearSharedMediaCache();

    void setPerspectiveCorners(const int corners[8]) {
        std::copy(corners, corners + 8, perspectiveCorners_);
        perspectiveCornersInitialized_ = true;
    }
    const int* getPerspectiveCorners() const { return perspectiveCorners_; }



private:
    static int nextListId;
    int listId_;

    void clearPoints();
    void clearTweenPoints();
    void rebuildSlotTopology_(bool initializeComponents = true);
    bool isSlotVisible_(size_t index) const;
    unsigned int preparationTierForIndex_(size_t index) const;
    unsigned int layerForIndex_(size_t index) const;
    void refreshPreparationPriorityHints_();
    std::vector<size_t> visualPriorityOrder_() const;
    
    void resetTweens(
        Component* c,
        const std::shared_ptr<AnimationEvents>& sets,
        ViewInfo* currentViewInfo,
        ViewInfo* nextViewInfo,
        float scrollTime,
        const TweenSet* transitionTemplate = nullptr,
        bool restartChanges = false) const;
    inline size_t loopIncrement(size_t offset, size_t index, size_t size) const;
    inline size_t loopDecrement(size_t offset, size_t index, size_t size) const;

    struct ResolvedMedia {
        std::string videoPath;
        std::string idleImagePath;
        std::string selectedImagePath;
    };

    using SharedResolvedMedia = std::shared_ptr<const ResolvedMedia>;
    static std::unordered_map<std::string, SharedResolvedMedia>
        sharedMediaCache_;
    std::vector<SharedResolvedMedia> mediaCache_;
    const ResolvedMedia& resolveMediaAt_(size_t fullListIndex);
    ResolvedMedia resolveMediaForItem_(const Item& item) const;
    std::string mediaCacheKey_(const Item& item) const;
    TweenSet buildTweenTemplate_(
        const ViewInfo& current,
        const ViewInfo& next) const;

    bool layoutMode_;
    bool commonMode_;
    bool playlistType_;
    bool selectedImage_;
    bool textFallback_{ false };

    std::vector<ViewInfo*>* scrollPoints_{ nullptr };
    std::shared_ptr<std::vector<std::shared_ptr<AnimationEvents>>> tweenPoints_;

    size_t itemIndex_{ 0 };
    size_t selectedOffsetIndex_{ 0 };
    struct TweenNeighbor {
        std::shared_ptr<AnimationEvents> tween;
        ViewInfo* cur;
        ViewInfo* next;
        TweenSet transitionTemplate;
        bool restartChanges{ false };
    };

    std::vector<size_t> forwardMap_;
    std::vector<size_t> backwardMap_;
    std::vector<TweenNeighbor> forwardTween_;
    std::vector<TweenNeighbor> backwardTween_;

    float scrollAcceleration_{ 0 };
    float startScrollTime_{ 0.500 };
    float minScrollTime_{ 0.500 };
    float scrollPeriod_{ 0 };
	float letterSkipTimer_{ 0 };
    float coastFriction_{ 0.0f };
    float currentDt_{0.0f};
    bool coasting_ = false;
    float coastElapsed_ = 0.0f;
    float coastStartPeriod_ = 0.0f;

    Configuration& config_;
    FontManager* fontInst_;
    std::string    layoutKey_;
    std::string    imageType_;
    std::string    videoType_;
    std::string layoutName_;
    std::string imageTypeLC_;
    std::string layoutCollectionsBase_;
    std::string commonCollectionsBase_;

    std::vector<Item*>* items_{ nullptr };
    RotatableView<Component*> components_;

    bool useTextureCaching_{ false };

    bool perspectiveCornersInitialized_{ false };
    int perspectiveCorners_[8]; // stores x,y coordinates for all 4 corners in order: topLeft, topRight, bottomLeft, bottomRight

    bool cachedIdle_{ true };
    bool cachedAttractIdle_{ true };


};

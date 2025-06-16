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
#include <mutex>
#include "Component.h"
#include "../Animate/Tween.h"
#include "../Page.h"
#include "../ViewInfo.h"
#include "../../Database/Configuration.h"
#include <SDL2/SDL.h>

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
        if (forward) {
            head_ = (head_ + 1) % capacity_;
        }
        else {
            head_ = (head_ - 1 + capacity_) % capacity_;
        }
    }

    // Access element at a given offset from the head
    T& operator[](size_t index) {
        return data_[(head_ + index) % capacity_];
    }

    const T& operator[](size_t index) const {
        return data_[(head_ + index) % capacity_];
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

    ~ScrollingList() override;
    const std::vector<Item*>& getItems() const;
    
    int getListId() const;

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

    bool allocateTexture(size_t index, const Item* i);
    void buildPaths(std::string& imagePath, std::string& videoPath, const std::string& base, const std::string& subPath, const std::string& mediaType, const std::string& videoType);
    void deallocateTexture(size_t index);
    void setItems(std::vector<Item*>* items);
    void selectItemByName(std::string_view name);
    std::string getSelectedItemName();
    void destroyItems();
    void setPoints(std::vector<ViewInfo*>* points, std::shared_ptr<std::vector<std::shared_ptr<AnimationEvents>>> tweenPoints);
    size_t getSelectedIndex() const;
    void setSelectedIndex(unsigned int index);
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
    bool update(float dt) override;
    const std::vector<Component*>& getComponents() const;
    void setScrollAcceleration(float value);
    void setStartScrollTime(float value);
    void setMinScrollTime(float value);
    void enableTextFallback(bool value);
    bool horizontalScroll{ false };
    void deallocateSpritePoints();
    void allocateSpritePoints();
    void reallocateSpritePoints();
    void resetScrollPeriod();
    void updateScrollPeriod();
    bool isFastScrolling() const;
    void scroll(bool forward);
    bool isPlaylist() const;

    void setPerspectiveCorners(const int corners[8]) {
        std::copy(corners, corners + 8, perspectiveCorners_);
        perspectiveCornersInitialized_ = true;
    }
    const int* getPerspectiveCorners() const { return perspectiveCorners_; }



private:

    static int nextListId;
    static std::mutex listIdMutex;  // Add mutex for thread safety
    int listId_;

    void clearPoints();
    void clearTweenPoints();
    
    void resetTweens(Component* c, std::shared_ptr<AnimationEvents> sets, ViewInfo* currentViewInfo, ViewInfo* nextViewInfo, double scrollTime) const;
    inline size_t loopIncrement(size_t offset, size_t index, size_t size) const;
    inline size_t loopDecrement(size_t offset, size_t index, size_t size) const;

    bool layoutMode_;
    bool commonMode_;
    bool playlistType_;
    bool selectedImage_;
    bool textFallback_{ false };

    std::vector<ViewInfo*>* scrollPoints_{ nullptr };
    std::shared_ptr<std::vector<std::shared_ptr<AnimationEvents>>> tweenPoints_;

    size_t itemIndex_{ 0 };
    size_t selectedOffsetIndex_{ 0 };

    float scrollAcceleration_{ 0 };
    float startScrollTime_{ 0.500 };
    float minScrollTime_{ 0.500 };
    float scrollPeriod_{ 0 };

    Configuration& config_;
    FontManager* fontInst_;
    std::string    layoutKey_;
    std::string    imageType_;
    std::string    videoType_;

    std::vector<Item*>* items_{ nullptr };
    RotatableView<Component*> components_;

    bool useTextureCaching_{ false };

    bool perspectiveCornersInitialized_{ false };
    int perspectiveCorners_[8]; // stores x,y coordinates for all 4 corners in order: topLeft, topRight, bottomLeft, bottomRight


};
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
#include "Component.h"
#include "../Animate/Tween.h"
#include "../Page.h"
#include "../ViewInfo.h"
#include "../../Database/Configuration.h"
#include <SDL2/SDL.h>


class Configuration;
class Font;

class ScrollingList : public Component
{

public:

    ScrollingList(Configuration& c,
        Page& p,
        bool          layoutMode,
        bool          commonMode,
        bool          playlistType,
        bool          selectedImage,
        Font* font,
        const std::string& layoutKey,
        const std::string& imageType,
        const std::string& videoType);

    ScrollingList(const ScrollingList& copy);
    ~ScrollingList() override;
    const std::vector<Item*>& getItems() const;
    void triggerEnterEvent();
    void triggerExitEvent();
    void triggerMenuEnterEvent(int menuIndex = -1);
    void triggerMenuExitEvent(int menuIndex = -1);
    void triggerGameEnterEvent(int menuIndex = -1);
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

    bool allocateTexture(unsigned int index, const Item* i);
    void buildPaths(std::string& imagePath, std::string& videoPath, const std::string& base, const std::string& subPath, const std::string& mediaType, const std::string& videoType);
    void deallocateTexture(unsigned int index);
    void setItems(std::vector<Item*>* items);
    void selectItemByName(std::string_view name);
    std::string getSelectedItemName();
    void destroyItems();
    void setPoints(std::vector<ViewInfo*>* scrollPoints, std::vector<AnimationEvents*>* tweenPoints);
    unsigned int getSelectedIndex() const;
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
    bool isIdle();
    bool isAttractIdle();
    unsigned int getScrollOffsetIndex() const;
    void setScrollOffsetIndex(unsigned int index);
    void setSelectedIndex(int selectedIndex);
    Item* getItemByOffset(int offset);
    Item* getSelectedItem();
    unsigned int getSelectedItemPosition();
    void allocateGraphicsMemory() override;
    void freeGraphicsMemory() override;
    bool update(float dt) override;
    void draw() override;
    void draw(unsigned int layer);
    void setScrollAcceleration(float value);
    void setStartScrollTime(float value);
    void setMinScrollTime(float value);
    void enableTextFallback(bool value);
    bool horizontalScroll{ false };
    void deallocateSpritePoints();
    void allocateSpritePoints();
    void resetScrollPeriod();
    void updateScrollPeriod();
    void scroll(bool forward);
    bool isPlaylist() const;
private:

    void resetTweens(Component* c, AnimationEvents* sets, ViewInfo* currentViewInfo, ViewInfo* nextViewInfo, double scrollTime) const;
    inline unsigned int loopIncrement(size_t offset, size_t index, size_t size) const;
    inline unsigned int loopDecrement(size_t offset, size_t index, size_t size) const;

    bool layoutMode_;
    bool commonMode_;
    bool playlistType_;
    bool selectedImage_;
    bool textFallback_{ true };

    std::vector<Component*>* spriteList_{ nullptr };
    std::vector<ViewInfo*>* scrollPoints_{ nullptr };
    std::vector<AnimationEvents*>* tweenPoints_{ nullptr };

    unsigned int itemIndex_{ 0 };
    unsigned int selectedOffsetIndex_{ 0 };

    float scrollAcceleration_{ 0 };
    float startScrollTime_{ 0.500 };
    float minScrollTime_{ 0.500 };
    float scrollPeriod_{ 0 };

    Configuration& config_;
    Font* fontInst_;
    std::string    layoutKey_;
    std::string    imageType_;
    std::string    videoType_;

    std::vector<Item*>* items_{ nullptr };
    std::vector<Component*> components_;

};
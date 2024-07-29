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

#include "../../SDL.h"
#include "../Page.h"
#include "../ViewInfo.h"
#include "../Animate/Tween.h"
#include "../Animate/AnimationEvents.h"
#include "../../Collection/Item.h"
#include <memory>

class Component
{
public:
    explicit Component(Page &p);
    virtual ~Component();
    virtual void freeGraphicsMemory();
    virtual void allocateGraphicsMemory();
    virtual void deInitializeFonts();
    virtual void initializeFonts();
    const std::string& getAnimationRequestedType() const;
    void triggerEvent(const std::string_view& event, int menuIndex = -1);
    void setPlaylist(const std::string_view& name );
    void setNewItemSelected();
    void setNewScrollItemSelected();
    bool isIdle() const;
    bool isAttractIdle() const;
    bool isMenuScrolling() const;
    bool isPlaylistScrolling() const;
    bool newItemSelected;
    bool newScrollItemSelected;
    void setId( int id );

    virtual std::string_view filePath();
    virtual bool update(float dt);
    virtual void draw();
    void setTweens(AnimationEvents *set);
    virtual bool isPlaying();
    virtual bool isJukeboxPlaying();
    virtual void skipForward( ) {};
    virtual void skipBackward( ) {};
    virtual void skipForwardp( ) {};
    virtual void skipBackwardp( ) {};
    virtual void pause( ) {};
    virtual void restart( ) {};
    virtual unsigned long long getCurrent( ) {return 0;};
    virtual unsigned long long getDuration( ) {return 0;};
    virtual bool isPaused( ) {return false;};
    ViewInfo baseViewInfo;
    std::string collectionName;
    void setMenuScrollReload(bool menuScrollReload);
    bool getMenuScrollReload() const;
    void setAnimationDoneRemove(bool value);
    bool getAnimationDoneRemove() const;
    void setPauseOnScroll(bool value);
    bool getPauseOnScroll() const;
    virtual void setText(const std::string& text, int id = -1) {};
    virtual void setImage(const std::string& filePath, int id = -1) {};
    int getId( ) const;
    std::string playlistName;
    

protected:
    Page &page;

private:

    bool animate();
    bool tweenSequencingComplete();

    AnimationEvents *tweens_;
    std::shared_ptr<Animation> currentTweens_; // Use shared_ptr instead of raw pointer
    SDL_Texture *backgroundTexture_;
    bool         pauseOnScroll_;
    ViewInfo     storeViewInfo_;
    unsigned int currentTweenIndex_;
    bool         currentTweenComplete_;
    float        elapsedTweenTime_;
    std::string  animationRequestedType_;
    std::string  animationType_;
    bool         animationRequested_;
    bool         menuScrollReload_;
    bool         animationDoneRemove_;
    int          menuIndex_;
    int          id_;

};

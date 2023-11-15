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
#include "../../Database/Configuration.h"
#include "../../Collection/Item.h"
#include "../../Utility/Utils.h"
#include "../../Utility/Log.h"
#include "../../SDL.h"
#include "../ViewInfo.h"
#include <math.h>
#include <SDL2/SDL_image.h>
#include <sstream>
#include <cctype>
#include <iomanip>
#include <algorithm>


ScrollingList::ScrollingList( Configuration &c,
                              Page          &p,
                              bool           layoutMode,
                              bool           commonMode,
                              bool          playlistType,
                              bool          selectedImage,
                              Font          *font,
                              const std::string    &layoutKey,
                              const std::string    &imageType,
                              const std::string    &videoType)
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
 {
}


ScrollingList::ScrollingList( const ScrollingList &copy )
    : Component( copy )
    , horizontalScroll( copy.horizontalScroll )
    , layoutMode_( copy.layoutMode_ )
    , commonMode_( copy.commonMode_ )
    , playlistType_(copy.playlistType_)
    , selectedImage_(copy.selectedImage_)
    , selectedOffsetIndex_( copy.selectedOffsetIndex_ )
    , scrollAcceleration_( copy.scrollAcceleration_ )
    , startScrollTime_( copy.startScrollTime_ )
    , minScrollTime_( copy.minScrollTime_ )
    , scrollPeriod_( copy.startScrollTime_ )
    , config_( copy.config_ )
    , fontInst_( copy.fontInst_ )
    , layoutKey_( copy.layoutKey_ )
    , imageType_( copy.imageType_ )
{


    setPoints( copy.scrollPoints_, copy.tweenPoints_ );

}


ScrollingList::~ScrollingList()
{
    destroyItems();
    while (!scrollPoints_->empty()) // Using empty() here for clarity.
    {
        ViewInfo* scrollPoint = scrollPoints_->back();
        delete scrollPoint;
        scrollPoints_->pop_back();
    }
}

const std::vector<Item*>& ScrollingList::getItems() const
{
    return *items_;
}

void ScrollingList::setItems( std::vector<Item *> *items )
{
    items_ = items;
    if (items_)
    {
        itemIndex_ = loopDecrement(0, selectedOffsetIndex_, items_->size());
    }
}

void ScrollingList::selectItemByName(std::string_view name)
{
    size_t size = items_->size();
    unsigned int index = 0;

    for (size_t i = 0; i < size; ++i)
    {
        index = loopDecrement(itemIndex_, i, size);

        // Since items_ is likely storing std::string, using std::string_view for comparison is fine.
        if ((*items_)[(index + selectedOffsetIndex_) % size]->name == name) {
            itemIndex_ = index;
            break;
        }
    }
}


std::string ScrollingList::getSelectedItemName()
{
    size_t size = items_->size();
    if (!size)
        return "";
    
    return (*items_)[(itemIndex_ + selectedOffsetIndex_) % static_cast<int>(size)]->name;
}


unsigned int ScrollingList::loopIncrement(size_t offset, size_t index, size_t size) const
{
    if (size == 0) return 0;
    return static_cast<unsigned int>((offset + index) % size);
}

unsigned int ScrollingList::loopDecrement(size_t offset, size_t index, size_t size) const
{
    if (size == 0) return 0;
    size_t result = offset + size - index;
    return static_cast<unsigned int>(result % size);
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

void ScrollingList::deallocateSpritePoints( )
{
    size_t componentSize = components_.size();
  
    for ( unsigned int i = 0; i < componentSize; ++i )
    {
        deallocateTexture( i );
    }
}


void ScrollingList::allocateSpritePoints()
{
    if (!items_ || items_->empty()) return;
    if (!scrollPoints_ || scrollPoints_->empty()) return;
    if (components_.empty()) return;

    size_t itemsSize = items_->size();
    size_t scrollPointsSize = scrollPoints_->size();

    for (unsigned int i = 0; i < scrollPointsSize; ++i)
    {
        unsigned int index = loopIncrement(itemIndex_, i, itemsSize);
        Item const* item = (*items_)[index];  // using [] instead of at()

        Component* old = components_[i];  // using [] instead of at()

        allocateTexture(i, item);

        Component* c = components_[i];  // using [] instead of at()
        if (c)
        {
            c->allocateGraphicsMemory();

            ViewInfo* view = (*scrollPoints_)[i];  // using [] instead of at()

            resetTweens(c, (*tweenPoints_)[i], view, view, 0);  // using [] instead of at()

            if (old && !newItemSelected)
            {
                c->baseViewInfo = old->baseViewInfo;
                delete old;
            }
        }
    }
}



void ScrollingList::destroyItems()
{
    size_t componentSize = components_.size();

    for (unsigned int i = 0; i < componentSize; ++i)
    {
        Component* component = components_[i];
        if (component)
        {
            component->freeGraphicsMemory();
            delete component;
            components_[i] = NULL;
        }
    }
}


void ScrollingList::setPoints( std::vector<ViewInfo *> *scrollPoints, std::vector<AnimationEvents *> *tweenPoints )
{
    scrollPoints_ = scrollPoints;
    tweenPoints_  = tweenPoints;

    // empty out the list as we will resize it
    components_.clear( );

    size_t size = 0;

    if ( scrollPoints )
    {
        size = scrollPoints_->size();
    }
    components_.resize(size);

    if ( items_ )
    {
        itemIndex_ = loopDecrement( 0, selectedOffsetIndex_, items_->size());
    }
}


unsigned int ScrollingList::getScrollOffsetIndex( ) const
{
    return loopIncrement( itemIndex_, selectedOffsetIndex_, items_->size());
}


void ScrollingList::setScrollOffsetIndex( unsigned int index )
{
    itemIndex_ = loopDecrement( index, selectedOffsetIndex_, items_->size());
}


void ScrollingList::setSelectedIndex( int selectedIndex )
{
    selectedOffsetIndex_ = selectedIndex;
}


Item *ScrollingList::getItemByOffset(int offset)
{
    size_t itemSize = items_->size();
    if (!items_ || itemSize == 0) return nullptr;

    unsigned int index = getSelectedIndex();
    if (offset >= 0)
    {
        index = loopIncrement(index, offset, itemSize);
    }
    else
    {
        index = loopDecrement(index, offset * -1, itemSize);
    }
    
    return (*items_)[index];
}



Item* ScrollingList::getSelectedItem()
{
    size_t itemSize = items_->size();
    if (!items_ || itemSize == 0) return nullptr;
    
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
    size_t itemSize = items_->size();
    if ( !items_ || itemSize == 0 ) return;
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


void ScrollingList::letterChange(bool increment)
{
    size_t itemSize = items_->size();
    if (!items_ || itemSize == 0) return;

    Item const* startItem = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize];
    std::string startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

    for (unsigned int i = 0; i < itemSize; ++i)
    {
        unsigned int index = increment ? loopIncrement(itemIndex_, i, itemSize) : loopDecrement(itemIndex_, i, itemSize);

        std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

        if ((isalpha(startname[0]) ^ isalpha(endname[0])) ||
            (isalpha(startname[0]) && isalpha(endname[0]) && startname[0] != endname[0]))
        {
            itemIndex_ = index;
            break;
        }
    }

    if (!increment)
    {
        bool prevLetterSubToCurrent = false;
        config_.getProperty("prevLetterSubToCurrent", prevLetterSubToCurrent);
        if (!prevLetterSubToCurrent || (*items_)[(itemIndex_ + 1 + selectedOffsetIndex_) % itemSize] == startItem)
        {
            startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

            for (unsigned int i = 0; i < itemSize; ++i)
            {
                unsigned int index = loopDecrement(itemIndex_, i, itemSize);

                std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

                if ((isalpha(startname[0]) ^ isalpha(endname[0])) ||
                    (isalpha(startname[0]) && isalpha(endname[0]) && startname[0] != endname[0]))
                {
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
    size_t itemSize = items_->size();

    if (!items_ || itemSize == 0) return;

    const Item* startItem = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize];
    std::string startValue = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->getMetaAttribute(attribute);

    for (unsigned int i = 0; i < itemSize; ++i)
    {
        unsigned int index = increment ? loopIncrement(itemIndex_, i, itemSize) : loopDecrement(itemIndex_, i, itemSize);
        std::string endValue = (*items_)[(index + selectedOffsetIndex_) % itemSize]->getMetaAttribute(attribute);

        if (startValue != endValue) {
            itemIndex_ = index;
            break;
        }
    }

    if (!increment)
    {
        bool prevLetterSubToCurrent = false;
        config_.getProperty("prevLetterSubToCurrent", prevLetterSubToCurrent);
        if (!prevLetterSubToCurrent || (*items_)[(itemIndex_ + 1 + selectedOffsetIndex_) % itemSize] == startItem)
        {
            startValue = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->getMetaAttribute(attribute);

            for (unsigned int i = 0; i < itemSize; ++i)
            {
                unsigned int index = loopDecrement(itemIndex_, i, itemSize);
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
    size_t itemSize = items_->size();

    if (!items_ || itemSize == 0) return;

    const Item* startItem = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize];
    std::string startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

    for (unsigned int i = 0; i < itemSize; ++i)
    {
        unsigned int index = increment ? loopIncrement(itemIndex_, i, itemSize) : loopDecrement(itemIndex_, i, itemSize);
        std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

        if (startname != endname)
        {
            itemIndex_ = index;
            break;
        }
    }

    if (!increment) // For decrement, find the first game of the new sub
    {
        bool prevLetterSubToCurrent = false;
        config_.getProperty("prevLetterSubToCurrent", prevLetterSubToCurrent);
        if (!prevLetterSubToCurrent || (*items_)[(itemIndex_ + 1 + selectedOffsetIndex_) % itemSize] == startItem)
        {
            startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

            for (unsigned int i = 0; i < itemSize; ++i)
            {
                unsigned int index = loopDecrement(itemIndex_, i, itemSize);
                std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->collectionInfo->lowercaseName();

                if (startname != endname)
                {
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


void ScrollingList::cfwLetterSubUp()
{
    if (Utils::toLower(collectionName) != (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName())
        subChange(true);
    else
        letterChange(true);
}


void ScrollingList::cfwLetterSubDown()
{
    if (Utils::toLower(collectionName) != (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName())
    {
        subChange(false);
        if (Utils::toLower(collectionName) == (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName())
        {
            subChange(true);
            letterChange(false);
        }
    }
    else
    {
        letterChange(false);
        if (Utils::toLower(collectionName) != (*items_)[(itemIndex_+selectedOffsetIndex_) % items_->size()]->collectionInfo->lowercaseName())
        {
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


void ScrollingList::freeGraphicsMemory( )
{
    Component::freeGraphicsMemory( );
    scrollPeriod_ = 0;
    
    deallocateSpritePoints( );
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
    for (size_t i = 0; i < componentSize; ++i)
    {
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
    
    for (unsigned int i = 0; i < scrollPointsSize; i++)
    {
        Component *c = components_[i];
        if (c) 
        {
            c->playlistName = playlistName;
            done &= c->update(dt);
        }
    }

    return done;
}



unsigned int ScrollingList::getSelectedIndex( ) const
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


void ScrollingList::resetTweens( Component *c, AnimationEvents *sets, ViewInfo *currentViewInfo, ViewInfo *nextViewInfo, double scrollTime ) const
{
    if ( !c ) return;
    if ( !sets ) return;
    if ( !currentViewInfo ) return;
    if ( !nextViewInfo ) return;

    currentViewInfo->ImageHeight  = c->baseViewInfo.ImageHeight;
    currentViewInfo->ImageWidth   = c->baseViewInfo.ImageWidth;
    nextViewInfo->ImageHeight     = c->baseViewInfo.ImageHeight;
    nextViewInfo->ImageWidth      = c->baseViewInfo.ImageWidth;
    nextViewInfo->BackgroundAlpha = c->baseViewInfo.BackgroundAlpha;
	

    c->setTweens(sets );

    Animation *scrollTween = sets->getAnimation("menuScroll" );
    scrollTween->Clear( );
    c->baseViewInfo = *currentViewInfo;

    auto *set = new TweenSet( );
    // don't trigger video restart if scrolling fast 
    if (currentViewInfo->Restart && scrollPeriod_ > minScrollTime_)
        set->push(new Tween(TWEEN_PROPERTY_RESTART, LINEAR, currentViewInfo->Restart, nextViewInfo->Restart, 0));

    set->push(new Tween(TWEEN_PROPERTY_HEIGHT, LINEAR, currentViewInfo->Height, nextViewInfo->Height, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_WIDTH, LINEAR, currentViewInfo->Width, nextViewInfo->Width, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_ANGLE, LINEAR, currentViewInfo->Angle, nextViewInfo->Angle, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_ALPHA, LINEAR, currentViewInfo->Alpha, nextViewInfo->Alpha, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_X, LINEAR, currentViewInfo->X, nextViewInfo->X, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_Y, LINEAR, currentViewInfo->Y, nextViewInfo->Y, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_X_ORIGIN, LINEAR, currentViewInfo->XOrigin, nextViewInfo->XOrigin, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_Y_ORIGIN, LINEAR, currentViewInfo->YOrigin, nextViewInfo->YOrigin, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_X_OFFSET, LINEAR, currentViewInfo->XOffset, nextViewInfo->XOffset, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_Y_OFFSET, LINEAR, currentViewInfo->YOffset, nextViewInfo->YOffset, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_FONT_SIZE, LINEAR, currentViewInfo->FontSize, nextViewInfo->FontSize, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_BACKGROUND_ALPHA, LINEAR, currentViewInfo->BackgroundAlpha, nextViewInfo->BackgroundAlpha, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_MAX_WIDTH, LINEAR, currentViewInfo->MaxWidth, nextViewInfo->MaxWidth, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_MAX_HEIGHT, LINEAR, currentViewInfo->MaxHeight, nextViewInfo->MaxHeight, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_LAYER, LINEAR, currentViewInfo->Layer, nextViewInfo->Layer, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_VOLUME, LINEAR, currentViewInfo->Volume, nextViewInfo->Volume, scrollTime ) );
    set->push(new Tween(TWEEN_PROPERTY_MONITOR, LINEAR, currentViewInfo->Monitor, nextViewInfo->Monitor, scrollTime ) );

    scrollTween->Push( set );
}

bool ScrollingList::allocateTexture( unsigned int index, const Item *item )
{

    if ( index >= components_.size( ) ) return false;

    std::string imagePath;
    std::string videoPath;

    Component *t = nullptr;

    ImageBuilder imageBuild;
    VideoBuilder videoBuild{};

    std::string layoutName;
    config_.getProperty( "layout", layoutName );

    std::string typeLC = Utils::toLower( imageType_ );

    std::vector<std::string> names;
    names.push_back( item->name );
    names.push_back( item->fullTitle );
    if ( item->cloneof != "" )
        names.push_back( item->cloneof );
    if ( typeLC == "numberbuttons" )
        names.push_back( item->numberButtons );
    if ( typeLC == "numberplayers" )
        names.push_back( item->numberPlayers );
    if ( typeLC == "year" )
        names.push_back( item->year );
    if ( typeLC == "title" )
        names.push_back( item->title );
    if ( typeLC == "developer" )
    {
        if ( item->developer == "" )
        {
            names.push_back( item->manufacturer );
        }
        else
        {
            names.push_back( item->developer );
        }
    }
    if ( typeLC == "manufacturer" )
        names.push_back( item->manufacturer );
    if ( typeLC == "genre" )
        names.push_back( item->genre );
    if ( typeLC == "ctrltype" )
        names.push_back( item->ctrlType );
    if ( typeLC == "joyways" )
        names.push_back( item->joyWays );
    if ( typeLC == "rating" )
        names.push_back( item->rating );
    if ( typeLC == "score" )
        names.push_back( item->score );
    if (typeLC.rfind("playlist", 0) == 0)
        names.push_back(item->name);
    names.emplace_back("default");

    std::string name;
    std::string selectedItemName = getSelectedItemName();
    for (const auto& name : names) {
        std::string imagePath;
        std::string videoPath;

        // Determine paths based on modes
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

        // Create video or image
        if (!t) {
            if (videoType_ != "null") {
                t = videoBuild.createVideo(videoPath, page, name, baseViewInfo.Monitor);
            }
            else {
                std::string imageName = selectedImage_ && item->name == selectedItemName ? name + "-selected" : name;
                t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive);
            }
        }

        // Check for early exit
        if (t) break;

        // Check sub-collection path for art
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
                    t = videoBuild.createVideo(videoPath, page, name, baseViewInfo.Monitor);
                }
                else {
                    std::string imageName = selectedImage_ && item->name == selectedItemName ? name + "-selected" : name;
                    t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive);
                }
            }
        }

        // Check for early exit again
        if (t) break;
    }

    // check collection path for art based on system name
    if ( !t )
    {
        if ( layoutMode_ )
        {
            if ( commonMode_ )
                imagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", "_common");
            else
                imagePath = Utils::combinePath( Configuration::absolutePath, "layouts", layoutName, "collections", item->name );
            imagePath = Utils::combinePath( imagePath, "system_artwork" );
            videoPath = imagePath;
        }
        else
        {
            if ( commonMode_ )
            {
                imagePath = Utils::combinePath(Configuration::absolutePath, "collections", "_common" );
                imagePath = Utils::combinePath( imagePath, "system_artwork" );
                videoPath = imagePath;
            }
            else
            {
                config_.getMediaPropertyAbsolutePath( item->name, imageType_, true, imagePath );
                config_.getMediaPropertyAbsolutePath( item->name, videoType_, true, videoPath );
            }
        }
        if ( videoType_ != "null" )
        {
            t = videoBuild.createVideo( videoPath, page, videoType_, baseViewInfo.Monitor);
        }
        else
        {
            name = imageType_;
            if (selectedImage_ && item->name == selectedItemName) {
                t = imageBuild.CreateImage(imagePath, page, name + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive);
            }
            if (!t) {
                t = imageBuild.CreateImage(imagePath, page, name, baseViewInfo.Monitor, baseViewInfo.Additive);
            }
        }
    }

    // check rom directory path for art
    if ( !t )
    {
        if ( videoType_ != "null" )
        {
            t = videoBuild.createVideo( item->filepath, page, videoType_, baseViewInfo.Monitor);
        }
        else
        {
            name = imageType_;
            if (selectedImage_ && item->name == selectedItemName) {
                t = imageBuild.CreateImage(item->filepath, page, name + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive);
            }
            if (!t) {
                t = imageBuild.CreateImage(item->filepath, page, name, baseViewInfo.Monitor, baseViewInfo.Additive);
            }
        }
    }

    // Check for fallback art in case no video could be found
    if ( videoType_ != "null" && !t)
    {
        for (const auto& name : names) {
            if (t) break; // Early exit if media is already created

            // Build paths for medium artwork
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

            // Try to create image
            std::string imageName = selectedImage_ && item->name == selectedItemName ? name + "-selected" : name;
            t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive);

            // Check sub-collection path for art if needed
            if (!t && !commonMode_) {
                if (layoutMode_) {
                    std::string base = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", item->collectionInfo->name);
                    buildPaths(imagePath, videoPath, base, "", imageType_, videoType_);
                }
                else {
                    config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, imageType_, false, imagePath);
                    config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, videoType_, false, videoPath);
                }

                // Try to create image again
                imageName = selectedImage_ && item->name == selectedItemName ? name + "-selected" : name;
                t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive);
            }
        }


        // check collection path for art based on system name
        if ( !t )
        {
            if ( layoutMode_ )
            {
                if ( commonMode_ )
                    imagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", "_common");
                else
                    imagePath = Utils::combinePath( Configuration::absolutePath, "layouts", layoutName, "collections", item->name );
                imagePath = Utils::combinePath( imagePath, "system_artwork" );
            }
            else
            {
                if ( commonMode_ )
                {
                    imagePath = Utils::combinePath(Configuration::absolutePath, "collections", "_common" );
                    imagePath = Utils::combinePath( imagePath, "system_artwork" );
                }
                else
                {
                    config_.getMediaPropertyAbsolutePath( item->name, imageType_, true, imagePath );
                }
            }
            if ( !t )
            {
                name = imageType_;
                if (selectedImage_ && item->name == selectedItemName) {
                    t = imageBuild.CreateImage(imagePath, page, name + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive);
                }
                if (!t) {
                    t = imageBuild.CreateImage(imagePath, page, name, baseViewInfo.Monitor, baseViewInfo.Additive);
                }
            }
        }
        // check rom directory path for art
        if ( !t )
        {
            name = imageType_;
            if (selectedImage_ && item->name == selectedItemName) {
                t = imageBuild.CreateImage(item->filepath, page, name + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive);
            }
            if (!t) {
                t = imageBuild.CreateImage(item->filepath, page, name, baseViewInfo.Monitor, baseViewInfo.Additive);
            }
        }

    }

    if (!t)
    {
        std::string title = item->title;
        if (!textFallback_){
            title = "";
        }
        t = new Text(title, page, fontInst_, baseViewInfo.Monitor );
    }

    if ( t )
    {
        components_[index] = t;
    }

    return true;
}


void ScrollingList::buildPaths(std::string& imagePath, std::string& videoPath, const std::string& base, const std::string& subPath, const std::string& mediaType, const std::string& videoType) {
    imagePath = Utils::combinePath(base, subPath, "medium_artwork", mediaType);
    videoPath = Utils::combinePath(imagePath, "medium_artwork", videoType);
}

void ScrollingList::deallocateTexture( unsigned int index )
{
    if ( components_.size(  ) <= index ) return;

    Component *s = components_[index];

    if ( s )
        s->freeGraphicsMemory(  );
}

void ScrollingList::draw(  )
{
    //todo: Poor design implementation.
    // caller should instead call ScrollingList::Draw( unsigned int layer )
}


void ScrollingList::draw(unsigned int layer)
{
    size_t componentSize = components_.size();
    
    if (componentSize == 0) return;

    for (unsigned int i = 0; i < componentSize; ++i)
    {
        Component *c = components_[i];
        if (c && c->baseViewInfo.Layer == layer) c->draw();
    }
}


bool ScrollingList::isIdle(  )
{
    size_t componentSize = components_.size();
    if ( !Component::isIdle(  ) ) return false;

    for ( unsigned int i = 0; i < componentSize; ++i )
    {
        Component const *c = components_[i];
        if ( c && !c->isIdle(  ) ) return false;
    }

    return true;
}


bool ScrollingList::isAttractIdle(  )
{
    size_t componentSize = components_.size();
    if ( !Component::isAttractIdle(  ) ) return false;

    for ( unsigned int i = 0; i < componentSize; ++i )
    {
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


void ScrollingList::scroll(bool forward)
{
    // Exit conditions
    if (playlistType_ || !items_ || items_->empty() || !scrollPoints_ || scrollPoints_->empty())
        return;

    if (scrollPeriod_ < minScrollTime_)
        scrollPeriod_ = minScrollTime_;

    size_t itemsSize = items_->size();
    size_t scrollPointsSize = scrollPoints_->size();

    // Replace the item that's scrolled out
    Item const *itemToScroll;
    if (forward)
    {
        itemToScroll = (*items_)[loopIncrement(itemIndex_, scrollPointsSize, itemsSize)];
        itemIndex_ = loopIncrement(itemIndex_, 1, itemsSize);
        deallocateTexture(0);
        allocateTexture(0, itemToScroll);
    }
    else
    {
        itemToScroll = (*items_)[loopDecrement(itemIndex_, 1, itemsSize)];
        itemIndex_ = loopDecrement(itemIndex_, 1, itemsSize);
        deallocateTexture(loopDecrement(0, 1, components_.size()));
        allocateTexture(loopDecrement(0, 1, components_.size()), itemToScroll);
    }

    // Set the animations
    for (size_t index = 0; index < scrollPointsSize; ++index)  // Renamed from 'i' to 'index'
    {
        size_t nextIndex;
        if (forward)
        {
            nextIndex = (index == 0) ? scrollPointsSize - 1 : index - 1;
        }
        else
        {
            nextIndex = (index == scrollPointsSize - 1) ? 0 : index + 1;
        }

        Component* component = components_[index];  // Renamed from 'c' to 'component' for clarity
        if (component)
        {
            auto& nextTweenPoint = (*tweenPoints_)[nextIndex];
            auto& currentScrollPoint = (*scrollPoints_)[index];
            auto& nextScrollPoint = (*scrollPoints_)[nextIndex];

            component->allocateGraphicsMemory();
            resetTweens(component, nextTweenPoint, currentScrollPoint, nextScrollPoint, scrollPeriod_);
            component->baseViewInfo.font = nextScrollPoint->font;
            component->triggerEvent("menuScroll");
        }
    }

    // Reorder the components using std::rotate
    if (forward)
    {
        std::rotate(components_.begin(), components_.begin() + 1, components_.begin() + scrollPointsSize);
    }
    else
    {
        std::rotate(components_.begin(), components_.end() - 1, components_.end());
    }

    return;
}


bool ScrollingList::isPlaylist() const
{
    return playlistType_;
}

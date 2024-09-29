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
#include "../../Database/GlobalOpts.h"
#include "../../Collection/Item.h"
#include "../../Utility/Utils.h"
#include "../../Utility/Log.h"
#include "../../SDL.h"
#include "../ViewInfo.h"
#include <math.h>
#if (__APPLE__)
    #include <SDL2_image/SDL_image.h>
#else
    #include <SDL2/SDL_image.h>
#endif
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


ScrollingList::~ScrollingList() {
    clearPoints();
    destroyItems();
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

std::string ScrollingList::getSelectedItemName()
{
    size_t size = items_->size();
    if (!size)
        return "";
    
    return (*items_)[(itemIndex_ + selectedOffsetIndex_) % static_cast<int>(size)]->name;
}

static size_t loopIncrement(size_t offset, size_t i, size_t size) {
    if (size == 0) return 0;
    return (offset + i) % size;
}

static size_t loopDecrement(size_t offset, size_t i, size_t size) {
    if (size == 0) return 0;
    return (offset + size - i) % size; // Adjusted to use size_t and ensure no underflow
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

void ScrollingList::allocateSpritePoints() {
    if (!items_ || items_->empty()) return;
    if (!scrollPoints_ || scrollPoints_->empty()) return;
    if (components_.empty()) return;

    size_t itemsSize = items_->size();
    size_t scrollPointsSize = scrollPoints_->size();

    for (size_t i = 0; i < scrollPointsSize; ++i) {
        size_t index = loopIncrement(itemIndex_, i, itemsSize);
        Item const* item = (*items_)[index];  // using [] instead of at()

        Component* old = components_[i];  // using [] instead of at()

        allocateTexture(i, item);

        Component* c = components_[i];  // using [] instead of at()
        if (c) {
            c->allocateGraphicsMemory();

            ViewInfo* view = (*scrollPoints_)[i];  // using [] instead of at()

            resetTweens(c, (*tweenPoints_)[i], view, view, 0);  // using [] instead of at()

            if (old && !newItemSelected) {
                c->baseViewInfo = old->baseViewInfo;
                delete old;
            }
        }
    }
}

void ScrollingList::destroyItems()
{
    size_t componentSize = components_.size();

    for (unsigned int i = 0; i < componentSize; ++i) {
        if (Component* component = components_[i]) {
            component->freeGraphicsMemory();
            delete component;
        }
        components_[i] = NULL;
    }
}

void ScrollingList::setPoints(std::vector<ViewInfo*>* scrollPoints, std::shared_ptr<std::vector<std::shared_ptr<AnimationEvents>>> tweenPoints) {
    clearPoints();
    clearTweenPoints();

    scrollPoints_ = scrollPoints;
    tweenPoints_ = tweenPoints;

    components_.clear();

    size_t size = 0;

    if (scrollPoints_) {
        size = scrollPoints_->size();
    }
    components_.resize(size);

    if (items_) {
        itemIndex_ = loopDecrement(0, selectedOffsetIndex_, items_->size());
    }
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

void ScrollingList::letterChange(bool increment)
{
    // First, check if items_ is nullptr or empty
    if (!items_ || items_->empty()) return;
    size_t itemSize = items_->size();

    Item const* startItem = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize];
    std::string startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

    for (size_t i = 0; i < itemSize; ++i) {
        size_t index = increment ? loopIncrement(itemIndex_, i, itemSize) : loopDecrement(itemIndex_, i, itemSize);

        std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

        if ((isalpha(startname[0]) ^ isalpha(endname[0])) ||
            (isalpha(startname[0]) && isalpha(endname[0]) && startname[0] != endname[0])) {
            itemIndex_ = index;
            break;
        }
    }

    if (!increment) {
        bool prevLetterSubToCurrent = false;
        config_.getProperty(OPTION_PREVLETTERSUBTOCURRENT, prevLetterSubToCurrent);
        if (!prevLetterSubToCurrent || (*items_)[(itemIndex_ + 1 + selectedOffsetIndex_) % itemSize] == startItem) {
            startname = (*items_)[(itemIndex_ + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

            for (size_t i = 0; i < itemSize; ++i) {
                size_t index = loopDecrement(itemIndex_, i, itemSize);

                std::string endname = (*items_)[(index + selectedOffsetIndex_) % itemSize]->lowercaseFullTitle();

                if ((isalpha(startname[0]) ^ isalpha(endname[0])) ||
                    (isalpha(startname[0]) && isalpha(endname[0]) && startname[0] != endname[0])) {
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

size_t ScrollingList::loopIncrement(size_t currentIndex, size_t incrementAmount, size_t listSize) const {
    if (listSize == 0) return 0;
    return (currentIndex + incrementAmount) % listSize;
}


size_t ScrollingList::loopDecrement(size_t currentIndex, size_t decrementAmount, size_t listSize) const {
    if (listSize == 0) return 0;
    return (currentIndex + listSize - decrementAmount) % listSize;
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

void ScrollingList::resetTweens(Component* c, std::shared_ptr<AnimationEvents> sets, ViewInfo* currentViewInfo, ViewInfo* nextViewInfo, double scrollTime) const {
    if (!c || !sets || !currentViewInfo || !nextViewInfo) return;

    // Ensure that currentViewInfo and nextViewInfo are initialized from the component's baseViewInfo
    currentViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    currentViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;
    nextViewInfo->ImageHeight = c->baseViewInfo.ImageHeight;
    nextViewInfo->ImageWidth = c->baseViewInfo.ImageWidth;
    nextViewInfo->BackgroundAlpha = c->baseViewInfo.BackgroundAlpha;

    // Prepare the component to handle tweens
    c->setTweens(sets);

    // Get the scroll animation
    std::shared_ptr<Animation> scrollTween = sets->getAnimation("menuScroll");

    // Try to retrieve the first TweenSet, if it exists
    std::shared_ptr<TweenSet> set = scrollTween->tweenSet(0);  // Assuming you're dealing with a single TweenSet

    if (!set) {
        // If no TweenSet exists, create a new one and push it into the Animation
        set = std::make_shared<TweenSet>();
        scrollTween->Push(set);
    } else {
        // Clear the existing TweenSet for reuse
        set->clear();  // This clears the tweens but keeps the TweenSet object intact
    }

    // Update the component's base view info
    c->baseViewInfo = *currentViewInfo;

    // Lambda to add a tween whether the property has changed or not (to ensure a full set)
    auto addTween = [&](double currentProperty, double nextProperty, TweenProperty propertyType) {
        set->push(std::make_unique<Tween>(propertyType, LINEAR, currentProperty, nextProperty, scrollTime));
        };

    // Add tweens for all properties
    addTween(currentViewInfo->Height, nextViewInfo->Height, TWEEN_PROPERTY_HEIGHT);
    addTween(currentViewInfo->Width, nextViewInfo->Width, TWEEN_PROPERTY_WIDTH);
    addTween(currentViewInfo->Angle, nextViewInfo->Angle, TWEEN_PROPERTY_ANGLE);
    addTween(currentViewInfo->Alpha, nextViewInfo->Alpha, TWEEN_PROPERTY_ALPHA);
    addTween(currentViewInfo->X, nextViewInfo->X, TWEEN_PROPERTY_X);
    addTween(currentViewInfo->Y, nextViewInfo->Y, TWEEN_PROPERTY_Y);
    addTween(currentViewInfo->XOrigin, nextViewInfo->XOrigin, TWEEN_PROPERTY_X_ORIGIN);
    addTween(currentViewInfo->YOrigin, nextViewInfo->YOrigin, TWEEN_PROPERTY_Y_ORIGIN);
    addTween(currentViewInfo->XOffset, nextViewInfo->XOffset, TWEEN_PROPERTY_X_OFFSET);
    addTween(currentViewInfo->YOffset, nextViewInfo->YOffset, TWEEN_PROPERTY_Y_OFFSET);
    addTween(currentViewInfo->FontSize, nextViewInfo->FontSize, TWEEN_PROPERTY_FONT_SIZE);
    addTween(currentViewInfo->BackgroundAlpha, nextViewInfo->BackgroundAlpha, TWEEN_PROPERTY_BACKGROUND_ALPHA);
    addTween(currentViewInfo->MaxWidth, nextViewInfo->MaxWidth, TWEEN_PROPERTY_MAX_WIDTH);
    addTween(currentViewInfo->MaxHeight, nextViewInfo->MaxHeight, TWEEN_PROPERTY_MAX_HEIGHT);
    addTween(currentViewInfo->Layer, nextViewInfo->Layer, TWEEN_PROPERTY_LAYER);
    addTween(currentViewInfo->Volume, nextViewInfo->Volume, TWEEN_PROPERTY_VOLUME);
    addTween(currentViewInfo->Monitor, nextViewInfo->Monitor, TWEEN_PROPERTY_MONITOR);

    // Special case for 'Restart' property, only set if scrollPeriod_ > minScrollTime_
    if (scrollPeriod_ > minScrollTime_) {
        addTween(currentViewInfo->Restart, nextViewInfo->Restart, TWEEN_PROPERTY_RESTART);
    }
}

bool ScrollingList::allocateTexture( size_t index, const Item *item )
{

    if ( index >= components_.size( ) ) return false;

    std::string imagePath;
    std::string videoPath;

    Component *t = nullptr;

    ImageBuilder imageBuild;
    VideoBuilder videoBuild{};

    std::string layoutName;
    config_.getProperty( OPTION_LAYOUT, layoutName );

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
    if ( typeLC == "developer" ) {
        if ( item->developer == "" ) {
            names.push_back( item->manufacturer );
        }
        else {
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

    std::string name;  // Declare `name` here for use outside the loop
    std::string selectedItemName = getSelectedItemName();
    
    for (const auto& currentName : names) {  // Use `currentName` to avoid shadowing
        std::string imagePath;
        std::string videoPath;

        // Determine paths based on modes
        if (layoutMode_) {
            std::string base = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections");
            std::string subPath = commonMode_ ? "_common" : collectionName;
            buildPaths(imagePath, videoPath, base, subPath, imageType_, videoType_);
        } else {
            if (commonMode_) {
                buildPaths(imagePath, videoPath, Configuration::absolutePath, "collections/_common", imageType_, videoType_);
            } else {
                config_.getMediaPropertyAbsolutePath(collectionName, imageType_, false, imagePath);
                config_.getMediaPropertyAbsolutePath(collectionName, videoType_, false, videoPath);
            }
        }

        // Create video or image
        if (!t) {
            if (videoType_ != "null") {
                t = videoBuild.createVideo(videoPath, page, currentName, baseViewInfo.Monitor);
            } else {
                std::string imageName = selectedImage_ && item->name == selectedItemName ? currentName + "-selected" : currentName;
                t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive);
            }
        }

        // Check for early exit
        if (t) {
            name = currentName;  // Keep track of the last `currentName` used
            break;
        }

        // Check sub-collection path for art
        if (!commonMode_) {
            if (layoutMode_) {
                std::string base = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", item->collectionInfo->name);
                buildPaths(imagePath, videoPath, base, "", imageType_, videoType_);
            } else {
                config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, imageType_, false, imagePath);
                config_.getMediaPropertyAbsolutePath(item->collectionInfo->name, videoType_, false, videoPath);
            }

            if (!t) {
                if (videoType_ != "null") {
                    t = videoBuild.createVideo(videoPath, page, currentName, baseViewInfo.Monitor);
                } else {
                    std::string imageName = selectedImage_ && item->name == selectedItemName ? currentName + "-selected" : currentName;
                    t = imageBuild.CreateImage(imagePath, page, imageName, baseViewInfo.Monitor, baseViewInfo.Additive);
                }
            }
        }

        // Check for early exit again
        if (t) {
            name = currentName;  // Keep track of the last `currentName` used
            break;
        }
    }

    // check collection path for art based on system name
    if ( !t ) {
        if ( layoutMode_ ) {
            if ( commonMode_ )
                imagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", "_common");
            else
                imagePath = Utils::combinePath( Configuration::absolutePath, "layouts", layoutName, "collections", item->name );
            imagePath = Utils::combinePath( imagePath, "system_artwork" );
            videoPath = imagePath;
        }
        else {
            if ( commonMode_ ) {
                imagePath = Utils::combinePath(Configuration::absolutePath, "collections", "_common" );
                imagePath = Utils::combinePath( imagePath, "system_artwork" );
                videoPath = imagePath;
            }
            else {
                config_.getMediaPropertyAbsolutePath( item->name, imageType_, true, imagePath );
                config_.getMediaPropertyAbsolutePath( item->name, videoType_, true, videoPath );
            }
        }
        if ( videoType_ != "null" ) {
            t = videoBuild.createVideo( videoPath, page, videoType_, baseViewInfo.Monitor);
        }
        else {
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
    if ( !t ) {
        if ( videoType_ != "null" ) {
            t = videoBuild.createVideo( item->filepath, page, videoType_, baseViewInfo.Monitor);
        }
        else {
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
    if ( videoType_ != "null" && !t) {
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
        if ( !t ) {
            if ( layoutMode_ ) {
                if ( commonMode_ )
                    imagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", "_common");
                else
                    imagePath = Utils::combinePath( Configuration::absolutePath, "layouts", layoutName, "collections", item->name );
                imagePath = Utils::combinePath( imagePath, "system_artwork" );
            }
            else {
                if ( commonMode_ ) {
                    imagePath = Utils::combinePath(Configuration::absolutePath, "collections", "_common" );
                    imagePath = Utils::combinePath( imagePath, "system_artwork" );
                }
                else {
                    config_.getMediaPropertyAbsolutePath( item->name, imageType_, true, imagePath );
                }
            }
            if ( !t ) {
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
        if ( !t ) {
            name = imageType_;
            if (selectedImage_ && item->name == selectedItemName) {
                t = imageBuild.CreateImage(item->filepath, page, name + "-selected", baseViewInfo.Monitor, baseViewInfo.Additive);
            }
            if (!t) {
                t = imageBuild.CreateImage(item->filepath, page, name, baseViewInfo.Monitor, baseViewInfo.Additive);
            }
        }

    }

    if (!t) {
        if (textFallback_) {  // Check if fallback text should be used
            t = new Text(item->title, page, fontInst_, baseViewInfo.Monitor);  // Use item's title
        }
    }

    if ( t ) {
        components_[index] = t;
    }

    return true;
}
void ScrollingList::buildPaths(std::string& imagePath, std::string& videoPath, const std::string& base, const std::string& subPath, const std::string& mediaType, const std::string& videoType) {
    imagePath = Utils::combinePath(base, subPath, "medium_artwork", mediaType);
    videoPath = Utils::combinePath(imagePath, "medium_artwork", videoType);
}

void ScrollingList::deallocateTexture( size_t index )
{
    if ( components_.size(  ) <= index ) return;

    Component *s = components_[index];

    if (s) {
        s->freeGraphicsMemory();
        delete s;
        components_[index] = nullptr;
    }
}

const std::vector<Component*>& ScrollingList::getComponents() const {
    return components_;
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

void ScrollingList::scroll(bool forward)
{
    // Exit conditions
    if (!items_ || items_->empty() || !scrollPoints_ || scrollPoints_->empty())
        return;

    if (scrollPeriod_ < minScrollTime_)
        scrollPeriod_ = minScrollTime_;

    size_t itemsSize = items_->size();
    size_t scrollPointsSize = scrollPoints_->size();

    // Identify which component to scroll out and in
    Item const* itemToScrollIn;
    size_t indexToDeallocate = forward ? 0 : loopDecrement(0, 1, components_.size());
    size_t indexToAllocate = forward ? loopIncrement(itemIndex_, scrollPointsSize, itemsSize)
        : loopDecrement(itemIndex_, 1, itemsSize);

    if (forward) {
        itemToScrollIn = (*items_)[indexToAllocate];
        itemIndex_ = loopIncrement(itemIndex_, 1, itemsSize);
    }
    else {
        itemToScrollIn = (*items_)[indexToAllocate];
        itemIndex_ = loopDecrement(itemIndex_, 1, itemsSize);
    }

    // 1. **Only deallocate the component that's scrolled out**
    deallocateTexture(indexToDeallocate);  // Deallocate the component that's scrolled out of view

    // 2. **Allocate a new component for the newly scrolled-in item**
    allocateTexture(indexToDeallocate, itemToScrollIn);  // Allocate the new one for the newly visible item

    // 3. **Do not deallocate/reallocate components that are just shifting**
    // Set animations and tweens for components that remain in view
    for (size_t index = 0; index < scrollPointsSize; ++index) {
        size_t nextIndex = forward ? (index == 0 ? scrollPointsSize - 1 : index - 1)
            : (index == scrollPointsSize - 1 ? 0 : index + 1);

        Component* component = components_[index];  // Use the current component in the list
        if (component) {
            auto const& nextTweenPoint = (*tweenPoints_)[nextIndex];
            auto& currentScrollPoint = (*scrollPoints_)[index];
            auto& nextScrollPoint = (*scrollPoints_)[nextIndex];

            component->allocateGraphicsMemory();  // Ensure the component has allocated resources
            resetTweens(component, nextTweenPoint, currentScrollPoint, nextScrollPoint, scrollPeriod_);
            component->baseViewInfo.font = nextScrollPoint->font;  // Set the font for the next scroll point
            component->triggerEvent("menuScroll");  // Trigger menu scroll event for animation
        }
    }

    // Reorder components using std::rotate for forward or backward scroll
    if (forward) {
        std::rotate(components_.begin(), components_.begin() + 1, components_.end());
    }
    else {
        std::rotate(components_.rbegin(), components_.rbegin() + 1, components_.rend());
    }
}
bool ScrollingList::isPlaylist() const
{
    return playlistType_;
}

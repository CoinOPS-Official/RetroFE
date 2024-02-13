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

#include "Page.h"
#include "Component/Component.h"
#include "../Collection/CollectionInfo.h"
#include "Component/Text.h"
#include "../Utility/Log.h"
#include "Component/ScrollingList.h"
#include "../Sound/Sound.h"
#include "PageBuilder.h"
#include <algorithm>
#include <sstream>
#include "../Utility/Utils.h"


Page::Page(Configuration &config, int layoutWidth, int layoutHeight)
    : config_(config)
{

    for (int i = 0; i < MAX_LAYOUTS; i++)
    {
        layoutWidth_.push_back(layoutWidth);
        layoutHeight_.push_back(layoutHeight);
    }
    for (int i = 0; i < SDL::getScreenCount(); i++)
    {
        layoutWidthByMonitor_.push_back(layoutWidth);
        layoutHeightByMonitor_.push_back(layoutHeight);
    }

    currentLayout_ = 0;
}


Page::~Page() = default;


void Page::deInitialize()
{
    cleanup();

    // Deinitialize and clear menus_
    for (auto& menuVector : menus_)
    {
        for (ScrollingList* menu : menuVector)
        {
            delete menu;
        }
        menuVector.clear();
    }
    menus_.clear();

    // Deinitialize and clear LayerComponents
    for (Component* component : LayerComponents)
    {
        component->freeGraphicsMemory();
        delete component;
    }
    LayerComponents.clear();

    // Delete sound chunks and reset pointers
    if (loadSoundChunk_)
    {
        delete loadSoundChunk_;
        loadSoundChunk_ = nullptr;
    }

    if (unloadSoundChunk_)
    {
        delete unloadSoundChunk_;
        unloadSoundChunk_ = nullptr;
    }


    if (highlightSoundChunk_)
    {
        delete highlightSoundChunk_;
        highlightSoundChunk_ = nullptr;
    }

    if (selectSoundChunk_)
    {
        delete selectSoundChunk_;
        selectSoundChunk_ = nullptr;
    }

    // Deinitialize and clear collections_
    for (auto& collectionEntry : collections_)
    {
        delete collectionEntry.collection;
    }
    collections_.clear();
}

bool Page::isMenusFull() const
{
  return (menuDepth_ > menus_.size());
}


void Page::setLoadSound(Sound *chunk)
{
  loadSoundChunk_ = chunk;
}


void Page::setUnloadSound(Sound *chunk)
{
  unloadSoundChunk_ = chunk;
}


void Page::setHighlightSound(Sound *chunk)
{
  highlightSoundChunk_ = chunk;
}


void Page::setSelectSound(Sound *chunk)
{
  selectSoundChunk_ = chunk;
}

ScrollingList* Page::getAnActiveMenu()
{
    if (!anActiveMenu_) {
        size_t size = activeMenu_.size();
        if (size) {
            for (unsigned int i = 0; i < size; i++) {
                if (!activeMenu_[i]->isPlaylist()) {
                    anActiveMenu_ = activeMenu_[i];
                    break;
                }
            }
        }
    }
    
    return anActiveMenu_;
}

void Page::setActiveMenuItemsFromPlaylist(MenuInfo_S info, ScrollingList* menu)
{
    // keep playlist menu
    if (menu->isPlaylist() && info.collection->playlistItems.size()) {
        menu->setItems(&info.collection->playlistItems);
    }
    else {
        menu->setItems(playlist_->second);
    }
}


void Page::onNewItemSelected() {
    if (!getAnActiveMenu()) return;

    // Iterate through each menu list and then each menu
    std::for_each(menus_.begin(), menus_.end(),
        [](auto& menuList) {
            std::for_each(menuList.begin(), menuList.end(),
            [](ScrollingList* menu) {
                    if (menu) menu->setNewItemSelected();
                });
        });

    // Iterate through each layer component
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [](Component* component) {
            component->setNewItemSelected();
        });
}


void Page::returnToRememberSelectedItem()
{
    if (!getAnActiveMenu()) return;

    if (std::string name = getPlaylistName(); name != "" && lastPlaylistOffsets_[name]) {
        setScrollOffsetIndex(lastPlaylistOffsets_[name]);
    }
    onNewItemSelected();
}

void Page::rememberSelectedItem()
{
    ScrollingList const* amenu = getAnActiveMenu();
    if (!amenu || !amenu->getItems().size()) return;

    std::string name = getPlaylistName();
    if (name != "" && selectedItem_) {
        lastPlaylistOffsets_[name] = amenu->getScrollOffsetIndex();
    }
}

std::map<std::string, size_t> Page::getLastPlaylistOffsets() const
{
    return lastPlaylistOffsets_;
}

void Page::onNewScrollItemSelected() {
    if (!getAnActiveMenu()) return;

    // Use std::for_each to iterate over LayerComponents
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [](Component* component) {
            if (component) { // Ensure the component is not null
                component->setNewScrollItemSelected();
            }
        });
}


void Page::highlightLoadArt() {
    if (!getAnActiveMenu()) return;

    // Loading new items art
    setSelectedItem();

    // Use std::for_each to iterate over LayerComponents
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [](Component* component) {
            if (component) { // Check to ensure the component is not null
                component->setNewItemSelected();
            }
        });
}


void Page::pushMenu(ScrollingList *s, int index)
{
    // If index < 0 then append to the menus_ vector
    if(index < 0)
    {
        index = static_cast<int>(menus_.size());
    }

    // Increase menus_ as needed
    while(index >= static_cast<int>(menus_.size()))
    {
        std::vector<ScrollingList *> menus;
        menus_.push_back(menus);
    }

    menus_[index].push_back(s);
}


unsigned int Page::getMenuDepth() const
{
    return menuDepth_;
}


void Page::setStatusTextComponent(Text *t)
{
    textStatusComponent_ = t;
}


bool Page::addComponent(Component *c)
{
    bool retVal = false;

    if(c->baseViewInfo.Layer < NUM_LAYERS)
    {
        LayerComponents.push_back(c);
        retVal = true;
    }
    else
    {
        std::stringstream ss;
        ss << "Component layer too large Layer: " << c->baseViewInfo.Layer;
        LOG_ERROR("Page", ss.str());
    }

    return retVal;
}


bool Page::isMenuIdle() {
    // Check if any menu list is not idle
    return !std::any_of(menus_.begin(), menus_.end(),
        [](const auto& menuList) {
            // Check if any menu within each menu list is not idle
            return std::any_of(menuList.begin(), menuList.end(),
            [](const ScrollingList* menu) {
                    return !menu->isScrollingListIdle();
                });
        });
}



bool Page::isIdle() {
    // Check if the menu is not idle, indicating the page is not idle.
    if (!isMenuIdle()) {
        return false;
    }

    // Check if any component is not idle
    return !std::any_of(LayerComponents.begin(), LayerComponents.end(),
        [](const Component* component) {
            return component && !component->isIdle();
        });
}



bool Page::isAttractIdle() {
    // Check if any menus are not attract idle
    if (std::any_of(menus_.begin(), menus_.end(),
        [](const auto& menuList) {
            return std::any_of(menuList.begin(), menuList.end(),
            [](const ScrollingList* menu) {
                    return !menu->isAttractIdle();
                });
        })) {
        return false;
    }

    // Check if any layer components are not attract idle
    return !std::any_of(LayerComponents.begin(), LayerComponents.end(),
        [](const Component* component) {
            return !component->isAttractIdle();
        });
}



bool Page::isGraphicsIdle() {
    // Check if any graphics component is not idle
    return !std::any_of(LayerComponents.begin(), LayerComponents.end(),
        [](const Component* component) {
            return !component->isIdle();
        });
}


void Page::start() {
    // Trigger events for menus
    std::for_each(menus_.begin(), menus_.end(),
        [](auto& menuList) {
            std::for_each(menuList.begin(), menuList.end(),
            [](ScrollingList* menu) {
                    if (menu) { // Ensure menu is not null
                        menu->triggerEvent("enter");
                        menu->triggerEnterEvent();
                    }
                });
        });

    // Play load sound if available
    if (loadSoundChunk_) {
        loadSoundChunk_->play();
    }

    // Trigger enter event for layer components
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [](Component* component) {
            if (component) { // Check to ensure the component is not null
                component->triggerEvent("enter");
            }
        });
}


void Page::stop() {
    // Trigger exit events for menus
    std::for_each(menus_.begin(), menus_.end(),
        [](auto& menuList) {
            std::for_each(menuList.begin(), menuList.end(),
            [](ScrollingList* menu) {
                    if (menu) { // Ensure menu is not null
                        menu->triggerEvent("exit");
                        menu->triggerExitEvent();
                    }
                });
        });

    // Play unload sound if available
    if (unloadSoundChunk_) {
        unloadSoundChunk_->play();
    }

    // Trigger exit event for layer components
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [](Component* component) {
            if (component) { // Check to ensure the component is not null
                component->triggerEvent("exit");
            }
        });
}


void Page::setSelectedItem()
{
    selectedItem_ = getSelectedMenuItem();
}

Item *Page::getSelectedItem()
{
    if (!selectedItem_) {
       setSelectedItem();
    }

    return selectedItem_;
}

Item *Page::getSelectedItem(int offset)
{
    ScrollingList* amenu = getAnActiveMenu();
    if (!amenu) return nullptr;

    return amenu->getItemByOffset(offset);
}


Item* Page::getSelectedMenuItem()
{
    ScrollingList* amenu = getAnActiveMenu();
    if (!amenu) return nullptr;

    return amenu->getSelectedItem();
}


void Page::removeSelectedItem()
{
    /*
    todo: change method to RemoveItem() and pass in SelectedItem
    if(Menu)
    {
        Menu->RemoveSelectedItem();
    }
    */
    selectedItem_ = nullptr;

}


void Page::setScrollOffsetIndex(size_t i) {
    if (!getAnActiveMenu()) return;

    std::for_each(activeMenu_.begin(), activeMenu_.end(),
        [i](ScrollingList* menu) {
            if (menu && !menu->isPlaylist()) {
                menu->setScrollOffsetIndex(i);
            }
        });
}


size_t Page::getScrollOffsetIndex()
{
    ScrollingList const* amenu = getAnActiveMenu();
    if (!amenu) return -1;

    return amenu->getScrollOffsetIndex();
}


void Page::setMinShowTime(float value)
{
    minShowTime_ = value;
}


float Page::getMinShowTime() const
{
    return minShowTime_;
}

std::string Page::controlsType() const
{
    return controlsType_;
}

void Page::setControlsType(const std::string_view& type)
{
    controlsType_ = type;
}

void Page::playlistChange() {
    std::string playlistName = getPlaylistName(); // Call once and use the result

    // Set playlist for active menus
    std::for_each(activeMenu_.begin(), activeMenu_.end(),
        [&playlistName](ScrollingList* menu) {
            if (menu) {
                menu->setPlaylist(playlistName);
            }
        });

    // Set playlist for layer components
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [&playlistName](Component* component) {
            if (component) { // Check to ensure the component is not null
                component->setPlaylist(playlistName);
            }
        });

    updatePlaylistMenuPosition();
}


void Page::menuScroll() {
    if (!selectedItem_) return; // Early return if selectedItem_ is null

    // Trigger "menuScroll" event for each layer component
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [this](Component* component) { // Capture 'this' to access menuDepth_
            if (component) { // Ensure the component is not null
                component->triggerEvent("menuScroll", menuDepth_ - 1);
            }
        });
}

void Page::highlightEnter()
{
    triggerEventOnAllMenus("highlightEnter");
}

void Page::highlightExit()
{
    triggerEventOnAllMenus("highlightExit");
}

void Page::playlistEnter()
{
    // entered in new playlist set selected item
    setSelectedItem();
    triggerEventOnAllMenus("playlistEnter");
}

void Page::playlistExit()
{
    triggerEventOnAllMenus("playlistExit");
}

void Page::playlistNextEnter()
{
    fromPlaylistNav = true;
    fromPreviousPlaylist = false;
    triggerEventOnAllMenus("playlistNextEnter");
}

void Page::playlistNextExit()
{
    fromPreviousPlaylist = false;
    triggerEventOnAllMenus("playlistNextExit");
    fromPlaylistNav = false;
}

void Page::playlistPrevEnter()
{
    fromPlaylistNav = true;
    fromPreviousPlaylist = true;
    triggerEventOnAllMenus("playlistPrevEnter");
}

void Page::playlistPrevExit()
{
    fromPreviousPlaylist = true;
    triggerEventOnAllMenus("playlistPrevExit");
    fromPlaylistNav = false;
}

void Page::menuJumpEnter()
{
    // jumped into new item
    setSelectedItem();
    triggerEventOnAllMenus("menuJumpEnter");
}

void Page::menuJumpExit()
{
    triggerEventOnAllMenus("menuJumpExit");
}


void Page::attractEnter()
{
    triggerEventOnAllMenus("attractEnter");
}

void Page::attract()
{
    triggerEventOnAllMenus("attract");
}

void Page::attractExit()
{
    triggerEventOnAllMenus("attractExit");
}

void Page::gameInfoEnter()
{
    triggerEventOnAllMenus("gameInfoEnter");
}
void Page::gameInfoExit()
{
    triggerEventOnAllMenus("gameInfoExit");
}

void Page::collectionInfoEnter()
{
    triggerEventOnAllMenus("collectionInfoEnter");
}
void Page::collectionInfoExit()
{
    triggerEventOnAllMenus("collectionInfoExit");
}

void Page::buildInfoEnter()
{
    triggerEventOnAllMenus("buildInfoEnter");
}
void Page::buildInfoExit()
{
    triggerEventOnAllMenus("buildInfoExit");
}

void Page::jukeboxJump()
{
    triggerEventOnAllMenus("jukeboxJump");
}

void Page::triggerEventOnAllMenus(const std::string& event)
{
    if (!selectedItem_)
        return;

    unsigned int depth = menuDepth_ - 1;
    for (size_t i = 0; i < menus_.size(); ++i)
    {
        for (ScrollingList* menu : menus_[i])
        {
            unsigned int index = (depth == i) ? MENU_INDEX_HIGH + depth : depth;

            menu->triggerEvent(event, index);
            menu->triggerEventOnAll(event, index);
        }
    }

    unsigned int index = depth;
    for (Component* component : LayerComponents)
    {
        if (component)
            component->triggerEvent(event, index);
    }
}



void Page::triggerEvent(const std::string& action) {
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [&action](Component* component) {
            if (component) { // Ensure the component is not null
                component->triggerEvent(action);
            }
        });
}


void Page::setText(const std::string& text, int id) {
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [&text, id](Component* component) {
            component->setText(text, id);
        });
}


void Page::setScrolling(ScrollDirection direction)
{
    switch(direction)
    {
    case ScrollDirectionForward:
    case ScrollDirectionBack:
        if(!scrollActive_)
        {
            menuScroll();
        }
        scrollActive_ = true;
        break;
    case ScrollDirectionIdle:
    default:
        scrollActive_ = false;
        break;
    }

}


bool Page::isHorizontalScroll()
{
    ScrollingList const* amenu = getAnActiveMenu();
    if(!amenu) return false;

    return amenu->horizontalScroll;
}


void Page::pageScroll(ScrollDirection direction) {
    ScrollingList* amenu = getAnActiveMenu();
    if (!amenu) return; // Early return if there's no active menu

    // Determine scroll direction and perform the action
    if (direction == ScrollDirectionForward) {
        amenu->pageDown();
    }
    else if (direction == ScrollDirectionBack) {
        amenu->pageUp();
    }

    // Get the updated scroll offset index from the active menu
    size_t index = amenu->getScrollOffsetIndex();

    // Set the scroll offset index for all menus
    std::for_each(activeMenu_.begin(), activeMenu_.end(),
        [index](ScrollingList* menu) {
            if (menu) { // Ensure the menu is not null
                menu->setScrollOffsetIndex(index);
            }
        });
}

void Page::selectRandom() {
    ScrollingList* amenu = getAnActiveMenu();
    if (!amenu) return;

    amenu->random();
    size_t index = amenu->getScrollOffsetIndex();

    for (ScrollingList* menu : activeMenu_) {
        if (menu && !menu->isPlaylist()) {
            menu->setScrollOffsetIndex(index);
        }
    }
}


void Page::selectRandomPlaylist(CollectionInfo* collection, std::vector<std::string> cycleVector)
{
    size_t size = collection->playlists.size();
    if (size == 0) return;

    int index = rand() % size;
    int i = 0;
    std::string playlistName;
    std::string settingsPlaylist = "settings";
    config_.setProperty("settingsPlaylist", settingsPlaylist);

    for (auto it = collection->playlists.begin(); it != collection->playlists.end(); it++)
    {
        if (i == index && 
            it->first != settingsPlaylist && 
            it->first != "favorites" &&
            it->first != "lastplayed" &&
            std::find(cycleVector.begin(), cycleVector.end(), it->first) != cycleVector.end()
        ) {
            playlistName = it->first;
            break;
        }
        i++;
    }
    if (playlistName != "")
        selectPlaylist(playlistName);
}

void Page::letterScroll(ScrollDirection direction) {
    std::for_each(activeMenu_.begin(), activeMenu_.end(), [direction](ScrollingList* menu) {
        if (menu && !menu->isPlaylist()) {
            switch (direction) {
            case ScrollDirectionForward:
                menu->letterDown();
                break;
            case ScrollDirectionBack:
                menu->letterUp();
                break;
            default:
                // Handle unexpected direction if necessary
                break;
            }
        }
        });
}

// if playlist is same name as metadata to sort upon, then jump by unique sorted metadata
void Page::metaScroll(ScrollDirection direction, std::string attribute) {
    // Convert attribute to lower case
    std::transform(attribute.begin(), attribute.end(), attribute.begin(),
        [](unsigned char c) { return std::tolower(c); });

    // Use std::for_each to iterate over activeMenu_ and apply the scroll operation
    std::for_each(activeMenu_.begin(), activeMenu_.end(), [&](ScrollingList* menu) {
        if (menu && !menu->isPlaylist()) {
            switch (direction) {
            case ScrollDirectionForward:
                menu->metaDown(attribute);
                break;
            case ScrollDirectionBack:
                menu->metaUp(attribute);
                break;
            default:
                // Optionally handle unexpected direction
                break;
            }
        }
        });
}


void Page::cfwLetterSubScroll(ScrollDirection direction) {
    std::for_each(activeMenu_.begin(), activeMenu_.end(), [direction](ScrollingList* menu) {
        if (menu && !menu->isPlaylist()) {
            switch (direction) {
            case ScrollDirectionForward:
                menu->cfwLetterSubDown();
                break;
            case ScrollDirectionBack:
                menu->cfwLetterSubUp();
                break;
            default:
                // Optionally handle unexpected direction
                break;
            }
        }
        });
}

size_t Page::getCollectionSize()
{
    ScrollingList const* amenu = getAnActiveMenu();
    if (!amenu) return 0;

    return amenu->getSize();
}


size_t Page::getSelectedIndex()
{
    ScrollingList const* amenu = getAnActiveMenu();
    if (!amenu) return 0;

    return amenu->getSelectedIndex();
}


bool Page::pushCollection(CollectionInfo* collection) {
    if (!collection) return false;

    // Grow the menu as needed
    if (menus_.size() <= menuDepth_ && getAnActiveMenu()) {
        for (const auto* menu : activeMenu_) {
            auto* newMenu = new ScrollingList(*menu);
            if (newMenu->isPlaylist()) {
                playlistMenu_ = newMenu;
            }
            pushMenu(newMenu, menuDepth_);
        }
    }

    if (!menus_.empty()) {
        activeMenu_ = menus_[menuDepth_];
        anActiveMenu_ = nullptr;
        selectedItem_ = nullptr;
        for (auto* menu : activeMenu_) {
            if (!menu) continue;
            menu->collectionName = collection->name;
            if (menu->isPlaylist() && !collection->playlistItems.empty()) {
                menu->setItems(&collection->playlistItems);
            }
            else {
                // add item collection menu
                menu->setItems(&collection->items);
            }
        }
    }
    else {
        LOG_WARNING("RetroFE", "layout.xml doesn't have any menus");
    }

    // Build the collection info instance
    MenuInfo_S info;
    info.collection = collection;
    info.playlist = collection->playlists.begin();
    info.queueDelete = false;
    collections_.push_back(info);

    playlist_ = info.playlist;
    playlistChange();
    if (menuDepth_ < menus_.size()) {
        menuDepth_++;
    }

    for (auto* component : LayerComponents) {
        if (component) component->collectionName = collection->name;
    }

    return true;
}


bool Page::popCollection() {
    if (!getAnActiveMenu()) return false;
    if (menuDepth_ <= 1) return false;
    if (collections_.size() <= 1) return false;

    // Queue the collection for deletion
    MenuInfo_S* info = &collections_.back();
    info->queueDelete = true;
    deleteCollections_.push_back(*info);

    // Get the next collection off of the stack
    collections_.pop_back();
    info = &collections_.back();

    // Build playlist menu
    if (playlistMenu_ && info->collection->playlistItems.size()) {
        playlistMenu_->setItems(&info->collection->playlistItems);
    }

    playlist_ = info->playlist;
    playlistChange();

    menuDepth_--;
    activeMenu_ = menus_[menuDepth_ - 1];
    anActiveMenu_ = nullptr;
    selectedItem_ = nullptr;

    // Use std::for_each to set collectionName for each component
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [info](Component* component) {
            component->collectionName = info->collection->name;
        });

    return true;
}


void Page::enterMenu()
{
    triggerEventOnAllMenus("menuEnter");
}


void Page::exitMenu()
{
    triggerEventOnAllMenus("menuExit");
}


void Page::enterGame()
{
    triggerEventOnAllMenus("gameEnter");
}


void Page::exitGame()
{
    triggerEventOnAllMenus("gameExit");
}


std::string Page::getPlaylistName() const
{
   return !collections_.empty() ? playlist_->first : "";
}


void Page::favPlaylist()
{
    if(getPlaylistName() == "favorites")
    {
        selectPlaylist("all");
    }
    else
    {
        selectPlaylist("favorites");
    }
    return;
}

void Page::nextPlaylist() {
    MenuInfo_S& info = collections_.back();
    rememberSelectedItem(); // Save last playlist selected item

    auto& playlists = info.collection->playlists;
    auto next = std::next(playlist_); // Move to next or wrap to begin
    next = (next == playlists.end()) ? playlists.begin() : next;

    // Loop until a non-empty playlist is found or we've checked all playlists
    for (size_t i = 0; i < playlists.size(); ++i, next = std::next(next == playlists.end() ? playlists.begin() : next)) {
        if (!next->second->empty()) {
            playlist_ = next; // Update current playlist
            break;
        }
    }

    playlistNextEnter(); // Perform actions for entering the next playlist

    // Update active menu items
    for (ScrollingList* menu : activeMenu_) {
        setActiveMenuItemsFromPlaylist(info, menu);
    }
    playlistChange(); // Notify about the playlist change
}


void Page::prevPlaylist() {
    MenuInfo_S& info = collections_.back();
    rememberSelectedItem(); // Save last playlist selected item

    auto& playlists = info.collection->playlists;
    auto prev = (playlist_ == playlists.begin()) ? playlists.end() : playlist_; // Wrap to end if at begin
    prev = prev == playlists.begin() ? playlists.end() : std::prev(prev); // Move to prev or wrap to end

    // Loop until a non-empty playlist is found or we've checked all playlists
    for (size_t i = 0; i < playlists.size(); ++i, prev = (prev == playlists.begin() ? playlists.end() : std::prev(prev))) {
        if (!prev->second->empty()) {
            playlist_ = prev; // Update current playlist
            break;
        }
    }

    // Update active menu items
    for (ScrollingList* menu : activeMenu_) {
        setActiveMenuItemsFromPlaylist(info, menu);
    }
    playlistChange(); // Notify about the playlist change
}



void Page::selectPlaylist(const std::string_view& playlist)
{
    MenuInfo_S &info = collections_.back();
    //info.collection->saveFavorites();
    size_t numlists = info.collection->playlists.size();
    // save last playlist selected item
    rememberSelectedItem();

    // Store current playlist
    CollectionInfo::Playlists_T::iterator playlist_store = playlist_;

    for(size_t i = 0; i <= numlists; ++i)
    {
        playlist_++;
        // wrap
        if(playlist_ == info.collection->playlists.end()) 
            playlist_ = info.collection->playlists.begin();

        // find the first playlist
        if(!playlist_->second->empty() && getPlaylistName() == playlist) 
            break;
    }

    // Do not change playlist if it does not exist or if it's empty
    if ( playlist_->second->empty() || getPlaylistName() != playlist)
      playlist_ = playlist_store;

    for(auto it = activeMenu_.begin(); it != activeMenu_.end(); it++)
    {
        setActiveMenuItemsFromPlaylist(info, *it);
    }
    playlistChange();
}

void Page::updatePlaylistMenuPosition()
{
    if (playlistMenu_) {
        std::string name = getPlaylistName();
        if (name != "") {
            playlistMenu_->selectItemByName(name);
        }
    }
}

void Page::nextCyclePlaylist(std::vector<std::string> list) {
    if (list.empty()) return;

    std::string settingsPlaylist;
    config_.getProperty("settingsPlaylist", settingsPlaylist);
    std::string currentPlaylist = getPlaylistName();

    playlistNextEnter();

    auto cycle = [&](const std::string& playlist) {
        return playlist != settingsPlaylist && playlistExists(playlist);
        };

    auto it = std::find(list.begin(), list.end(), currentPlaylist);
    if (it == list.end()) {
        // If current playlist not found, switch to the first valid playlist
        auto foundIt = std::find_if(list.begin(), list.end(), cycle);
        if (foundIt != list.end()) { // Check if a valid playlist was found
            selectPlaylist(*foundIt);
        }
    }
    else {
        // Start searching from the next item. Wrap around if at the end.
        auto startIt = it;
        do {
            if (++it == list.end()) it = list.begin(); // wrap
            if (it == startIt) break; // Prevent infinite loop if no valid playlist is found
        } while (!cycle(*it) || it == startIt); // Added check to prevent re-selection of the start item if it's the only valid playlist

        // Make sure to select the playlist if a valid one is found
        if (cycle(*it) && it != startIt) {
            selectPlaylist(*it);
        }
    }
}


void Page::prevCyclePlaylist(std::vector<std::string> list) {
    if (list.empty()) return;

    std::string settingsPlaylist;
    config_.getProperty("settingsPlaylist", settingsPlaylist);
    std::string currentPlaylist = getPlaylistName();

    // Define a lambda to check for a valid playlist
    auto isValidPlaylist = [&](const std::string& playlist) {
        return playlist != settingsPlaylist && playlistExists(playlist);
        };

    // Attempt to find the current playlist in the list
    auto currentIt = std::find(list.begin(), list.end(), currentPlaylist);

    if (currentIt == list.end()) {
        // If current playlist not found, switch to the first valid playlist from the end
        auto foundIt = std::find_if(list.rbegin(), list.rend(), isValidPlaylist).base();
        if (foundIt != list.begin()) { // Adjust because base() moves past the found element
            selectPlaylist(*--foundIt);
        }
    }
    else {
        // Current playlist found; switch to the previous found playlist in the list
        auto startIt = currentIt; // Remember the start position to detect full cycles
        do {
            // Wrap to the end if we're at the beginning
            if (currentIt == list.begin()) currentIt = list.end();
            --currentIt; // Move to the previous item

            // Check if the playlist is valid or if we've cycled through all options
            if (isValidPlaylist(*currentIt) || currentIt == startIt) {
                selectPlaylist(*currentIt);
                break;
            }

            // If we've wrapped around without finding a valid playlist, stop the loop
            if (currentIt == list.begin()) {
                break;
            }
        } while (true);
    }
}


bool Page::playlistExists(const std::string& playlist)
{
    MenuInfo_S& info = collections_.back();
    CollectionInfo::Playlists_T p = info.collection->playlists;

    // playlist exists in cycle and contains items
    return p.end() != p.find(playlist) && !info.collection->playlists[playlist]->empty();
}


void Page::update(float dt) {
    std::string playlistName = getPlaylistName();

    if (useThreading_) {
        // Asynchronous (threaded) version for non-OpenGL backends

        // Future for asynchronous update of ScrollingLists within menus_
        auto menuUpdateFuture = pool_.enqueue([this, dt, playlistName]() {
            for (auto const& menuList : menus_) {
                for (auto* menu : menuList) {
                    menu->playlistName = playlistName;
                    menu->update(dt);
                }
            }
            });

        // Future for asynchronous update of LayerComponents
        auto layerUpdateFuture = pool_.enqueue([this, dt, playlistName]() {
            for (auto it = LayerComponents.begin(); it != LayerComponents.end();) {
                if (*it) {
                    (*it)->playlistName = playlistName;
                    if ((*it)->update(dt) && (*it)->getAnimationDoneRemove()) {
                        (*it)->freeGraphicsMemory();
                        delete* it;
                        it = LayerComponents.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
                else {
                    ++it;
                }
            }
            });

        // Wait for asynchronous operations to complete
        menuUpdateFuture.get();
        layerUpdateFuture.get();
    }
    else {
        // Synchronous (non-threaded) version for OpenGL backend

        for (auto const& menuList : menus_) {
            for (auto* menu : menuList) {
                menu->playlistName = playlistName;
                menu->update(dt);
            }
        }

        for (auto it = LayerComponents.begin(); it != LayerComponents.end();) {
            if (*it) {
                (*it)->playlistName = playlistName;
                if ((*it)->update(dt) && (*it)->getAnimationDoneRemove()) {
                    (*it)->freeGraphicsMemory();
                    delete* it;
                    it = LayerComponents.erase(it);
                }
                else {
                    ++it;
                }
            }
            else {
                ++it;
            }
        }
    }

    // Common update code for textStatusComponent_
    if (textStatusComponent_) {
        std::string status; // Populate 'status' as needed
        config_.setProperty("status", status);
        textStatusComponent_->setText(status);
    }
}


void Page::updateReloadables(float dt) {
    std::for_each(LayerComponents.begin(), LayerComponents.end(),
        [dt](Component* component) {
            if (component) {
                component->update(dt);
            }
        });
}

void Page::cleanup()
{
    auto del = deleteCollections_.begin();

    while(del != deleteCollections_.end())
    {
        MenuInfo_S &info = *del;
        if(info.queueDelete)
        {
            std::list<MenuInfo_S>::iterator next = del;
            ++next;

            if(info.collection)
            {
                delete info.collection;
            }
            deleteCollections_.erase(del);
            del = next;
        }
        else
        {
            ++del;
        }
    }
}


void Page::draw()
{
    for (unsigned int i = 0; i < NUM_LAYERS; ++i)
    {
        // Drawing Components based on their layer with range-based for-loop
        for (auto* component : LayerComponents)
        {
            if (component && component->baseViewInfo.Layer == i)
            {
                component->draw();
            }
        }

        // Drawing Menus with range-based for-loop
        for (const auto& menuList : menus_)
        {
            for (ScrollingList* menu : menuList)
            {
                if (menu) // Checking for nullptr just in case
                {
                    menu->draw(i);
                }
            }
        }
    }
}


void Page::removePlaylist()
{
    if (!selectedItem_)
        return;

    MenuInfo_S &info = collections_.back();
    CollectionInfo *collection = info.collection;

    std::vector<Item *> *items = collection->playlists["favorites"];

    if (auto it = std::find(items->begin(), items->end(), selectedItem_); it != items->end())
    {
        size_t index = 0;  // Initialize with 0 instead of NULL
        ScrollingList const* amenu = nullptr;  // Use nullptr for pointer types
        // get the deleted item's position
        if (getPlaylistName() == "favorites")
        {
            amenu = getAnActiveMenu();
            if (amenu)
            {
                index = amenu->getScrollOffsetIndex();
            }
        }
        items->erase(it);
        selectedItem_->isFavorite = false;
        collection->sortPlaylists();
        collection->saveRequest = true;

        // set to position to the old deleted position
        if (amenu)
        {
            setScrollOffsetIndex(index);
        }
    }
    bool globalFavLast = false;
    (void)config_.getProperty("globalFavLast", globalFavLast);
    if (globalFavLast && collection->name != "Favorites") {
        collection->saveRequest = true;
        collection->saveFavorites(selectedItem_);

        return;
    }

    collection->saveFavorites();
    onNewItemSelected();
}



void Page::addPlaylist()
{
    if(!selectedItem_) return;

    MenuInfo_S &info = collections_.back();
    CollectionInfo *collection = info.collection;

    if(std::vector<Item *> *items = collection->playlists["favorites"]; getPlaylistName() != "favorites" && std::find(items->begin(), items->end(), selectedItem_) == items->end())
    {
        items->push_back(selectedItem_);
        selectedItem_->isFavorite = true;
        collection->sortPlaylists();
        collection->saveRequest = true;
    }
    collection->saveFavorites();
}


void Page::togglePlaylist()
{
    if (!selectedItem_) return;

    if (getPlaylistName() != "favorites")
    {
        if (selectedItem_->isFavorite)
            removePlaylist();
        else
            addPlaylist();
    }
}

std::string Page::getCollectionName()
{
    if(collections_.size() == 0) return "";

    MenuInfo_S const &info = collections_.back();
    return info.collection->name;

}


CollectionInfo *Page::getCollection()
{
    return collections_.back().collection;
}


void Page::freeGraphicsMemory()
{
    for (auto const& menuVector : menus_)
    {
        for (ScrollingList* menu : menuVector)
        {
            menu->freeGraphicsMemory();
        }
    }

    if (loadSoundChunk_) loadSoundChunk_->free();
    if (unloadSoundChunk_) unloadSoundChunk_->free();
    if (highlightSoundChunk_) highlightSoundChunk_->free();
    if (selectSoundChunk_) selectSoundChunk_->free();

    for (Component* component : LayerComponents)
    {
        component->freeGraphicsMemory();
    }
}


void Page::allocateGraphicsMemory()
{
    LOG_DEBUG("Page", "Allocating graphics memory");

    int currentDepth = 0;
    for (auto const& menuList : menus_)
    {
        if (currentDepth < static_cast<int>(menuDepth_))
        {
            for (auto& menu : menuList)
            {
                if (menu) 
                {
                    menu->allocateGraphicsMemory();
                }
            }
        }
        ++currentDepth;
    }

    if (loadSoundChunk_) loadSoundChunk_->allocate();
    if (unloadSoundChunk_) unloadSoundChunk_->allocate();
    if (highlightSoundChunk_) highlightSoundChunk_->allocate();
    if (selectSoundChunk_) selectSoundChunk_->allocate();

    for (auto& component : LayerComponents)
    {
        if (component) 
        {
            component->allocateGraphicsMemory();
        }
    }
    LOG_DEBUG("Page", "Allocate graphics memory complete");
}


void Page::deInitializeFonts() const
{
    for (auto& menuVector : menus_)
    {
        for (ScrollingList* menu : menuVector)
        {
            menu->deInitializeFonts();
        }
    }

    for (Component* component : LayerComponents)
    {
        component->deInitializeFonts();
    }
}

void Page::initializeFonts() const
{
    for (auto& menuVector : menus_)
    {
        for (ScrollingList* menu : menuVector)
        {
            menu->initializeFonts();
        }
    }

    for (Component* component : LayerComponents)
    {
        component->initializeFonts();
    }
}


void Page::playSelect()
{
    if(selectSoundChunk_)
    {
        selectSoundChunk_->play();
    }
}


bool Page::isSelectPlaying()
{
    if ( selectSoundChunk_ )
    {
        return selectSoundChunk_->isPlaying();
    }
    return false;
}


void Page::reallocateMenuSpritePoints(bool updatePlaylistMenu) const
{
    for(ScrollingList *menu : activeMenu_)
    {
        if(menu && (!menu->isPlaylist() || updatePlaylistMenu))
        {
            menu->deallocateSpritePoints();
            menu->allocateSpritePoints();
        }
    }
}


bool Page::isMenuScrolling() const
{
    return scrollActive_;
}


bool Page::isPlaying() const
{
    for(auto& component : LayerComponents)
    {
        if (component->baseViewInfo.Monitor == 0 && component->isPlaying())
        {
            return true;
        }
    }
    return false;
}


void Page::resetScrollPeriod() const
{
    for(auto& menu : activeMenu_)
    {
        if(menu)
        {
            menu->resetScrollPeriod();
        }
    }
}


void Page::updateScrollPeriod() const
{
    for(auto& menu : activeMenu_)
    {
        if(menu)
        {
            menu->updateScrollPeriod();
        }
    }
}


void Page::scroll(bool forward) {
    if (useThreading_) {
        // Asynchronous version
        auto scrollFuture = pool_.enqueue([this, forward]() {
            for (auto& menu : activeMenu_) {
                if (menu && !menu->isPlaylist()) {
                    menu->scroll(forward);
                }
            }
            onNewScrollItemSelected();
            });


        // Wait for the scroll operation to complete
        scrollFuture.get();
        if (highlightSoundChunk_) {
            highlightSoundChunk_->play();
        }
    
    }
    else {
        // Synchronous version
        for (auto& menu : activeMenu_) {
            if (menu && !menu->isPlaylist()) {
                menu->scroll(forward);
            }
        }
        onNewScrollItemSelected();
        if (highlightSoundChunk_) {
            highlightSoundChunk_->play();
        }
    }
}



bool Page::hasSubs()
{
    return collections_.back().collection->hasSubs;
}

void Page::setCurrentLayout(int layout)
{
    currentLayout_ = layout;
}

int Page::getCurrentLayout() const
{
    return currentLayout_;
}


int Page::getLayoutWidthByMonitor(int monitor)
{
    if (monitor < SDL::getScreenCount())
        return layoutWidthByMonitor_[monitor];
    else
        return 0;
}


int Page::getLayoutHeightByMonitor(int monitor)
{
    if (monitor < SDL::getScreenCount())
        return layoutHeightByMonitor_[monitor];
    else
        return 0;
}


void Page::setLayoutWidthByMonitor(int monitor, int width)
{
    if (monitor < SDL::getScreenCount())
        layoutWidthByMonitor_[monitor] = width;
}


void Page::setLayoutHeightByMonitor(int monitor, int height)
{
    if (monitor < SDL::getScreenCount())
        layoutHeightByMonitor_[monitor] = height;
}

int Page::getLayoutWidth(int layout)
{
    currentLayout_ = layout;
    return layoutWidth_[layout];
}


int Page::getLayoutHeight(int layout)
{
    currentLayout_ = layout;
    return layoutHeight_[layout];
}


void Page::setLayoutWidth(int layout, int width)
{
    currentLayout_ = layout;
    layoutWidth_[layout] = width;
}


void Page::setLayoutHeight(int layout, int height)
{
    currentLayout_ = layout;
    layoutHeight_[layout] = height;
}

void Page::setJukebox()
{
    jukebox_ = true;
    return;
}


bool Page::isJukebox() const
{
    return jukebox_;
}


bool Page::isJukeboxPlaying()
{

    bool retVal = false;

    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        retVal |= (*it)->isJukeboxPlaying();
    }

    return retVal;

}


void Page::skipForward( )
{
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        (*it)->skipForward( );
    }
}


void Page::skipBackward( )
{
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        (*it)->skipBackward( );
    }
}


void Page::skipForwardp( )
{
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        (*it)->skipForwardp( );
    }
}


void Page::skipBackwardp( )
{
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        (*it)->skipBackwardp( );
    }
}


void Page::pause( )
{
    for (auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        (*it)->pause();
    }
}


void Page::restart( )
{
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        (*it)->restart( );
    }
}


unsigned long long Page::getCurrent( )
{
    unsigned long long ret = 0;
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        ret += (*it)->getCurrent( );
    }
    return ret;
}


unsigned long long Page::getDuration( )
{
    unsigned long long ret = 0;
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        ret += (*it)->getDuration( );
    }
    return ret;
}


bool Page::isPaused( )
{
    bool ret = false;
    for(auto it = LayerComponents.begin(); it != LayerComponents.end(); ++it)
    {
        ret |= (*it)->isPaused( );
    }
    return ret;
}

void Page::setLocked(bool locked)
{
    locked_ = locked;
}

bool Page::isLocked() const
{
    return locked_;
}

ScrollingList* Page::getPlaylistMenu()
{
    return playlistMenu_;
}

void Page::setPlaylistMenu(ScrollingList* menu)
{
    playlistMenu_ = menu;
}

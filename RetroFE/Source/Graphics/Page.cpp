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
#include "ComponentItemBinding.h"
#include "Component/Component.h"
#include "../Collection/CollectionInfo.h"
#include "Component/Text.h"
#include "../Utility/Log.h"
#include "Component/ScrollingList.h"
#include "../Sound/Sound.h"
#include "ComponentItemBindingBuilder.h"
#include "PageBuilder.h"
#include "../Utility/Utils.h"
#include "../Database/GlobalOpts.h"
#include <algorithm>
#include <sstream>


Page::Page(Configuration& config, int layoutWidth, int layoutHeight)
	: fromPreviousPlaylist(false)
	, fromPlaylistNav(false)
	, config_(config)
	, controlsType_("")
	, locked_(false)
	, anActiveMenu_(NULL)
	, playlistMenu_(NULL)
	, menuDepth_(0)
	, scrollActive_(false)
	, playlistScrollActive_(false)
	, gameScrollActive_(false)
	, selectedItem_(NULL)
	, textStatusComponent_(NULL)
	, loadSoundChunk_(NULL)
	, unloadSoundChunk_(NULL)
	, highlightSoundChunk_(NULL)
	, selectSoundChunk_(NULL)
	, minShowTime_(0)
	, jukebox_(false)
{

	for (int i = 0; i < MAX_LAYOUTS; i++) {
		layoutWidth_.push_back(layoutWidth);
		layoutHeight_.push_back(layoutHeight);
	}
	for (int i = 0; i < SDL::getScreenCount(); i++) {
		layoutWidthByMonitor_.push_back(layoutWidth);
		layoutHeightByMonitor_.push_back(layoutHeight);
	}

	currentLayout_ = 0;
}


Page::~Page() = default;


void Page::deInitialize() {
	cleanup();

	// Deinitialize and clear menus_
	for (auto& menuVector : menus_) {
		for (ScrollingList* menu : menuVector) {
			delete menu;
		}
		menuVector.clear();
	}
	menus_.clear();

	// Deinitialize and clear LayerComponents_
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			//component->freeGraphicsMemory();
			delete component;
		}
		layer.clear();
	}
	LayerComponents_.clear();

	// Delete sound chunks and reset pointers
	if (loadSoundChunk_) {
		delete loadSoundChunk_;
		loadSoundChunk_ = nullptr;
	}

	if (unloadSoundChunk_) {
		delete unloadSoundChunk_;
		unloadSoundChunk_ = nullptr;
	}

	if (highlightSoundChunk_)
	{
		delete highlightSoundChunk_;
		highlightSoundChunk_ = nullptr;
	}

	if (selectSoundChunk_) {
		delete selectSoundChunk_;
		selectSoundChunk_ = nullptr;
	}

	// Deinitialize and clear collections_
	for (auto& collectionEntry : collections_) {
		delete collectionEntry.collection;
	}
	collections_.clear();
}

bool Page::isMenusFull() const {
	return (menuDepth_ > menus_.size());
}


void Page::setLoadSound(Sound* chunk) {
	loadSoundChunk_ = chunk;
}


void Page::setUnloadSound(Sound* chunk) {
	unloadSoundChunk_ = chunk;
}


void Page::setHighlightSound(Sound* chunk) {
	highlightSoundChunk_ = chunk;
}


void Page::setSelectSound(Sound* chunk) {
	selectSoundChunk_ = chunk;
}

ScrollingList* Page::getAnActiveMenu() {
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

void Page::setActiveMenuItemsFromPlaylist(MenuInfo_S info, ScrollingList* menu) {
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

	for (auto it = menus_.begin(); it != menus_.end(); ++it) {
		for (auto it2 = it->begin(); it2 != it->end(); ++it2) {
			ScrollingList* menu = *it2;
			if (menu)
				menu->setNewItemSelected();
		}
	}

	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component) {
				component->setNewItemSelected();
			}
		}
	}
}


void Page::returnToRememberSelectedItem() {
	if (!getAnActiveMenu()) return;

	if (std::string name = getPlaylistName(); name != "" && lastPlaylistOffsets_[name]) {
		setScrollOffsetIndex(lastPlaylistOffsets_[name]);
	}
}

void Page::rememberSelectedItem() {
	ScrollingList const* amenu = getAnActiveMenu();
	if (!amenu || !amenu->getItems().size()) return;

	std::string name = getPlaylistName();
	if (name == "lastplayed") {
		// For the "last played" list, we only remember our position in "alpha" mode.
		std::string lastPlayedSort = "time";
		config_.getProperty(OPTION_LASTPLAYEDSORTTYPE, lastPlayedSort);

		if (lastPlayedSort == "alpha") {
			lastPlaylistOffsets_[name] = amenu->getScrollOffsetIndex();
		}
		// Implicitly, if the mode is "time", we do nothing, "forgetting" the position.

	}
	else if (name != "" && selectedItem_) {
		// For any other playlist, we always remember the position.
		lastPlaylistOffsets_[name] = amenu->getScrollOffsetIndex();
	}
}

std::map<std::string, size_t> Page::getLastPlaylistOffsets() const {
	return lastPlaylistOffsets_;
}

void Page::onNewScrollItemSelected() {
	if (!getAnActiveMenu()) return;

	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component) {
				component->setNewScrollItemSelected();
			}
		}
	}

}


void Page::highlightLoadArt() {
	if (!getAnActiveMenu()) return;

	// loading new items art
	setSelectedItem();

	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component) {
				component->setNewItemSelected();
			}
		}
	}

}


void Page::pushMenu(ScrollingList* s, int index) {
	// If index < 0 then append to the menus_ vector
	if (index < 0) {
		index = static_cast<int>(menus_.size());
	}

	// Increase menus_ as needed
	while (index >= static_cast<int>(menus_.size())) {
		std::vector<ScrollingList*> menus;
		menus_.push_back(menus);
	}

	menus_[index].push_back(s);
}


unsigned int Page::getMenuDepth() const {
	return menuDepth_;
}


void Page::setStatusTextComponent(Text* t) {
	textStatusComponent_ = t;
}


bool Page::addComponent(Component* c) {
	if (c->baseViewInfo.Layer < NUM_LAYERS) {
		// Ensure that the vector for the specific layer exists
		if (LayerComponents_.size() <= c->baseViewInfo.Layer) {
			LayerComponents_.resize(NUM_LAYERS);
		}
		// Add the component to the appropriate layer vector
		LayerComponents_[c->baseViewInfo.Layer].push_back(c);
		return true;
	}
	else {
		std::stringstream ss;
		ss << "Component layer too large. Layer: " << c->baseViewInfo.Layer;
		LOG_ERROR("Page", ss.str());
		return false;
	}
}


bool Page::isMenuIdle() {
	if (playlistMenu_ && !playlistMenu_->isScrollingListIdle())
		return false;

	for (auto it = menus_.begin(); it != menus_.end(); ++it) {
		for (auto it2 = it->begin(); it2 != it->end(); ++it2) {
			ScrollingList* menu = *it2;
			if (menu && !menu->isScrollingListIdle()) {
				return false;
			}
		}
	}
	return true;
}


bool Page::isIdle() {
	bool idle = isMenuIdle();

	// Iterate from the highest layer to the lowest
	for (auto it = LayerComponents_.rbegin(); it != LayerComponents_.rend() && idle; ++it) {
		for (const Component* component : *it) {
			if (!component->isIdle()) {
				idle = false;
				break;  // Exit inner loop early if a component is not idle
			}
		}
	}

	return idle;
}



bool Page::isAttractIdle() {
	// Check if any menu is not attract idle
	for (const auto& menuVector : menus_) {
		for (const ScrollingList* menu : menuVector) {
			if (!menu->isAttractIdle()) {
				return false;
			}
		}
	}

	// Iterate from the highest layer to the lowest
	for (auto it = LayerComponents_.rbegin(); it != LayerComponents_.rend(); ++it) {
		for (const Component* component : *it) {
			if (!component->isAttractIdle()) {
				return false;
			}
		}
	}

	return true;
}



bool Page::isGraphicsIdle() {
	// Iterate from the highest layer to the lowest
	for (auto it = LayerComponents_.rbegin(); it != LayerComponents_.rend(); ++it) {
		for (const Component* component : *it) {
			if (!component->isIdle()) {
				return false;
			}
		}
	}

	return true;
}


void Page::start() {
	for (auto it = menus_.begin(); it != menus_.end(); ++it) {
		for (auto it2 = it->begin(); it2 != it->end(); ++it2) {
			ScrollingList* menu = *it2;
			menu->triggerEvent("enter");
			menu->triggerEnterEvent();
		}
	}

	if (loadSoundChunk_) {
		loadSoundChunk_->play();
	}

	// Trigger "enter" events for all components, iterating from lowest to highest layer
	for (const auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			component->triggerEvent("enter");
		}
	}
}


void Page::stop() {
	for (auto it = menus_.begin(); it != menus_.end(); ++it) {
		for (auto it2 = it->begin(); it2 != it->end(); ++it2) {
			ScrollingList* menu = *it2;
			menu->triggerEvent("exit");
			menu->triggerExitEvent();
		}
	}

	if (unloadSoundChunk_) {
		unloadSoundChunk_->play();
	}

	// Trigger "exit" events for all components, iterating from highest to lowest layer
	for (auto it = LayerComponents_.rbegin(); it != LayerComponents_.rend(); ++it) {
		for (Component* component : *it) {
			component->triggerEvent("exit");
		}
	}
}


void Page::setSelectedItem() {
	selectedItem_ = getSelectedMenuItem();
}

Item* Page::getSelectedItem() {
	if (!selectedItem_) {
		setSelectedItem();
	}

	return selectedItem_;
}

Item* Page::getSelectedItem(int offset) {
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return nullptr;

	return amenu->getItemByOffset(offset);
}


Item* Page::getSelectedMenuItem() {
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return nullptr;

	return amenu->getSelectedItem();
}


void Page::removeSelectedItem() {
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

	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); ++it) {
		if ((*it) && !(*it)->isPlaylist())
			(*it)->setScrollOffsetIndex(i);
	}
}


size_t Page::getScrollOffsetIndex() {
	ScrollingList const* amenu = getAnActiveMenu();
	if (!amenu) return -1;

	return amenu->getScrollOffsetIndex();
}


void Page::setMinShowTime(float value) {
	minShowTime_ = value;
}


float Page::getMinShowTime() const {
	return minShowTime_;
}

std::string Page::controlsType() const {
	return controlsType_;
}

void Page::setControlsType(const std::string& type) {
	controlsType_ = type;
}

void Page::playlistChange() {
	std::string playlistName = getPlaylistName();

	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu)
			menu->setPlaylist(playlistName);
	}

	// Update the playlist for all components, layer by layer
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			component->setPlaylist(playlistName);
		}
	}

	lastPlaylistName_ = playlistName;

	updatePlaylistMenuPosition();
}

void Page::menuScroll() {
	if (Item const* item = selectedItem_; !item)
		return;

	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			component->triggerEvent("menuScroll", menuDepth_ - 1);
		}
	}
}

void Page::playlistScroll() {
	if (Item const* item = selectedItem_; !item)
		return;

	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			component->triggerEvent("playlistScroll", menuDepth_ - 1);
		}
	}
}

void Page::highlightEnter() {
	triggerEventOnAllMenus("highlightEnter");
}

void Page::highlightExit() {
	triggerEventOnAllMenus("highlightExit");
}

void Page::playlistEnter() {
	// entered in new playlist set selected item
	setSelectedItem();
	triggerEventOnAllMenus("playlistEnter");
}

void Page::playlistExit() {
	triggerEventOnAllMenus("playlistExit");
}

void Page::playlistNextEnter() {
	fromPlaylistNav = true;
	fromPreviousPlaylist = false;
	triggerEventOnAllMenus("playlistNextEnter");
}

void Page::playlistNextExit() {
	fromPreviousPlaylist = false;
	triggerEventOnAllMenus("playlistNextExit");
	fromPlaylistNav = false;
}

void Page::playlistPrevEnter() {
	fromPlaylistNav = true;
	fromPreviousPlaylist = true;
	triggerEventOnAllMenus("playlistPrevEnter");
}

void Page::playlistPrevExit() {
	fromPreviousPlaylist = true;
	triggerEventOnAllMenus("playlistPrevExit");
	fromPlaylistNav = false;
}

void Page::menuJumpEnter() {
	// jumped into new item
	setSelectedItem();
	triggerEventOnAllMenus("menuJumpEnter");
}

void Page::menuJumpExit() {
	triggerEventOnAllMenus("menuJumpExit");
}


void Page::attractEnter() {
	triggerEventOnAllMenus("attractEnter");
}

void Page::attract() {
	triggerEventOnAllMenus("attract");
}

void Page::attractExit() {
	triggerEventOnAllMenus("attractExit");
}

void Page::gameInfoEnter() {
	triggerEventOnAllMenus("gameInfoEnter");
}
void Page::gameInfoExit() {
	triggerEventOnAllMenus("gameInfoExit");
}

void Page::collectionInfoEnter() {
	triggerEventOnAllMenus("collectionInfoEnter");
}
void Page::collectionInfoExit() {
	triggerEventOnAllMenus("collectionInfoExit");
}

void Page::buildInfoEnter() {
	triggerEventOnAllMenus("buildInfoEnter");
}
void Page::buildInfoExit() {
	triggerEventOnAllMenus("buildInfoExit");
}

void Page::jukeboxJump() {
	triggerEventOnAllMenus("jukeboxJump");
}

void Page::triggerEventOnAllMenus(const std::string& event) {
	if (!selectedItem_)
		return;

	unsigned int depth = menuDepth_ - 1;
	for (size_t i = 0; i < menus_.size(); ++i) {
		for (ScrollingList* menu : menus_[i]) {
			unsigned int index = (depth == i) ? MENU_INDEX_HIGH + depth : depth;

			menu->triggerEvent(event, index);
			menu->triggerEventOnAll(event, index);
		}
	}

	unsigned int index = depth;
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component)
				component->triggerEvent(event, index);
		}
	}
}



void Page::triggerEvent(const std::string& action) {
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component)
				component->triggerEvent(action);
		}
	}
}


void Page::setText(const std::string& text, int id) {
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component)
				component->setText(text, id);
		}
	}
}


void Page::setScrolling(ScrollDirection direction) {
	switch (direction) {
		case ScrollDirectionForward:
		case ScrollDirectionBack:
		if (!scrollActive_)
		{
			menuScroll();
		}
		scrollActive_ = gameScrollActive_ = true;
		playlistScrollActive_ = false;
		break;
		case ScrollDirectionPlaylistForward:
		case ScrollDirectionPlaylistBack:
		if (!scrollActive_)
		{
			playlistScroll();
		}
		scrollActive_ = playlistScrollActive_ = true;
		gameScrollActive_ = false;
		break;
		case ScrollDirectionIdle:
		default:
		scrollActive_ = playlistScrollActive_ = gameScrollActive_ = false;
		break;
	}

}


bool Page::isHorizontalScroll() {
	ScrollingList const* amenu = getAnActiveMenu();
	if (!amenu) return false;

	return amenu->horizontalScroll;
}


void Page::pageScroll(ScrollDirection direction) {
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return;

	if (direction == ScrollDirectionForward) {
		amenu->pageDown();
	}
	else if (direction == ScrollDirectionBack) {
		amenu->pageUp();
	}

	size_t index = amenu->getScrollOffsetIndex();
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu)
			menu->setScrollOffsetIndex(index);
	}
}

void Page::selectRandom() {
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return;

	amenu->random();
	size_t index = amenu->getScrollOffsetIndex();
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && !menu->isPlaylist())
			menu->setScrollOffsetIndex(index);
	}
	setSelectedItem();
}

void Page::selectRandomPlaylist(CollectionInfo* collection, std::vector<std::string> cycleVector) {
	size_t size = collection->playlists.size();
	if (size == 0) return;

	int index = rand() % size;
	int i = 0;
	std::string playlistName;
	std::string settingsPlaylist = "settings";
	std::string quickListPlaylist = "quicklist";
	config_.setProperty("settingsPlaylist", settingsPlaylist);
	config_.setProperty("quickListPlaylist", quickListPlaylist);

	for (auto it = collection->playlists.begin(); it != collection->playlists.end(); it++) {
		if (i == index &&
			it->first != settingsPlaylist &&
			it->first != quickListPlaylist &&
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
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && !menu->isPlaylist()) {
			if (direction == ScrollDirectionForward) {
				menu->letterDown();
			}
			if (direction == ScrollDirectionBack) {
				menu->letterUp();
			}
		}
	}
}

// if playlist is same name as metadata to sort upon, then jump by unique sorted metadata
void Page::metaScroll(ScrollDirection direction, std::string attribute) {
	std::transform(attribute.begin(), attribute.end(), attribute.begin(), ::tolower);

	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && !menu->isPlaylist()) {
			if (direction == ScrollDirectionForward) {
				menu->metaDown(attribute);
			}
			if (direction == ScrollDirectionBack) {
				menu->metaUp(attribute);
			}
		}
	}
}


void Page::cfwLetterSubScroll(ScrollDirection direction) {
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && !menu->isPlaylist()) {
			if (direction == ScrollDirectionForward) {
				menu->cfwLetterSubDown();
			}
			if (direction == ScrollDirectionBack) {
				menu->cfwLetterSubUp();
			}
		}
	}
}


size_t Page::getCollectionSize() {
	ScrollingList const* amenu = getAnActiveMenu();
	if (!amenu) return 0;

	return amenu->getSize();
}


size_t Page::getSelectedIndex() {
	ScrollingList const* amenu = getAnActiveMenu();
	if (!amenu) return 0;

	return amenu->getSelectedIndex();
}


bool Page::pushCollection(CollectionInfo* collection) {
	if (!collection) {
		return false;
	}

	// Before creating new menus, cleanup existing ones at base level
	if (menus_.size() <= menuDepth_ && getAnActiveMenu()) {
		// Store the current state/positions that we want to preserve
		std::vector<size_t> menuPositions;
		for (auto* menu : activeMenu_) {
			if (menu) {
				menuPositions.push_back(menu->getScrollOffsetIndex());
				menu->freeGraphicsMemory(); // Free graphics memory but don't delete yet
			}
		}

		// Now create new menus for the next depth
		size_t posIndex = 0;
		for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
			ScrollingList const* menu = *it;
			auto* newMenu = new ScrollingList(*menu);
			if (newMenu->isPlaylist()) {
				playlistMenu_ = newMenu;
			}
			pushMenu(newMenu, menuDepth_);

			// Restore position if we have one stored
			if (posIndex < menuPositions.size()) {
				newMenu->setScrollOffsetIndex(menuPositions[posIndex]);
			}
			posIndex++;
		}
	}

	// Set active menu and update with new collection items
	if (menus_.size()) {
		activeMenu_ = menus_[menuDepth_];
		anActiveMenu_ = nullptr;
		selectedItem_ = nullptr;

		for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
			ScrollingList* menu = *it;
			menu->collectionName = collection->name;
			// add playlist menu items
			if (menu->isPlaylist() && collection->playlistItems.size()) {
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

	// build the collection info instance
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

	// Update collection name for layer components
	for (const auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component) {
				component->collectionName = collection->name;
			}
		}
	}

	return true;
}

bool Page::popCollection() {
	if (!getAnActiveMenu()) return false;
	if (menuDepth_ <= 1) return false;
	if (collections_.size() <= 1) return false;

	// Clean up menus at current depth
	if (menuDepth_ < menus_.size()) {
		for (ScrollingList* menu : menus_[menuDepth_ - 1]) {
			if (menu) {
				menu->freeGraphicsMemory();
				delete menu;
			}
		}
		menus_[menuDepth_ - 1].clear();
	}

	// queue the collection for deletion
	MenuInfo_S* info = &collections_.back();
	info->queueDelete = true;
	deleteCollections_.push_back(*info);

	// get the next collection off of the stack
	collections_.pop_back();
	info = &collections_.back();

	// build playlist menu
	if (playlistMenu_ && info->collection->playlistItems.size()) {
		playlistMenu_->setItems(&info->collection->playlistItems);
	}

	playlist_ = info->playlist;
	playlistChange();

	menuDepth_--;
	activeMenu_ = menus_[menuDepth_ - 1];

	// Reallocate graphics memory for the menu we're returning to
	for (ScrollingList* menu : activeMenu_) {
		if (menu) {
			menu->allocateGraphicsMemory();
		}
	}

	anActiveMenu_ = nullptr;
	selectedItem_ = nullptr;

	// Update collection name for all layer components
	for (const auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component) {
				component->collectionName = info->collection->name;
			}
		}
	}

	return true;
}

void Page::enterMenu() {
	triggerEventOnAllMenus("menuEnter");
}


void Page::exitMenu() {
	triggerEventOnAllMenus("menuExit");
}


void Page::enterGame() {
	triggerEventOnAllMenus("gameEnter");
}

void Page::trackChange() {
	triggerEventOnAllMenus("trackChange");
}


void Page::exitGame() {
	triggerEventOnAllMenus("gameExit");
}


std::string Page::getPlaylistName() const {
	return !collections_.empty() ? playlist_->first : "";
}


void Page::favPlaylist() {
	if (getPlaylistName() == "favorites") {
		selectPlaylist("all");
	}
	else {
		selectPlaylist("favorites");
	}
	return;
}

void Page::nextPlaylist() {
	MenuInfo_S& info = collections_.back();
	size_t numlists = info.collection->playlists.size();
	// save last playlist selected item
	rememberSelectedItem();

	for (size_t i = 0; i <= numlists; ++i) {
		playlist_++;
		// wrap
		if (playlist_ == info.collection->playlists.end())
			playlist_ = info.collection->playlists.begin();

		// find the first playlist
		if (!playlist_->second->empty())
			break;
	}

	playlistNextEnter();

	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		setActiveMenuItemsFromPlaylist(info, *it);
	}
	playlistChange();
}

void Page::prevPlaylist() {
	MenuInfo_S& info = collections_.back();
	size_t numlists = info.collection->playlists.size();
	// save last playlist selected item
	rememberSelectedItem();

	for (size_t i = 0; i <= numlists; ++i) {
		// wrap
		if (playlist_ == info.collection->playlists.begin()) {
			playlist_ = info.collection->playlists.end();
		}
		playlist_--;

		// find the first playlist
		if (!playlist_->second->empty())
			break;
	}

	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		setActiveMenuItemsFromPlaylist(info, *it);
	}
	playlistChange();
}


void Page::selectPlaylist(const std::string& playlist) {
	MenuInfo_S& info = collections_.back();

	// Check if "remember menu" functionality is enabled.
	// Our modified rememberSelectedItem() will handle when to actually store the offset.
	bool rememberMenu = false;
	config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
	if (rememberMenu)
		rememberSelectedItem();

	// Store the current playlist to restore if the target is not found or empty.
	CollectionInfo::Playlists_T::iterator playlist_store = playlist_;

	// Find the target playlist.
	auto it_playlist = info.collection->playlists.find(playlist);

	// If the playlist doesn't exist or is empty, restore the original and exit.
	if (it_playlist == info.collection->playlists.end() || it_playlist->second->empty()) {
		// Log a warning if the playlist exists but is just empty, as this is useful info.
		if (it_playlist != info.collection->playlists.end()) {
			LOG_WARNING("Page", "Attempted to select playlist '" + playlist + "', but it is empty.");
		}
		playlist_ = playlist_store;
		return;
	}

	// The playlist is valid, so set it.
	playlist_ = it_playlist;

	// Update the active menu's items from the newly selected playlist.
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		setActiveMenuItemsFromPlaylist(info, *it);
	}

	// --- Determine the initial scroll position for the new playlist ---
	size_t initialOffset = 0;
	ScrollingList* amenu = getAnActiveMenu();

	// Priority 1: Check for a remembered position.
	// This will only be true for 'lastplayed' if the mode is 'alpha'.
	if (lastPlaylistOffsets_.count(playlist) > 0) {
		initialOffset = lastPlaylistOffsets_[playlist];
	}
	else {
		// Priority 2: If no position was remembered, check if we should apply a random start.
		bool randomStart = false;
		config_.getProperty(OPTION_RANDOMSTART, randomStart);

		bool applyRandomStart = randomStart; // Assume we can apply it based on the global setting.

		// Now, apply exclusion rules.
		if (playlist == "lastplayed") {
			// For the "last played" list, we must check its sort mode.
			std::string lastPlayedSort = "time";
			config_.getProperty(OPTION_LASTPLAYEDSORTTYPE, lastPlayedSort);
			if (lastPlayedSort == "time") {
				// In "time" mode, we explicitly FORBID random start.
				applyRandomStart = false;
			}
		}
		else if (playlist == "settings") { // You can add other lists here if needed.
			// Never apply random start to the settings playlist.
			applyRandomStart = false;
		}

		// If, after all checks, we should apply random start, do it now.
		if (applyRandomStart && amenu) {
			amenu->random();
			initialOffset = amenu->getScrollOffsetIndex();

			// If "remember menu" is on, store this random position so it's consistent
			// for the rest of the session.
			if (rememberMenu) {
				lastPlaylistOffsets_[playlist] = initialOffset;
			}
		}
		// If we reach here and applyRandomStart was false, initialOffset remains 0,
		// which is the correct default for "time" mode and other excluded lists.
	}

	setScrollOffsetIndex(initialOffset);

	// Trigger the necessary UI updates.
	playlistChange();
	setSelectedItem();
}


void Page::updatePlaylistMenuPosition() {
	if (playlistMenu_) {
		std::string name = getPlaylistName();
		if (name != "") {
			playlistMenu_->selectItemByName(name);
		}
	}
}

void Page::nextCyclePlaylist(std::vector<std::string> list) {
	if (list.empty()) return;

	std::string settingsPlaylist = "";
	std::string quickListPlaylist = "";
	config_.getProperty("settingsPlaylist", settingsPlaylist);
	config_.getProperty("quickListPlaylist", quickListPlaylist);

	auto it = std::find(list.begin(), list.end(), getPlaylistName());

	playlistNextEnter();

	std::string nextPlaylist;
	if (it == list.end()) {
		for (auto it2 = list.begin(); it2 != list.end(); ++it2) {
			if (*it2 != settingsPlaylist && *it2 != quickListPlaylist && playlistExists(*it2)) {
				nextPlaylist = *it2;
				break;
			}
		}
	}
	else {
		do {
			++it;
			if (it == list.end()) it = list.begin();
		} while (*it == settingsPlaylist || *it == quickListPlaylist || !playlistExists(*it));
		nextPlaylist = *it;
	}

	// Call selectPlaylist (restores remembered item automatically)
	selectPlaylist(nextPlaylist);
}

void Page::prevCyclePlaylist(std::vector<std::string> list) {
	// Empty list
	if (list.empty()) return;

	std::string settingsPlaylist = "";
	std::string quickListPlaylist = "";
	config_.getProperty("settingsPlaylist", settingsPlaylist);
	config_.getProperty("quickListPlaylist", quickListPlaylist);

	// Find the current playlist in the list
	auto it = std::find(list.begin(), list.end(), getPlaylistName());

	std::string prevPlaylist;

	// If current playlist not found, switch to the last playlist in the list
	if (it == list.end()) {
		for (auto it2 = list.rbegin(); it2 != list.rend(); ++it2) {
			if (*it2 != settingsPlaylist && *it2 != quickListPlaylist && playlistExists(*it2)) {
				prevPlaylist = *it2;
				break;
			}
		}
	}
	else {
		// Switch to the previous playlist in the list
		do {
			if (it == list.begin()) {
				it = list.end(); // wrap
			}
			--it;
		} while (*it == settingsPlaylist || *it == quickListPlaylist || !playlistExists(*it));

		prevPlaylist = *it;
	}

	// Call selectPlaylist with the determined playlist
	selectPlaylist(prevPlaylist);
}

bool Page::playlistExists(const std::string& playlist) {
	MenuInfo_S& info = collections_.back();
	CollectionInfo::Playlists_T p = info.collection->playlists;

	// playlist exists in cycle and contains items
	return p.end() != p.find(playlist) && !info.collection->playlists[playlist]->empty();
}


void Page::update(float dt) {
	std::string playlistName = getPlaylistName();

	// Check if the playlist name has changed since the last update
	bool playlistNameChanged = false;
	if (playlistName != lastPlaylistName_) {
		lastPlaylistName_ = playlistName;
		playlistNameChanged = true;
	}

	// Synchronous (non-threaded) version for OpenGL backend

	for (auto& menuList : menus_) {
		for (auto* menu : menuList) {
			if (playlistNameChanged) {
				menu->playlistName = lastPlaylistName_;
			}
			menu->update(dt);
		}
	}

	for (auto& layer : LayerComponents_) {
		for (auto it = layer.begin(); it != layer.end();) {
			if (*it) {
				if (playlistNameChanged) {
					(*it)->playlistName = lastPlaylistName_;
				}
				if ((*it)->update(dt) && (*it)->getAnimationDoneRemove()) {
					(*it)->freeGraphicsMemory();
					delete* it;
					it = layer.erase(it);
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
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component) {
				component->update(dt);
			}
		}
	}
}

void Page::cleanup() {
	auto del = deleteCollections_.begin();

	while (del != deleteCollections_.end()) {
		MenuInfo_S& info = *del;
		if (info.queueDelete) {
			std::list<MenuInfo_S>::iterator next = del;
			++next;

			if (info.collection) {
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


void Page::draw(int monitor) {
	for (unsigned int i = 0; i < NUM_LAYERS; ++i) {
		if (i >= LayerComponents_.size()) {
			LOG_ERROR("Page::draw", "Layer index out of bounds: " + std::to_string(i));
			break;
		}

		if (LayerComponents_[i].empty() && menus_.empty()) {
			continue;
		}

		for (Component* component : LayerComponents_[i]) {
			if (!component) {
				continue;
			}
			if (component->baseViewInfo.Monitor == monitor) {
				component->draw();
			}
		}

		for (const auto& menuList : menus_) {
			for (ScrollingList* const menu : menuList) {
				if (!menu) {
					continue;
				}
				for (Component* c : menu->getComponents()) {
					if (!c) {
						continue;
					}
					if (c->baseViewInfo.Layer == i && c->baseViewInfo.Monitor == monitor) {
						c->draw();
					}
				}
			}
		}
	}
}



void Page::removePlaylist() {
	if (!selectedItem_) { LOG_WARNING("Page::removePlaylist", "No selectedItem_"); return; }

	MenuInfo_S& info = collections_.back();
	CollectionInfo* collection = info.collection;
	std::vector<Item*>* favItems = collection->playlists["favorites"];

	Item* itemToRemove = selectedItem_; // Ensured fresh by caller
	size_t originalDataIndex_of_itemToRemove = -1;
	size_t oldFavItemsSize = favItems->size(); // Size BEFORE removal

	if (oldFavItemsSize == 0) {
		LOG_WARNING("Page::removePlaylist", "Attempting to remove from an already empty favorites list.");
		return; // Nothing to do
	}

	auto it_find_original = std::find(favItems->begin(), favItems->end(), itemToRemove);
	if (it_find_original != favItems->end()) {
		originalDataIndex_of_itemToRemove = std::distance(favItems->begin(), it_find_original);
	}
	else {
		LOG_ERROR("Page::removePlaylist", "CRITICAL: itemToRemove '" + itemToRemove->name + "' not found in favItems. Page::selectedItem_ was: " + (selectedItem_ ? selectedItem_->name : "nullptr") + ". Aborting.");
		return;
	}

	// LOG_DEBUG("Page::removePlaylist", "Removing: " + itemToRemove->getName() + " from original index: " + std::to_string(originalDataIndex_of_itemToRemove) + ". List size before: " + std::to_string(oldFavItemsSize));

	// --- Step 1: Determine the Item* that was cyclically previous to itemToRemove ---
	Item* candidateItemToSelect = nullptr;
	if (oldFavItemsSize > 1) { // Only if there are other items to select from the original list
		// Calculate the index of the item cyclically AFTER the one being removed
		size_t indexOfCyclicallyNext = (originalDataIndex_of_itemToRemove + 1) % oldFavItemsSize;
		candidateItemToSelect = (*favItems)[indexOfCyclicallyNext];
		// LOG_DEBUG("Page::removePlaylist", "Candidate for new selection (cyclically next): " + candidateItemToSelect->getName());
	}
	else {
		// List had only one item (itemToRemove). After removal, it will be empty.
		// candidateItemToSelect remains nullptr.
		// LOG_DEBUG("Page::removePlaylist", "List had only one item. Will be empty after removal.");
	}

	// --- Step 2: Modify shared data ---
	favItems->erase(it_find_original); // itemToRemove is now out of favItems
	itemToRemove->isFavorite = false;
	collection->sortPlaylists();     // favItems is now shorter and sorted
	collection->saveRequest = true;
	size_t newFavItemsSize = favItems->size();
	// LOG_DEBUG("Page::removePlaylist", "Data modified. New list size: " + std::to_string(newFavItemsSize));


	// --- Step 3: Calculate new target index in the final favItems ---
	size_t newTargetIndexInFavItems = 0; // Default if list becomes empty or candidate not found

	if (newFavItemsSize > 0) { // Only if the list is not empty after removal
		if (candidateItemToSelect) { // candidateItemToSelect was determined from old list
			// Find the candidateItemToSelect in the NEW (shorter, sorted) favItems list
			auto it_find_candidate_new_pos = std::find(favItems->begin(), favItems->end(), candidateItemToSelect);
			if (it_find_candidate_new_pos != favItems->end()) {
				newTargetIndexInFavItems = std::distance(favItems->begin(), it_find_candidate_new_pos);
				// LOG_DEBUG("Page::removePlaylist", "Found candidate '" + candidateItemToSelect->getName() + "' at new index: " + std::to_string(newTargetIndexInFavItems));
			}
			else {
				// This can happen if candidateItemToSelect was ALSO itemToRemove (e.g., list size 1, though caught above)
				// OR if sorting removed/changed it, or duplicates. Unlikely for favorites.
				// Fallback: select the new first item if candidate is gone.
				LOG_WARNING("Page::removePlaylist", "Candidate item '" + candidateItemToSelect->name + "' not found in new list. Selecting first item.");
				newTargetIndexInFavItems = 0;
			}
		}
		else {
			// This case should ideally not be hit if oldFavItemsSize > 0, because if oldFavItemsSize was 1,
			// newFavItemsSize would be 0 and we wouldn't be in this block.
			// If oldFavItemsSize was > 1, candidateItemToSelect should have been set.
			// But as a safeguard, if list is not empty but candidate is null, select first.
			LOG_WARNING("Page::removePlaylist", "List not empty, but no candidate. Selecting first item.");
			newTargetIndexInFavItems = 0;
		}
	}
	// LOG_DEBUG("Page::removePlaylist", "Final newTargetIndexInFavItems: " + std::to_string(newTargetIndexInFavItems));


	// --- Step 4: Update ALL synchronized ScrollingLists in the current view ---
	if (getPlaylistName() == "favorites") {
		for (ScrollingList* menu : activeMenu_) { // Page::activeMenu_ is std::vector<ScrollingList*>
			if (menu && !menu->isPlaylist()) {
				menu->setItems(favItems); // Inform of new data/size, resets menu->itemIndex_
				if (newFavItemsSize > 0) {
					menu->setScrollOffsetIndex(newTargetIndexInFavItems); // Set to our calculated target
				}
				else {
					menu->setScrollOffsetIndex(0); // List is empty
				}
			}
		}
	}

	// Refresh Page's main selectedItem_
	setSelectedItem();

	// --- Save & standard UI update ---
	collection->saveFavorites();
	onNewItemSelected();
	// Caller (input handler) then calls Page::reallocateMenuSpritePoints().
}


void Page::addPlaylist() {
	if (!selectedItem_) return;

	MenuInfo_S& info = collections_.back();
	CollectionInfo* collection = info.collection;

	if (std::vector<Item*>* items = collection->playlists["favorites"]; getPlaylistName() != "favorites" && std::find(items->begin(), items->end(), selectedItem_) == items->end()) {
		items->push_back(selectedItem_);
		selectedItem_->isFavorite = true;
		collection->sortPlaylists();
		collection->saveRequest = true;
	}
	collection->saveFavorites();
}


void Page::togglePlaylist() {
	if (!selectedItem_) return;

	if (getPlaylistName() != "favorites") {
		if (selectedItem_->isFavorite)
			removePlaylist();
		else
			addPlaylist();
	}
}

std::string Page::getCollectionName() {
	if (collections_.size() == 0) return "";

	MenuInfo_S const& info = collections_.back();
	return info.collection->name;

}


CollectionInfo* Page::getCollection() {
	return collections_.back().collection;
}


void Page::freeGraphicsMemory() {
	for (auto const& menuVector : menus_) {
		for (ScrollingList* menu : menuVector) {
			menu->freeGraphicsMemory();
		}
	}

	if (loadSoundChunk_) loadSoundChunk_->free();
	if (unloadSoundChunk_) unloadSoundChunk_->free();
	if (highlightSoundChunk_) highlightSoundChunk_->free();
	if (selectSoundChunk_) selectSoundChunk_->free();

	// Free graphics memory for all components across layers
	for (const auto& layerComponents : LayerComponents_) {
		for (Component* component : layerComponents) {
			component->freeGraphicsMemory();
		}
	}
}


void Page::allocateGraphicsMemory() {
	LOG_DEBUG("Page", "Allocating graphics memory");

	int currentDepth = 0;
	for (auto const& menuList : menus_) {
		if (currentDepth < static_cast<int>(menuDepth_)) {
			for (auto& menu : menuList) {
				if (menu) {
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

	for (const auto& layerComponents : LayerComponents_) {
		for (Component* component : layerComponents) {
			if (component) {
				component->allocateGraphicsMemory();
			}
		}
	}
	LOG_DEBUG("Page", "Allocate graphics memory complete");
}


void Page::deInitializeFonts() const {
	for (auto& menuVector : menus_) {
		for (ScrollingList* menu : menuVector) {
			menu->deInitializeFonts();
		}
	}

	// Free graphics memory for all components in each layer
	for (const auto& layerComponents : LayerComponents_) {
		for (Component* component : layerComponents) {
			if (component) {
				component->deInitializeFonts();
			}
		}
	}
}

void Page::initializeFonts() const {
	for (auto& menuVector : menus_) {
		for (ScrollingList* menu : menuVector) {
			menu->initializeFonts();
		}
	}

	// Free graphics memory for all components in each layer
	for (const auto& layerComponents : LayerComponents_) {
		for (Component* component : layerComponents) {
			if (component) {
				component->initializeFonts();
			}
		}
	}
}


void Page::playSelect() {
	if (selectSoundChunk_) {
		selectSoundChunk_->play();
	}
}


bool Page::isSelectPlaying() {
	if (selectSoundChunk_) {
		return selectSoundChunk_->isPlaying();
	}
	return false;
}


void Page::reallocateMenuSpritePoints(bool updatePlaylistMenu) const {
	for (ScrollingList* menu : activeMenu_) {
		if (menu && (!menu->isPlaylist() || updatePlaylistMenu)) {
			menu->reallocateSpritePoints();
		}
	}
}


bool Page::isMenuScrolling() const {
	return scrollActive_;
}

bool Page::isPlaylistScrolling() const {
	return playlistScrollActive_;
}

bool Page::isGamesScrolling() const {
	return gameScrollActive_;
}

bool Page::isPlaying() const {
	for (const auto& layerComponents : LayerComponents_) {
		for (const auto& component : layerComponents) {
			if (component->baseViewInfo.Monitor == 0 && component->isPlaying()) {
				return true;
			}
		}
	}
	return false;
}


void Page::resetScrollPeriod() const {
	for (auto& menu : activeMenu_) {
		if (menu) {
			menu->resetScrollPeriod();
		}
	}
}


void Page::updateScrollPeriod() const {
	for (auto& menu : activeMenu_) {
		if (menu) {
			menu->updateScrollPeriod();
		}
	}
}

bool Page::isMenuFastScrolling() const {
	for (const auto& menu : activeMenu_) {
		if (menu && menu->isFastScrolling()) {
			return true;
		}
	}
	return false;
}


void Page::scroll(bool forward, bool playlist) {

	for (auto& menu : activeMenu_) {
		if (menu && ((playlist && menu->isPlaylist()) || (!playlist && !menu->isPlaylist()))) {
			menu->scroll(forward);
		}
	}
	onNewScrollItemSelected();
	if (highlightSoundChunk_) {
		highlightSoundChunk_->play();
	}

}



bool Page::hasSubs() {
	return collections_.back().collection->hasSubs;
}

void Page::setCurrentLayout(int layout) {
	currentLayout_ = layout;
}

int Page::getCurrentLayout() const {
	return currentLayout_;
}


int Page::getLayoutWidthByMonitor(int monitor) {
	if (monitor < SDL::getScreenCount())
		return layoutWidthByMonitor_[monitor];
	else
		return 0;
}


int Page::getLayoutHeightByMonitor(int monitor) {
	if (monitor < SDL::getScreenCount())
		return layoutHeightByMonitor_[monitor];
	else
		return 0;
}


void Page::setLayoutWidthByMonitor(int monitor, int width) {
	if (monitor < SDL::getScreenCount())
		layoutWidthByMonitor_[monitor] = width;
}


void Page::setLayoutHeightByMonitor(int monitor, int height) {
	if (monitor < SDL::getScreenCount())
		layoutHeightByMonitor_[monitor] = height;
}

int Page::getLayoutWidth(int layout) {
	currentLayout_ = layout;
	return layoutWidth_[layout];
}


int Page::getLayoutHeight(int layout) {
	currentLayout_ = layout;
	return layoutHeight_[layout];
}


void Page::setLayoutWidth(int layout, int width) {
	currentLayout_ = layout;
	layoutWidth_[layout] = width;
}


void Page::setLayoutHeight(int layout, int height) {
	currentLayout_ = layout;
	layoutHeight_[layout] = height;
}

void Page::setJukebox() {
	jukebox_ = true;
	return;
}


bool Page::isJukebox() const {
	return jukebox_;
}


bool Page::isJukeboxPlaying() {
	bool retVal = false;
	for (const auto& layerComponents : LayerComponents_) {
		for (const auto& component : layerComponents) {
			retVal |= component->isJukeboxPlaying();
		}
	}
	return retVal;
}

void Page::skipForward() {
	for (auto& layerComponents : LayerComponents_) {
		for (auto& component : layerComponents) {
			component->skipForward();
		}
	}
}

void Page::skipBackward() {
	for (auto& layerComponents : LayerComponents_) {
		for (auto& component : layerComponents) {
			component->skipBackward();
		}
	}
}

void Page::skipForwardp() {
	for (auto& layerComponents : LayerComponents_) {
		for (auto& component : layerComponents) {
			component->skipForwardp();
		}
	}
}

void Page::skipBackwardp() {
	for (auto& layerComponents : LayerComponents_) {
		for (auto& component : layerComponents) {
			component->skipBackwardp();
		}
	}
}

void Page::pause() {
	for (auto& layerComponents : LayerComponents_) {
		for (auto& component : layerComponents) {
			component->pause();
		}
	}
}

void Page::resume() {
	for (auto& layerComponents : LayerComponents_) {
		for (auto& component : layerComponents) {
			component->resume();
		}
	}
}

void Page::restart() {
	for (auto& layerComponents : LayerComponents_) {
		for (auto& component : layerComponents) {
			component->restart();
		}
	}
}

void Page::restartAllByMonitor(int monitor) {
	// 1. Restart all LayerComponents_ for this monitor
	for (auto& layerComponents : LayerComponents_) {
		for (Component* component : layerComponents) {
			if (component && component->baseViewInfo.Monitor == monitor)
				component->restart();
		}
	}

	// 2. Restart all menu (ScrollingList) components for this monitor
	for (const auto& menuVector : menus_) {
		for (ScrollingList* menu : menuVector) {
			if (menu) {
				menu->restartByMonitor(monitor);
			}
		}
	}
}

unsigned long long Page::getCurrent() {
	unsigned long long ret = 0;
	for (const auto& layerComponents : LayerComponents_) {
		for (const auto& component : layerComponents) {
			ret += component->getCurrent();
		}
	}
	return ret;
}

unsigned long long Page::getDuration() {
	unsigned long long ret = 0;
	for (const auto& layerComponents : LayerComponents_) {
		for (const auto& component : layerComponents) {
			ret += component->getDuration();
		}
	}
	return ret;
}

bool Page::isPaused() {
	bool ret = false;
	for (const auto& layerComponents : LayerComponents_) {
		for (const auto& component : layerComponents) {
			ret |= component->isPaused();
		}
	}
	return ret;
}


void Page::setLocked(bool locked) {
	locked_ = locked;
}

bool Page::isLocked() const {
	return locked_;
}

ScrollingList* Page::getPlaylistMenu() {
	return playlistMenu_;
}

void Page::setPlaylistMenu(ScrollingList* menu) {
	playlistMenu_ = menu;
}

void Page::setIsLaunched(bool isLaunched) {
	isLaunched_ = isLaunched;
}

bool Page::getIsLaunched() {
	return isLaunched_;
}
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
#include "PresentationPreload.h"
#include "ComponentItemBinding.h"
#include "Component/Component.h"
#include "../Collection/CollectionInfo.h"
#include "../Collection/PlaylistDirtyRegistry.h"
#include "Component/Text.h"
#include "Component/Image.h"
#include "Font.h"
#include "../Utility/Log.h"
#include "Component/ScrollingList.h"
#include "../Sound/Sound.h"
#include "ComponentItemBindingBuilder.h"
#include "PageBuilder.h"
#include "../Utility/Utils.h"
#include "../Database/GlobalOpts.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>

namespace {
	unsigned int graphicsPreparationTier(const Component& component)
	{
		if (component.isVisibleForGraphicsPreparation() ||
			!component.isIdle())
		{
			return 0;
		}

		return component.hasHighPriority() ? 1 : 2;
	}

	std::vector<ScrollingList*> menusByVisualPriority(
		const std::vector<ScrollingList*>& menus)
	{
		std::vector<ScrollingList*> order;
		order.reserve(menus.size());

		for (ScrollingList* menu : menus) {
			if (menu) {
				order.push_back(menu);
			}
		}

		std::stable_sort(
			order.begin(),
			order.end(),
			[](const ScrollingList* lhs, const ScrollingList* rhs) {
				if (lhs->getVisualPriorityTier() !=
					rhs->getVisualPriorityTier())
				{
					return lhs->getVisualPriorityTier() <
						rhs->getVisualPriorityTier();
				}

				return lhs->getVisualPriorityLayer() >
					rhs->getVisualPriorityLayer();
			}
		);

		return order;
	}

	std::string presentationImageKey(
		const PresentationImageRequest& request)
	{
		std::string key;
		key.reserve(request.path.size() + 24);
		key.append(std::to_string(request.monitor));
		key.push_back('|');
		key.append(request.path);
		return key;
	}

	std::string presentationTextKey(
		const PresentationTextRequest& request)
	{
		std::string key;
		key.reserve(request.text.size() + 48);
		key.append(std::to_string(
			reinterpret_cast<std::uintptr_t>(request.font)));
		key.push_back('|');
		key.append(std::to_string(request.fontSize));
		key.push_back('|');
		key.append(request.text);
		return key;
	}
}


Page::Page(Configuration& config, int layoutWidth, int layoutHeight)
	: fromPreviousPlaylist(false)
	, fromPlaylistNav(false)
	, config_(config)
	, controlsType_("")
	, locked_(false)
	, anActiveMenu_(NULL)
	, playlistMenu_(NULL)
	, menuDepth_(0)
	, LayerComponents_(NUM_LAYERS)
	, scrollActive_(false)
	, playlistScrollActive_(false)
	, gameScrollActive_(false)
	, selectedItem_(NULL)
	, textStatusComponent_(NULL)
	, loadSoundChunk_(NULL)
	, unloadSoundChunk_(NULL)
	, highlightSoundChunk_(NULL)
	, selectSoundChunk_(NULL)
	, minShowTime_(0.5)
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


Page::~Page() {
	deInitialize();
}


void Page::deInitialize() {
	resetPresentationPreload_();
	cleanup();

	// Deinitialize and clear menus_
	for (auto& menuVector : menus_) {
		for (ScrollingList* menu : menuVector) {
			delete menu;
		}
		menuVector.clear();
	}
	menus_.clear();
	activeMenu_.clear();
	anActiveMenu_ = nullptr;
	playlistMenu_ = nullptr;
	selectedItem_ = nullptr;

	// Deinitialize and clear LayerComponents_
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			//component->freeGraphicsMemory();
			delete component;
		}
		layer.clear();
	}

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
	if (loadSoundChunk_) delete loadSoundChunk_;
	loadSoundChunk_ = chunk;
}

void Page::setUnloadSound(Sound* chunk) {
	if (unloadSoundChunk_) delete unloadSoundChunk_;
	unloadSoundChunk_ = chunk;
}

void Page::setHighlightSound(Sound* chunk) {
	if (highlightSoundChunk_) delete highlightSoundChunk_;
	highlightSoundChunk_ = chunk;
}

void Page::setSelectSound(Sound* chunk) {
	if (selectSoundChunk_) delete selectSoundChunk_;
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

	pendingScrollSelect_ = false;
	setSelectedItem();

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
	if (!getAnActiveMenu())
		return;

	if (std::string name = getPlaylistName();
		!name.empty() && name != "themes")
	{
		auto it = lastPlaylistOffsets_.find(name);
		if (it != lastPlaylistOffsets_.end())
		{
			setScrollOffsetIndex(it->second);
			onNewItemSelected();
		}
	}
}


void Page::rememberSelectedItem() {
	ScrollingList const* amenu = getAnActiveMenu();
	if (!amenu || !amenu->getItems().size()) return;

	std::string name = getPlaylistName();

	// Exclude "themes" from remember-last-selected behavior
	if (name == "themes") {
		return;
	}

	if (name == "lastplayed") {
		// For the "last played" list, we only remember our position in "alpha" mode.
		std::string lastPlayedSort = "time";
		config_.getProperty(OPTION_LASTPLAYEDSORTTYPE, lastPlayedSort);

		if (lastPlayedSort == "alpha") {
			lastPlaylistOffsets_[name] = amenu->getScrollOffsetIndex();
		}
		// If the mode is "time", we do nothing, "forgetting" the position.
	}
	else if (!name.empty() && selectedItem_) {
		// For any other playlist (except "themes"), we always remember the position.
		lastPlaylistOffsets_[name] = amenu->getScrollOffsetIndex();
	}
}

std::map<std::string, size_t> Page::getLastPlaylistOffsets() const {
	return lastPlaylistOffsets_;
}

void Page::setLastPlaylistOffsets(const std::map<std::string, size_t>& offsets) {
	lastPlaylistOffsets_ = offsets;
}

CollectionInfo* Page::detachCollection() {
	if (collections_.empty()) return nullptr;
	CollectionInfo* c = collections_.back().collection;
	collections_.back().collection = nullptr; // prevent deInitialize() from deleting it
	return c;
}

void Page::onNewScrollItemSelected() {
	if (!getAnActiveMenu()) return;

	pendingScrollSelect_ = false;
	setSelectedItem();

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
	invalidateFrameLayerBuckets_();
}


unsigned int Page::getMenuDepth() const {
	return menuDepth_;
}


void Page::setStatusTextComponent(Text* t) {
	textStatusComponent_ = t;
}


bool Page::addComponent(Component* c) {
	if (c->baseViewInfo.Layer < NUM_LAYERS) {
		// No need to resize�guaranteed by constructor
		LayerComponents_[c->baseViewInfo.Layer].push_back(c);
		invalidateFrameLayerBuckets_();
		return true;
	}
	else {
		std::stringstream ss;
		ss << "Component layer too large. Layer: " << c->baseViewInfo.Layer;
		LOG_ERROR("Page", ss.str());
		return false;
	}
}

void Page::invalidatePresentationPreload() {
	resetPresentationPreload_();
}

void Page::invalidateIdleCache() {
	cachedIsIdle_ = false;
	cachedIsMenuIdle_ = false;
	cachedIsGraphicsIdle_ = false;
	cachedIsAttractIdle_ = false;
}

bool Page::isMenuIdle() { return cachedIsMenuIdle_; }
bool Page::isIdle() { return cachedIsIdle_; }
bool Page::isAttractIdle() { return cachedIsAttractIdle_; }
bool Page::isGraphicsIdle() { return cachedIsGraphicsIdle_; }


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
	invalidateIdleCache();
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
	invalidateIdleCache();
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

	for (ScrollingList* menu : activeMenu_) {
		if (menu && !menu->isPlaylist()) {
			menu->syncToSelectedIndex(i);
		}
	}

	// selectPlaylist() changes the menu data before playlistChange() updates
	// lastPlaylistName_. Let playlistChange() start the new playlist's work so
	// that we do not briefly schedule the old presentation here.
	if (getPlaylistName() == lastPlaylistName_) {
		refreshPresentationPreloadQueue_();
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
	// A playlist switch supersedes any delayed notification from the
	// previously active scrolling transaction.
	pendingScrollSelect_ = false;

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
	resetPresentationPreload_();
	rebuildPresentationLetterAnchors_();
	refreshPresentationPreloadQueue_();
}

void Page::menuScroll() {
	if (!selectedItem_) return;
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			// Check if it's already scrolling before triggering!
			if (component->getAnimationRequestedType() != "menuScroll") {
				component->triggerEvent("menuScroll", menuDepth_ - 1);
			}
		}
	}
}

void Page::playlistScroll() {
	if (Item const* item = selectedItem_; !item)
		return;

	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component->getAnimationRequestedType() != "playlistScroll") {
				component->triggerEvent("playlistScroll", menuDepth_ - 1);
			}
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
	invalidateIdleCache();
}



void Page::triggerEvent(const std::string& action) {
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component)
				component->triggerEvent(action);
		}
	}
	invalidateIdleCache();
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
	const bool wasScrolling = scrolling_ != ScrollDirectionIdle;
	const bool becomingIdle = direction == ScrollDirectionIdle && wasScrolling;

	scrolling_ = direction;

	switch (direction) {
		case ScrollDirectionForward:
		case ScrollDirectionBack:
		if (!scrollActive_) menuScroll();
		scrollActive_ = gameScrollActive_ = true;
		playlistScrollActive_ = false;
		break;

		case ScrollDirectionPlaylistForward:
		case ScrollDirectionPlaylistBack:
		if (!scrollActive_) playlistScroll();
		scrollActive_ = playlistScrollActive_ = true;
		gameScrollActive_ = false;
		break;

		case ScrollDirectionIdle:
		default:
		scrollActive_ = playlistScrollActive_ = gameScrollActive_ = false;

		if (becomingIdle) {
			resetScrollPeriod();
		}

		break;
	}

	invalidateIdleCache();
}

bool Page::isHorizontalScroll() {
	ScrollingList const* amenu = getAnActiveMenu();
	if (!amenu) return false;

	return amenu->horizontalScroll;
}


void Page::pageScroll(ScrollDirection direction) {
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return;

	// 1. Master calculates jump
	if (direction == ScrollDirectionForward) {
		amenu->pageDown();
	}
	else if (direction == ScrollDirectionBack) {
		amenu->pageUp();
	}

	// 2. Broadcast to Followers (Safely ignoring Playlists)
	size_t index = amenu->getScrollOffsetIndex();
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && menu != amenu && !menu->isPlaylist()) {
			menu->syncToSelectedIndex(index);
		}
	}

	// 3. Trigger UI updates
	onNewScrollItemSelected();
	refreshPresentationPreloadQueue_(
		true,
		direction == ScrollDirectionForward
	);
	if (highlightSoundChunk_) {
		highlightSoundChunk_->play();
	}
	invalidateIdleCache();
}

void Page::selectRandom() {
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return;

	// 1. Master picks random index
	amenu->random();

	// 2. Broadcast to Followers
	size_t index = amenu->getScrollOffsetIndex();
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && menu != amenu && !menu->isPlaylist()) {
			menu->syncToSelectedIndex(index);
		}
	}

	// 3. Trigger UI updates
	onNewScrollItemSelected();
	refreshPresentationPreloadQueue_();
	if (highlightSoundChunk_) {
		highlightSoundChunk_->play();
	}
	invalidateIdleCache();
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
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return;

	if (direction == ScrollDirectionForward) {
		amenu->letterDown();
	}
	else if (direction == ScrollDirectionBack) {
		amenu->letterUp();
	}

	size_t index = amenu->getScrollOffsetIndex();
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && menu != amenu && !menu->isPlaylist()) {
			menu->syncToSelectedIndex(index);
		}
	}

	onNewScrollItemSelected();
	refreshPresentationPreloadQueue_(
		true,
		direction == ScrollDirectionForward
	);
	if (highlightSoundChunk_) {
		highlightSoundChunk_->play();
	}
	invalidateIdleCache();
}

void Page::metaScroll(ScrollDirection direction, std::string attribute) {
	std::transform(attribute.begin(), attribute.end(), attribute.begin(), ::tolower);

	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return;

	if (direction == ScrollDirectionForward) {
		amenu->metaDown(attribute);
	}
	else if (direction == ScrollDirectionBack) {
		amenu->metaUp(attribute);
	}

	size_t index = amenu->getScrollOffsetIndex();
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && menu != amenu && !menu->isPlaylist()) {
			menu->syncToSelectedIndex(index);
		}
	}

	onNewScrollItemSelected();
	refreshPresentationPreloadQueue_(
		true,
		direction == ScrollDirectionForward
	);
	invalidateIdleCache();
}

void Page::cfwLetterSubScroll(ScrollDirection direction) {
	ScrollingList* amenu = getAnActiveMenu();
	if (!amenu) return;

	if (direction == ScrollDirectionForward) {
		amenu->cfwLetterSubDown();
	}
	else if (direction == ScrollDirectionBack) {
		amenu->cfwLetterSubUp();
	}

	size_t index = amenu->getScrollOffsetIndex();
	for (auto it = activeMenu_.begin(); it != activeMenu_.end(); it++) {
		ScrollingList* menu = *it;
		if (menu && menu != amenu && !menu->isPlaylist()) {
			menu->syncToSelectedIndex(index);
		}
	}

	onNewScrollItemSelected();
	refreshPresentationPreloadQueue_(
		true,
		direction == ScrollDirectionForward
	);
	invalidateIdleCache();
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
	invalidateFrameLayerBuckets_();

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
	invalidateFrameLayerBuckets_();

	// Correctly target the current depth and shrink the vector!
	if (menuDepth_ <= menus_.size() && menuDepth_ > 1) {
		for (ScrollingList* menu : menus_[menuDepth_ - 1]) {
			if (menu) {
				menu->freeGraphicsMemory();
				delete menu;
			}
		}
		menus_[menuDepth_ - 1].clear();
		menus_.pop_back(); // <--- CRITICAL: Keeps size() synced with depth!
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
	for (ScrollingList* menu :
		menusByVisualPriority(activeMenu_))
	{
		menu->allocateGraphicsMemory();
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


const std::string& Page::getPlaylistName() const {
	static const std::string emptyString = "";
	return !collections_.empty() ? playlist_->first : emptyString;
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

	// Save the current playlist name before switching
	std::string previousPlaylist = getPlaylistName();

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
	if (!previousPlaylist.empty() && previousPlaylist != getPlaylistName()) {
		reloadPlaylistIfDirty(previousPlaylist, info.collection);
	}
}

void Page::prevPlaylist() {
	MenuInfo_S& info = collections_.back();
	size_t numlists = info.collection->playlists.size();

	// Save the current playlist name before switching
	std::string previousPlaylist = getPlaylistName();

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
	if (!previousPlaylist.empty() && previousPlaylist != getPlaylistName()) {
		reloadPlaylistIfDirty(previousPlaylist, info.collection);
	}
}

void Page::selectPlaylist(const std::string& playlist) {
	MenuInfo_S& info = collections_.back();

	// Save the current playlist name before switching
	std::string previousPlaylist = getPlaylistName();

	// Check if "remember menu" functionality is enabled.
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

	// Priority 1: Check for a remembered position (but not for "themes").
	if (playlist != "themes" && lastPlaylistOffsets_.count(playlist) > 0) {
		initialOffset = lastPlaylistOffsets_[playlist];
	}
	else {
		// Priority 2: If no position was remembered, check if we should apply a random start.
		bool randomStart = false;
		config_.getProperty(OPTION_RANDOMSTART, randomStart);

		bool applyRandomStart = randomStart;

		// Apply exclusion rules.
		if (playlist == "lastplayed") {
			std::string lastPlayedSort = "time";
			config_.getProperty(OPTION_LASTPLAYEDSORTTYPE, lastPlayedSort);
			if (lastPlayedSort == "time") {
				applyRandomStart = false;
			}
		}
		else if (playlist == "settings" || playlist == "themes") {
			applyRandomStart = false;
		}

		if (applyRandomStart && amenu) {
			amenu->random();
			initialOffset = amenu->getScrollOffsetIndex();

			bool rememberMenu = false;
			config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
			if (rememberMenu && playlist != "themes") {
				lastPlaylistOffsets_[playlist] = initialOffset;
			}
		}
	}

	setScrollOffsetIndex(initialOffset);

	// Trigger the necessary UI updates.
	playlistChange();
	setSelectedItem();

	if (!previousPlaylist.empty() && previousPlaylist != getPlaylistName()) {
		reloadPlaylistIfDirty(previousPlaylist, info.collection);
	}
}

void Page::reloadPlaylistFromDisk(const std::string& playlistName, CollectionInfo* collection) {
	// Skip dynamic/system playlists - they're generated, not file-based
// Skip dynamic/system playlists - they're generated, not file-based
	if (playlistName == "all" ||
		playlistName == "lastplayed" ||
		playlistName == "favorites" ||
		playlistName == "settings" ||
		playlistName == "quicklist") {
		return;
	}

	// Favorites might be global, handle specially
	std::string playlistCollectionName = collection->name;
	bool globalFavLast = false;
	config_.getProperty(OPTION_GLOBALFAVLAST, globalFavLast);
	if (globalFavLast && playlistName == "favorites") {
		playlistCollectionName = "Favorites";
	}

	std::string playlistFile = Utils::combinePath(Configuration::absolutePath,
		"collections", playlistCollectionName, "playlists", playlistName + ".txt");

	// If the file doesn't exist, nothing to reload
	if (!std::filesystem::exists(playlistFile)) {
		return;
	}

	LOG_INFO("Page", "Reloading playlist: " + playlistName);

	// Clear existing playlist (but don't delete if it points to items vector)
	if (collection->playlists[playlistName] &&
		collection->playlists[playlistName] != &collection->items) {
		collection->playlists[playlistName]->clear();
	}
	else {
		collection->playlists[playlistName] = new std::vector<Item*>();
	}

	// Load the playlist file into a temporary filter
	std::vector<Item*> playlistFilter;
	std::ifstream includeStream(playlistFile);

	if (!includeStream.good()) {
		LOG_WARNING("Page", "Could not read playlist file: " + playlistFile);
		return;
	}

	std::string line;
	while (std::getline(includeStream, line)) {
		line = Utils::filterComments(line);
		if (!line.empty()) {
			auto* filterItem = new Item();
			line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
			filterItem->fullTitle = line;
			filterItem->name = line;
			filterItem->title = line;
			filterItem->collectionInfo = collection;
			playlistFilter.push_back(filterItem);
		}
	}
	includeStream.close();

	// Rebuild playlist with actual items from collection
	for (Item* pfItem : playlistFilter) {
		std::string collectionName = collection->name;
		std::string itemName = pfItem->name;

		if (!itemName.empty() && itemName.at(0) == '_') {
			itemName.erase(0, 1);
			size_t colonPos = itemName.find(":");
			if (colonPos != std::string::npos) {
				collectionName = itemName.substr(0, colonPos);
				itemName = itemName.erase(0, colonPos + 1);
			}
		}

		for (Item* item : collection->items) {
			if ((item->name == itemName || itemName == "*") &&
				item->collectionInfo->name == collectionName) {

				// Update favorite status
				if (playlistName == "favorites") {
					item->isFavorite = true;
				}

				collection->playlists[playlistName]->push_back(item);

				if (itemName != "*") {
					break;
				}
			}
		}
	}

	// Cleanup temporary filter items
	for (Item* item : playlistFilter) {
		delete item;
	}
	playlistFilter.clear();

	collection->sortPlaylistByName(playlistName);

	LOG_INFO("Page", "Playlist '" + playlistName + "' reloaded with " +
		std::to_string(collection->playlists[playlistName]->size()) + " items");
}

void Page::reloadPlaylistIfDirty(const std::string& playlistName, CollectionInfo* collection) {
	if (!collection || playlistName.empty()) return;

	// Skip dynamic/system lists
	if (playlistName == "all" || playlistName == "lastplayed") return;

	const std::string& colName = collection->name;

	if (!PlaylistDirtyRegistry::isDirty(colName, playlistName)) {
		return; // nothing to do
	}

	LOG_INFO("Page", "Reloading dirty playlist: " + playlistName);
	reloadPlaylistFromDisk(playlistName, collection);

	// Clear only this one
	PlaylistDirtyRegistry::clearOne(colName, playlistName);
}
void Page::updatePlaylistMenuPosition() {
	if (playlistMenu_) {
		std::string name = getPlaylistName();
		if (name != "") {
			playlistMenu_->selectItemByName(name);
		}
	}
}

void Page::nextCyclePlaylist(const std::vector<std::string>& list) {
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

void Page::prevCyclePlaylist(const std::vector<std::string>& list) {
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
	const MenuInfo_S& info = collections_.back();
	const CollectionInfo::Playlists_T& playlists =
		info.collection->playlists;
	const auto found = playlists.find(playlist);

	// playlist exists in cycle and contains items
	return found != playlists.end() &&
		found->second &&
		!found->second->empty();
}


void Page::update(float dt) {
	const std::string& playlistName = getPlaylistName();
	bool playlistNameChanged = false;

	if (playlistName != lastPlaylistName_) {
		lastPlaylistName_ = playlistName;
		playlistNameChanged = true;
	}

	prepareGraphicsByLayer_();

	// Update page-level components first.
	// This allows standalone <video> components to begin prerolling
	// before videos belonging to menu items.
	bool graphicsIdle = true;
	bool graphicsAttractIdle = true;

	for (auto& layer : LayerComponents_) {
		for (auto it = layer.begin(); it != layer.end();) {
			Component* component = *it;

			if (!component) {
				++it;
				continue;
			}

			if (playlistNameChanged) {
				component->playlistName = lastPlaylistName_;
			}

			const bool done = component->update(dt);

			if (!component->isIdle()) {
				graphicsIdle = false;
			}

			if (!component->isAttractIdle()) {
				graphicsAttractIdle = false;
			}

			if (done && component->getAnimationDoneRemove()) {
				component->freeGraphicsMemory();
				delete component;
				it = layer.erase(it);
			}
			else {
				++it;
			}
		}
	}

	// Update menu components after page-level components.
	bool menuIdle = true;
	bool menuAttractIdle = true;

	for (auto& menuList : menus_) {
		for (ScrollingList* menu : menuList) {
			if (!menu) {
				continue;
			}

			if (playlistNameChanged) {
				menu->playlistName = lastPlaylistName_;
			}

			menu->update(dt);

			if (!menu->isScrollingListIdle()) {
				menuIdle = false;
			}

			if (!menu->isScrollingListAttractIdle()) {
				menuAttractIdle = false;
			}
		}
	}

	// Commit aggregate state only after both component groups update.
	cachedIsMenuIdle_ = menuIdle;
	cachedIsGraphicsIdle_ = graphicsIdle;
	cachedIsIdle_ = menuIdle && graphicsIdle;
	cachedIsAttractIdle_ =
		menuAttractIdle && graphicsAttractIdle;

	if (pendingScrollSelect_ && cachedIsMenuIdle_) {
		pendingScrollSelect_ = false;
		onNewScrollItemSelected();
	}

	// Detect the transition from held scroll input to released input.
	const bool releasedScrollInput =
		wasUserScrollInputActive_ &&
		!userScrollInputActive_;

	if (releasedScrollInput &&
		scrolling_ != ScrollDirectionIdle)
	{
		if (canBeginMenuCoast()) {
			beginMenuCoast();
		}
		else {
			setScrolling(ScrollDirectionIdle);
		}
	}

	wasUserScrollInputActive_ = userScrollInputActive_;

	// Centralized scroll physics.
	if (scrolling_ != ScrollDirectionIdle &&
		cachedIsMenuIdle_)
	{
		const bool forward =
			scrolling_ == ScrollDirectionForward ||
			scrolling_ == ScrollDirectionPlaylistForward;

		const bool playlist =
			scrolling_ == ScrollDirectionPlaylistForward ||
			scrolling_ == ScrollDirectionPlaylistBack;

		if (userScrollInputActive_) {
			scroll(forward, playlist);
			updateScrollPeriod();
		}
		else {
			decelerateScrollPeriod(dt);

			if (!canMenuCoast()) {
				setScrolling(ScrollDirectionIdle);
			}

			if (scrolling_ != ScrollDirectionIdle) {
				scroll(forward, playlist);
			}
		}
	}

	if (textStatusComponent_) {
		std::string status;
		config_.getProperty("status", status);
		textStatusComponent_->setText(status);
	}

	pumpPresentationPreload_(dt);

	// Capture the post-animation layer state once for drawing this frame
	// and for preparation at the beginning of the next frame.
	rebuildFrameLayerBuckets_();
}

void Page::collectPresentationPreloads(
	const PresentationPreloadContext& context,
	PresentationPreloadCollector& collector) const
{
	// Match graphics preparation policy: describe higher-layer resources
	// first so overlays are warm before lower-layer content.
	for (auto layer = LayerComponents_.rbegin();
		layer != LayerComponents_.rend();
		++layer)
	{
		for (Component* component : *layer) {
			if (component) {
				component->collectPresentationPreloads(
					context,
					collector
				);
			}
		}
	}
}

bool Page::presentationPreloadEnabled_() const {
	ScrollingList* master = getMasterMenuForScroll(false);
	if (!master) {
		return false;
	}

	const auto& items = master->getItems();
	for (ScrollingList* menu : activeMenu_) {
		if (menu && !menu->isPlaylist() &&
			menu->enablesPresentationPreload() &&
			menu->presentsItems(items))
		{
			return true;
		}
	}

	for (const auto& layer : LayerComponents_) {
		for (const Component* component : layer) {
			if (component &&
				component->enablesPresentationImagePreload())
			{
				return true;
			}
		}
	}

	return false;
}

void Page::rebuildPresentationLetterAnchors_() {
	presentationLetterAnchors_.clear();

	ScrollingList* master = getMasterMenuForScroll(false);
	if (!master || master->getItems().empty()) {
		return;
	}

	bool haveGroup = false;
	int previousGroup = 0;
	const auto& items = master->getItems();

	for (size_t index = 0; index < items.size(); ++index) {
		const Item* item = items[index];
		if (!item || item->fullTitle.empty()) {
			continue;
		}

		const unsigned char first =
			static_cast<unsigned char>(item->fullTitle.front());
		const int group = std::isalpha(first)
			? 1 + std::tolower(first)
			: 0;

		if (!haveGroup || group != previousGroup) {
			presentationLetterAnchors_.push_back(index);
			previousGroup = group;
			haveGroup = true;
		}
	}
}

void Page::refreshPresentationPreloadQueue_(
	bool directionKnown,
	bool forward)
{
	constexpr size_t MAX_PRELOAD_CANDIDATES = 192;
	constexpr size_t LETTER_ANCHORS_EACH_WAY = 2;

	presentationPreloadQueue_.clear();

	ScrollingList* master = getMasterMenuForScroll(false);
	if (!presentationPreloadEnabled_() ||
		!master || master->getItems().empty() ||
		getPlaylistName().empty())
	{
		return;
	}

	beginPresentationPreloadTelemetry_();

	const size_t itemCount = master->getItems().size();
	const auto& items = master->getItems();
	const size_t selected = master->getSelectedIndex();
	size_t maxSlotCount = 1;

	for (ScrollingList* menu : activeMenu_) {
		if (menu && !menu->isPlaylist() &&
			menu->enablesPresentationPreload() &&
			menu->presentsItems(items))
		{
			maxSlotCount = std::max(
				maxSlotCount,
				menu->getSlotCount()
			);
		}
	}

	presentationPreloadQueued_.clear();
	if (presentationPreloadQueued_.bucket_count() <
		MAX_PRELOAD_CANDIDATES)
	{
		presentationPreloadQueued_.reserve(
			MAX_PRELOAD_CANDIDATES
		);
	}

	auto increment = [itemCount](size_t value, size_t amount) {
		return itemCount == 0
			? size_t{ 0 }
			: (value + amount % itemCount) % itemCount;
	};
	auto decrement = [itemCount](size_t value, size_t amount) {
		if (itemCount == 0) {
			return size_t{ 0 };
		}
		const size_t distance = amount % itemCount;
		return (value + itemCount - distance) % itemCount;
	};
	auto addCandidate = [&](size_t index, bool idleOnly) {
		if (presentationPreloadQueue_.size() >=
				MAX_PRELOAD_CANDIDATES ||
			presentationPreloadAttempted_.find(index) !=
				presentationPreloadAttempted_.end() ||
			!presentationPreloadQueued_.insert(index).second)
		{
			return;
		}

		presentationPreloadQueue_.push_back({
			index,
			idleOnly
		});
	};

	// The current destination is the first complete presentation state.
	addCandidate(selected, false);

	size_t highPriorityRadius = 0;
	if (directionKnown) {
		const size_t ahead =
			std::max(maxSlotCount * 4, size_t{ 24 });
		const size_t behind =
			std::max(maxSlotCount * 2, size_t{ 12 });
		highPriorityRadius = std::max(ahead, behind);

		for (size_t distance = 1;
			distance <= highPriorityRadius;
			++distance)
		{
			if (distance <= ahead) {
				addCandidate(
					forward
						? increment(selected, distance)
						: decrement(selected, distance),
					false
				);
			}
			if (distance <= behind) {
				addCandidate(
					forward
						? decrement(selected, distance)
						: increment(selected, distance),
					false
				);
			}
		}
	}
	else {
		highPriorityRadius =
			std::max(maxSlotCount * 3, size_t{ 16 });

		for (size_t distance = 1;
			distance <= highPriorityRadius;
			++distance)
		{
			addCandidate(
				increment(selected, distance),
				false
			);
			addCandidate(
				decrement(selected, distance),
				false
			);
		}
	}

	if (presentationLetterAnchors_.size() > 1) {
		const auto afterCurrent = std::upper_bound(
			presentationLetterAnchors_.begin(),
			presentationLetterAnchors_.end(),
			selected
		);
		const size_t currentGroup =
			afterCurrent == presentationLetterAnchors_.begin()
			? presentationLetterAnchors_.size() - 1
			: static_cast<size_t>(
				std::distance(
					presentationLetterAnchors_.begin(),
					afterCurrent
				) - 1
			);

		for (size_t step = 1;
			step <= LETTER_ANCHORS_EACH_WAY;
			++step)
		{
			addCandidate(
				presentationLetterAnchors_[
					(currentGroup + step) %
					presentationLetterAnchors_.size()
				],
				false
			);
			addCandidate(
				presentationLetterAnchors_[
					(currentGroup +
						presentationLetterAnchors_.size() -
						step % presentationLetterAnchors_.size()) %
					presentationLetterAnchors_.size()
				],
				false
			);
		}
	}

	for (size_t distance = 1;
		distance < itemCount &&
		presentationPreloadQueue_.size() <
			MAX_PRELOAD_CANDIDATES;
		++distance)
	{
		addCandidate(
			increment(selected, distance),
			true
		);
		addCandidate(
			decrement(selected, distance),
			true
		);
	}
}

void Page::queuePresentationState_(size_t selectedIndex) {
	ScrollingList* master = getMasterMenuForScroll(false);
	if (!master || master->getItems().empty()) {
		return;
	}

	const auto& items = master->getItems();
	selectedIndex %= items.size();

	PresentationPreloadCollector collector;
	PresentationPreloadContext context{
		&items,
		selectedIndex
	};

	// Reloadable images opt in independently with useTextureCache,
	// while page text components continue to warm shared layouts.
	collectPresentationPreloads(context, collector);
	presentationPreloadStats_.pageImages +=
		collector.images.size();
	presentationPreloadStats_.pageTexts +=
		collector.texts.size();

	for (ScrollingList* menu : activeMenu_) {
		if (!menu || menu->isPlaylist() ||
			!menu->enablesPresentationPreload() ||
			!menu->presentsItems(items))
		{
			continue;
		}

		const PresentationPreloadContribution contribution =
			menu->collectPresentationPreloadsForSelection(
				selectedIndex,
				collector
			);
		presentationPreloadStats_.listImages +=
			contribution.listImages;
		presentationPreloadStats_.textFallbacks +=
			contribution.textFallbacks;
		presentationPreloadStats_.videosSkipped +=
			contribution.videosSkipped;
	}

	auto higherLayerFirst = [](const auto& lhs, const auto& rhs) {
		return lhs.layer > rhs.layer;
	};
	std::stable_sort(
		collector.images.begin(),
		collector.images.end(),
		higherLayerFirst
	);
	std::stable_sort(
		collector.texts.begin(),
		collector.texts.end(),
		higherLayerFirst
	);

	for (auto& request : collector.images) {
		const std::string key = presentationImageKey(request);
		if (!presentationImagesScheduled_.insert(key).second) {
			++presentationPreloadStats_.imageDuplicates;
			continue;
		}

		++presentationPreloadStats_.imagesScheduled;
		if (Image::isTextureCached(
			request.path,
			request.monitor))
		{
			++presentationPreloadStats_.imageTextureHits;
			continue;
		}

		presentationImageQueue_.push_back(
			std::move(request)
		);
	}

	for (auto& request : collector.texts) {
		const std::string key = presentationTextKey(request);
		if (!presentationTextsScheduled_.insert(key).second) {
			++presentationPreloadStats_.textDuplicates;
			continue;
		}

		++presentationPreloadStats_.textsScheduled;
		presentationTextQueue_.push_back(
			std::move(request)
		);
	}

	++presentationPreloadStats_.statesExamined;
	logPresentationPreloadProgress_();
}

void Page::pumpPresentationImageLanes_(bool allowFinalization) {
	// Complete no more than one worker result per frame. Image finalization
	// creates renderer-owned textures and must remain on the main thread.
	if (allowFinalization) {
		for (size_t offset = 0;
			offset < presentationImagePreloads_.size();
			++offset)
		{
			const size_t index =
				(presentationImageFinalizeCursor_ + offset) %
				presentationImagePreloads_.size();
			auto& preload = presentationImagePreloads_[index];
			if (!preload) {
				continue;
			}

			if (preload->isGraphicsReadyForFirstRender()) {
				preload.reset();
				continue;
			}

			if (preload->isDecodeReadyForFinalization()) {
				preload->pumpGraphicsPreparation();
				if (preload->isGraphicsReadyForFirstRender()) {
					preload.reset();
				}
				presentationImageFinalizeCursor_ =
					(index + 1) %
					presentationImagePreloads_.size();
				break;
			}
		}
	}

	// Keep both CPU decode slots occupied. Cache hits are released
	// immediately because they require neither decoding nor uploading.
	while (!presentationImageQueue_.empty()) {
		auto freeLane = std::find(
			presentationImagePreloads_.begin(),
			presentationImagePreloads_.end(),
			nullptr
		);
		if (freeLane == presentationImagePreloads_.end()) {
			break;
		}

		PresentationImageRequest request =
			std::move(presentationImageQueue_.front());
		presentationImageQueue_.pop_front();

		auto preload = std::make_shared<Image>(
			request.path,
			"",
			*this,
			request.monitor,
			request.additive,
			true
		);
		preload->allocateGraphicsMemory();

		switch (preload->getLoadSource()) {
		case Image::LoadSource::TextureCache:
			++presentationPreloadStats_.imageTextureHits;
			break;
		case Image::LoadSource::SharedInFlight:
			++presentationPreloadStats_.imageInflightJoins;
			break;
		case Image::LoadSource::NewDecode:
			++presentationPreloadStats_.imageDecodesStarted;
			break;
		case Image::LoadSource::None:
			break;
		}

		if (!preload->isGraphicsReadyForFirstRender()) {
			*freeLane = std::move(preload);
		}
	}
}

size_t Page::activePresentationImagePreloads_() const {
	return static_cast<size_t>(std::count_if(
		presentationImagePreloads_.begin(),
		presentationImagePreloads_.end(),
		[](const std::shared_ptr<Image>& preload) {
			return static_cast<bool>(preload);
		}
	));
}

void Page::pumpPresentationPreload_(float dt) {
	constexpr unsigned int MAX_STATES_PER_FRAME = 4;
	constexpr unsigned int MAX_TEXT_LAYOUTS_PER_FRAME = 8;
	constexpr float IDLE_SETTLE_SECONDS = 0.5f;

	if (!presentationPreloadEnabled_()) {
		return;
	}
	if (getPlaylistName().empty()) {
		return;
	}

	// List contents and slot geometry can change independently of the Page
	// navigation entry points. Invalidation clears the old plan; restart it
	// lazily once the complete new presentation is available.
	if (!presentationPreloadTelemetryStarted_) {
		rebuildPresentationLetterAnchors_();
		refreshPresentationPreloadQueue_();
		if (!presentationPreloadTelemetryStarted_) {
			return;
		}
	}

	if (cachedIsIdle_ &&
		scrolling_ == ScrollDirectionIdle)
	{
		presentationIdleStableSeconds_ +=
			std::max(dt, 0.0f);
	}
	else {
		presentationIdleStableSeconds_ = 0.0f;
	}

	auto pumpTextLayouts = [&]() {
		for (unsigned int pass = 0;
			pass < MAX_TEXT_LAYOUTS_PER_FRAME &&
			!presentationTextQueue_.empty();
			++pass)
		{
			PresentationTextRequest request =
				std::move(presentationTextQueue_.front());
			presentationTextQueue_.pop_front();

			if (!request.font) {
				continue;
			}

			bool cacheHit = false;
			const auto layout = request.font->getTextLayout(
				request.text,
				request.fontSize,
				&cacheHit
			);
			if (!layout) {
				continue;
			}

			if (cacheHit) {
				++presentationPreloadStats_.textLayoutHits;
			}
			else {
				++presentationPreloadStats_.textLayoutsBuilt;
			}
		}
	};

	pumpPresentationImageLanes_(true);
	pumpTextLayouts();
	if (activePresentationImagePreloads_() >=
			PRESENTATION_IMAGE_DECODE_LANES ||
		!presentationImageQueue_.empty())
	{
		return;
	}

	const bool allowIdleWork =
		presentationIdleStableSeconds_ >=
		IDLE_SETTLE_SECONDS;

	for (unsigned int pass = 0;
		pass < MAX_STATES_PER_FRAME;
		++pass)
	{
		if (presentationPreloadQueue_.empty() &&
			allowIdleWork &&
			!presentationIdleSweepComplete_)
		{
			ScrollingList* master =
				getMasterMenuForScroll(false);
			if (!master || master->getItems().empty()) {
				break;
			}

			const size_t itemCount =
				master->getItems().size();
			bool foundCandidate = false;
			for (size_t checked = 0;
				checked < itemCount;
				++checked)
			{
				const size_t index =
					presentationIdleCursor_++ % itemCount;
				if (presentationPreloadAttempted_.find(index) ==
					presentationPreloadAttempted_.end())
				{
					presentationPreloadQueue_.push_back({
						index,
						true
					});
					foundCandidate = true;
					break;
				}
			}

			if (!foundCandidate) {
				presentationIdleSweepComplete_ = true;
				completePresentationPreloadTelemetry_();
			}
		}

		if (presentationPreloadQueue_.empty()) {
			break;
		}

		const PresentationPreloadCandidate candidate =
			presentationPreloadQueue_.front();
		if (candidate.idleOnly && !allowIdleWork) {
			break;
		}
		presentationPreloadQueue_.pop_front();

		if (!presentationPreloadAttempted_.insert(
			candidate.selectedIndex).second)
		{
			continue;
		}

		queuePresentationState_(candidate.selectedIndex);
		if (!presentationImageQueue_.empty() ||
			!presentationTextQueue_.empty())
		{
			break;
		}
	}

	pumpTextLayouts();
	// This second call can start work discovered above, but texture
	// finalization remains limited to the first call of the frame.
	pumpPresentationImageLanes_(false);
}

void Page::releasePresentationImagePreloads_() {
	for (auto& preload : presentationImagePreloads_) {
		preload.reset();
	}
	presentationImageFinalizeCursor_ = 0;
}

void Page::resetPresentationPreload_() {
	if (presentationPreloadTelemetryStarted_ &&
		!presentationPreloadTelemetryComplete_)
	{
		LOG_INFO(
			"PresentationPreload",
			"stopped playlist='"
			<< presentationPreloadPlaylist_
			<< "' states="
			<< presentationPreloadStats_.statesExamined
			<< "/" << presentationPreloadStats_.totalStates
			<< " images(cache="
			<< presentationPreloadStats_.imageTextureHits
			<< ", joined="
			<< presentationPreloadStats_.imageInflightJoins
			<< ", decode="
			<< presentationPreloadStats_.imageDecodesStarted
			<< ") text(hit="
			<< presentationPreloadStats_.textLayoutHits
			<< ", built="
			<< presentationPreloadStats_.textLayoutsBuilt
			<< ")"
		);
	}

	releasePresentationImagePreloads_();
	presentationPreloadQueue_.clear();
	presentationPreloadAttempted_.clear();
	presentationPreloadQueued_.clear();
	presentationImageQueue_.clear();
	presentationTextQueue_.clear();
	presentationImagesScheduled_.clear();
	presentationTextsScheduled_.clear();
	presentationLetterAnchors_.clear();
	presentationIdleCursor_ = 0;
	presentationIdleStableSeconds_ = 0.0f;
	presentationIdleSweepComplete_ = false;
	presentationPreloadStats_ = {};
	presentationPreloadPlaylist_.clear();
	presentationPreloadNextProgress_ = 64;
	presentationPreloadTelemetryStarted_ = false;
	presentationPreloadTelemetryComplete_ = false;
}

void Page::beginPresentationPreloadTelemetry_() {
	if (presentationPreloadTelemetryStarted_) {
		return;
	}

	ScrollingList* master = getMasterMenuForScroll(false);
	if (!master) {
		return;
	}

	presentationPreloadStats_ = {};
	presentationPreloadStats_.totalStates =
		master->getItems().size();
	const auto& items = master->getItems();
	for (ScrollingList* menu : activeMenu_) {
		if (menu && !menu->isPlaylist() &&
			menu->enablesPresentationPreload() &&
			menu->presentsItems(items))
		{
			++presentationPreloadStats_.contributingLists;
		}
	}

	presentationPreloadPlaylist_ = getPlaylistName();
	presentationPreloadNextProgress_ = 64;
	presentationPreloadTelemetryStarted_ = true;
	presentationPreloadTelemetryComplete_ = false;

	LOG_INFO(
		"PresentationPreload",
		"started playlist='"
		<< presentationPreloadPlaylist_
		<< "' states="
		<< presentationPreloadStats_.totalStates
		<< " lists="
		<< presentationPreloadStats_.contributingLists
		<< " decodeLanes="
		<< PRESENTATION_IMAGE_DECODE_LANES
		<< " selected="
		<< master->getSelectedIndex()
	);
}

void Page::logPresentationPreloadProgress_() {
	if (!presentationPreloadTelemetryStarted_ ||
		presentationPreloadTelemetryComplete_ ||
		presentationPreloadStats_.statesExamined <
			presentationPreloadNextProgress_)
	{
		return;
	}

	LOG_DEBUG(
		"PresentationPreload",
		"progress playlist='"
		<< presentationPreloadPlaylist_
		<< "' states="
		<< presentationPreloadStats_.statesExamined
		<< "/" << presentationPreloadStats_.totalStates
		<< " images(cache="
		<< presentationPreloadStats_.imageTextureHits
		<< ", joined="
		<< presentationPreloadStats_.imageInflightJoins
		<< ", decode="
		<< presentationPreloadStats_.imageDecodesStarted
		<< ", pending="
		<< presentationImageQueue_.size()
		<< ", active="
		<< activePresentationImagePreloads_()
		<< ") text(hit="
		<< presentationPreloadStats_.textLayoutHits
		<< ", built="
		<< presentationPreloadStats_.textLayoutsBuilt
		<< ", pending="
		<< presentationTextQueue_.size()
		<< ") videoSkipped="
		<< presentationPreloadStats_.videosSkipped
	);

	presentationPreloadNextProgress_ =
		(presentationPreloadStats_.statesExamined /
			64 + 1) * 64;
}

void Page::completePresentationPreloadTelemetry_() {
	if (!presentationPreloadTelemetryStarted_ ||
		presentationPreloadTelemetryComplete_)
	{
		return;
	}

	presentationPreloadTelemetryComplete_ = true;
	LOG_INFO(
		"PresentationPreload",
		"complete playlist='"
		<< presentationPreloadPlaylist_
		<< "' states="
		<< presentationPreloadStats_.statesExamined
		<< "/" << presentationPreloadStats_.totalStates
		<< " lists="
		<< presentationPreloadStats_.contributingLists
		<< " described(pageImages="
		<< presentationPreloadStats_.pageImages
		<< ", pageTexts="
		<< presentationPreloadStats_.pageTexts
		<< ", listImages="
		<< presentationPreloadStats_.listImages
		<< ", textFallbacks="
		<< presentationPreloadStats_.textFallbacks
		<< ", videoSkipped="
		<< presentationPreloadStats_.videosSkipped
		<< ") images(unique="
		<< presentationPreloadStats_.imagesScheduled
		<< ", duplicate="
		<< presentationPreloadStats_.imageDuplicates
		<< ", cache="
		<< presentationPreloadStats_.imageTextureHits
		<< ", joined="
		<< presentationPreloadStats_.imageInflightJoins
		<< ", decode="
		<< presentationPreloadStats_.imageDecodesStarted
		<< ") text(unique="
		<< presentationPreloadStats_.textsScheduled
		<< ", duplicate="
		<< presentationPreloadStats_.textDuplicates
		<< ", hit="
		<< presentationPreloadStats_.textLayoutHits
		<< ", built="
		<< presentationPreloadStats_.textLayoutsBuilt
		<< ")"
	);
}

void Page::updateReloadables(float dt) {
	for (auto& layer : LayerComponents_) {
		for (Component* component : layer) {
			if (component) {
				component->update(dt);
			}
		}
	}

	rebuildFrameLayerBuckets_();
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
	if (!frameLayerBucketsValid_) {
		rebuildFrameLayerBuckets_();
	}

	for (unsigned int i = 0; i < NUM_LAYERS; ++i) {
		for (Component* c : pageDrawLayers_[i]) {
			if (c && c->baseViewInfo.Monitor == monitor) {
				c->draw();
			}
		}

		for (Component* c : menuDrawLayers_[i]) {
			if (c && c->baseViewInfo.Monitor == monitor) {
				c->draw();
			}
		}
	}
}

void Page::invalidateFrameLayerBuckets_() {
	frameLayerBucketsValid_ = false;
}

void Page::rebuildFrameLayerBuckets_() {
	for (auto& tier : preparationLayers_) {
		for (auto& layer : tier) {
			layer.clear();
		}
	}
	for (auto& layer : pageDrawLayers_) {
		layer.clear();
	}
	for (auto& layer : menuDrawLayers_) {
		layer.clear();
	}

	auto queueComponent = [this](
		Component* component,
		std::array<std::vector<Component*>, NUM_LAYERS>& drawLayers,
		bool queueForPreparation)
	{
		if (!component) {
			return;
		}

		const unsigned int layer = component->baseViewInfo.Layer;
		if (layer >= NUM_LAYERS) {
			return;
		}

		const unsigned int tier =
			graphicsPreparationTier(*component);

		drawLayers[layer].push_back(component);
		if (queueForPreparation) {
			preparationLayers_[tier][layer].push_back(component);
		}
	};

	// Preserve page-component XML order within each live layer.
	for (const auto& ownershipLayer : LayerComponents_) {
		for (Component* component : ownershipLayer) {
			queueComponent(component, pageDrawLayers_, true);
		}
	}

	// Menu children draw after page components on equal layers. Retained
	// menu depths still draw as before, but only active depths prepare.
	size_t menuDepth = 0;
	for (const auto& menuList : menus_) {
		const bool activeDepth = menuDepth < menuDepth_;
		for (ScrollingList* menu : menuList) {
			if (!menu) {
				continue;
			}

			for (Component* component : menu->getComponents()) {
				queueComponent(
					component,
					menuDrawLayers_,
					activeDepth
				);
			}
		}
		++menuDepth;
	}

	frameLayerBucketsValid_ = true;
}

void Page::prepareGraphicsByLayer_() {
	if (!frameLayerBucketsValid_) {
		rebuildFrameLayerBuckets_();
	}

	for (unsigned int tier = 0;
		tier < NUM_PREPARATION_TIERS;
		++tier)
	{
		for (int layer = static_cast<int>(NUM_LAYERS) - 1;
			layer >= 0;
			--layer)
		{
			auto& components = preparationLayers_[tier][layer];

			// Reverse draw order: topmost menu children prepare first,
			// followed by page components on the same layer.
			for (auto component = components.rbegin();
				component != components.rend();
				++component)
			{
				(*component)->pumpGraphicsPreparation();
			}
		}
	}
}

void Page::pumpGraphicsPreparation() {
	prepareGraphicsByLayer_();
}

void Page::waitForGraphicsPreparation() {
	for (const auto& layerComponents : LayerComponents_) {
		for (Component* component : layerComponents) {
			if (component) {
				component->waitForGraphicsPreparation();
			}
		}
	}

	int currentDepth = 0;
	for (const auto& menuList : menus_) {
		if (currentDepth < static_cast<int>(menuDepth_)) {
			for (ScrollingList* menu : menuList) {
				if (menu) {
					menu->waitForGraphicsPreparation();
				}
			}
		}
		++currentDepth;
	}
}

bool Page::isGraphicsReadyForFirstRender() const {
	for (const auto& layerComponents : LayerComponents_) {
		for (Component* component : layerComponents) {
			if (component && !component->isGraphicsReadyForFirstRender()) {
				return false;
			}
		}
	}

	int currentDepth = 0;
	for (auto const& menuList : menus_) {
		if (currentDepth < static_cast<int>(menuDepth_)) {
			for (auto& menu : menuList) {
				if (menu && !menu->isGraphicsReadyForFirstRender()) {
					return false;
				}
			}
		}
		++currentDepth;
	}

	return true;
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

void Page::consumeDirtyPlaylistsForActiveCollection() {
	CollectionInfo* c = getCollection();
	if (!c) return;

	const std::string current = getPlaylistName();

	auto dirtyList = PlaylistDirtyRegistry::drainForCollection(c->name);
	if (dirtyList.empty()) return;

	for (const auto& pname : dirtyList) {
		// Optionally skip the currently viewed playlist
		if (pname == current) {
			LOG_INFO("Page", "Dirty playlist is active, deferring: " + pname);
			PlaylistDirtyRegistry::addPath("collections/" + c->name + "/playlists/" + pname + ".txt");
			continue;
		}
		// Reuse your existing loader
		reloadPlaylistFromDisk(pname, c);
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
	invalidateFrameLayerBuckets_();
	resetPresentationPreload_();

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
	invalidateFrameLayerBuckets_();

	struct AllocationCandidate {
		unsigned int tier;
		unsigned int layer;
		Component* component;
		ScrollingList* menu;
	};

	std::vector<AllocationCandidate> candidates;

	for (unsigned int layer = 0; layer < NUM_LAYERS; ++layer) {
		const auto& components = LayerComponents_[layer];

		for (auto component = components.rbegin();
			component != components.rend();
			++component)
		{
			if (*component) {
				candidates.push_back(
					{
						graphicsPreparationTier(**component),
						(*component)->baseViewInfo.Layer,
						*component,
						nullptr
					}
				);
			}
		}
	}

	int currentDepth = 0;
	for (const auto& menuList : menus_) {
		if (currentDepth < static_cast<int>(menuDepth_)) {
			for (auto menu = menuList.rbegin();
				menu != menuList.rend();
				++menu)
			{
				if (*menu) {
					candidates.push_back(
						{
							(*menu)->getVisualPriorityTier(),
							(*menu)->getVisualPriorityLayer(),
							nullptr,
							*menu
						}
					);
				}
			}
		}
		++currentDepth;
	}

	std::stable_sort(
		candidates.begin(),
		candidates.end(),
		[](const AllocationCandidate& lhs,
			const AllocationCandidate& rhs)
		{
			if (lhs.tier != rhs.tier) {
				return lhs.tier < rhs.tier;
			}

			if (lhs.layer != rhs.layer) {
				return lhs.layer > rhs.layer;
			}

			// Menu children draw after page components on equal layers.
			return lhs.menu && !rhs.menu;
		}
	);

	for (const AllocationCandidate& candidate : candidates) {
		if (candidate.menu) {
			candidate.menu->allocateGraphicsMemory();
		}
		else {
			candidate.component->allocateGraphicsMemory();
		}
	}

	if (loadSoundChunk_) loadSoundChunk_->allocate();
	if (unloadSoundChunk_) unloadSoundChunk_->allocate();
	if (highlightSoundChunk_) highlightSoundChunk_->allocate();
	if (selectSoundChunk_) selectSoundChunk_->allocate();

	rebuildPresentationLetterAnchors_();
	refreshPresentationPreloadQueue_();

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


void Page::allocateMenuSpritePoints(bool updatePlaylistMenu) {
	invalidateFrameLayerBuckets_();

	for (ScrollingList* menu :
		menusByVisualPriority(activeMenu_))
	{
		if (!menu->isPlaylist() || updatePlaylistMenu) {
			menu->allocateSpritePoints();
		}
	}
}


void Page::reallocateMenuSpritePoints(bool updatePlaylistMenu) {
	invalidateFrameLayerBuckets_();

	for (ScrollingList* menu :
		menusByVisualPriority(activeMenu_))
	{
		if (!menu->isPlaylist() || updatePlaylistMenu) {
			menu->reallocateSpritePoints();
		}
	}
}


bool Page::isMenuScrolling() const {
	return scrollActive_;
}

bool Page::isUserScrollInputActive() const
{
	return userScrollInputActive_;
}

void Page::setUserScrollInputActive(bool active)
{
	userScrollInputActive_ = active;
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
	const bool playlist =
		scrolling_ == ScrollDirectionPlaylistForward ||
		scrolling_ == ScrollDirectionPlaylistBack;

	if (ScrollingList* master = getMasterMenuForScroll(playlist)) {
		master->resetScrollPeriod();
	}
}

void Page::decelerateScrollPeriod(float dt) {
	const bool playlist =
		scrolling_ == ScrollDirectionPlaylistForward ||
		scrolling_ == ScrollDirectionPlaylistBack;

	if (ScrollingList* master = getMasterMenuForScroll(playlist)) {
		master->decelerateScrollPeriod(dt);
	}
}

bool Page::canMenuCoast() const {
	const bool playlist =
		scrolling_ == ScrollDirectionPlaylistForward ||
		scrolling_ == ScrollDirectionPlaylistBack;

	if (ScrollingList* master = getMasterMenuForScroll(playlist)) {
		return master->canCoast();
	}

	return false;
}

bool Page::canBeginMenuCoast() const {
	const bool playlist =
		scrolling_ == ScrollDirectionPlaylistForward ||
		scrolling_ == ScrollDirectionPlaylistBack;

	if (ScrollingList* master = getMasterMenuForScroll(playlist)) {
		return master->canBeginCoast();
	}

	return false;
}

void Page::updateScrollPeriod() const {
	const bool playlist =
		scrolling_ == ScrollDirectionPlaylistForward ||
		scrolling_ == ScrollDirectionPlaylistBack;

	if (ScrollingList* master = getMasterMenuForScroll(playlist)) {
		master->updateScrollPeriod();
	}
}

void Page::beginMenuCoast() {
	const bool playlist =
		scrolling_ == ScrollDirectionPlaylistForward ||
		scrolling_ == ScrollDirectionPlaylistBack;

	if (ScrollingList* master = getMasterMenuForScroll(playlist)) {
		master->beginCoast();
	}
}

bool Page::isMenuFastScrolling() const {
	if (scrolling_ == ScrollDirectionIdle)
		return false;

	if (!gameScrollActive_)
		return false;

	ScrollingList* master = getMasterMenuForScroll(false);
	return master && master->isFastScrolling();
}

ScrollingList* Page::getMasterMenuForScroll(bool playlist) const {
	if (playlist) {
		return playlistMenu_;
	}

	for (ScrollingList* menu : activeMenu_) {
		if (menu && !menu->isPlaylist()) {
			return menu;
		}
	}

	return nullptr;
}

void Page::scroll(bool forward, bool playlist) {
	ScrollingList* masterMenu = getMasterMenuForScroll(playlist);
	if (!masterMenu) {
		return;
	}

	const float masterPeriod = masterMenu->getScrollPeriod();
	const size_t newSelectedIndex = masterMenu->nextSelectedIndex(forward);

	for (ScrollingList* menu : activeMenu_) {
		if (!menu) {
			continue;
		}

		const bool correctType =
			(playlist && menu->isPlaylist()) ||
			(!playlist && !menu->isPlaylist());

		if (!correctType) {
			continue;
		}

		menu->scrollToSelectedIndex(newSelectedIndex, forward, masterPeriod);
	}

	if (!playlist) {
		refreshPresentationPreloadQueue_(true, forward);
	}

	pendingScrollSelect_ = true;

	if (highlightSoundChunk_) {
		highlightSoundChunk_->play();
	}

	invalidateIdleCache();
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
	if (monitor >= 0 && static_cast<std::size_t>(monitor) < layoutWidthByMonitor_.size())
		return layoutWidthByMonitor_[monitor];
	return 0;
}


int Page::getLayoutHeightByMonitor(int monitor) {
	if (monitor >= 0 && static_cast<std::size_t>(monitor) < layoutHeightByMonitor_.size())
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

bool Page::getIsLaunched() const {
	return isLaunched_;
}

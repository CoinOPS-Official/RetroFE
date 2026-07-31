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

#include "../Collection/CollectionInfo.h"
#include "PresentationPreload.h"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <list>
#include <unordered_set>
#include <vector>

class Component;
class Configuration;
class ScrollingList;
class Text;
class Item;
class Image;
class Sound;

class Page
{

public:

    enum ScrollDirection
    {
        ScrollDirectionForward,
        ScrollDirectionBack,
        ScrollDirectionIdle,
        ScrollDirectionPlaylistForward,
        ScrollDirectionPlaylistBack,
    };

    Page(Configuration &c, int layoutWidth, int layoutHeight );
    virtual ~Page();
    void deInitialize();
    virtual void onNewItemSelected();
    virtual void onNewScrollItemSelected();
    void returnToRememberSelectedItem();
    void rememberSelectedItem();
    std::map<std::string, size_t> getLastPlaylistOffsets() const;
    void setLastPlaylistOffsets(const std::map<std::string, size_t>& offsets);
    CollectionInfo* detachCollection();
    void highlightLoadArt();
    bool pushCollection(CollectionInfo *collection);
    bool popCollection();
    void enterMenu();
    void exitMenu();
    void enterGame();
    void trackChange();
    void exitGame();
    const std::string& getPlaylistName() const;
    void favPlaylist();
    void nextPlaylist();
    void prevPlaylist();
    void selectPlaylist(const std::string& playlist);
    void nextCyclePlaylist(const std::vector<std::string>& list);
    void prevCyclePlaylist(const std::vector<std::string>& list);
    void pushMenu(ScrollingList *s, int index = -1);
    void updatePlaylistMenuPosition();
    bool isMenusFull() const;
    void setLoadSound(Sound *chunk);
    void setUnloadSound(Sound *chunk);
    void setHighlightSound(Sound *chunk);
    void setSelectSound(Sound *chunk);
    Item *getSelectedMenuItem();
    ScrollingList* getAnActiveMenu();
    bool addComponent(Component *c);
    void invalidatePresentationPreload();
    void collectPresentationPreloads(
        const PresentationPreloadContext& context,
        PresentationPreloadCollector& collector) const;
    void pageScroll(ScrollDirection direction);
    void letterScroll(ScrollDirection direction);
    void metaScroll(ScrollDirection direction, std::string attribute);
    void cfwLetterSubScroll(ScrollDirection direction);
    size_t getCollectionSize();
    size_t getSelectedIndex();
    void selectRandom();
    void selectRandomPlaylist(CollectionInfo* collection, std::vector<std::string> cycleVector);
    void start();
    void stop();
    void setCurrentLayout(int layout);
    int getCurrentLayout() const;
    int getLayoutWidthByMonitor(int monitor);
    int getLayoutHeightByMonitor(int monitor);
    void setLayoutWidthByMonitor(int monitor, int width);
    void setLayoutHeightByMonitor(int monitor, int height);
    void setScrolling(ScrollDirection direction);
    bool isHorizontalScroll();
    unsigned int getMenuDepth() const;
    Item *getSelectedItem();
    Item *getSelectedItem(int offset);
    void removeSelectedItem();
    void setScrollOffsetIndex(size_t i);
    size_t getScrollOffsetIndex();
    bool isIdle();
    bool isAttractIdle();
    bool isGraphicsIdle();
    bool isMenuIdle();
    void setStatusTextComponent(Text *t);
    void update(float dt);
    void updateReloadables(float dt);
    void cleanup();
    void draw(int monitor);
    void freeGraphicsMemory();
    void allocateGraphicsMemory();
    void pumpGraphicsPreparation();
    void waitForGraphicsPreparation();
    bool isGraphicsReadyForFirstRender() const;
    void deInitializeFonts( ) const;
    void initializeFonts( ) const;
    void playSelect();
    bool isSelectPlaying();
    void allocateMenuSpritePoints(bool updatePlaylistMenu);
    std::string getCollectionName();
    CollectionInfo *getCollection();
    void  setMinShowTime(float value);
    float getMinShowTime() const;
    std::string controlsType() const;
    void setControlsType(const std::string& type);
    void  menuScroll();
    void playlistScroll();
    void  highlightEnter();
    void  highlightExit();
    void  playlistEnter();
    void  playlistExit();
    void playlistNextEnter();
    void playlistNextExit();
    void playlistPrevEnter();
    void playlistPrevExit();
    void triggerEventOnAllMenus(const std::string& event);
    void  menuJumpEnter();
    void  menuJumpExit();
    void  attractEnter( );
    void  attract( );
    void  attractExit( );
    void gameInfoEnter();
    void gameInfoExit();
    void collectionInfoEnter();
    void collectionInfoExit();
    void buildInfoEnter();
    void buildInfoExit();
    void  jukeboxJump( );
    void  triggerEvent( const std::string& action );
    void  setText( const std::string& text, int id );
    void  addPlaylist();
    void  removePlaylist();
    void  togglePlaylist();
    void consumeDirtyPlaylistsForActiveCollection();
    void  reallocateMenuSpritePoints(bool updatePlaylistMenu = true);
    bool  isMenuScrolling() const;
    bool  isUserScrollInputActive() const;
    void  setUserScrollInputActive(bool active);
    bool isPlaylistScrolling() const;
    bool isGamesScrolling() const;
    bool  isPlaying() const;
    void  resetScrollPeriod() const;
    void decelerateScrollPeriod(float dt);
    bool canMenuCoast() const;
    bool canBeginMenuCoast() const;
    bool wasUserScrollInputActive_ = false;
    void beginMenuCoast();
    void  updateScrollPeriod() const;
    bool  isMenuFastScrolling() const;
    void  scroll(bool forward, bool playlist);
    ScrollDirection getScrolling() const { return scrolling_; }
    bool  hasSubs();
    int   getLayoutWidth(int layout);
    int   getLayoutHeight(int layout);
    void  setLayoutWidth(int layout, int width);
    void  setLayoutHeight(int layout, int height);
    void  setJukebox();
    bool  isJukebox() const;
    bool  isJukeboxPlaying();
    void  skipForward( );
    void  skipBackward( );
    void  skipForwardp( );
    void  skipBackwardp( );
    void  pause( );
    void resume();
    void  restart( );
    void restartAllByMonitor(int monitor);
    unsigned long long getCurrent( );
    unsigned long long getDuration( );
    bool  isPaused( );
    void setLocked(bool locked);
    bool isLocked() const;
    ScrollingList* getPlaylistMenu();
    void setPlaylistMenu(ScrollingList*);
    void setIsLaunched(bool isLaunched);
    bool getIsLaunched() const;
    bool playlistExists(const std::string&);
    void setSelectedItem();
    bool fromPreviousPlaylist = false;
    bool fromPlaylistNav = false;
    static const int MAX_LAYOUTS = 20; // TODO Put this behind a key 

private:
    void playlistChange();
    std::string lastPlaylistName_;
    std::string collectionName_;
    Configuration &config_;
    std::string controlsType_;
    bool locked_;
    int currentLayout_;

    struct MenuInfo_S
    {
        CollectionInfo* collection;
        CollectionInfo::Playlists_T::iterator playlist;
        bool queueDelete;
    };
    using CollectionVector_T = std::list<MenuInfo_S>;
    
    using MenuVector_T = std::vector<std::vector<ScrollingList *>>;
    void reloadPlaylistFromDisk(const std::string& playlistName, CollectionInfo* collection);
    void reloadPlaylistIfDirty(const std::string& playlistName, CollectionInfo* collection);
    void setActiveMenuItemsFromPlaylist(MenuInfo_S info, ScrollingList* menu);

    std::vector<ScrollingList *> activeMenu_;
    ScrollingList* anActiveMenu_;
    ScrollingList* playlistMenu_;
    ScrollingList* getMasterMenuForScroll(bool playlist) const;
    unsigned int menuDepth_;
    MenuVector_T menus_;
    CollectionVector_T collections_;
    CollectionVector_T deleteCollections_;

    static const unsigned int NUM_LAYERS = 20;
    static const unsigned int NUM_PREPARATION_TIERS = 3;
    std::vector<std::vector<Component*>> LayerComponents_; // Grouped by layer
    std::array<
        std::array<std::vector<Component*>, NUM_LAYERS>,
        NUM_PREPARATION_TIERS
    > preparationLayers_;
    std::array<std::vector<Component*>, NUM_LAYERS> pageDrawLayers_;
    std::array<std::vector<Component*>, NUM_LAYERS> menuDrawLayers_;
    bool frameLayerBucketsValid_{ false };
    std::list<ScrollingList *> deleteMenuList_;
    std::list<CollectionInfo *> deleteCollectionList_;
    std::map<std::string, size_t> lastPlaylistOffsets_;

    ScrollDirection scrolling_ = ScrollDirectionIdle;
    bool scrollActive_;
    bool playlistScrollActive_;
    bool gameScrollActive_;
    bool userScrollInputActive_ = false;

    Item *selectedItem_;
    Text *textStatusComponent_;
    Sound *loadSoundChunk_;
    Sound *unloadSoundChunk_;
    Sound *highlightSoundChunk_;
    Sound *selectSoundChunk_;
    float minShowTime_;
    CollectionInfo::Playlists_T::iterator playlist_;
    std::vector<int> layoutWidth_;
    std::vector<int> layoutHeight_;
    std::vector<int> layoutWidthByMonitor_;
    std::vector<int> layoutHeightByMonitor_;
    bool jukebox_;
    bool isLaunched_ = false;

    bool cachedIsIdle_{ true };
    bool cachedIsMenuIdle_{ true };
    bool cachedIsGraphicsIdle_{ true };
    bool cachedIsAttractIdle_{ true };
	bool pendingScrollSelect_{ false };

    void invalidateIdleCache(); // Helper to flag state as active
    void invalidateFrameLayerBuckets_();
    void rebuildFrameLayerBuckets_();
    void prepareGraphicsByLayer_();

    struct PresentationPreloadCandidate {
        size_t selectedIndex = 0;
        bool idleOnly = false;
    };
    struct PresentationPreloadStats {
        size_t totalStates{ 0 };
        size_t statesExamined{ 0 };
        size_t contributingLists{ 0 };
        size_t pageImages{ 0 };
        size_t pageTexts{ 0 };
        size_t listImages{ 0 };
        size_t textFallbacks{ 0 };
        size_t videosSkipped{ 0 };
        size_t imagesScheduled{ 0 };
        size_t imageDuplicates{ 0 };
        size_t imageTextureHits{ 0 };
        size_t imageInflightJoins{ 0 };
        size_t imageDecodesStarted{ 0 };
        size_t textsScheduled{ 0 };
        size_t textDuplicates{ 0 };
        size_t textLayoutHits{ 0 };
        size_t textLayoutsBuilt{ 0 };
    };

    bool presentationPreloadEnabled_() const;
    void resetPresentationPreload_();
    void refreshPresentationPreloadQueue_(
        bool directionKnown = false,
        bool forward = true);
    void rebuildPresentationLetterAnchors_();
    void queuePresentationState_(size_t selectedIndex);
    void pumpPresentationPreload_(float dt);
    void pumpPresentationImageLanes_(bool allowFinalization);
    size_t activePresentationImagePreloads_() const;
    void releasePresentationImagePreloads_();
    void beginPresentationPreloadTelemetry_();
    void logPresentationPreloadProgress_();
    void completePresentationPreloadTelemetry_();

    std::deque<PresentationPreloadCandidate>
        presentationPreloadQueue_;
    std::unordered_set<size_t> presentationPreloadAttempted_;
    std::unordered_set<size_t> presentationPreloadQueued_;
    std::deque<PresentationImageRequest>
        presentationImageQueue_;
    std::deque<PresentationTextRequest>
        presentationTextQueue_;
    std::unordered_set<std::string>
        presentationImagesScheduled_;
    std::unordered_set<std::string>
        presentationTextsScheduled_;
    static constexpr size_t PRESENTATION_IMAGE_DECODE_LANES = 2;
    std::array<std::shared_ptr<Image>,
        PRESENTATION_IMAGE_DECODE_LANES>
        presentationImagePreloads_;
    size_t presentationImageFinalizeCursor_{ 0 };
    std::vector<size_t> presentationLetterAnchors_;
    size_t presentationIdleCursor_{ 0 };
    float presentationIdleStableSeconds_{ 0.0f };
    bool presentationIdleSweepComplete_{ false };
    PresentationPreloadStats presentationPreloadStats_;
    std::string presentationPreloadPlaylist_;
    size_t presentationPreloadNextProgress_{ 64 };
    bool presentationPreloadTelemetryStarted_{ false };
    bool presentationPreloadTelemetryComplete_{ false };

};

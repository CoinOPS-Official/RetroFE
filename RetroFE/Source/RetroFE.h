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


#include "Collection/Item.h"
#include "Control/UserInput.h"
#include "Database/DB.h"
#include "Database/MetadataDatabase.h"
#include "Execute/AttractMode.h"
#include "Graphics/FontCache.h"
#include "Video/IVideo.h"
#include "Video/VideoFactory.h"
#include "Video/GStreamerVideo.h"
#include "Sound/MusicPlayer.h"
#include <SDL2/SDL.h>
#include "Control/Restrictor/Restrictor.h"
#if __has_include(<SDL2/SDL_ttf.h>)
#include <SDL2/SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#include <SDL2_ttf/SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif
#include <list>
#include <stack>
#include <map>
#include <string>
#include <memory>
#include "Graphics/Page.h"
#ifdef WIN32
    #include <Windows.h>
#endif


class CollectionInfo;
class Configuration;
class Page;


class RetroFE
{

public:
    explicit RetroFE( Configuration &c );
    virtual ~RetroFE( );
    bool     deInitialize( );
    bool     run( );
    void     freeGraphicsMemory( );
    void     allocateGraphicsMemory( );
    void     launchEnter( );
    void     launchExit(bool userInitiated = true, bool isRebooting = false);
    std::vector<std::string>     getPlaylistCycle();
    bool getAttractModeCyclePlaylist();
    MetadataDatabase* getMetaDb();
    uint64_t freq_ = SDL_GetPerformanceFrequency();

private:
#ifdef WIN32	
    HWND hwnd;
#endif
    volatile bool initialized;
    volatile bool initializeError;
    SDL_Thread   *initializeThread;
    static int    initialize( void *context );

	double lastFrameTimeMs_ = 0.0;
	double lastFrameTimePointMs_ = 0.0;

    void initializeMusicPlayer();

    //fps counter resources
    std::unique_ptr<Text> fpsOverlayText_;
    TTF_Font* debugFont_ = nullptr;
    SDL_Texture* fpsOverlayTexture_ = nullptr;
    int fpsOverlayW_ = 0;
    int fpsOverlayH_ = 0;
    std::string lastOverlayText_ = "";
    bool showFps_ = false;

    enum RETROFE_STATE
    {
        RETROFE_IDLE,
        RETROFE_LOAD_ART,
        RETROFE_ENTER,
        RETROFE_SPLASH_EXIT,
        RETROFE_PLAYLIST_NEXT,
        RETROFE_PLAYLIST_PREV,
        RETROFE_PLAYLIST_NEXT_CYCLE,
        RETROFE_PLAYLIST_PREV_CYCLE,
        RETROFE_PLAYLIST_REQUEST,
        RETROFE_PLAYLIST_EXIT,
        RETROFE_PLAYLIST_LOAD_ART,
        RETROFE_PLAYLIST_ENTER,
        RETROFE_MENUJUMP_REQUEST,
        RETROFE_MENUJUMP_EXIT,
        RETROFE_MENUJUMP_LOAD_ART,
        RETROFE_MENUJUMP_ENTER,
        RETROFE_HIGHLIGHT_REQUEST,
        RETROFE_HIGHLIGHT_EXIT,
        RETROFE_HIGHLIGHT_LOAD_ART,
        RETROFE_HIGHLIGHT_ENTER,
        RETROFE_NEXT_PAGE_REQUEST,
        RETROFE_NEXT_PAGE_MENU_EXIT,
        RETROFE_NEXT_PAGE_MENU_LOAD_ART,
        RETROFE_NEXT_PAGE_MENU_ENTER,
        RETROFE_COLLECTION_UP_REQUEST,
        RETROFE_COLLECTION_UP_EXIT,
        RETROFE_COLLECTION_UP_MENU_ENTER,
        RETROFE_COLLECTION_UP_ENTER,
        RETROFE_COLLECTION_UP_SCROLL,
        RETROFE_COLLECTION_HIGHLIGHT_REQUEST,
        RETROFE_COLLECTION_HIGHLIGHT_EXIT,
        RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART,
        RETROFE_COLLECTION_HIGHLIGHT_ENTER,
        RETROFE_COLLECTION_DOWN_REQUEST,
        RETROFE_COLLECTION_DOWN_EXIT,
        RETROFE_COLLECTION_DOWN_MENU_ENTER,
        RETROFE_COLLECTION_DOWN_ENTER,
        RETROFE_COLLECTION_DOWN_SCROLL,
        RETROFE_HANDLE_MENUENTRY,
        RETROFE_ATTRACT_LAUNCH_ENTER,
        RETROFE_ATTRACT_LAUNCH_REQUEST,        
        RETROFE_LAUNCH_ENTER,
        RETROFE_LAUNCH_REQUEST,
        RETROFE_LAUNCH_EXIT,
        RETROFE_BACK_REQUEST,
        RETROFE_BACK_MENU_EXIT,
        RETROFE_BACK_MENU_LOAD_ART,
        RETROFE_BACK_MENU_ENTER,
        RETROFE_MENUMODE_START_REQUEST,
        RETROFE_MENUMODE_START_LOAD_ART,
        RETROFE_MENUMODE_START_ENTER,
       // RETROFE_GAMEINFO_REQUEST,
        RETROFE_QUICKLIST_REQUEST,
        RETROFE_QUICKLIST_PAGE_REQUEST,
        RETROFE_QUICKLIST_PAGE_MENU_EXIT,
        RETROFE_SETTINGS_REQUEST,
        RETROFE_SETTINGS_PAGE_REQUEST,
        RETROFE_SETTINGS_PAGE_MENU_EXIT,
        RETROFE_GAMEINFO_EXIT,
        RETROFE_GAMEINFO_ENTER,
        RETROFE_COLLECTIONINFO_ENTER,
        RETROFE_COLLECIONINFO_EXIT,
        RETROFE_BUILDINFO_ENTER,
        RETROFE_BUILDINFO_EXIT,
        RETROFE_SCROLL_FORWARD,
        RETROFE_SCROLL_BACK,
        RETROFE_NEW,
        RETROFE_QUIT_REQUEST,
        RETROFE_QUIT,
        RETROFE_SCROLL_PLAYLIST_FORWARD,
        RETROFE_SCROLL_PLAYLIST_BACK,
    };

    RETROFE_STATE state_ = RETROFE_IDLE;  // Private version of your state variable

    void setState(RETROFE_STATE newState);
    RETROFE_STATE getState() const;
    std::string stateToString(RETROFE_STATE s) const;

    void            render();
    bool            back( bool &exit );
    bool isStandalonePlaylist(std::string playlist);
    bool isInAttractModeSkipPlaylist(std::string playlist);
    void goToNextAttractModePlaylistByCycle(std::vector<std::string> cycleVector);
    void handleMusicControls(UserInput::KeyCode_E input);
    void            quit( );
    Page           *loadPage(const std::string& collectionName);
    Page           *loadSplashPage( );

    std::vector<std::string> collectionCycle_;
    std::vector<std::string>::iterator collectionCycleIt_;

    RETROFE_STATE   processUserInput( Page *page );
    void            update( float dt, bool scrollActive );
    CollectionInfo *getCollection( const std::string& collectionName );
    void updatePageControls(const std::string& type);
    CollectionInfo *getMenuCollection( const std::string& collectionName );
    bool isUserActive(double now, double threshold = 3.0) const;
	void            saveRetroFEState( ) const;
    std::string getLayoutFileName();
    void resetInfoToggle();

    Configuration     &config_;
    DB                *db_;
    MetadataDatabase  *metadb_;
    UserInput          input_;
    Page              *currentPage_;
    MusicPlayer* musicPlayer_;
    std::unique_ptr<IRestrictor> restrictor_;
    
    std::stack<Page *> pages_;
    float              keyInputDisable_;
    float              currentTime_;
    float              lastLaunchReturnTime_;
    float              keyLastTime_;
    float              keyDelayTime_;
	float              keyLetterSkipDelayTime_;
    Item              *nextPageItem_;
    FontCache          fontcache_;
    AttractMode        attract_;
    bool               menuMode_;
    bool               attractMode_;
	int                attractModePlaylistCollectionNumber_;
	bool               reboot_;
    bool               kioskLock_;
    bool               paused_;
    bool                buildInfo_;
    bool                collectionInfo_;
    bool                gameInfo_;
	std::string        firstPlaylist_;
    std::map<std::string, bool> lkupAttractModeSkipPlaylist_;
    std::map<std::string, size_t> lastMenuOffsets_;
    std::map<std::string, std::string>  lastMenuPlaylists_;
    std::vector<std::string> cycleVector_;
    std::filesystem::file_time_type lastHiFileModifiedTime_{};
};

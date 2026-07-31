#pragma once
#include "Component.h"
#include "ReloadableText.h"
#include "../../Video/IVideo.h"
#include "../../Collection/Item.h"
#include <SDL.h>
#include <string>
#include <string_view>   // <-- add
#include <vector>        // <-- add

class Image;

//todo: this class should aggregate Image, Text, and Video component classes
class ReloadableMedia : public Component {
public:
    ReloadableMedia(Configuration& config, bool systemMode, bool layoutMode, bool commonMode, [[maybe_unused]] bool menuMode,
        const std::string& type, const std::string& imageType,
        Page& p, int displayOffset, bool isVideo, FontManager* font,
        bool jukebox, int jukeboxNumLoops, int randomSelect,
        bool useTextureCaching);

    ~ReloadableMedia() override;

    void enableTextFallback_(bool value);

    bool update(float dt) override;
    void draw() override;
    void freeGraphicsMemory() override;
    void allocateGraphicsMemory() override;
    void pumpGraphicsPreparation() override;
    void waitForGraphicsPreparation() override;
    bool isGraphicsReadyForFirstRender() const override;
    void collectPresentationPreloads(
        const PresentationPreloadContext& context,
        PresentationPreloadCollector& collector) const override;
    bool enablesPresentationImagePreload() const override {
        return useTextureCaching_ && !isVideo_;
    }
    Component* findComponent(const std::string& collection, const std::string& type,
        const std::string& basename, std::string_view filepath, bool systemMode, bool isVideo);

    bool isJukeboxPlaying() override;
    void skipForward() override;
    void skipBackward() override;
    void skipForwardp() override;
    void skipBackwardp() override;
    void pause() override;
    void restart() override;
    unsigned long long getCurrent() override;
    unsigned long long getDuration() override;
    bool isPaused() override;
    std::string_view filePath() override;

private:
    std::vector<std::string> buildNamesForItem_(
        Item& item) const;
    bool resolveComponentFile_(
        const std::string& collection,
        const std::string& type,
        const std::string& basename,
        std::string_view filepath,
        bool systemMode,
        bool isVideo,
        std::string& foundFilePath) const;
    bool resolveImagePathForItem_(
        Item& item,
        size_t selectedIndex,
        bool chooseRandomVariant,
        std::string& foundFilePath) const;
    Component* reloadTexture();
    void realizePendingMedia(bool allowChildPump, float dt, bool allowChildUpdate);
    // NEW: playlist change detection for playlist-driven media
    bool isPlaylistDriven_;
    bool isPlaylistDrivenType_() const;
    std::string lastPlaylistName_;
    bool lastPlaylistNameInit_{ false };

    Configuration& config_;
    bool systemMode_;
    bool layoutMode_;
    bool commonMode_;
    int randomSelect_;
    Component* loadedComponent_{ nullptr };
    bool isVideo_;
    FontManager* FfntInst_;
    bool textFallback_{ false };
    std::string type_;
    int displayOffset_;
    std::string imageType_;
    bool jukebox_;
    int  jukeboxNumLoops_;
    int numberOfImages_{ 27 };
    bool useTextureCaching_{ false };
    static inline const std::vector<std::string> imageExtensions = {
#ifdef WIN32
        "qoi", "png", "gif", "jpg", "jpeg"
#else
        "qoi", "QOI", "png", "PNG", "gif", "GIF", "jpg", "JPG", "jpeg", "JPEG",
#endif
    };

    static inline const std::vector<std::string> videoExtensions = {
#ifdef WIN32
        "mp4", "avi", "mkv", "mp3", "wav", "flac"
#else
        "mp4", "MP4", "avi", "AVI", "mkv", "MKV",
        "mp3", "MP3", "wav", "WAV", "flac", "FLAC"
#endif
    };
};

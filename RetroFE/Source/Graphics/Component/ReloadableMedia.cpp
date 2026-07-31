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

#include "ReloadableMedia.h"
#include "../PresentationPreload.h"
#include "ImageBuilder.h"
#include "VideoBuilder.h"
#include "ReloadableText.h"
#include "../ViewInfo.h"
#include "../../Video/VideoFactory.h"
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../SDL.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

ReloadableMedia::ReloadableMedia(Configuration& config, bool systemMode, bool layoutMode, bool commonMode, [[maybe_unused]] bool menuMode, const std::string& type, const std::string& imageType,
    Page& p, int displayOffset, bool isVideo, FontManager* font, bool jukebox, int jukeboxNumLoops, int randomSelect, bool useTextureCaching)
    : Component(p)
    , config_(config)
    , systemMode_(systemMode)
    , layoutMode_(layoutMode)
    , commonMode_(commonMode)
    , randomSelect_(randomSelect)
    , isVideo_(isVideo)
    , FfntInst_(font)
    , type_(type)
    , displayOffset_(displayOffset)
    , imageType_(imageType)
    , jukebox_(jukebox)
    , jukeboxNumLoops_(jukeboxNumLoops)
    , useTextureCaching_(useTextureCaching) {
    allocateGraphicsMemory();
    isPlaylistDriven_ = isPlaylistDrivenType_();
}

ReloadableMedia::~ReloadableMedia() {
    // 1. Drop VRAM first
    ReloadableMedia::freeGraphicsMemory();
    // 2. Now safely destroy the child
    if (loadedComponent_ != nullptr) {
        delete loadedComponent_;
        loadedComponent_ = nullptr;
    }
}

void ReloadableMedia::enableTextFallback_(bool value) {
    textFallback_ = value;
}

bool ReloadableMedia::update(float dt) {
    // --- Force reload on playlist change for playlist-driven types ---
    if (isPlaylistDriven_) {
        const std::string curPlaylist = page.getPlaylistName();

        if (!lastPlaylistNameInit_) {
            lastPlaylistName_ = curPlaylist;
            lastPlaylistNameInit_ = true;
        }
        else if (curPlaylist != lastPlaylistName_) {
            lastPlaylistName_ = curPlaylist;

            // Force a reload even if the selected item didn't change.
            newItemSelected = true;
            newScrollItemSelected = true;
        }
    }

    const bool hadPendingReload =
        newItemSelected || (newScrollItemSelected && getMenuScrollReload());

    realizePendingMedia(false, dt, true);

    if (!hadPendingReload && loadedComponent_) {
        // Keep this in sync even when no reload happens.
        if (isPlaylistDrivenType_()) {
            loadedComponent_->playlistName = page.getPlaylistName();
        }
        loadedComponent_->update(dt);
    }

    return Component::update(dt);
}

void ReloadableMedia::pumpGraphicsPreparation() {
    // Mirror playlist-driven invalidation here too, because prep may run
    // before the normal update loop advances.
    if (isPlaylistDriven_) {
        const std::string curPlaylist = page.getPlaylistName();

        if (!lastPlaylistNameInit_) {
            lastPlaylistName_ = curPlaylist;
            lastPlaylistNameInit_ = true;
        }
        else if (curPlaylist != lastPlaylistName_) {
            lastPlaylistName_ = curPlaylist;
            newItemSelected = true;
            newScrollItemSelected = true;
        }
    }

    realizePendingMedia(true, 0.0f, false);

    if (loadedComponent_) {
        if (isPlaylistDrivenType_()) {
            loadedComponent_->playlistName = page.getPlaylistName();
        }
        loadedComponent_->pumpGraphicsPreparation();
    }
}

void ReloadableMedia::waitForGraphicsPreparation() {
    realizePendingMedia(false, 0.0f, false);

    if (loadedComponent_) {
        loadedComponent_->waitForGraphicsPreparation();
    }
}

bool ReloadableMedia::isGraphicsReadyForFirstRender() const {
    if (!loadedComponent_) {
        return true;
    }

    return loadedComponent_->isGraphicsReadyForFirstRender();
}

void ReloadableMedia::allocateGraphicsMemory() {
    if (loadedComponent_) {
        loadedComponent_->allocateGraphicsMemory();
    }

    // NOTICE! needs to be done last to prevent flags from being missed
    Component::allocateGraphicsMemory();
}


void ReloadableMedia::freeGraphicsMemory() {
    Component::freeGraphicsMemory();
    // Just tell the child to drop its VRAM. Do NOT delete it!
    if (loadedComponent_ != nullptr) {
        loadedComponent_->freeGraphicsMemory();
    }
}

std::vector<std::string> ReloadableMedia::buildNamesForItem_(
    Item& item) const
{
    const std::string typeLC = Utils::toLower(type_);
    std::vector<std::string> names;
    names.reserve(7);
    names.push_back(item.name);
    names.push_back(item.fullTitle);
    if (!item.cloneof.empty()) {
        names.push_back(item.cloneof);
    }

    if (typeLC == "isfavorite") {
        names.emplace_back(item.isFavorite ? "yes" : "no");
    }
    if (typeLC == "islastplayed") {
        CollectionInfo* currentCollection = page.getCollection();
        names.emplace_back(
            currentCollection &&
            currentCollection->isItemInLastPlayed(&item)
                ? "yes"
                : "no"
        );
    }
    if (typeLC == "ispaused") {
        names.emplace_back(page.isPaused() ? "yes" : "no");
    }
    if (typeLC == "islocked") {
        names.emplace_back(page.isLocked() ? "yes" : "no");
    }

    names.emplace_back("default");
    return names;
}

bool ReloadableMedia::resolveComponentFile_(
    const std::string& collection,
    const std::string& type,
    const std::string& basename,
    std::string_view filepath,
    bool systemMode,
    bool isVideo,
    std::string& foundFilePath) const
{
    std::string mediaPath;

    if (!filepath.empty()) {
        mediaPath = filepath;
    }
    else if (layoutMode_) {
        std::string layoutName;
        config_.getProperty(
            "collections." + collection + ".layout",
            layoutName
        );
        if (layoutName.empty()) {
            config_.getProperty(OPTION_LAYOUT, layoutName);
        }

        mediaPath = commonMode_
            ? Utils::combinePath(
                Configuration::absolutePath,
                "layouts",
                layoutName,
                "collections",
                "_common")
            : Utils::combinePath(
                Configuration::absolutePath,
                "layouts",
                layoutName,
                "collections",
                collection);
        mediaPath = systemMode
            ? Utils::combinePath(mediaPath, "system_artwork")
            : Utils::combinePath(
                mediaPath, "medium_artwork", type);
    }
    else if (commonMode_) {
        mediaPath = Utils::combinePath(
            Configuration::absolutePath,
            "collections",
            "_common"
        );
        mediaPath = systemMode
            ? Utils::combinePath(mediaPath, "system_artwork")
            : Utils::combinePath(
                mediaPath, "medium_artwork", type);
    }
    else {
        config_.getMediaPropertyAbsolutePath(
            collection,
            type,
            systemMode,
            mediaPath
        );
    }

    if (mediaPath.empty()) {
        return false;
    }

    const auto& extensions =
        isVideo ? videoExtensions : imageExtensions;
    return Utils::findMatchingFile(
        Utils::combinePath(mediaPath, basename),
        extensions,
        foundFilePath
    );
}

bool ReloadableMedia::resolveImagePathForItem_(
    Item& item,
    size_t selectedIndex,
    bool chooseRandomVariant,
    std::string& foundFilePath) const
{
    std::string typeLC =
        Utils::toLower(isVideo_ ? imageType_ : type_);
    const std::string type = isVideo_ ? imageType_ : type_;
    std::vector<std::string> names = buildNamesForItem_(item);

    for (std::string basename : names) {
        bool defined = false;

        if (basename == "default") {
            defined = true;
        }
        else if (typeLC == "numberbuttons") {
            basename = item.numberButtons;
            defined = true;
        }
        else if (typeLC == "numberplayers") {
            basename = item.numberPlayers;
            defined = true;
        }
        else if (typeLC == "year") {
            basename = item.year;
            defined = true;
        }
        else if (typeLC == "title") {
            basename = item.title;
            defined = true;
        }
        else if (typeLC == "developer") {
            basename = item.developer.empty()
                ? item.manufacturer
                : item.developer;
            defined = true;
        }
        else if (typeLC == "manufacturer") {
            basename = item.manufacturer;
            defined = true;
        }
        else if (typeLC == "genre") {
            basename = item.genre;
            defined = true;
        }
        else if (typeLC == "ctrltype") {
            basename = item.ctrlType;
            defined = true;
        }
        else if (typeLC == "joyways") {
            basename = item.joyWays;
            defined = true;
        }
        else if (typeLC == "rating") {
            basename = item.rating;
            defined = true;
        }
        else if (typeLC == "score") {
            basename = item.score;
            defined = true;
        }
        else if (typeLC == "playcount") {
            basename = std::to_string(item.playCount);
            defined = true;
        }
        else if (typeLC.rfind("playlist", 0) == 0) {
            basename = page.getPlaylistName();
            defined = true;
        }
        else if (typeLC == "firstletter") {
            basename = item.fullTitle.empty()
                ? ""
                : std::string(1, item.fullTitle.front());
            defined = true;
        }
        else if (typeLC == "position" &&
            item.collectionInfo &&
            !item.collectionInfo->items.empty())
        {
            const size_t position = selectedIndex + 1;
            if (position == 1) {
                basename = "1";
            }
            else if (position == page.getCollectionSize()) {
                basename = std::to_string(numberOfImages_);
            }
            else {
                basename = std::to_string(
                    static_cast<int>(std::ceil(
                        static_cast<float>(position) /
                        static_cast<float>(
                            page.getCollectionSize()) *
                        static_cast<float>(numberOfImages_)
                    ))
                );
            }
            defined = true;
        }

        if (!item.leaf) {
            (void)config_.getProperty(
                "collections." + item.name + "." + type,
                basename
            );
        }

        bool overwriteXML = false;
        config_.getProperty(OPTION_OVERWRITEXML, overwriteXML);
        if (!defined || overwriteXML) {
            std::string configuredName;
            item.getInfo(type, configuredName);
            if (!configuredName.empty()) {
                basename = std::move(configuredName);
            }
        }

        Utils::replaceSlashesWithUnderscores(basename);

        if (chooseRandomVariant && randomSelect_ > 0) {
            basename += " - " +
                std::to_string(1 + rand() % randomSelect_);
        }

        auto resolve = [&](const std::string& collection,
            const std::string& name,
            std::string_view filepath,
            bool system) {
            return resolveComponentFile_(
                collection,
                type,
                name,
                filepath,
                system,
                false,
                foundFilePath
            );
        };

        if (systemMode_) {
            if (resolve(collectionName, type, "", true) ||
                (item.collectionInfo &&
                    resolve(
                        item.collectionInfo->name,
                        type,
                        "",
                        true)) ||
                (!item.leaf &&
                    resolve(item.name, type, "", true)))
            {
                return true;
            }
        }
        else if (item.leaf) {
            if (resolve(collectionName, basename, "", false) ||
                (item.collectionInfo &&
                    resolve(
                        item.collectionInfo->name,
                        basename,
                        "",
                        false)) ||
                (item.collectionInfo &&
                    resolve(
                        item.collectionInfo->name,
                        type,
                        item.filepath,
                        false)))
            {
                return true;
            }
        }
        else {
            if (resolve(collectionName, basename, "", false) ||
                (item.collectionInfo &&
                    resolve(
                        item.collectionInfo->name,
                        basename,
                        "",
                        false)) ||
                resolve(item.name, type, "", true))
            {
                return true;
            }
        }
    }

    return false;
}

void ReloadableMedia::collectPresentationPreloads(
    const PresentationPreloadContext& context,
    PresentationPreloadCollector& collector) const
{
    // Video tags remain governed by VideoPool/GStreamer. Their image/text
    // fallback is not warmed speculatively because the video lookup wins.
    if (isVideo_ || randomSelect_ > 0) {
        return;
    }

    Item* item = context.itemAtOffset(displayOffset_);
    if (!item) {
        return;
    }

    std::string imagePath;
    if (resolveImagePathForItem_(
        *item,
        context.selectedIndex,
        false,
        imagePath))
    {
        if (useTextureCaching_) {
            collector.addImage(
                std::move(imagePath),
                baseViewInfo.Monitor,
                baseViewInfo.Additive,
                baseViewInfo.Layer
            );
        }
    }
    else if (textFallback_ && FfntInst_) {
        collector.addText(
            FfntInst_,
            item->fullTitle,
            static_cast<int>(baseViewInfo.FontSize),
            baseViewInfo.Layer
        );
    }
}


Component* ReloadableMedia::reloadTexture() {
    std::string typeLC = Utils::toLower(type_);
    Item* selectedItem = page.getSelectedItem(displayOffset_);

    if (!selectedItem) return nullptr;

    // build clone list
    std::vector<std::string> names =
        buildNamesForItem_(*selectedItem);
    // if same playlist then use existing loaded component
    Component* foundComponent = nullptr;

    if (isVideo_) {
        for (unsigned int n = 0; n < names.size() && !foundComponent; ++n) {
            std::string basename = names[n];

            // support reloadable video based on playlist# type
            if (basename != "default" && typeLC.rfind("playlist", 0) == 0) {
                basename = page.getPlaylistName();
            }

            if (systemMode_) {
                // check the master collection for the system artifact
                foundComponent = findComponent(collectionName, type_, type_, "", true, true);

                // check the collection for the system artifact
                if (!foundComponent)
                {
                    foundComponent = findComponent(selectedItem->collectionInfo->name, type_, type_, "", true, true);
                }
            }
            else {

                // are we looking at a leaf or a submenu
                if (selectedItem->leaf) // item is a leaf 
                {

                    // check the master collection for the artifact 
                    foundComponent = findComponent(collectionName, type_, basename, "", false, true);

                    // check the collection for the artifact
                    if (!foundComponent) {
                        foundComponent = findComponent(selectedItem->collectionInfo->name, type_, basename, "", false, true);
                    }

                    // check the rom directory for the artifact
                    if (!foundComponent) {
                        foundComponent = findComponent(selectedItem->collectionInfo->name, type_, type_, selectedItem->filepath, false, true);
                    }
                }
                else // item is a submenu
                {
                    // check the master collection for the artifact 
                    foundComponent = findComponent(collectionName, type_, basename, "", false, true);

                    // check the collection for the artifact
                    if (!foundComponent) {
                        foundComponent = findComponent(selectedItem->collectionInfo->name, type_, basename, "", false, true);
                    }

                    // check the submenu collection for the system artifact
                    if (!foundComponent) {
                        foundComponent = findComponent(selectedItem->name, type_, type_, "", true, true);
                    }
                }
            }
            if (foundComponent) {
                return foundComponent;
            }
        }
    }

    // Check for images, including the fallback image for a video tag.
    std::string imagePath;
    if (resolveImagePathForItem_(
        *selectedItem,
        page.getSelectedIndex(),
        true,
        imagePath))
    {
        if (loadedComponent_ &&
            loadedComponent_->filePath() == imagePath)
        {
            return loadedComponent_;
        }

        if (loadedComponent_ &&
            loadedComponent_->recycleAsImage(imagePath))
        {
            return loadedComponent_;
        }

        return new Image(
            imagePath,
            "",
            page,
            baseViewInfo.Monitor,
            baseViewInfo.Additive,
            useTextureCaching_
        );
    }

    // if image and artwork was not specified, fall back to displaying text
    if (!foundComponent && textFallback_) {
        return new Text(selectedItem->fullTitle, page, FfntInst_, baseViewInfo.Monitor);
    }
    return foundComponent;
}

void ReloadableMedia::realizePendingMedia(bool allowChildPump, float dt, bool allowChildUpdate) {
    if (newItemSelected || (newScrollItemSelected && getMenuScrollReload())) {

        newItemSelected = false;
        newScrollItemSelected = false;

        Component* foundComponent = reloadTexture();
        if (foundComponent) {
            foundComponent->playlistName = page.getPlaylistName();
            foundComponent->allocateGraphicsMemory();

            if (allowChildPump) {
                foundComponent->pumpGraphicsPreparation();
            }

            baseViewInfo.ImageWidth = foundComponent->baseViewInfo.ImageWidth;
            baseViewInfo.ImageHeight = foundComponent->baseViewInfo.ImageHeight;

            if (allowChildUpdate) {
                foundComponent->update(dt);
            }

            if (foundComponent != loadedComponent_) {
                delete loadedComponent_;
                loadedComponent_ = foundComponent;
            }
        }
        else {
            delete loadedComponent_;
            loadedComponent_ = nullptr;
        }
    }
}

Component* ReloadableMedia::findComponent(
    const std::string& collection,
    const std::string& type,
    const std::string& basename,
    std::string_view filepath,
    bool systemMode,
    bool isVideo) {
    Component* component = nullptr;
    VideoBuilder videoBuild{};
    ImageBuilder imageBuild{};

    std::string foundFilePath;
    if (resolveComponentFile_(
        collection,
        type,
        basename,
        filepath,
        systemMode,
        isVideo,
        foundFilePath))
    {
        // 1. The Super-Fast Path: It's the exact same file we are already showing
        if (loadedComponent_ != nullptr && foundFilePath == loadedComponent_->filePath()) {
            return loadedComponent_;
        }

        // 2. THE HOT-SWAP (The Heap Churn Fix)
        // If we found a new Image file, and our current component is an Image, DO NOT create a new one.
        if (!isVideo && loadedComponent_ != nullptr) {
            if (auto* imgComp = dynamic_cast<Image*>(loadedComponent_)) {
                imgComp->recycleAsImage(foundFilePath, "");
                return imgComp;
            }
        }

        // 3. The Slow Path (Fallback if we switch from Text to Image, or create for the first time)
        if (isVideo) {
            if (jukebox_)
                component = videoBuild.createVideoFromResolved(
                    foundFilePath,
                    basename,
                    page,
                    baseViewInfo.Monitor,
                    jukeboxNumLoops_,
                    false,
                    -1,
                    nullptr,
                    nullptr
                );
            else
                component = videoBuild.createVideoFromResolved(
                    foundFilePath,
                    basename,
                    page,
                    baseViewInfo.Monitor,
                    -1,
                    false,
                    -1,
                    nullptr,
                    nullptr
                );
        }
        else {
            component = imageBuild.CreateImageFromResolved(
                foundFilePath,
                page,
                baseViewInfo.Monitor,
                baseViewInfo.Additive,
                useTextureCaching_,
                nullptr
            );
        }

        return component;
    }

    return nullptr;
}

bool ReloadableMedia::isPlaylistDrivenType_() const {
    const std::string typeLC = Utils::toLower(type_);
    if (typeLC.rfind("playlist", 0) == 0) {
        return true;
    }

    // If this ReloadableMedia is "video with image fallback", the *imageType_*
    // might be the playlist-driven one.
    if (isVideo_) {
        const std::string imageTypeLC = Utils::toLower(imageType_);
        if (imageTypeLC.rfind("playlist", 0) == 0) {
            return true;
        }
    }

    return false;
}


std::string_view ReloadableMedia::filePath() {
    if (loadedComponent_ != nullptr) {
        return loadedComponent_->filePath();
    }
    return "";
}

void ReloadableMedia::draw() {
    Component::draw();

    if (loadedComponent_) {
        baseViewInfo.ImageHeight = loadedComponent_->baseViewInfo.ImageHeight;
        baseViewInfo.ImageWidth = loadedComponent_->baseViewInfo.ImageWidth;
        loadedComponent_->baseViewInfo = baseViewInfo;
        if (baseViewInfo.Alpha > 0.0f)
            loadedComponent_->draw();
    }
}


bool ReloadableMedia::isJukeboxPlaying() {
    if (jukebox_ && loadedComponent_)
        return loadedComponent_->isPlaying();
    else
        return false;
}


void ReloadableMedia::skipForward() {
    if (jukebox_ && loadedComponent_)
        loadedComponent_->skipForward();
}


void ReloadableMedia::skipBackward() {
    if (jukebox_ && loadedComponent_)
        loadedComponent_->skipBackward();
}


void ReloadableMedia::skipForwardp() {
    if (jukebox_ && loadedComponent_)
        loadedComponent_->skipForwardp();
}


void ReloadableMedia::skipBackwardp() {
    if (jukebox_ && loadedComponent_)
        loadedComponent_->skipBackwardp();
}


void ReloadableMedia::pause() {
    if (jukebox_ && loadedComponent_)
        loadedComponent_->pause();
}


void ReloadableMedia::restart() {
    if (jukebox_ && loadedComponent_)
        loadedComponent_->restart();
}


unsigned long long ReloadableMedia::getCurrent() {
    if (jukebox_ && loadedComponent_)
        return loadedComponent_->getCurrent();
    else
        return 0;
}


unsigned long long ReloadableMedia::getDuration() {
    if (jukebox_ && loadedComponent_)
        return loadedComponent_->getDuration();
    else
        return 0;
}


bool ReloadableMedia::isPaused() {
    if (jukebox_ && loadedComponent_)
        return loadedComponent_->isPaused();
    else
        return false;
}


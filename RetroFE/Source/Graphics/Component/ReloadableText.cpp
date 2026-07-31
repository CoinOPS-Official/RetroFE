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

#include "ReloadableText.h"
#include "../PresentationPreload.h"
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Sound/MusicPlayer.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../ViewInfo.h"
#include "ScrollingList.h"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iostream>
#include <time.h>
#include <vector>

ReloadableText::ReloadableText(std::string type, Page &page, Configuration &config, bool systemMode, FontManager *font,
                               std::string layoutKey, std::string timeFormat, std::string textFormat,
                               std::string singlePrefix, std::string singlePostfix, std::string pluralPrefix,
                               std::string pluralPostfix, std::string location)
    : Component(page), config_(config), systemMode_(systemMode), imageInst_(NULL), type_(type), layoutKey_(layoutKey),
      fontInst_(font), timeFormat_(timeFormat), textFormat_(textFormat), singlePrefix_(singlePrefix),
      singlePostfix_(singlePostfix), pluralPrefix_(pluralPrefix), pluralPostfix_(pluralPostfix), location_(location)
{
    if (type_ == "file")
    {
        filePath_ = Utils::combinePath(Configuration::absolutePath, location_);
    }
    allocateGraphicsMemory();
}

ReloadableText::~ReloadableText() {
    ReloadableText::freeGraphicsMemory();
    if (imageInst_ != NULL) {
        delete imageInst_;
        imageInst_ = NULL;
    }
}

bool ReloadableText::update(float dt)
{
    if (type_ == "time") {
        const time_t now = time(nullptr);
        if (now != lastTimeSecond_) {
            lastTimeSecond_ = now;
            ReloadTexture();
        }
        return Component::update(dt);
    }

    // LINKED COLLECTION STATS:
    // Force all collection-stat widgets to update together whenever EITHER changes.
    if (type_ == "collectionIndex" || type_ == "collectionSize" || type_ == "collectionIndexSize")
    {
        const size_t idx = page.getSelectedIndex();
        const size_t size = page.getCollectionSize();

        if (idx != lastCollectionIdx_ || size != lastCollectionSize_) {
            lastCollectionIdx_ = idx;
            lastCollectionSize_ = size;
            ReloadTexture();
        }

        // Important: consume flags so we don’t “double reload” later in the frame
        newItemSelected = false;
        newScrollItemSelected = false;

        return Component::update(dt);
    }

    
    if (newItemSelected || (newScrollItemSelected && getMenuScrollReload()) || type_ == "current" ||
        type_ == "duration" || type_ == "isPaused" || type_ == "timeSpent")
    {
        ReloadTexture();
        newItemSelected = false;
        newScrollItemSelected = false;
    }
    else if (type_ == "file")
    {
        Uint32 now = SDL_GetTicks();
        if (now - lastFileReloadTime_ >= fileDebounceDuration_) {
            ReloadTexture();
            lastFileReloadTime_ = now;
        }
    }
    else if (type_ == "trackInfo")
    {
        // Get the MusicPlayer instance
        MusicPlayer* musicPlayer = MusicPlayer::getInstance();

        // Check if the music player exists and if the track has changed
        if (musicPlayer && (musicPlayer->hasTrackChanged())) {
            ReloadTexture();
        }
    }
    // needs to be ran at the end to prevent the NewItemSelected flag from being detected
    return Component::update(dt);
}

void ReloadableText::allocateGraphicsMemory() {
    if (!fontInst_) return;

    ReloadTexture();

    // Wake up the existing text object's VRAM
    if (imageInst_ != NULL) {
        imageInst_->allocateGraphicsMemory();
    }

    Component::allocateGraphicsMemory();
}

void ReloadableText::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    // Drop the VRAM, but keep the C++ object alive for setText()!
    if (imageInst_ != NULL)
    {
        imageInst_->freeGraphicsMemory();
    }
}

void ReloadableText::initializeFonts()
{
    if (fontInst_) fontInst_->initialize();
}

void ReloadableText::deInitializeFonts()
{
    if (fontInst_) fontInst_->deInitialize();
}

// Add a method to check the transition state
bool ReloadableText::isInTransition() const
{
    return (getAnimationRequestedType() == "playlistExit" || getAnimationRequestedType() == "playlistPrevEnter" ||
            getAnimationRequestedType() == "playlistPrevExit" || getAnimationRequestedType() == "playlistNextEnter" ||
            getAnimationRequestedType() == "playlistNextExit");
}

bool ReloadableText::isItemDrivenType_() const {
    return type_ != "time" &&
        type_ != "file" &&
        type_ != "trackInfo" &&
        type_.rfind("playlist", 0) != 0 &&
        type_ != "collectionName" &&
        type_ != "collectionSize" &&
        type_ != "collectionIndex" &&
        type_ != "collectionIndexSize" &&
        type_ != "isPaused" &&
        type_ != "current" &&
        type_ != "duration";
}

std::string ReloadableText::resolveItemText_(
    const Item& item) const
{
    std::string text;

    if (type_ == "numberButtons")        text = item.numberButtons;
    else if (type_ == "numberPlayers")   text = item.numberPlayers;
    else if (type_ == "ctrlType")        text = item.ctrlType;
    else if (type_ == "numberJoyWays")   text = item.joyWays;
    else if (type_ == "rating")          text = item.rating;
    else if (type_ == "score")           text = item.score;
    else if (type_ == "year")            text = item.year;
    else if (type_ == "title")           text = item.title;
    else if (type_ == "developer") {
        text = item.developer;
        if (text.empty()) text = item.manufacturer;
    }
    else if (type_ == "manufacturer")    text = item.manufacturer;
    else if (type_ == "genre")           text = item.genre;
    else if (type_ == "playCount")       text = std::to_string(item.playCount);
    else if (type_ == "timeSpent") {
        const int totalMinutes =
            static_cast<int>(item.timeSpent / 60);
        const int hours = totalMinutes / 60;
        const int minutes = totalMinutes % 60;

        if (totalMinutes < 1) text.clear();
        else if (hours > 0)
            text = std::to_string(hours) + "h " +
                std::to_string(minutes) + "m";
        else
            text = std::to_string(minutes) + "m";
    }
    else if (type_ == "lastPlayed") {
        if (item.lastPlayed != "0") {
            text = item.lastPlayed;
        }
    }
    else if (type_ == "firstLetter") {
        if (!item.fullTitle.empty()) {
            text.assign(1, item.fullTitle.front());
        }
    }
    else if (type_ == "isFavorite") {
        text = item.isFavorite ? "yes" : "no";
    }

    if (text.empty() && (!item.leaf || systemMode_)) {
        (void)config_.getProperty(
            "collections." + item.name + "." + type_,
            text
        );
    }

    if (text.empty() && systemMode_) {
        (void)config_.getProperty(
            "collections." + page.getCollectionName() +
                "." + type_,
            text
        );
    }

    bool overwriteXML = false;
    config_.getProperty(OPTION_OVERWRITEXML, overwriteXML);
    if (text.empty() || overwriteXML) {
        std::string textFromInfo;
        item.getInfo(type_, textFromInfo);
        if (!textFromInfo.empty()) {
            text = std::move(textFromInfo);
        }
    }

    return text;
}

std::string ReloadableText::formatItemText_(
    std::string text) const
{
    if (text == "0")
        text = singlePrefix_ + text + pluralPostfix_;
    else if (text == "1")
        text = singlePrefix_ + text + singlePostfix_;
    else if (!text.empty())
        text = pluralPrefix_ + text + pluralPostfix_;

    if (textFormat_ == "uppercase") {
        std::transform(
            text.begin(), text.end(), text.begin(), ::toupper);
    }
    else if (textFormat_ == "lowercase") {
        std::transform(
            text.begin(), text.end(), text.begin(), ::tolower);
    }

    return text;
}

void ReloadableText::collectPresentationPreloads(
    const PresentationPreloadContext& context,
    PresentationPreloadCollector& collector) const
{
    if (!fontInst_ || !isItemDrivenType_()) {
        return;
    }

    const Item* item = context.itemAtOffset(0);
    if (!item) {
        return;
    }

    collector.addText(
        fontInst_,
        formatItemText_(resolveItemText_(*item)),
        static_cast<int>(baseViewInfo.FontSize),
        baseViewInfo.Layer
    );
}

void ReloadableText::ReloadTexture() {

    if (!fontInst_) {
        static bool errorLogged = false;
        if (!errorLogged) {
            LOG_ERROR("ReloadableText", "Component disabled. Font resource mapping is missing or invalid.");
            errorLogged = true;
        }
        return;
    }

    auto isPlaylistType = [this]() -> bool {
        return (type_.rfind("playlist", 0) == 0);
        };

    // Types that do NOT require a selected item
    auto isIndependentType = [this, &isPlaylistType]() -> bool {
        return (type_ == "time" ||
            type_ == "file" ||
            type_ == "trackInfo" ||
            isPlaylistType() ||
            type_ == "collectionName" ||
            type_ == "collectionSize" ||
            type_ == "collectionIndex" ||
            type_ == "collectionIndexSize" ||
            type_ == "isPaused" ||
            type_ == "current" ||
            type_ == "duration");
        };

    const bool independentType = isIndependentType();

    // During transitions: don't update item-dependent text,
    // but allow time/file/trackInfo/collection stats to keep updating (no freezing).
    if (isInTransition() && !independentType)
        return;

    Item* selectedItem = nullptr;
    if (!independentType) {
        selectedItem = page.getSelectedMenuItem();
        if (!selectedItem) {
            return; // keep last displayed value; no flicker + no crash
        }
    }

    std::stringstream ss;
    std::string text;
    bool alreadyWrapped = false; // if we manually applied prefix/postfix rules into ss

    // ------------------------------------------------------------
    // Independent sources
    // ------------------------------------------------------------
    if (type_ == "file")
    {
        std::filesystem::path file(filePath_);
        auto roundToNearestSecond = [](std::filesystem::file_time_type ftt) {
            return std::chrono::time_point_cast<std::chrono::seconds>(ftt);
            };

        if (!std::filesystem::exists(file))
            return;

        std::filesystem::file_time_type currentWriteTime;
        try {
            currentWriteTime = roundToNearestSecond(std::filesystem::last_write_time(file));
        }
        catch (const std::filesystem::filesystem_error& e) {
            LOG_ERROR("ReloadableText", "Failed to retrieve file modification time: " + std::string(e.what()));
            return;
        }

        if (currentWriteTime == lastWriteTime_ && imageInst_ != nullptr)
            return;

        std::ifstream fileStream(filePath_);
        if (fileStream) {
            text.assign((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
            fileStream.close();
            lastWriteTime_ = currentWriteTime;
        }
        else {
            LOG_ERROR("ReloadableText", "Failed to open file: " + filePath_);
            return;
        }
    }
    else if (type_ == "trackInfo")
    {
        MusicPlayer const* musicPlayer = MusicPlayer::getInstance();
        if (!musicPlayer || musicPlayer->getCurrentTrackName().empty()) {
            text.clear();
        }
        else {
            std::string currentArtist = musicPlayer->getCurrentArtist();
            std::string currentTitle = musicPlayer->getCurrentTitle();

            if (!currentArtist.empty() && !currentTitle.empty())
                text = currentArtist + " - " + currentTitle;
            else if (!currentTitle.empty())
                text = currentTitle;
            else
                text = musicPlayer->getCurrentTrackName();
        }
    }
    else if (type_ == "time")
    {
        if (timeFormat_.empty())
            timeFormat_ = "%I:%M:%S %p";

        time_t now = time(nullptr);
        std::tm tstruct{};
#if defined(_WIN32)
        localtime_s(&tstruct, &now);
#else
        localtime_r(&now, &tstruct);
#endif
        char buf[80];
        strftime(buf, sizeof(buf), timeFormat_.c_str(), &tstruct);
        text = buf;
    }
    else if (type_ == "collectionName")
    {
        text = page.getCollectionName();
    }
    else if (type_ == "collectionSize")
    {
        const size_t size = page.getCollectionSize();

        if (size == 0)        ss << singlePrefix_ << size << pluralPostfix_;
        else if (size == 1)   ss << singlePrefix_ << size << singlePostfix_;
        else                  ss << pluralPrefix_ << size << pluralPostfix_;

        alreadyWrapped = true;
    }
    else if (type_ == "collectionIndex")
    {
        const size_t idx = page.getSelectedIndex(); // 0-based

        if (idx == 0)         ss << singlePrefix_ << (idx + 1) << pluralPostfix_;
        else if (idx == 1)    ss << singlePrefix_ << (idx + 1) << singlePostfix_;
        else                  ss << pluralPrefix_ << (idx + 1) << pluralPostfix_;

        alreadyWrapped = true;
    }
    else if (type_ == "collectionIndexSize")
    {
        const size_t idx = page.getSelectedIndex();
        const size_t size = page.getCollectionSize();

        if (idx == 0)         ss << singlePrefix_ << (idx + 1) << "/" << size << pluralPostfix_;
        else if (idx == 1)    ss << singlePrefix_ << (idx + 1) << "/" << size << singlePostfix_;
        else                  ss << pluralPrefix_ << (idx + 1) << "/" << size << pluralPostfix_;

        alreadyWrapped = true;
    }
    else if (type_ == "isPaused")
    {
        text = page.isPaused() ? "Paused" : "";
    }
    else if (type_ == "current")
    {
        unsigned long long current = page.getCurrent() / 1000000000ULL;
        int seconds = static_cast<int>(current % 60);
        int minutes = static_cast<int>((current / 60) % 60);
        int hours = static_cast<int>(current / 3600);

        text = std::to_string(hours) + ":";
        text += (minutes < 10) ? "0" + std::to_string(minutes) + ":" : std::to_string(minutes) + ":";
        text += (seconds < 10) ? "0" + std::to_string(seconds) : std::to_string(seconds);

        if (page.getDuration() == 0)
            text = "--:--:--";
    }
    else if (type_ == "duration")
    {
        unsigned long long duration = page.getDuration() / 1000000000ULL;
        int seconds = static_cast<int>(duration % 60);
        int minutes = static_cast<int>((duration / 60) % 60);
        int hours = static_cast<int>(duration / 3600);

        text = std::to_string(hours) + ":";
        text += (minutes < 10) ? "0" + std::to_string(minutes) + ":" : std::to_string(minutes) + ":";
        text += (seconds < 10) ? "0" + std::to_string(seconds) : std::to_string(seconds);

        if (page.getDuration() == 0)
            text = "--:--:--";
    }
    else if (isPlaylistType())
    {
        text = playlistName;
    }
    // ------------------------------------------------------------
    // Item-driven sources (selectedItem guaranteed non-null here)
    // ------------------------------------------------------------
    else
    {
        text = resolveItemText_(*selectedItem);
    }

    // Apply the generic prefix/postfix + case transforms unless we already built ss ourselves
    if (!alreadyWrapped)
    {
        if (text == "0")               text = singlePrefix_ + text + pluralPostfix_;
        else if (text == "1")          text = singlePrefix_ + text + singlePostfix_;
        else if (!text.empty())        text = pluralPrefix_ + text + pluralPostfix_;

        if (!text.empty())
        {
            if (textFormat_ == "uppercase")
                std::transform(text.begin(), text.end(), text.begin(), ::toupper);
            else if (textFormat_ == "lowercase")
                std::transform(text.begin(), text.end(), text.begin(), ::tolower);

            ss << text;
        }
    }
    else
    {
        // Also apply textFormat_ to ss-built strings (keeps behavior consistent)
        std::string tmp = ss.str();
        if (!tmp.empty())
        {
            if (textFormat_ == "uppercase")
                std::transform(tmp.begin(), tmp.end(), tmp.begin(), ::toupper);
            else if (textFormat_ == "lowercase")
                std::transform(tmp.begin(), tmp.end(), tmp.begin(), ::tolower);

            ss.str(std::string());
            ss.clear();
            ss << tmp;
        }
    }

    const std::string newText = ss.str();

    const bool typeChanged = (currentType_ != type_);
    const bool valueChanged = (currentValue_ != newText);

    currentType_ = type_;
    currentValue_ = newText;

    if (!typeChanged && !valueChanged && imageInst_ != nullptr)
        return;

    if (imageInst_)
    {
        if (!typeChanged && valueChanged)
        {
            imageInst_->setText(newText);
            return;
        }

        delete imageInst_;
        imageInst_ = nullptr;
    }

    if (!newText.empty())
    {
        imageInst_ = new Text(newText, page, fontInst_, baseViewInfo.Monitor);
    }
}

void ReloadableText::draw()
{
    if(imageInst_)
    {
        imageInst_->baseViewInfo = baseViewInfo;
        imageInst_->draw();
    }
}

// thank you chatgpt
std::string ReloadableText::getTimeSince(std::string sinceTimestamp)
{
    const char *timestamp = sinceTimestamp.c_str();
    std::time_t t2 = (time_t)strtol(timestamp, NULL, 10);

    // error checking, make sure timestamp is valid
    if (t2 == 0 && errno == EINVAL)
    {
        return "";
    }

    // error checking, make sure the timestamp is not in the future
    std::time_t t1 = std::time(nullptr);
    if (t2 > t1)
    {
        return "";
    }

    std::tm tm1{};
    std::tm tm2{};

#if defined(_WIN32)
    localtime_s(&tm1, &t1);
    localtime_s(&tm2, &t2);
#else
    localtime_r(&t1, &tm1);
    localtime_r(&t2, &tm2);
#endif

    // Calculate the difference in years, months, and days
    int yearsDiff = tm1.tm_year - tm2.tm_year;
    int monthsDiff = tm1.tm_mon - tm2.tm_mon;
    int daysDiff = tm1.tm_mday - tm2.tm_mday;

    // Adjust the difference in case of negative values
    if (daysDiff < 0)
    {
        monthsDiff--;
        std::time_t tempT2 = t2;
        std::tm tempTm2 = tm2;

        while (tempTm2.tm_mon != tm1.tm_mon)
        {
            tempT2 += 24 * 60 * 60; // Add 1 day
            tempTm2 = *std::localtime(&tempT2);
            daysDiff += tempTm2.tm_mday; // Add the number of days in the current month
        }

        if (daysDiff < 0)
        {
            std::time_t tempT2 = t2;
            std::tm tempTm2 = tm2;
            int totalDaysDiff = 0;

            while (tempTm2.tm_year != tm1.tm_year || tempTm2.tm_mon != tm1.tm_mon)
            {
                tempT2 += 24 * 60 * 60; // Add 1 day
                tempTm2 = *std::localtime(&tempT2);
                totalDaysDiff++;
            }

            monthsDiff--;
            daysDiff = totalDaysDiff - daysDiff;
        }
    }

    // Adjust the difference in case of negative months
    if (monthsDiff < 0)
    {
        yearsDiff--;
        monthsDiff += 12;
    }

    // Calculate the number of days in each month
    const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    int totalDays = 0;
    for (int i = tm2.tm_mon; i < 12; ++i)
    {
        totalDays += daysInMonth[i];
    }
    totalDays -= tm2.tm_mday - 1;

    for (int i = 0; i < tm1.tm_mon; ++i)
    {
        totalDays += daysInMonth[i];
    }
    totalDays += tm1.tm_mday;

    // Adjust the difference by the total number of days
    if (totalDays >= daysDiff)
    {
        totalDays -= daysDiff;
    }
    else
    {
        monthsDiff--;
        totalDays = daysInMonth[(tm1.tm_mon + 11) % 12] - (daysDiff - totalDays - 1);
    }

    // construct the result string
    std::string result = "";
    if (yearsDiff > 0)
    {
        result += std::to_string(yearsDiff) + (yearsDiff == 1 ? " year " : " years ");
    }
    if (monthsDiff > 0)
    {
        result += std::to_string(monthsDiff) + (monthsDiff == 1 ? " month " : " months ");
    }
    if (daysDiff > 0)
    {
        result += std::to_string(daysDiff) + (daysDiff == 1 ? " day " : " days ");
    }

    if (result == "")
    {
        result = "today";
    }
    else
    {
        result += "ago";
    }

    return result;
}

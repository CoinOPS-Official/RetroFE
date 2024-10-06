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
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Database/Configuration.h"
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

ReloadableText::ReloadableText(std::string type, Page &page, Configuration &config, bool systemMode, Font *font,
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

ReloadableText::~ReloadableText()
{
    ReloadableText::freeGraphicsMemory();
}

bool ReloadableText::update(float dt)
{
    if (newItemSelected || (newScrollItemSelected && getMenuScrollReload()) || type_ == "time" || type_ == "current" ||
        type_ == "duration" || type_ == "isPaused")
    {
        ReloadTexture();
        newItemSelected = false;
    }
    else if (type_ == "file")
    {
        static Uint32 lastFileReloadTime_ = 0; // Time of the last file reload in milliseconds
        static const Uint32 fileDebounceDuration_ = 1000; // 1 second debounce for files

        Uint32 now = SDL_GetTicks();
        // Apply debounce logic specifically for file type
        if (now - lastFileReloadTime_ >= fileDebounceDuration_)
        {
            ReloadTexture();
            lastFileReloadTime_ = now;
        }
    }
    // needs to be ran at the end to prevent the NewItemSelected flag from being detected
    return Component::update(dt);
}

void ReloadableText::allocateGraphicsMemory()
{
    ReloadTexture();

    // NOTICE! needs to be done last to prevent flags from being missed
    Component::allocateGraphicsMemory();
}

void ReloadableText::freeGraphicsMemory()
{
    Component::freeGraphicsMemory();

    if (imageInst_ != NULL)
    {
        delete imageInst_;
        imageInst_ = NULL;
    }
}

void ReloadableText::initializeFonts()
{
    fontInst_->initialize();
}

void ReloadableText::deInitializeFonts()
{
    fontInst_->deInitialize();
}

// Add a method to check the transition state
bool ReloadableText::isInTransition() const
{
    return (getAnimationRequestedType() == "playlistExit" || getAnimationRequestedType() == "playlistPrevEnter" ||
            getAnimationRequestedType() == "playlistPrevExit" || getAnimationRequestedType() == "playlistNextEnter" ||
            getAnimationRequestedType() == "playlistNextExit");
}

void ReloadableText::ReloadTexture()
{
    // Check if the component is in a transition state
    if (isInTransition())
    {
        // In transition state, do not update the text
        currentType_.clear();
        currentValue_.clear();
        return;
    }

    // Get the selected item
    Item *selectedItem = page.getSelectedMenuItem();

    // If there's no selected item, we might be in a transition state
    if (selectedItem == nullptr)
    {
        currentType_.clear();
        currentValue_.clear();
        return;
    }

    std::stringstream ss;
    std::string text = "";

    // Update text based on the type
    if (type_ == "file")
    {
        std::filesystem::path file(filePath_);
        std::filesystem::file_time_type currentWriteTime;

        // Lambda to round the file time to the nearest second
        auto roundToNearestSecond = [](std::filesystem::file_time_type ftt) {
            return std::chrono::time_point_cast<std::chrono::seconds>(ftt);
            };

        try
        {
            currentWriteTime = std::filesystem::last_write_time(file);
            currentWriteTime = roundToNearestSecond(currentWriteTime); // Round to nearest second
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            LOG_ERROR("ReloadableText", "Failed to retrieve file modification time: " + std::string(e.what()));
            return;
        }

        // Check if the file has changed since the last read
        if (currentWriteTime == lastWriteTime_ && imageInst_ != nullptr)
        {
            // No change in file, skip update
            return;
        }

        // Read the text from the specified file
        std::ifstream fileStream(filePath_);
        if (fileStream)
        {
            text.assign((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
            fileStream.close();

            // Update lastWriteTime_ with the rounded time
            lastWriteTime_ = currentWriteTime;
        }
        else
        {
            LOG_ERROR("ReloadableText", "Failed to open file: " + filePath_);
            return;
        }
    }
    else if (type_ == "time") {
        // If timeFormat_ is undefined, assign a reasonable default
        if (timeFormat_.empty()) {
            timeFormat_ = "%I:%M:%S %p";
        }


            time_t now = time(0);           // Get current time in seconds
            struct tm tstruct;
            char buf[80];
            tstruct = *localtime(&now);     // Convert to local time
            strftime(buf, sizeof(buf), timeFormat_.c_str(), &tstruct);  // Format the time
            ss << buf;                      // Append formatted time to stringstream
        
    }

    else if (type_ == "numberButtons")
    {
        text = selectedItem->numberButtons;
    }
    else if (type_ == "numberPlayers")
    {
        text = selectedItem->numberPlayers;
    }
    else if (type_ == "ctrlType")
    {
        text = selectedItem->ctrlType;
    }
    else if (type_ == "numberJoyWays")
    {
        text = selectedItem->joyWays;
    }
    else if (type_ == "rating")
    {
        text = selectedItem->rating;
    }
    else if (type_ == "score")
    {
        text = selectedItem->score;
    }
    else if (type_ == "year")
    {
        text = selectedItem->year;
    }
    else if (type_ == "title")
    {
        text = selectedItem->title;
    }
    else if (type_ == "developer")
    {
        text = selectedItem->developer;
        // Overwrite in case developer has not been specified
        if (text.empty())
        {
            text = selectedItem->manufacturer;
        }
    }
    else if (type_ == "manufacturer")
    {
        text = selectedItem->manufacturer;
    }
    else if (type_ == "genre")
    {
        text = selectedItem->genre;
    }
    else if (type_ == "playCount")
    {
        text = std::to_string(selectedItem->playCount);
    }
    else if (type_ == "lastPlayed")
    {
        if (selectedItem->lastPlayed != "0")
        {
            text = selectedItem->lastPlayed;
        }
    }
    else if (type_.rfind("playlist", 0) == 0)
    {
        text = playlistName;
    }
    else if (type_ == "firstLetter")
    {
        text = selectedItem->fullTitle.at(0);
    }
    else if (type_ == "collectionName")
    {
        text = page.getCollectionName();
    }
    else if (type_ == "collectionSize")
    {
        if (page.getCollectionSize() == 0)
        {
            ss << singlePrefix_ << page.getCollectionSize() << pluralPostfix_;
        }
        else if (page.getCollectionSize() == 1)
        {
            ss << singlePrefix_ << page.getCollectionSize() << singlePostfix_;
        }
        else
        {
            ss << pluralPrefix_ << page.getCollectionSize() << pluralPostfix_;
        }
    }
    else if (type_ == "collectionIndex")
    {
        size_t test = page.getSelectedIndex();
        if (test == 0)
        {
            ss << singlePrefix_ << (test + 1) << pluralPostfix_;
        }
        else if (test == 1)
        {
            ss << singlePrefix_ << (test + 1) << singlePostfix_;
        }
        else
        {
            ss << pluralPrefix_ << (test + 1) << pluralPostfix_;
        }
    }
    else if (type_ == "collectionIndexSize")
    {
        size_t test = page.getSelectedIndex();
        if (test == 0)
        {
            ss << singlePrefix_ << (test + 1) << "/" << page.getCollectionSize() << pluralPostfix_;
        }
        else if (test == 1)
        {
            ss << singlePrefix_ << (test + 1) << "/" << page.getCollectionSize() << singlePostfix_;
        }
        else
        {
            ss << pluralPrefix_ << (test + 1) << "/" << page.getCollectionSize() << pluralPostfix_;
        }
    }
    else if (type_ == "isFavorite")
    {
        text = selectedItem->isFavorite ? "yes" : "no";
    }
    else if (type_ == "isPaused")
    {
        text = page.isPaused() ? "Paused" : "";
    }
    else if (type_ == "current")
    {
        unsigned long long current = 0;
        current = page.getCurrent();
        current /= 1000000000;
        int seconds = current % 60;
        int minutes = (current / 60) % 60;
        int hours = int(current / 3600);
        text = std::to_string(hours) + ":";
        if (minutes < 10)
            text += "0" + std::to_string(minutes) + ":";
        else
            text += std::to_string(minutes) + ":";
        if (seconds < 10)
            text += "0" + std::to_string(seconds);
        else
            text += std::to_string(seconds);
        if (page.getDuration() == 0)
            text = "--:--:--";
    }
    else if (type_ == "duration")
    {
        unsigned long long duration = 0;
        duration = page.getDuration();
        duration /= 1000000000;
        int seconds = duration % 60;
        int minutes = (duration / 60) % 60;
        int hours = int(duration / 3600);
        text = std::to_string(hours) + ":";
        if (minutes < 10)
            text += "0" + std::to_string(minutes) + ":";
        else
            text += std::to_string(minutes) + ":";
        if (seconds < 10)
            text += "0" + std::to_string(seconds);
        else
            text += std::to_string(seconds);
        if (page.getDuration() == 0)
            text = "--:--:--";
    }

    if (text.empty() && (!selectedItem->leaf || systemMode_)) // item is not a leaf
    {
        (void)config_.getProperty("collections." + selectedItem->name + "." + type_, text);
    }

    if (text.empty() && systemMode_) // Get the system information instead
    {
        (void)config_.getProperty("collections." + page.getCollectionName() + "." + type_, text);
    }

    bool overwriteXML = false;
    config_.getProperty(OPTION_OVERWRITEXML, overwriteXML);
    if (text.empty() || overwriteXML) // No text was found yet; check the info instead
    {
        std::string text_tmp;
        selectedItem->getInfo(type_, text_tmp);
        if (!text_tmp.empty())
        {
            text = text_tmp;
        }
    }

    if (text == "0")
    {
        text = singlePrefix_ + text + pluralPostfix_;
    }
    else if (text == "1")
    {
        text = singlePrefix_ + text + singlePostfix_;
    }
    else if (!text.empty())
    {
        text = pluralPrefix_ + text + pluralPostfix_;
    }

    if (!text.empty())
    {
        if (textFormat_ == "uppercase")
        {
            std::transform(text.begin(), text.end(), text.begin(), ::toupper);
        }
        if (textFormat_ == "lowercase")
        {
            std::transform(text.begin(), text.end(), text.begin(), ::tolower);
        }
        ss << text;
    }

    // Update the tracked attributes
    bool typeChanged = (currentType_ != type_);
    bool valueChanged = (currentValue_ != ss.str());

    if (!typeChanged && !valueChanged && imageInst_ != nullptr)
    {
        // No changes and the image instance already exists, so no need to recreate it
        return;
    }

    // Delete the old component if a new one is required or if it's missing
    if (imageInst_ != nullptr)
    {
        delete imageInst_;
        imageInst_ = nullptr;
    }

    currentType_ = type_;
    currentValue_ = ss.str();

    // Create a new image instance
    if (!ss.str().empty())
    {
        imageInst_ = new Text(ss.str(), page, fontInst_, baseViewInfo.Monitor);
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

    // Convert time_t to struct tm
    std::tm tm1 = *std::localtime(&t1);
    std::tm tm2 = *std::localtime(&t2);

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

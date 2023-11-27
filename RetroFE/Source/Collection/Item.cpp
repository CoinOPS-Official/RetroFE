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

#include "Item.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>


Item::~Item() = default;


std::string Item::filename() const
{
    return Utils::getFileName(filepath);
}

std::string Item::lowercaseTitle() const
{
    std::string lcstr = title;
    std::transform(lcstr.begin(), lcstr.end(), lcstr.begin(), ::tolower);
    return lcstr;
}

std::string Item::lowercaseFullTitle() const
{
    std::string lcstr = fullTitle;
    std::transform(lcstr.begin(), lcstr.end(), lcstr.begin(), ::tolower);
    return lcstr;
}

bool Item::isSortDesc(std::string attribute)
{
    std::transform(attribute.begin(), attribute.end(), attribute.begin(), ::tolower);

    bool valid = false;
    if (attribute == "lastplayed") valid = true;
    if (attribute == "playcount") valid = true;

    return valid;
}

bool Item::validSortType(std::string attribute)
{
    std::transform(attribute.begin(), attribute.end(), attribute.begin(), ::tolower);

    bool valid = false;
    if (attribute == "year") valid = true;
    else if (attribute == "manufacturer") valid = true;
    else if (attribute == "developer") valid = true;
    else if (attribute == "genre") valid = true;
    else if (attribute == "numberplayers") valid = true;
    else if (attribute == "numberbuttons") valid = true;
    else if (attribute == "ctrltype") valid = true;
    else if (attribute == "joyways") valid = true;
    else if (attribute == "rating") valid = true;
    else if (attribute == "score") valid = true;
    else if (attribute == "lastplayed") valid = true;
    else if (attribute == "playcount") valid = true;

    return valid;
}

std::string Item::getMetaAttribute(const std::string& attribute) const {
    std::string lowerAttribute = attribute;
    std::transform(lowerAttribute.begin(), lowerAttribute.end(), lowerAttribute.begin(), ::tolower);

    std::string value = "";

    if (lowerAttribute == "year") value = year;
    else if (lowerAttribute == "manufacturer") value = manufacturer;
    else if (lowerAttribute == "developer") value = developer;
    else if (lowerAttribute == "genre") value = genre;
    else if (lowerAttribute == "numberplayers") value = numberPlayers;
    else if (lowerAttribute == "numberbuttons") value = numberButtons;
    else if (lowerAttribute == "ctrltype") value = ctrlType;
    else if (lowerAttribute == "joyways") value = joyWays;
    else if (lowerAttribute == "rating") value = rating;
    else if (lowerAttribute == "score") value = score;
    else if (lowerAttribute == "lastplayed") value = lastPlayed;
    else if (lowerAttribute == "playcount") value = std::to_string(playCount);

    
    std::string lowerValue = value;
    std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
    
    return lowerValue;
}

void Item::setInfo( std::string key, std::string value )
{
    info_.try_emplace(std::move(key), std::move(value));
}


bool Item::getInfo(const std::string& key, std::string& value)
{
    bool retVal = false;
    if (!info_.empty()) {
        if (auto it = info_.find(key); it != info_.end()) {
            value = it->second;  // Use iterator to access the value directly
            retVal = true;
        }
    }

    return retVal;
}


void Item::loadInfo(const std::string& path)
{
    int           lineCount = 0;
    std::string   line;
    std::ifstream ifs(path.c_str());  // No need to call .c_str() in C++11 and above
    size_t        position;
    std::string   key;
    std::string   value;

    if (!ifs.is_open())
    {
        return;
    }

    while (std::getline(ifs, line))
    {
        lineCount++;
        line = Utils::filterComments(line);
        // Check if the line has an assignment operator
        if ((position = line.find("=")) != std::string::npos)
        {
            key = line.substr(0, position);
            key = Utils::trimEnds(key);
            value = line.substr(position + 1);
            value = Utils::trimEnds(value);
            setInfo(key, value);
        }
        else
        {
            std::stringstream ss;
            ss << "Missing an assignment operator (=) on line " << lineCount;
            LOG_ERROR("Item", ss.str());
        }
    }
}


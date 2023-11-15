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

#include <string>
#include <map>
#include <vector>

class Configuration
{
public:
    Configuration();
    virtual ~Configuration();
    static void initialize();
    static std::string convertToAbsolutePath(const std::string& prefix, const std::string& path);
    static std::string trimEnds(std::string str);
	void clearProperties( );
    // gets the global configuration
    bool import(const std::string& keyPrefix, const std::string& file, bool mustExist = true);
    bool import(const std::string& collection, const std::string& keyPrefix, const std::string& file, bool mustExist = true);
    bool getProperty(const std::string& key, std::string &value);
    bool getProperty(const std::string& key, int &value);
    bool getProperty(const std::string& key, bool &value);
    void childKeyCrumbs(const std::string& parent, std::vector<std::string> &children);
    void setProperty(const std::string& key, const std::string& value);
    bool propertiesEmpty() const;
    bool propertyExists(const std::string& key);
    bool propertyPrefixExists(const std::string& key);
    bool getPropertyAbsolutePath(const std::string& key, std::string &value);
    void getMediaPropertyAbsolutePath(const std::string& collectionName, const std::string& mediaType, std::string &value);
    void getMediaPropertyAbsolutePath(const std::string& collectionName, const std::string& mediaType, bool system, std::string &value);
    void getCollectionAbsolutePath(const std::string& collectionName, std::string &value);
    static std::string absolutePath;
	static int AvdecMaxThreads;
	static bool HardwareVideoAccel; // Declare HardwareVideoAccel as a static member variable
	static bool MuteVideo;

private:
    bool getRawProperty(const std::string& key, std::string &value);
    bool parseLine(const std::string& collection, std::string keyPrefix, std::string line, int lineCount);
    using PropertiesType = std::map<std::string, std::string, std::less<>>;
    typedef std::pair<std::string, std::string> PropertiesPair;

    PropertiesType properties_;

};

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

#include "Video/GStreamerVideo.h"
#include "Database/Configuration.h"
#include "Collection/CollectionInfoBuilder.h"
#include "Execute/Launcher.h"
#include "Utility/Log.h"
#include "Utility/Utils.h"
#include "RetroFE.h"
#include "Version.h"
#include "SDL.h"
#include <cstdlib>
#include <fstream>
#include <time.h>
#include <locale>
#include <filesystem>
#include <string>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
static bool ImportConfiguration(Configuration* c);

int main(int argc, char** argv)
{
    // check to see if version or help was requested
    if (argc > 1)
    {
        std::string program = argv[0];
        std::string param = argv[1];

        if (argc == 3 && param == "-createcollection")
        {
            // Do nothing; we handle that later
        }
        else if (param == "-version" ||
            param == "--version" ||
            param == "-v")
        {
            std::cout << "RetroFE version " << Version::getString() << std::endl;
            return 0;
        }
        else
        {
            std::cout << "Usage:" << std::endl;
            std::cout << program << "                                           Run RetroFE" << std::endl;
            std::cout << program << " --version                                 Print the version of RetroFE." << std::endl;
            std::cout << program << " -createcollection <collection name>       Create a collection directory structure." << std::endl;
            return 0;
        }
    }

    // Initialize locale language
    setlocale(LC_ALL, "");

    // Initialize random seed
    srand(static_cast<unsigned int>(time(nullptr)));

    Configuration::initialize();

    Configuration config;

    gst_init(nullptr, nullptr);

    // check to see if createcollection was requested
    if (argc == 3)
    {
        std::string param = argv[1];
        std::string value = argv[2];

        if (param == "-createcollection")
        {
            CollectionInfoBuilder::createCollectionDirectory(value);
        }

        return 0;
    }
    try {

        while (true)
        {
            if (!ImportConfiguration(&config))
            {
                // Exit with a heads up...
                std::string logFile = Utils::combinePath(Configuration::absolutePath, "log.txt");
                fprintf(stderr, "RetroFE has failed to start due to configuration error.\nCheck log for details: %s\n", logFile.c_str());
                return -1;
            }
            RetroFE p(config);
            if (p.run()) // Check if we need to reboot after running
                config.clearProperties();
            else
                break;
        }
    }
    catch (std::exception& e)
    {
        LOG_ERROR("EXCEPTION", e.what());
    }

    Logger::deInitialize();

    return 0;
}

static bool ImportConfiguration(Configuration* c)
{
    std::string configPath = Configuration::absolutePath;

#ifdef WIN32
    std::string osType = "windows";
#elif __APPLE__
    std::string osType = "apple";
#else
    std::string osType = "linux";
#endif

    fs::path launchersPath = Utils::combinePath(Configuration::absolutePath, "launchers." + osType);

    fs::path collectionsPath = Utils::combinePath(Configuration::absolutePath, "collections");

    std::string settingsConfPath = Utils::combinePath(configPath, "settings");
    if (!c->import("", settingsConfPath + ".conf"))
    {
        LOG_ERROR("RetroFE", "Could not import \"" + settingsConfPath + ".conf\"");
        return false;
    }
    for (int i = 1; i < 16; i++) {
        std::string settingsFile = settingsConfPath + std::to_string(i) + ".conf";
        if (fs::exists(settingsFile)) {
            c->import("", "", settingsFile, false);
        }
    }
    std::string savedSettingsFile = settingsConfPath + "_saved.conf";
    if (fs::exists(savedSettingsFile)) {
        c->import("", "", savedSettingsFile, false);
    }

    // log version
    LOG_INFO("RetroFE", "Version " + Version::getString() + " starting");

#ifdef WIN32
    LOG_INFO("RetroFE", "OS: Windows");
#elif __APPLE__
    LOG_INFO("RetroFE", "OS: Mac");
#else
    LOG_INFO("RetroFE", "OS: Linux");
#endif
    
    // Check if GStreamer initialization was successful
    if (gst_is_initialized())
    {
#ifdef WIN32
        std::string path = Utils::combinePath(Configuration::absolutePath, "retrofe");
        GstRegistry* registry = gst_registry_get();
        gst_registry_scan_path(registry, path.c_str());
#endif
        LOG_INFO("RetroFE", "GStreamer successfully initialized");
    }
    else
    {
        LOG_ERROR("RetroFE", "Failed to initialize GStreamer");
        return false;
    }

    LOG_INFO("RetroFE", "Absolute path: " + Configuration::absolutePath);

    // Process launchers
    if (fs::is_directory(launchersPath))
    {
        for (const auto& entry : fs::directory_iterator(launchersPath))
        {
            if (entry.is_regular_file())
            {
                fs::path filePath = entry.path();
                if (filePath.extension() == ".conf")
                {
                    std::string basename = filePath.stem().string();
                    std::string prefix = "launchers." + basename;
                    std::string importFile = filePath.string();

                    if (!c->import(prefix, importFile))
                    {
                        LOG_ERROR("RetroFE", "Could not import \"" + importFile + "\"");
                    }
                }
            }
        }
    }
    else
    {
        LOG_NOTICE("RetroFE", "Launchers directory does not exist or is not a directory: " + launchersPath.string());
    }


    // Process collections
    if (!fs::exists(collectionsPath) || !fs::is_directory(collectionsPath)) {
        LOG_ERROR("RetroFE", "Could not read directory \"" + collectionsPath.string() + "\"");
        return false;
    }

    for (const auto& entry : fs::directory_iterator(collectionsPath)) {
        std::string collection = entry.path().filename().string();
        if (fs::is_directory(entry) && !collection.empty() && collection[0] != '_' && collection != "." && collection != "..") {
            std::string prefix = "collections." + collection;
            bool settingsImported = false;
            std::string settingsFile = Utils::combinePath(collectionsPath, collection, "settings.conf");
            if (fs::exists(settingsFile)) {
                settingsImported |= c->import(collection, prefix, settingsFile, false);
            }

            for (int i = 1; i < 16; i++) {
                std::string numberedSettingsFile = Utils::combinePath(collectionsPath, collection, "settings" + std::to_string(i) + ".conf");
                if (fs::exists(numberedSettingsFile)) {
                    settingsImported |= c->import(collection, prefix, numberedSettingsFile, false);
                }
            }

            std::string infoFile = Utils::combinePath(collectionsPath, collection, "info.conf");
            if (fs::exists(infoFile)) {
                c->import(collection, prefix, infoFile, false);
            }

            // Process collection-specific launcher overrides
            std::string osSpecificLauncherFile = Utils::combinePath(collectionsPath, collection, "launcher." + osType + ".conf");
            std::string defaultLauncherFile = Utils::combinePath(collectionsPath, collection, "launcher.conf");
            std::string launcherKey = "collectionLaunchers." + collection; // Unique key for collection-specific launchers

            std::string importFile = fs::exists(osSpecificLauncherFile) ? osSpecificLauncherFile
                : fs::exists(defaultLauncherFile) ? defaultLauncherFile
                : "";

            // Import the launcher file if it exists under the unique key
            if (!importFile.empty()) {
                c->import(collection, launcherKey, importFile, false);
                LOG_INFO("RetroFE", "Imported collection-specific launcher for: " + collection);
                if (!c->propertyExists("collections." + collection + ".launcher"))
                {
                    c->setProperty("collections." + collection + ".launcher", collection);
                }
            }

            fs::path localLaunchersPath = Utils::combinePath(collectionsPath, collection, "launchers." + osType + ".local");
            fs::path defaultLocalLaunchersPath = Utils::combinePath(collectionsPath, collection, "launchers.local");

            // Check if OS-specific launchers directory exists, otherwise use the default launchers directory
            if (!fs::is_directory(localLaunchersPath) && fs::is_directory(defaultLocalLaunchersPath)) {
                localLaunchersPath = defaultLocalLaunchersPath;
            }

            if (fs::is_directory(localLaunchersPath))
            {
                for (const auto& launcherEntry : fs::directory_iterator(localLaunchersPath)) {
                    if (launcherEntry.is_regular_file()) {
                        fs::path filePath = launcherEntry.path();
                        if (filePath.extension() == ".conf") {
                            std::string basename = filePath.stem().string();
                            std::string prefix = "localLaunchers." + collection + "." + basename;
                            std::string importFile = filePath.string();

                            if (!c->import(collection, prefix, importFile)) {
                                LOG_ERROR("RetroFE", "Could not import local launcher \"" + importFile + "\" for collection \"" + collection + "\"");
                            }
                            else {
                                LOG_INFO("RetroFE", "Imported local launcher \"" + basename + "\" for collection \"" + collection + "\"");
                            }
                        }
                    }
                }
            }
            // Set the launcher property if it's not already set in settings.conf
            std::string launcherPropertyKey = "collections." + collection + ".launcher";
            if (!c->propertyExists(launcherPropertyKey) && !importFile.empty()) {
                c->setProperty(launcherPropertyKey, collection);
            }

            // Update collectionLaunchers property
            std::string collectionLaunchers = "collectionLaunchers";
            std::string launchers = "";
            c->getProperty(collectionLaunchers, launchers);
            if (!importFile.empty() && (collection.size() < 3 || collection.substr(collection.size() - 3) != "SUB")) {
                c->setProperty(collectionLaunchers, launchers + collection + ",");
            }



            if (!settingsImported) {
                LOG_ERROR("RetroFE", "Could not import any collection settings for " + collection);
            }
            else {
                LOG_INFO("RetroFE", "Imported settings for collection: " + collection);
            }

            if (!importFile.empty()) {
                LOG_INFO("RetroFE", "Imported launcher configuration for collection: " + collection);
            }
        }
    }

    LOG_INFO("RetroFE", "Imported configuration");
    bool dumpProperties = false;
    c->getProperty("dumpProperties", dumpProperties);
    if (dumpProperties) {
            c->dumpPropertiesToFile(Utils::combinePath(Configuration::absolutePath, "properties.txt"));
    }
    return true;
}

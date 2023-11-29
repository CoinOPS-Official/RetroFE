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
        return -1;
    }

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
    std::string launchersPath = Utils::combinePath(Configuration::absolutePath, "launchers.windows");
#elif __APPLE__
    std::string launchersPath = Utils::combinePath(Configuration::absolutePath, "launchers.apple");
#else
    std::string launchersPath = Utils::combinePath(Configuration::absolutePath, "launchers.linux");
#endif
    std::string collectionsPath = Utils::combinePath(Configuration::absolutePath, "collections");

    std::string settingsConfPath = Utils::combinePath(configPath, "settings");
    if (!c->import("", settingsConfPath + ".conf"))
    {
        LOG_ERROR("RetroFE", "Could not import \"" + settingsConfPath + ".conf\"");
        return false;
    }
    for (int i = 1; i < 16; i++)
        c->import("", "", settingsConfPath + std::to_string(i) + ".conf", false);
    c->import("", "", settingsConfPath + "_saved.conf", false);

    // log version
    LOG_INFO("RetroFE", "Version " + Version::getString() + " starting");

#ifdef WIN32
    LOG_INFO("RetroFE", "OS: Windows");
#elif __APPLE__
    LOG_INFO("RetroFE", "OS: Mac");
#else
    LOG_INFO("RetroFE", "OS: Linux");
#endif

    LOG_INFO("RetroFE", "Absolute path: " + Configuration::absolutePath);

    // Process launchers
    if (!fs::exists(launchersPath) || !fs::is_directory(launchersPath))
    {
        launchersPath = Utils::combinePath(Configuration::absolutePath, "launchers");
        if (!fs::exists(launchersPath) || !fs::is_directory(launchersPath))
        {
            LOG_NOTICE("RetroFE", "Could not read directory \"" + launchersPath + "\"");
            return false;
        }
    }

    for (const auto& entry : fs::directory_iterator(launchersPath))
    {
        if (fs::is_regular_file(entry))
        {
            std::string file = entry.path().filename().string();
            size_t dot_position = file.find_last_of(".");

            if (dot_position != std::string::npos)
            {
                std::string extension = Utils::toLower(file.substr(dot_position));
                std::string basename = file.substr(0, dot_position);

                if (extension == ".conf")
                {
                    std::string prefix = "launchers." + Utils::toLower(basename);
                    std::string importFile = entry.path().string();

                    if (!c->import(prefix, importFile))
                    {
                        LOG_ERROR("RetroFE", "Could not import \"" + importFile + "\"");
                        return false;
                    }
                }
            }
        }
    }


    // Process collections
    if (!fs::exists(collectionsPath) || !fs::is_directory(collectionsPath))
    {
        LOG_ERROR("RetroFE", "Could not read directory \"" + collectionsPath + "\"");
        return false;
    }

    for (const auto& entry : fs::directory_iterator(collectionsPath))
    {
        std::string collection = entry.path().filename().string();
        if (fs::is_directory(entry) && !collection.empty() && collection[0] != '_' && collection != "." && collection != "..")
        {
            std::string prefix = "collections." + collection;
            bool settingsImported = false;
            std::string settingsPath = Utils::combinePath(collectionsPath, collection, "settings");

            settingsImported |= c->import(collection, prefix, settingsPath + ".conf", false);
            for (int i = 1; i < 16; i++)
            {
                settingsImported |= c->import(collection, prefix, settingsPath + std::to_string(i) + ".conf", false);
            }

            std::string infoFile = Utils::combinePath(collectionsPath, collection, "info.conf");
            c->import(collection, prefix, infoFile, false);

            if (!settingsImported)
            {
                LOG_ERROR("RetroFE", "Could not import any collection settings for " + collection);
            }
        }
    }


    LOG_INFO("RetroFE", "Imported configuration");

    return true;
}

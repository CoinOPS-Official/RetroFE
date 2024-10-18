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

#include "PageBuilder.h"
#include "Page.h"
#include "ViewInfo.h"
#include "Component/Container.h"
#include "Component/Image.h"
#include "Component/Text.h"
#include "Component/ReloadableText.h"
#include "Component/ReloadableMedia.h"
#include "Component/ReloadableScrollingText.h"
#include "Component/ScrollingList.h"
#include "Component/VideoBuilder.h"
#include "Animate/AnimationEvents.h"
#include "Animate/TweenTypes.h"
#include "../Sound/Sound.h"
#include "../Collection/Item.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "../Database/GlobalOpts.h"
#include <algorithm>
#include <cfloat>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <map>
#include <filesystem>
#include <memory>

using namespace rapidxml;

static const int MENU_FIRST = 0;   // first visible item in the list
static const int MENU_LAST = -3;   // last visible item in the list
static const int MENU_START = -1;  // first item transitions here after it scrolls "off the menu/screen"
static const int MENU_END = -2;    // last item transitions here after it scrolls "off the menu/screen"
//static const int MENU_CENTER = -4;

//todo: this file is starting to become a god class of building. Consider splitting into sub-builders
PageBuilder::PageBuilder(const std::string& layoutKey, const std::string& layoutPage, Configuration &c, FontCache *fc, bool isMenu)
    : layoutKey(layoutKey)
    , layoutPage(layoutPage)
    , config_(c)
    , fontCache_(fc)
    , isMenu_(isMenu)
{
    screenWidth_  = SDL::getWindowWidth(0);
    screenHeight_ = SDL::getWindowHeight(0);
    fontColor_.a  = 255;
    fontColor_.r  = 0;
    fontColor_.g  = 0;
    fontColor_.b  = 0;
}

PageBuilder::~PageBuilder() = default;

Page *PageBuilder::buildPage( const std::string& collectionName, bool defaultToCurrentLayout)
{
    Page *page = nullptr;

    std::string layoutFile;
    std::string layoutFileAspect;
    std::string layoutName = layoutKey;
    std::string layoutPathDefault = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName);
    bool fixedResLayouts = false;
    config_.getProperty(OPTION_FIXEDRESLAYOUTS, fixedResLayouts);
    namespace fs = std::filesystem;
    
    // These just prevented repeated logging
    bool splashInitialized = false;
    bool fixedResLayoutsInitialized = false;

    if ( isMenu_ ) {
        layoutPath = Utils::combinePath(Configuration::absolutePath, "menu");
    }
    else if ( collectionName != "" ) {
        layoutPath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", collectionName);
        layoutPath = Utils::combinePath(layoutPath, "layout");

        if (defaultToCurrentLayout) {
            std::ifstream file((layoutPath + ".xml").c_str());
            // check collection has layout otherwise it would have used default folder layout
            if (!file.good()) {
                return nullptr;
            }
        }
    }

    std::vector<std::string> layouts;
    layouts.push_back(layoutPage);
    // layout - #.xml
    for (int i = 0; i < Page::MAX_LAYOUTS; i++) {
        layouts.push_back("layout - " + std::to_string(i));
    }
    for ( unsigned int layout = 0; layout < layouts.size(); layout++ ) {
        rapidxml::xml_document<> doc;
        std::ifstream file;
        
        // Override layout with layoutFromAnotherCollection = <collection> in layouts/Arcades/collections/<collection>
        std::string layoutFromAnotherCollection;
        config_.getProperty("collections." + collectionName + ".layoutFromAnotherCollection", layoutFromAnotherCollection);
        if (layoutFromAnotherCollection != "") {
            LOG_INFO("Layout", "Using layout from collection: " + layoutFromAnotherCollection + " " + layouts[layout] + ".xml");
            std::string layoutFileFromAnotherCollection = Utils::combinePath(layoutPathDefault, "collections", layoutFromAnotherCollection, "layout");
            layoutPath = layoutFileFromAnotherCollection;
        }
        
        layoutFile = Utils::combinePath(layoutPath, layouts[layout] + ".xml");
        
        if(fixedResLayouts) {
            // Use fixed resolution layout ie layout1920x1080.xml
            if (!fixedResLayoutsInitialized) {
                LOG_INFO("Layout", "Fixed resolution layouts have been enabled");
                fixedResLayoutsInitialized = true;
            }
            layoutFileAspect = Utils::combinePath(layoutPathDefault,
                std::to_string(screenWidth_ / Utils::gcd(screenWidth_, screenHeight_)) + "x" +
                std::to_string(screenHeight_ / Utils::gcd(screenWidth_, screenHeight_)) + layouts[layout] +
                ".xml");
            if (fs::exists(layoutFileAspect)) {
                layoutFile = layoutFileAspect;
            }
            else {
                LOG_ERROR("Layout", "Unable to find fixed resolution layout: " + layoutFileAspect);
                exit(EXIT_FAILURE);
            }
        }
        
        if (fs::exists(layoutFile)) {
            // Check for layouts/<layout>/collections/<collectionName>/layout
            LOG_INFO("Layout", "Attempting to initialize collection layout: " + layoutFile);
            file.open( layoutFile.c_str() );
            std::ifstream file(layoutFile.c_str());
            file.close();
        }
        else {
            if (layoutPath != layoutPathDefault) {
                if ( layouts[layout] != "splash") {
                    if ( layoutFile = Utils::combinePath(layoutPathDefault, layouts[layout] + ".xml"); fs::exists(layoutFile) ) {
                        // Check for layouts/<layout>/layout
                        LOG_INFO("Layout", "Attempting to initialize default layout: " + layoutFile);
                        file.open( layoutFile.c_str() );
                        std::ifstream file(layoutFile.c_str());
                        file.close();
                    }
                    else if(!fs::exists(layoutFile)) {
                        // If layouts/<layout>/layout - x.xml doesn't exist log here but continue
                        LOG_WARNING("Layout", "Layout not found: " + layoutFile);
                        continue;
                    }
                }
                else if (!splashInitialized && fs::exists(Utils::combinePath(layoutPathDefault, "splash.xml"))) {
                    // Check for splash page then don't check again
                    std::string layoutSplashFile;
                    layoutSplashFile = Utils::combinePath(layoutPathDefault, "splash.xml");
                    LOG_INFO("Layout", "Attempting to initialize splash: " + layoutSplashFile);
                    file.open( layoutSplashFile.c_str() );
                    std::ifstream file(layoutSplashFile.c_str());
                    file.close();
                    splashInitialized = true;
                }
            }
        }
        
        std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        try {
            buffer.push_back('\0');

            doc.parse<0>(&buffer[0]);

            xml_node<>* root = doc.first_node("layout");

            if (!root) {
                LOG_ERROR("Layout", "Missing <layout> tag");
                return nullptr;
            }
            else {
                xml_attribute<> const* layoutWidthXml = root->first_attribute("width");
                xml_attribute<> const* layoutHeightXml = root->first_attribute("height");
                xml_attribute<> const* fontXml = root->first_attribute("font");
                xml_attribute<> const* fontColorXml = root->first_attribute("fontColor");
                xml_attribute<> const* fontSizeXml = root->first_attribute("loadFontSize");
                xml_attribute<> const* minShowTimeXml = root->first_attribute("minShowTime");
                xml_attribute<> const* controls = root->first_attribute("controls");
                xml_attribute<> const* layoutMonitorXml = root->first_attribute("monitor");

                if (fontXml) {
                    fontName_ = config_.convertToAbsolutePath(
                        Utils::combinePath(config_.absolutePath, "layouts", layoutKey, ""),
                        fontXml->value());

                    // Check if the font file exists
                    if (!std::filesystem::exists(fontName_)) {
                        // Fallback to a standard font if the file doesn't exist
                        LOG_ERROR("RetroFE", "Specified font at \n    " + fontName_ + "\n does not exist. Falling back to standard font.");
                        fontName_ = config_.convertToAbsolutePath(
                            Utils::combinePath(config_.absolutePath, "layouts", layoutKey, ""),
                            "fonts/standard.ttf");
                    }
                }

                if (fontColorXml) {
                    int intColor = 0;
                    std::stringstream ss;
                    ss << std::hex << fontColorXml->value();
                    ss >> intColor;

                    fontColor_.b = intColor & 0xFF;
                    intColor >>= 8;
                    fontColor_.g = intColor & 0xFF;
                    intColor >>= 8;
                    fontColor_.r = intColor & 0xFF;
                }

                if (fontSizeXml) {
                    fontSize_ = Utils::convertInt(fontSizeXml->value());
                }

                if (layoutWidthXml && layoutHeightXml) {
                    std::string layoutWidthStr = layoutWidthXml->value();
                    std::string layoutHeightStr = layoutHeightXml->value();

                    // Determine the monitor from the layout tag or use a default
                    int monitor = layoutMonitorXml ? Utils::convertInt(layoutMonitorXml->value()) : monitor_;

                    // Get the width and height based on whether "stretch" is specified
                    if (layoutWidthStr == "stretch") {
                        layoutWidth_ = SDL::getWindowWidth(monitor);
                    } else {
                        layoutWidth_ = Utils::convertInt(layoutWidthStr);
                    }

                    if (layoutHeightStr == "stretch") {
                        layoutHeight_ = SDL::getWindowHeight(monitor);
                    } else {
                        layoutHeight_ = Utils::convertInt(layoutHeightStr);
                    }

                    if (layoutWidth_ != 0 && layoutHeight_ != 0) {
                        std::stringstream ss;
                        ss << layoutWidth_ << "x" << layoutHeight_ << " (scale " << (float)SDL::getWindowWidth(monitor) / (float)layoutWidth_ << "x" << (float)SDL::getWindowHeight(monitor) / (float)layoutHeight_ << ")";
                        LOG_INFO("Layout", "Layout resolution " + ss.str());

                        if (!page)
                            page = new Page(config_, layoutWidth_, layoutHeight_);
                        else {
                            page->setLayoutWidth(layout, layoutWidth_);
                            page->setLayoutHeight(layout, layoutHeight_);

                            if (monitor) {
                                page->setLayoutWidthByMonitor(monitor, layoutWidth_);
                                page->setLayoutHeightByMonitor(monitor, layoutHeight_);
                            }
                        }
                    }
                }

                if (minShowTimeXml) {
                    page->setMinShowTime(Utils::convertFloat(minShowTimeXml->value()));
                }

                // add additional controls to replace others based on theme/layout
				if (controls && controls->value() && controls->value()[0] != '\0'){
                    std::string controlLayout = controls->value();
                    LOG_INFO("Layout", "Layout set custom control type " + controlLayout);
                    page->setControlsType(controlLayout);
                }

                // load sounds
                for (xml_node<> const* sound = root->first_node("sound"); sound; sound = sound->next_sibling("sound")) {
                    xml_attribute<> const* src = sound->first_attribute("src");
                    xml_attribute<> const* type = sound->first_attribute("type");
                    std::string file = Configuration::convertToAbsolutePath(layoutPath, src->value());

                    // check if collection's assets are in a different theme
                    std::string layoutName;
                    config_.getProperty("collections." + collectionName + ".layout", layoutName);
                    if (layoutName == "") {
                        config_.getProperty(OPTION_LAYOUT, layoutName);
                    }
                    std::string altfile = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, std::string(src->value()));
                    if (!type) {
                        LOG_ERROR("Layout", "Sound tag missing type attribute");
                    }
                    else {
                        // TODO MuteSound key, also this is such a mess
                        auto* sound = new Sound(file, altfile);
                        std::string soundType = type->value();

                        if (!soundType.compare("load")) {
                            page->setLoadSound(sound);
                        }
                        else if (!soundType.compare("unload")) {
                            page->setUnloadSound(sound);
                        }
                        else if (!soundType.compare("highlight")) {
                            page->setHighlightSound(sound);
                        }
                        else if (!soundType.compare("select")) {
                            page->setSelectSound(sound);
                        }
                        else {
                            LOG_WARNING("Layout", "Unsupported sound effect type \"" + soundType + "\"");
                        }
                    }
                }

                if (!buildComponents(root, page, collectionName)) {
                    delete page;
                    page = nullptr;
                }
            }
        }
        catch(rapidxml::parse_error &e) {
            std::string what = e.what();
            auto line = static_cast<long>(std::count(&buffer.front(), e.where<char>(), char('\n')) + 1);
            std::stringstream ss;
            ss << "Could not parse layout file. [Line: " << line << "] in " << layoutFile << " Reason: " << e.what();

            LOG_ERROR("Layout", ss.str());
        }
        catch(std::exception &e) {
            std::string what = e.what();
            LOG_ERROR("Layout", "Could not parse layout file. Reason: " + what);
        }

        if(page) {
            if(fixedResLayouts) {
                LOG_INFO("Layout", "Initialized " + layoutFileAspect);
            }
            else {
                LOG_INFO("Layout", "Initialized " + layoutFile);
            }
        }
        else {
            LOG_ERROR("Layout", "Could not initialize layout (see previous messages for reason)");
        }
    }

    return page;
}



float PageBuilder::getHorizontalAlignment(const xml_attribute<> *attribute, float valueIfNull) const
{
    float value;
    std::string str;

    if (!attribute) {
        value = valueIfNull;
    }
    else {
        str = attribute->value();

        if (!str.compare("left")) {
            value = 0;
        }
        else if (!str.compare("center")) {
            value = static_cast<float>(layoutWidth_) / 2;
        }
        else if (!str.compare("right") || !str.compare("stretch")) {
            value = static_cast<float>(layoutWidth_);
        }
        else if (str.back() == '%') {
            float percent = Utils::convertFloat(str.substr(0, str.length() - 1));
            value = std::round(static_cast<float>(layoutWidth_) * (percent / 100.0f));
        }
        else {
            value = Utils::convertFloat(str);
        }
    }

    return value;
}

float PageBuilder::getVerticalAlignment(const xml_attribute<> *attribute, float valueIfNull) const
{
    float value;
    std::string str;

    if (!attribute) {
        value = valueIfNull;
    }
    else {
        str = attribute->value();

        if (!str.compare("top")) {
            value = 0;
        }
        else if (!str.compare("center")) {
            value = static_cast<float>(layoutHeight_) / 2;
        }
        else if (!str.compare("bottom") || !str.compare("stretch")) {
            value = static_cast<float>(layoutHeight_);
        }
        else if (str.back() == '%') {
            std::string_view percentStr(str.data(), str.length() - 1);
            float percent = Utils::convertFloat(percentStr);
            value = std::round(static_cast<float>(layoutHeight_) * (percent / 100.0f));
        }
        else {
            value = Utils::convertFloat(str);
        }
    }

    return value;
}

bool PageBuilder::buildComponents(xml_node<>* layout, Page* page, const std::string& collectionName)

{
    xml_attribute<> const* layoutMonitorXml = layout->first_attribute("monitor");
    int layoutMonitor = layoutMonitorXml ? Utils::convertInt(layoutMonitorXml->value()) : monitor_;
    // Check if the specified monitor exists (for this "layout")
    if (layoutMonitor + 1 > SDL::getScreenCount()) {
        LOG_WARNING("Layout", "Skipping layout due to non-existent monitor index: " + std::to_string(layoutMonitor));
        return true; // Skip this layout
    }

    for (xml_node<>* componentXml = layout->first_node("menu"); componentXml; componentXml = componentXml->next_sibling("menu")) {
        // Extract "monitor" attribute specifically for this "menu" node
        xml_attribute<> const* menuMonitorXml = componentXml->first_attribute("monitor");
        int menuMonitor = menuMonitorXml ? Utils::convertInt(menuMonitorXml->value()) : layoutMonitor;
        // Check if the specified monitor exists (for this "menu")
        if (menuMonitor + 1 > SDL::getScreenCount()) {
            LOG_WARNING("Layout", "Skipping menu due to non-existent monitor index: " + std::to_string(menuMonitor));
            continue; // Skip this menu and go to the next
        }

        // If the monitor exists, proceed to build the menu
        ScrollingList* scrollingList = buildMenu(componentXml, *page, menuMonitor);
        xml_attribute<> const* indexXml = componentXml->first_attribute("menuIndex");
        int index = indexXml ? Utils::convertInt(indexXml->value()) : -1;
        if (scrollingList && scrollingList->isPlaylist()) {
            page->setPlaylistMenu(scrollingList);
        }
        if (scrollingList) {
            page->pushMenu(scrollingList, index);
        }
    }

    for(xml_node<> *componentXml = layout->first_node("container"); componentXml; componentXml = componentXml->next_sibling("container")) {
        auto* c = new Container(*page);
        if (auto const* menuScrollReload = componentXml->first_attribute("menuScrollReload");
            menuScrollReload && (Utils::toLower(menuScrollReload->value()) == "true" ||
                Utils::toLower(menuScrollReload->value()) == "yes")) {
                    c->setMenuScrollReload(true);
        }
        xml_attribute<> const* monitorXml = componentXml->first_attribute("monitor");
        c->baseViewInfo.Monitor = monitorXml ? Utils::convertInt(monitorXml->value()) : layoutMonitor;
        c->baseViewInfo.Layout = page->getCurrentLayout();

        buildViewInfo(componentXml, c->baseViewInfo);
        loadTweens(c, componentXml);
        page->addComponent(c);
    }
    for(xml_node<> *componentXml = layout->first_node("image"); componentXml; componentXml = componentXml->next_sibling("image")) {
        xml_attribute<> const *src        = componentXml->first_attribute("src");
        xml_attribute<> const *idXml      = componentXml->first_attribute("id");
        xml_attribute<> const *monitorXml = componentXml->first_attribute("monitor");
        xml_attribute<> const* additiveXml = componentXml->first_attribute("additive");

        int id = -1;
        if (idXml) {
            id = Utils::convertInt(idXml->value());
        }

        int imageMonitor = monitorXml ? Utils::convertInt(monitorXml->value()) : layoutMonitor; // Use layout's monitor if not specified

        // Check if the specified monitor exists (for this "image")
        if (imageMonitor + 1 > SDL::getScreenCount()) {
            LOG_WARNING("Layout", "Skipping image due to non-existent monitor index: " + std::to_string(imageMonitor));
            continue; // Skip this image and go to the next
        }

        if (!src) {
            LOG_ERROR("Layout", "Image component in layout does not specify a source image file");
        } else {
            std::string imagePath;

            std::string layoutFromAnotherCollection;
            config_.getProperty("collections." + collectionName + ".layoutFromAnotherCollection", layoutFromAnotherCollection);
            if (!layoutFromAnotherCollection.empty()) {
                namespace fs = std::filesystem;
                // Handle layout from another collection
                std::string layoutPathDefault = Utils::combinePath(Configuration::absolutePath, "layouts", layoutKey);
                layoutPath = Utils::combinePath(layoutPathDefault, "collections", collectionName, "layout");
                if (!fs::exists(layoutPath)) {
                    std::string layout_artworkCollectionPath = Utils::combinePath(Configuration::absolutePath, "collections", collectionName, "layout_artwork");
                    LOG_INFO("Layout", "Layout Artwork not found in layouts/" + layoutKey + "/collections/" + collectionName + "/layout/");
                    LOG_INFO("Layout", "Using layout_artwork folder in: " + layout_artworkCollectionPath);
                    layoutPath = layout_artworkCollectionPath;
                }
            }

            imagePath = Utils::combinePath(Configuration::convertToAbsolutePath(layoutPath, imagePath), std::string(src->value()));

            // Check if collection's assets are in a different theme
            std::string layoutName;
            config_.getProperty("collections." + collectionName + ".layout", layoutName);
            if (layoutName.empty()) {
                config_.getProperty(OPTION_LAYOUT, layoutName);
            }
            std::string altImagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, std::string(src->value()));

            bool additive = additiveXml ? bool(additiveXml->value()) : false;
            auto* c = new Image(imagePath, altImagePath, *page, imageMonitor, additive);
            c->allocateGraphicsMemory();
            c->setId(id);

            if (auto const* menuScrollReload = componentXml->first_attribute("menuScrollReload");
                menuScrollReload && (Utils::toLower(menuScrollReload->value()) == "true" || Utils::toLower(menuScrollReload->value()) == "yes")) {
                c->setMenuScrollReload(true);
            }

            // Explicitly set the monitor and layout
            c->baseViewInfo.Monitor = monitorXml ? Utils::convertInt(monitorXml->value()) : layoutMonitor;
            c->baseViewInfo.Layout = page->getCurrentLayout();

            buildViewInfo(componentXml, c->baseViewInfo);
            loadTweens(c, componentXml);
            page->addComponent(c);
        }
    }



    for(xml_node<> *componentXml = layout->first_node("video"); componentXml; componentXml = componentXml->next_sibling("video"))
    {
        xml_attribute<> const *srcXml      = componentXml->first_attribute("src");
        xml_attribute<> const *numLoopsXml = componentXml->first_attribute("numLoops");
        xml_attribute<> const *idXml = componentXml->first_attribute("id");
        xml_attribute<> const *monitorXml = componentXml->first_attribute("monitor");
        xml_attribute<> const *softOverlayXml = componentXml->first_attribute("softOverlay"); // New attribute

        int id = -1;
        if (idXml) {
            id = Utils::convertInt(idXml->value());
        }

        if (!srcXml) {
            LOG_ERROR("Layout", "Video component in layout does not specify a source video file");
        }
        else {
            VideoBuilder videoBuild{};
            std::string videoPath = Utils::combinePath(Configuration::convertToAbsolutePath(layoutPath, ""), std::string(srcXml->value()));

            // Check if collection's assets are in a different theme
            std::string layoutName;
            config_.getProperty("collections." + collectionName + ".layout", layoutName);
            if (layoutName.empty()) {
                config_.getProperty(OPTION_LAYOUT, layoutName);
            }
            
            bool softOverlay = false;
            if (softOverlayXml && (Utils::toLower(softOverlayXml->value()) == "true" || Utils::toLower(softOverlayXml->value()) == "yes")) {
                softOverlay = true;
            }
            
            std::string altVideoPath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, std::string(srcXml->value()));
            int numLoops = numLoopsXml ? Utils::convertInt(numLoopsXml->value()) : 1;

            int videoMonitor = monitorXml ? Utils::convertInt(monitorXml->value()) : layoutMonitor; // Use layout's monitor if not specified at the menu level

            // Check if the specified monitor exists (for this "image")
            if (videoMonitor + 1 > SDL::getScreenCount()) {
                LOG_WARNING("Layout", "Skipping video due to non-existent monitor index: " + std::to_string(videoMonitor));
                continue; // Skip this image and go to the next
            }

            // Don't add videos if display doesn't exist
            
                std::filesystem::path primaryPath(videoPath);
                std::filesystem::path altPath(altVideoPath);

                VideoComponent* c = videoBuild.createVideo(primaryPath.parent_path().string(), *page, primaryPath.stem().string(), videoMonitor, numLoops, softOverlay);

                if (!c) {
                    // Try alternative video path if the primary path did not yield a VideoComponent
                    c = videoBuild.createVideo(altPath.parent_path().string(), *page, altPath.stem().string(), videoMonitor, numLoops, softOverlay);
                }

                if (c) {
                    c->allocateGraphicsMemory();
                    c->setId(id);

                    // Additional settings and configurations
                    if (auto const* pauseOnScroll = componentXml->first_attribute("pauseOnScroll");
                        pauseOnScroll && (Utils::toLower(pauseOnScroll->value()) == "false" || Utils::toLower(pauseOnScroll->value()) == "no")) {
                        c->setPauseOnScroll(false);
                    }

                    if (auto const* menuScrollReload = componentXml->first_attribute("menuScrollReload"); menuScrollReload &&
                        (Utils::toLower(menuScrollReload->value()) == "true" || Utils::toLower(menuScrollReload->value()) == "yes")) {
                        c->setMenuScrollReload(true);
                    }

                    if (auto const* animationDoneRemove = componentXml->first_attribute("animationDoneRemove"); animationDoneRemove &&
                        (Utils::toLower(animationDoneRemove->value()) == "true" || Utils::toLower(animationDoneRemove->value()) == "yes")) {
                        c->setAnimationDoneRemove(true);
                    }
                    c->baseViewInfo.Monitor = monitorXml ? Utils::convertInt(monitorXml->value()) : layoutMonitor;
                    c->baseViewInfo.Layout = page->getCurrentLayout();

                    buildViewInfo(componentXml, c->baseViewInfo);
                    loadTweens(c, componentXml);
                    page->addComponent(c);
                }
            
        }

    }

    for(xml_node<> *componentXml = layout->first_node("text"); componentXml; componentXml = componentXml->next_sibling("text")) {
        xml_attribute<> const *value      = componentXml->first_attribute("value");
        xml_attribute<> const *idXml      = componentXml->first_attribute("id");
        xml_attribute<> const *monitorXml = componentXml->first_attribute("monitor");

        int id = -1;
        if (idXml) {
            id = Utils::convertInt(idXml->value());
        }

        if (!value) {
            LOG_WARNING("Layout", "Text component in layout does not specify a value");
        }
        else {
            int textMonitor = monitorXml ? Utils::convertInt(monitorXml->value()) : layoutMonitor;
            Font *font = addFont(componentXml, NULL, textMonitor);

            auto *c = new Text(value->value(), *page, font, textMonitor);
            c->setId( id );
            if (xml_attribute<> const *menuScrollReload = componentXml->first_attribute("menuScrollReload"); menuScrollReload &&
                (Utils::toLower(menuScrollReload->value()) == "true" ||
                 Utils::toLower(menuScrollReload->value()) == "yes"))
            {
                c->setMenuScrollReload(true);
            }

            buildViewInfo(componentXml, c->baseViewInfo);
            loadTweens(c, componentXml);
            page->addComponent(c);
        }
    }

    for(xml_node<> *componentXml = layout->first_node("statusText"); componentXml; componentXml = componentXml->next_sibling("statusText")) {
        xml_attribute<> const *monitorXml = componentXml->first_attribute("monitor");
        int statusTextMonitor = monitorXml ? Utils::convertInt(monitorXml->value()) : layoutMonitor;
        Font* font = addFont(componentXml, NULL, statusTextMonitor);

        auto* c = new Text("", *page, font, statusTextMonitor);
        if (auto const* menuScrollReload = componentXml->first_attribute("menuScrollReload");
            menuScrollReload && (Utils::toLower(menuScrollReload->value()) == "true" ||
                Utils::toLower(menuScrollReload->value()) == "yes")) 
        {
            c->setMenuScrollReload(true);
        }

        buildViewInfo(componentXml, c->baseViewInfo);
        loadTweens(c, componentXml);
        page->addComponent(c);
        page->setStatusTextComponent(c);
    }


    loadReloadableImages(layout, "reloadableImage",         page);
    loadReloadableImages(layout, "reloadableAudio",         page);
    loadReloadableImages(layout, "reloadableVideo",         page);
    loadReloadableImages(layout, "reloadableText",          page);
    loadReloadableImages(layout, "reloadableScrollingText", page);

    return true;
}

void PageBuilder::loadReloadableImages(const xml_node<> *layout, const std::string& tagName, Page *page)
{
    xml_attribute<> const* layoutMonitorXml = layout->first_attribute("monitor");
    int monitor = layoutMonitorXml ? Utils::convertInt(layoutMonitorXml->value()) : monitor_;
    int cMonitor = 0;
    for(xml_node<> *componentXml = layout->first_node(tagName.c_str()); componentXml; componentXml = componentXml->next_sibling(tagName.c_str())) {
        std::string reloadableImagePath;
        std::string reloadableVideoPath;
        xml_attribute<> const *type              = componentXml->first_attribute("type");
        xml_attribute<> const *imageType         = componentXml->first_attribute("imageType");
        xml_attribute<> const *mode              = componentXml->first_attribute("mode");
        xml_attribute<> const *timeFormatXml     = componentXml->first_attribute("timeFormat");
        xml_attribute<> const *textFormatXml     = componentXml->first_attribute("textFormat");
        xml_attribute<> const *singlePrefixXml   = componentXml->first_attribute("singlePrefix");
        xml_attribute<> const *singlePostfixXml  = componentXml->first_attribute("singlePostfix");
        xml_attribute<> const *pluralPrefixXml   = componentXml->first_attribute("pluralPrefix");
        xml_attribute<> const *pluralPostfixXml  = componentXml->first_attribute("pluralPostfix");
        xml_attribute<> const *selectedOffsetXml = componentXml->first_attribute("selectedOffset");
        xml_attribute<> const *directionXml      = componentXml->first_attribute("direction");
        xml_attribute<> const *scrollingSpeedXml = componentXml->first_attribute("scrollingSpeed");
        xml_attribute<> const *startPositionXml  = componentXml->first_attribute("startPosition");
        xml_attribute<> const *startTimeXml      = componentXml->first_attribute("startTime");
        xml_attribute<> const *endTimeXml        = componentXml->first_attribute("endTime");
        xml_attribute<> const *alignmentXml      = componentXml->first_attribute("alignment");
        xml_attribute<> const *idXml             = componentXml->first_attribute("id");
        xml_attribute<> const *randomSelectXml = componentXml->first_attribute("randomSelect");
        xml_attribute<> const *monitorXml = componentXml->first_attribute("monitor");
        xml_attribute<> const* locationXml = componentXml->first_attribute("location");

        bool systemMode = false;
        bool layoutMode = false;
        bool commonMode = false;
        bool menuMode   = false;
        int selectedOffset = 0;

        int id = -1;
        if (idXml) {
            id = Utils::convertInt(idXml->value());
        }

        if(!imageType && (tagName == "reloadableVideo" || tagName == "reloadableAudio")) {
            LOG_WARNING("Layout", "<reloadableImage> component in layout does not specify an imageType for when the video does not exist");
        }
        if(!type && (tagName == "reloadableImage" || tagName == "reloadableText")) {
            LOG_ERROR("Layout", "Image component in layout does not specify a source image file");
        }
        if(!type && tagName == "reloadableScrollingText") {
            LOG_ERROR("Layout", "Reloadable scroling text component in layout does not specify a type");
        }

        cMonitor = monitorXml ? Utils::convertInt(monitorXml->value()) : monitor;

        if(mode) {
            std::string sysMode = mode->value();
            if(sysMode == "system") {
                systemMode = true;
            }
            if(sysMode == "layout") {
                layoutMode = true;
            }
            if(sysMode == "common") {
                commonMode = true;
            }
            if(sysMode == "commonlayout") {
                layoutMode = true;
                commonMode = true;
            }
            if(sysMode == "systemlayout") {
                systemMode = true;
                layoutMode = true;
            }
            if(sysMode == "menu") {
                menuMode = true;
            }
        }

        if(selectedOffsetXml) {
            std::stringstream ss;
            ss << selectedOffsetXml->value();
            ss >> selectedOffset;
        }


        Component *c = nullptr;

        if (tagName == "reloadableText") {
            if (type) {
                Font* font = addFont(componentXml, NULL, cMonitor);
                std::string typeValue = type->value();
                std::string textFormat = "";

                if (typeValue == "file") {
                    // Ensure the location attribute is present
                    if (!locationXml) {
                        LOG_ERROR("Layout", "reloadableText type='file' requires a 'location' attribute.");
                        continue; // Skip this component if location is not provided
                    }

                    // Get the location value
                    std::string location = locationXml->value();

                    // Create the ReloadableText component with the location path
                    c = new ReloadableText(typeValue, *page, config_, systemMode, font, layoutKey, "", "", "", "", "", "", location);
                }
                else {
                    // Existing handling for other types

                    if (textFormatXml) {
                        textFormat = textFormatXml->value();
                    }
                    std::string singlePrefix = "";
                    if (singlePrefixXml) {
                        singlePrefix = singlePrefixXml->value();
                    }
                    std::string singlePostfix = "";
                    if (singlePostfixXml) {
                        singlePostfix = singlePostfixXml->value();
                    }
                    std::string pluralPrefix = "";
                    if (pluralPrefixXml) {
                        pluralPrefix = pluralPrefixXml->value();
                    }
                    std::string pluralPostfix = "";
                    if (pluralPostfixXml) {
                        pluralPostfix = pluralPostfixXml->value();
                    }
                    std::string timeFormat = "";
                    if (timeFormatXml) {
                        timeFormat = timeFormatXml->value();
                    }

                    // Create the ReloadableText component
                    c = new ReloadableText(typeValue, *page, config_, systemMode, font, layoutKey, timeFormat, textFormat, singlePrefix, singlePostfix, pluralPrefix, pluralPostfix);
                }

                if (c) {
                    // Set common properties for the component
                    c->baseViewInfo.Monitor = cMonitor;
                    c->baseViewInfo.Layout = page->getCurrentLayout();
                    c->setId(id);

                    // Check for and set menuScrollReload
                    xml_attribute<> const* menuScrollReload = componentXml->first_attribute("menuScrollReload");
                    if (menuScrollReload &&
                        (Utils::toLower(menuScrollReload->value()) == "true" ||
                            Utils::toLower(menuScrollReload->value()) == "yes")) {
                        c->setMenuScrollReload(true);
                        LOG_INFO("Layout", "menuScrollReload set to true for component ID: " + std::to_string(id));
                    }
                }
            }
        }
        else if(tagName == "reloadableScrollingText") {
            if(type) {
                Font *font = addFont(componentXml, NULL, cMonitor);
                std::string direction = "horizontal";
                std::string textFormat = "";
                std::string location = "";
                std::string typeValue = type->value();
                if (typeValue == "file") {
                    // Ensure the location attribute is present
                    if (!locationXml) {
                        LOG_ERROR("Layout", "reloadableScrollingText type='file' requires a 'location' attribute.");
                        continue; // Skip this component if location is not provided
                    }
                    // Get the location value
                    location = locationXml->value();
                }
                if (textFormatXml) {
                    textFormat = textFormatXml->value();
                }
                if (directionXml) {
                    direction = directionXml->value();
                }
                float scrollingSpeed = 1.0f;
                if (scrollingSpeedXml) {
                    scrollingSpeed = Utils::convertFloat(scrollingSpeedXml->value());
                }
                float startPosition = 0.0f;
                if (startPositionXml) {
                    startPosition = Utils::convertFloat(startPositionXml->value());
                }
                float startTime = 0.0f;
                if (startTimeXml) {
                    startTime = Utils::convertFloat(startTimeXml->value());
                }
                float endTime = 0.0f;
                if (endTimeXml) {
                    endTime = Utils::convertFloat(endTimeXml->value());
                }
                std::string alignment = "";
                if (alignmentXml) {
                    alignment = alignmentXml->value();
                }
                
                std::string singlePrefix = "";
                if (singlePrefixXml) {
                    singlePrefix = singlePrefixXml->value();
                }
                std::string singlePostfix = "";
                if (singlePostfixXml) {
                    singlePostfix = singlePostfixXml->value();
                }
                std::string pluralPrefix = "";
                if (pluralPrefixXml) {
                    pluralPrefix = pluralPrefixXml->value();
                }
                std::string pluralPostfix = "";
                if (pluralPostfixXml) {
                    pluralPostfix = pluralPostfixXml->value();
                }
                c = new ReloadableScrollingText(config_, systemMode, layoutMode, menuMode, typeValue, singlePrefix, singlePostfix, pluralPrefix, pluralPostfix, textFormat, alignment, *page, selectedOffset, font, direction, scrollingSpeed, startPosition, startTime, endTime, location);
                c->setId( id );
                c->baseViewInfo.Monitor = cMonitor;
                c->baseViewInfo.Layout = page->getCurrentLayout();

                xml_attribute<> const *menuScrollReload = componentXml->first_attribute("menuScrollReload");
                if (menuScrollReload &&
                    (Utils::toLower(menuScrollReload->value()) == "true" ||
                     Utils::toLower(menuScrollReload->value()) == "yes")) {
                    c->setMenuScrollReload(true);
                }
            }
        }
        else {
            xml_attribute<> const *jukeboxXml = componentXml->first_attribute("jukebox");
            bool jukebox         = false;
            int  jukeboxNumLoops = 0;
            if(jukeboxXml &&
               (Utils::toLower(jukeboxXml->value()) == "true" ||
                Utils::toLower(jukeboxXml->value()) == "yes")) {
                jukebox = true;
                page->setJukebox();
                xml_attribute<> const *numLoopsXml = componentXml->first_attribute("jukeboxNumLoops");
                jukeboxNumLoops = numLoopsXml ? Utils::convertInt(numLoopsXml->value()) : 1;
            }
            Font *font = addFont(componentXml, NULL, cMonitor);

            std::string typeString      = "video";
            std::string imageTypeString = "";
            if ( type )
                typeString = type->value();
            if ( imageType )
                imageTypeString = imageType->value();

            int randomSelectInt = randomSelectXml ? Utils::convertInt(randomSelectXml->value()) : 0;

            c = new ReloadableMedia(config_, systemMode, layoutMode, commonMode, menuMode, typeString, imageTypeString, *page, 
                selectedOffset, (tagName == "reloadableVideo") || (tagName == "reloadableAudio"), font, jukebox, jukeboxNumLoops, randomSelectInt);
            c->baseViewInfo.Monitor = cMonitor;
            c->baseViewInfo.Layout = page->getCurrentLayout();

            c->setId( id );
            c->allocateGraphicsMemory();
            if (xml_attribute<> const *menuScrollReload = componentXml->first_attribute("menuScrollReload"); menuScrollReload &&
                (Utils::toLower(menuScrollReload->value()) == "true" ||
                 Utils::toLower(menuScrollReload->value()) == "yes"))
            {
                c->setMenuScrollReload(true);
            }
            xml_attribute<> const *textFallback = componentXml->first_attribute("textFallback");
            if(textFallback && Utils::toLower(textFallback->value()) == "true") {
                static_cast<ReloadableMedia *>(c)->enableTextFallback_(true);
            }
            else {
                static_cast<ReloadableMedia *>(c)->enableTextFallback_(false);
            }
            xml_attribute<> const* useTextureCacheXml = componentXml->first_attribute("useTextureCache");
            if (useTextureCacheXml && 
                (Utils::toLower(useTextureCacheXml->value()) == "true" || 
                    Utils::toLower(useTextureCacheXml->value()) == "yes")) {
				static_cast<ReloadableMedia*>(c)->enableTextureCache_(true);
            }
        }

        if(c) {
            loadTweens(c, componentXml);
            page->addComponent(c);
        }
    }
}

Font *PageBuilder::addFont(const xml_node<> *component, const xml_node<> *defaults, int monitor)
{
    xml_attribute<> const *fontXml = component->first_attribute("font");
    xml_attribute<> const *fontColorXml = component->first_attribute("fontColor");
    xml_attribute<> const *fontSizeXml = component->first_attribute("loadFontSize");

    if(defaults) {
        if(!fontXml && defaults->first_attribute("font")) {
            fontXml = defaults->first_attribute("font");
        }

        if(!fontColorXml && defaults->first_attribute("fontColor")) {
            fontColorXml = defaults->first_attribute("fontColor");
        }

        if(!fontSizeXml && defaults->first_attribute("loadFontSize")) {
            fontSizeXml = defaults->first_attribute("loadFontSize");
        }
    }


    // use layout defaults unless overridden
    std::string fontName = fontName_;
    SDL_Color fontColor = fontColor_;
    int fontSize = fontSize_;

    if(fontXml) {
        fontName = Configuration::convertToAbsolutePath(
                    Utils::combinePath(Configuration::absolutePath, "layouts", layoutKey,""),
                    fontXml->value());

        LOG_DEBUG("Layout", "loading font " + fontName );
    }
    if(fontColorXml) {
        int intColor = 0;
        std::stringstream ss;
        ss << std::hex << fontColorXml->value();
        ss >> intColor;

        fontColor.b = intColor & 0xFF;
        intColor >>= 8;
        fontColor.g = intColor & 0xFF;
        intColor >>= 8;
        fontColor.r = intColor & 0xFF;
    }

    if(fontSizeXml) {
        fontSize = Utils::convertInt(fontSizeXml->value());
    }

    fontCache_->loadFont(fontName, fontSize, fontColor, monitor);
    return fontCache_->getFont(fontName, fontSize, fontColor);
}

void PageBuilder::loadTweens(Component *c, xml_node<> *componentXml)
{
    buildViewInfo(componentXml, c->baseViewInfo);

    c->setTweens(createTweenInstance(componentXml));
}

std::shared_ptr<AnimationEvents> PageBuilder::createTweenInstance(rapidxml::xml_node<>* componentXml)
{
    auto tweens = std::make_shared<AnimationEvents>();

    buildTweenSet(tweens.get(), componentXml, "onEnter", "enter");
    buildTweenSet(tweens.get(), componentXml, "onExit", "exit");
    buildTweenSet(tweens.get(), componentXml, "onIdle", "idle");
    buildTweenSet(tweens.get(), componentXml, "onMenuIdle", "menuIdle");
    buildTweenSet(tweens.get(), componentXml, "onMenuScroll", "menuScroll");
    buildTweenSet(tweens.get(), componentXml, "onPlaylistScroll", "playlistScroll");
    buildTweenSet(tweens.get(), componentXml, "onHighlightEnter", "highlightEnter");
    buildTweenSet(tweens.get(), componentXml, "onHighlightExit", "highlightExit");
    buildTweenSet(tweens.get(), componentXml, "onMenuEnter", "menuEnter");
    buildTweenSet(tweens.get(), componentXml, "onMenuExit", "menuExit");
    buildTweenSet(tweens.get(), componentXml, "onGameEnter", "gameEnter");
    buildTweenSet(tweens.get(), componentXml, "onGameExit", "gameExit");
    buildTweenSet(tweens.get(), componentXml, "onPlaylistEnter", "playlistEnter");
    buildTweenSet(tweens.get(), componentXml, "onPlaylistExit", "playlistExit");
    buildTweenSet(tweens.get(), componentXml, "onPlaylistNextEnter", "playlistNextEnter");
    buildTweenSet(tweens.get(), componentXml, "onPlaylistNextExit", "playlistNextExit");
    buildTweenSet(tweens.get(), componentXml, "onPlaylistPrevEnter", "playlistPrevEnter");
    buildTweenSet(tweens.get(), componentXml, "onPlaylistPrevExit", "playlistPrevExit");
    buildTweenSet(tweens.get(), componentXml, "onMenuJumpEnter", "menuJumpEnter");
    buildTweenSet(tweens.get(), componentXml, "onMenuJumpExit", "menuJumpExit");
    buildTweenSet(tweens.get(), componentXml, "onAttractEnter", "attractEnter");
    buildTweenSet(tweens.get(), componentXml, "onAttract", "attract");
    buildTweenSet(tweens.get(), componentXml, "onAttractExit", "attractExit");
    buildTweenSet(tweens.get(), componentXml, "onJukeboxJump", "jukeboxJump");

    buildTweenSet(tweens.get(), componentXml, "onGameInfoEnter", "gameInfoEnter");
    buildTweenSet(tweens.get(), componentXml, "onGameInfoExit", "gameInfoExit");
    buildTweenSet(tweens.get(), componentXml, "onCollectionInfoEnter", "collectionInfoEnter");
    buildTweenSet(tweens.get(), componentXml, "onCollectionInfoExit", "collectionInfoExit");
    buildTweenSet(tweens.get(), componentXml, "onBuildInfoEnter", "buildInfoEnter");
    buildTweenSet(tweens.get(), componentXml, "onBuildInfoExit", "buildInfoExit");

    buildTweenSet(tweens.get(), componentXml, "onMenuActionInputEnter", "menuActionInputEnter");
    buildTweenSet(tweens.get(), componentXml, "onMenuActionInputExit", "menuActionInputExit");
    buildTweenSet(tweens.get(), componentXml, "onMenuActionSelectEnter", "menuActionSelectEnter");
    buildTweenSet(tweens.get(), componentXml, "onMenuActionSelectExit", "menuActionSelectExit");

    return tweens;
}


void PageBuilder::buildTweenSet(AnimationEvents* tweens, xml_node<>* componentXml, const std::string& tagName, const std::string& tweenName) {
    for (componentXml = componentXml->first_node(tagName.c_str()); componentXml; componentXml = componentXml->next_sibling(tagName.c_str())) {
        xml_attribute<> const* indexXml = componentXml->first_attribute("menuIndex");

        if (indexXml) {
            std::string indexs = indexXml->value();
            if (indexs[0] == '!') {
                indexs.erase(0, 1);
                int index = Utils::convertInt(indexs);
                for (int i = 0; i < MENU_INDEX_HIGH - 1; i++) {
                    if (i != index) {
                        auto animation = std::make_shared<Animation>();
                        getTweenSet(componentXml, animation.get());
                        tweens->setAnimation(tweenName, i, std::move(animation));
                    }
                }
            }
            else if (indexs[0] == '<') {
                indexs.erase(0, 1);
                int index = Utils::convertInt(indexs);
                for (int i = 0; i < MENU_INDEX_HIGH - 1; i++) {
                    if (i < index) {
                        auto animation = std::make_shared<Animation>();
                        getTweenSet(componentXml, animation.get());
                        tweens->setAnimation(tweenName, i, std::move(animation));
                    }
                }
            }
            else if (indexs[0] == '>') {
                indexs.erase(0, 1);
                int index = Utils::convertInt(indexs);
                for (int i = 0; i < MENU_INDEX_HIGH - 1; i++) {
                    if (i > index) {
                        auto animation = std::make_shared<Animation>();
                        getTweenSet(componentXml, animation.get());
                        tweens->setAnimation(tweenName, i, std::move(animation));
                    }
                }
            }
            else if (indexs[0] == 'i') {
                auto animation = std::make_shared<Animation>();
                getTweenSet(componentXml, animation.get());
                tweens->setAnimation(tweenName, MENU_INDEX_HIGH, std::move(animation));
            }
            else {
                int index = Utils::convertInt(indexXml->value());
                auto animation = std::make_shared<Animation>();
                getTweenSet(componentXml, animation.get());
                tweens->setAnimation(tweenName, index, std::move(animation));
            }
        }
        else {
            auto animation = std::make_shared<Animation>();
            getTweenSet(componentXml, animation.get());
            tweens->setAnimation(tweenName, -1, std::move(animation));
        }
    }
}

ScrollingList * PageBuilder::buildMenu(xml_node<> *menuXml, Page &page, int monitor)
{
    ScrollingList *menu = nullptr;
    std::string menuType = "vertical";
    std::string imageType = "null";
    std::string videoType = "null";
    std::map<int, xml_node<> *> overrideItems;
    xml_node<> *itemDefaults               = menuXml->first_node("itemDefaults");
    xml_attribute<> const *modeXml               = menuXml->first_attribute("mode");
    xml_attribute<> const *imageTypeXml          = menuXml->first_attribute("imageType");
    xml_attribute<> const *videoTypeXml          = menuXml->first_attribute("videoType");
    xml_attribute<> const *menuTypeXml           = menuXml->first_attribute("type");
    xml_attribute<> const *scrollTimeXml         = menuXml->first_attribute("scrollTime");
    xml_attribute<> const *scrollAccelerationXml = menuXml->first_attribute("scrollAcceleration");
    xml_attribute<> const *minScrollTimeXml      = menuXml->first_attribute("minScrollTime");
    xml_attribute<> const *scrollOrientationXml  = menuXml->first_attribute("orientation");
    xml_attribute<> const *selectedImage         = menuXml->first_attribute("selectedImage");
    xml_attribute<> const *textFallback          = menuXml->first_attribute("textFallback");
    xml_attribute<> const *monitorXml = menuXml->first_attribute("monitor");
	xml_attribute<> const* useTextureCacheXml = menuXml->first_attribute("useTextureCache");

    if(menuTypeXml) {
        menuType = menuTypeXml->value();
    }

    // ensure <menu> has an <itemDefaults> tag
    if(!itemDefaults) {
        LOG_WARNING("Layout", "Menu tag is missing <itemDefaults> tag.");
    }

    bool playlistType = false;
    if(imageTypeXml) {
        imageType = imageTypeXml->value();
        if (imageType.rfind("playlist", 0) == 0) {
            playlistType = true;
        }
    }

    if(videoTypeXml) {
        videoType = videoTypeXml->value();
        if (videoType.rfind("playlist", 0) == 0) {
            playlistType = true;
        }
    }

    bool layoutMode = false;
    bool commonMode = false;
    if(modeXml) {
        std::string sysMode = modeXml->value();
        if(sysMode == "layout") {
            layoutMode = true;
        }
        if(sysMode == "common") {
            commonMode = true;
        }
        if(sysMode == "commonlayout") {
            layoutMode = true;
            commonMode = true;
        }
    }

    int cMonitor = monitorXml ? Utils::convertInt(monitorXml->value()) : monitor;

    // on default, text will be rendered to the menu. Preload it into cache.
    Font *font = addFont(itemDefaults, NULL, cMonitor);

    bool useTextureCache = false;
    if (useTextureCacheXml && (Utils::toLower(useTextureCacheXml->value()) == "true" ||
        Utils::toLower(useTextureCacheXml->value()) == "yes")){
		useTextureCache = true;
    }

    menu = new ScrollingList(config_, page, layoutMode, commonMode, playlistType, selectedImage, font, layoutKey, imageType, videoType, useTextureCache);
    menu->baseViewInfo.Monitor = cMonitor;
    menu->baseViewInfo.Layout = page.getCurrentLayout();

    buildViewInfo(menuXml, menu->baseViewInfo);

    if(scrollTimeXml) {
        menu->setStartScrollTime(Utils::convertFloat(scrollTimeXml->value()));
    }

    if(scrollAccelerationXml) {
        menu->setScrollAcceleration(Utils::convertFloat(scrollAccelerationXml->value()));
        menu->setMinScrollTime(Utils::convertFloat(scrollAccelerationXml->value()));
    }

    if(minScrollTimeXml) {
        menu->setMinScrollTime(Utils::convertFloat(minScrollTimeXml->value()));
    }

    if(scrollOrientationXml) {
        std::string scrollOrientation = scrollOrientationXml->value();
        if(scrollOrientation == "horizontal") {
            menu->horizontalScroll = true;
        }
    }

    menu->enableTextFallback(textFallback && Utils::toLower(textFallback->value()) == "true");

    buildViewInfo(menuXml, menu->baseViewInfo);

    if(menuType == "custom") {
        buildCustomMenu(menu, menuXml, itemDefaults);
    }
    else {
        buildVerticalMenu(menu, menuXml, itemDefaults);
    }

    loadTweens(menu, menuXml);

    return menu;
}


void PageBuilder::buildCustomMenu(ScrollingList* menu, const rapidxml::xml_node<>* menuXml, rapidxml::xml_node<>* itemDefaults) {
    auto points = new std::vector<ViewInfo*>(); // Leave ViewInfo unchanged
    auto tweenPoints = std::make_shared<std::vector<std::shared_ptr<AnimationEvents>>>();

    int i = 0;

    for (auto* componentXml = menuXml->first_node("item"); componentXml; componentXml = componentXml->next_sibling("item")) {
        auto* viewInfo = new ViewInfo(); // Leave ViewInfo unchanged
        viewInfo->Monitor = menu->baseViewInfo.Monitor;
        viewInfo->Layout = menu->baseViewInfo.Layout;

        buildViewInfo(componentXml, *viewInfo, itemDefaults);
        viewInfo->Additive = menu->baseViewInfo.Additive;

        points->push_back(viewInfo);
        tweenPoints->push_back(createTweenInstance(componentXml));

        if (componentXml->first_attribute("selected")) {
            menu->setSelectedIndex(i);
        }

        i++;
    }

    menu->setPoints(points, tweenPoints);
}

void PageBuilder::buildVerticalMenu(ScrollingList* menu, const rapidxml::xml_node<>* menuXml, rapidxml::xml_node<>* itemDefaults) {
    auto points = new std::vector<ViewInfo*>(); // Leave ViewInfo unchanged
    auto tweenPoints = std::make_shared<std::vector<std::shared_ptr<AnimationEvents>>>();

    int selectedIndex = MENU_FIRST;
    std::map<int, rapidxml::xml_node<>*> overrideItems;

    // By default the menu will automatically determine the offsets for your list items.
    // We can override individual menu points to have unique characteristics (i.e. make the first item opaque or
    // make the selected item a different color).
    for (auto* componentXml = menuXml->first_node("item"); componentXml; componentXml = componentXml->next_sibling("item")) {
        const auto* xmlIndex = componentXml->first_attribute("index");

        if (xmlIndex) {
            int itemIndex = parseMenuPosition(xmlIndex->value());
            overrideItems[itemIndex] = componentXml;

            // check to see if the item specified is the selected index
            const auto* xmlSelectedIndex = componentXml->first_attribute("selected");

            if (xmlSelectedIndex) {
                selectedIndex = itemIndex;
            }
        }
    }

    bool end = false;

    // menu start

    float height = 0;
    int index = 0;

    if (overrideItems.find(MENU_START) != overrideItems.end()) {
        auto* component = overrideItems[MENU_START];
        auto* viewInfo = createMenuItemInfo(component, itemDefaults, menu->baseViewInfo);
        viewInfo->Y = menu->baseViewInfo.Y + height;
        points->push_back(viewInfo);
        tweenPoints->push_back(createTweenInstance(component));
        height += viewInfo->Height;

        // increment the selected index to account for the new "invisible" menu item
        selectedIndex++;
    }
    while (!end) {
        auto* viewInfo = new ViewInfo();
        viewInfo->Monitor = menu->baseViewInfo.Monitor;
        viewInfo->Layout = menu->baseViewInfo.Layout;

        auto* component = itemDefaults;

        // use overridden item setting if specified by layout for the given index
        if (overrideItems.find(index) != overrideItems.end()) {
            component = overrideItems[index];
        }

        // calculate the total height of our menu items if we can load any additional items
        buildViewInfo(component, *viewInfo, itemDefaults);
        const auto* itemSpacingXml = component->first_attribute("spacing");
        int itemSpacing = itemSpacingXml ? Utils::convertInt(itemSpacingXml->value()) : 0;
        float nextHeight = height + viewInfo->Height + itemSpacing;

        if (nextHeight >= menu->baseViewInfo.Height) {
            end = true;
        }

        // we have reached the last menu item
        if (end && overrideItems.find(MENU_LAST) != overrideItems.end()) {
            component = overrideItems[MENU_LAST];

            buildViewInfo(component, *viewInfo, itemDefaults);
            const auto* itemSpacingXml = component->first_attribute("spacing");
            int itemSpacing = itemSpacingXml ? Utils::convertInt(itemSpacingXml->value()) : 0;
            nextHeight = height + viewInfo->Height + itemSpacing;
        }

        viewInfo->Y = menu->baseViewInfo.Y + height;
        points->push_back(viewInfo);
        tweenPoints->push_back(createTweenInstance(component));
        index++;
        height = nextHeight;
    }

    // menu end
    if (overrideItems.find(MENU_END) != overrideItems.end()) {
        auto* component = overrideItems[MENU_END];
        auto* viewInfo = createMenuItemInfo(component, itemDefaults, menu->baseViewInfo);
        viewInfo->Y = menu->baseViewInfo.Y + height;
        points->push_back(viewInfo);
        tweenPoints->push_back(createTweenInstance(component));
    }

    if (selectedIndex >= static_cast<int>(points->size())) {
        std::stringstream ss;
        ss << "Design error! Selected menu item was set to " << selectedIndex
            << " although there are only " << points->size()
            << " menu points that can be displayed";

        LOG_ERROR("Layout", ss.str());
        selectedIndex = 0;
    }

    menu->setSelectedIndex(selectedIndex);
    menu->setPoints(points, std::move(tweenPoints)); // Use std::move to transfer ownership of the shared pointer
}

ViewInfo *PageBuilder::createMenuItemInfo(xml_node<> *component, xml_node<> *defaults, const ViewInfo& menuViewInfo)
{
    auto *viewInfo = new ViewInfo();
    viewInfo->Monitor = menuViewInfo.Monitor;

    buildViewInfo(component, *viewInfo, defaults);

    return viewInfo;
}

int PageBuilder::parseMenuPosition(const std::string& strIndex)
{
    int index = MENU_FIRST;

    if(strIndex == "end") {
        index = MENU_END;
    }
    else if(strIndex == "last") {
        index = MENU_LAST;
    }
    else if(strIndex == "start") {
        index = MENU_START;
    }
    else if(strIndex == "first") {
        index = MENU_FIRST;
    }
    else {
        index = Utils::convertInt(strIndex);
    }
    return index;
}

xml_attribute<> *PageBuilder::findAttribute(const xml_node<> *componentXml, const std::string& attribute, const xml_node<> *defaultXml = nullptr)
{
    xml_attribute<> *attributeXml = componentXml->first_attribute(attribute.c_str());

    if(!attributeXml && defaultXml) {
        attributeXml = defaultXml->first_attribute(attribute.c_str());
    }

    return attributeXml;
}

void PageBuilder::buildViewInfo(xml_node<> *componentXml, ViewInfo &info, xml_node<> *defaultXml)
{
    xml_attribute<> const *x                  = findAttribute(componentXml, "x", defaultXml);
    xml_attribute<> const *y                  = findAttribute(componentXml, "y", defaultXml);
    xml_attribute<> const *xOffset            = findAttribute(componentXml, "xOffset", defaultXml);
    xml_attribute<> const *yOffset            = findAttribute(componentXml, "yOffset", defaultXml);
    xml_attribute<> const *xOrigin            = findAttribute(componentXml, "xOrigin", defaultXml);
    xml_attribute<> const *yOrigin            = findAttribute(componentXml, "yOrigin", defaultXml);
    xml_attribute<> const *height             = findAttribute(componentXml, "height", defaultXml);
    xml_attribute<> const *width              = findAttribute(componentXml, "width", defaultXml);
    xml_attribute<> const *fontSize           = findAttribute(componentXml, "fontSize", defaultXml);
    xml_attribute<> const *fontColor          = findAttribute(componentXml, "fontColor", defaultXml);
    xml_attribute<> const *minHeight          = findAttribute(componentXml, "minHeight", defaultXml);
    xml_attribute<> const *minWidth           = findAttribute(componentXml, "minWidth", defaultXml);
    xml_attribute<> const *maxHeight          = findAttribute(componentXml, "maxHeight", defaultXml);
    xml_attribute<> const *maxWidth           = findAttribute(componentXml, "maxWidth", defaultXml);
    xml_attribute<> const *alpha              = findAttribute(componentXml, "alpha", defaultXml);
    xml_attribute<> const *angle              = findAttribute(componentXml, "angle", defaultXml);
    xml_attribute<> const *layer              = findAttribute(componentXml, "layer", defaultXml);
    xml_attribute<> const *backgroundColor    = findAttribute(componentXml, "backgroundColor", defaultXml);
    xml_attribute<> const *backgroundAlpha    = findAttribute(componentXml, "backgroundAlpha", defaultXml);
    xml_attribute<> const *reflection         = findAttribute(componentXml, "reflection", defaultXml);
    xml_attribute<> const *reflectionDistance = findAttribute(componentXml, "reflectionDistance", defaultXml);
    xml_attribute<> const *reflectionScale    = findAttribute(componentXml, "reflectionScale", defaultXml);
    xml_attribute<> const *reflectionAlpha    = findAttribute(componentXml, "reflectionAlpha", defaultXml);
    xml_attribute<> const *containerX         = findAttribute(componentXml, "containerX", defaultXml);
    xml_attribute<> const *containerY         = findAttribute(componentXml, "containerY", defaultXml);
    xml_attribute<> const *containerWidth     = findAttribute(componentXml, "containerWidth", defaultXml);
    xml_attribute<> const *containerHeight    = findAttribute(componentXml, "containerHeight", defaultXml);
    xml_attribute<> const *monitor            = findAttribute(componentXml, "monitor", defaultXml);
    xml_attribute<> const *volume             = findAttribute(componentXml, "volume", defaultXml);
    xml_attribute<> const *restart            = findAttribute(componentXml, "restart", defaultXml);
    xml_attribute<> const *additive           = findAttribute(componentXml, "additive", defaultXml);
    xml_attribute<> const *pauseOnScroll      = findAttribute(componentXml, "pauseOnScroll", defaultXml);

    info.X = getHorizontalAlignment(x, 0);
    info.Y = getVerticalAlignment(y, 0);

    info.XOffset =  getHorizontalAlignment(xOffset, 0);
    info.YOffset =  getVerticalAlignment(yOffset, 0);
    float xOriginRelative = getHorizontalAlignment(xOrigin, 0);
    float yOriginRelative = getVerticalAlignment(yOrigin, 0);

    // the origins need to be saved as a percent since the heights and widths can be scaled
    info.XOrigin = xOriginRelative / layoutWidth_;
    info.YOrigin = yOriginRelative / layoutHeight_;


    if(!height && !width) {
        info.Height = -1;
        info.Width = -1;
    }
    else {
        info.Height = getVerticalAlignment(height, -1);
        info.Width = getHorizontalAlignment(width, -1);
    }
    info.FontSize           = getVerticalAlignment(fontSize, -1);
    info.MinHeight          = getVerticalAlignment(minHeight, 0);
    info.MinWidth           = getHorizontalAlignment(minWidth, 0);
    info.MaxHeight          = getVerticalAlignment(maxHeight, FLT_MAX);
    info.MaxWidth           = getVerticalAlignment(maxWidth, FLT_MAX);
    info.Alpha              = alpha              ? Utils::convertFloat(alpha->value())             : 1.f;
    info.Angle              = angle              ? Utils::convertFloat(angle->value())             : 0.f;
    info.Layer              = layer              ? Utils::convertInt(layer->value())               : 0;
    info.Reflection         = reflection         ? reflection->value()                             : "";
    info.ReflectionDistance = reflectionDistance ? Utils::convertInt(reflectionDistance->value())  : 0;
    info.ReflectionScale    = reflectionScale    ? Utils::convertFloat(reflectionScale->value())   : 0.25f;
    info.ReflectionAlpha    = reflectionAlpha    ? Utils::convertFloat(reflectionAlpha->value())   : 1.f;
    info.ContainerX         = containerX         ? Utils::convertFloat(containerX->value())        : 0.f;
    info.ContainerY         = containerY         ? Utils::convertFloat(containerY->value())        : 0.f;
    info.ContainerWidth     = containerWidth     ? Utils::convertFloat(containerWidth->value())    : -1.f;
    info.ContainerHeight    = containerHeight    ? Utils::convertFloat(containerHeight->value())   : -1.f;
    info.Monitor            = monitor            ? Utils::convertInt(monitor->value())             : info.Monitor;
    info.Volume             = volume             ? Utils::convertFloat(volume->value())            : 1.f;
    info.Restart            = restart            ? Utils::toLower(restart->value())     == "true" : false;
    info.Additive           = additive           ? Utils::toLower(additive->value())    == "true" : false;
    
    if (pauseOnScroll) {
        // If pauseOnScroll's value is "false", then set to false, otherwise true
        info.PauseOnScroll = Utils::toLower(pauseOnScroll->value()) != "false";
    }
    else {
        // If pauseOnScroll is null, default to true
        info.PauseOnScroll = true;
    }

    // This reads the configuration and sets Restart or PauseOnScroll accordingly
    bool disableVideoRestart = false;
    bool disablePauseOnScroll = false;

    // Check if the property exists and is set to true
    if (config_.getProperty(OPTION_DISABLEVIDEORESTART, disableVideoRestart) && disableVideoRestart) {
        info.Restart = false;
    }

    // Check if the property exists and is set to true
    if (config_.getProperty(OPTION_DISABLEPAUSEONSCROLL, disablePauseOnScroll) && disablePauseOnScroll) {
        info.PauseOnScroll = false;
    }

    if(fontColor) {
      Font *font = addFont(componentXml, defaultXml, info.Monitor);
      info.font = font;
    }

    if(backgroundColor) {
        std::stringstream ss(backgroundColor->value());
        int num;
        ss >> std::hex >> num;
        int red = num / 0x10000;
        int green = (num / 0x100) % 0x100;
        int blue = num % 0x100;

        info.BackgroundRed = static_cast<float>(red/255);
        info.BackgroundGreen = static_cast<float>(green/255);
        info.BackgroundBlue = static_cast<float>(blue/255);
    }

    if(backgroundAlpha) {
        info.BackgroundAlpha =  backgroundAlpha ? Utils::convertFloat(backgroundAlpha->value()) : 1.f;
    }
}

void PageBuilder::getTweenSet(const xml_node<>* node, Animation* animation) {
    if (node) {
        for (xml_node<>* set = node->first_node("set"); set; set = set->next_sibling("set")) {
            // Create a shared_ptr to manage the TweenSet instance.
            auto ts = std::make_shared<TweenSet>();
            getAnimationEvents(set, *ts);

            // Use the shared_ptr to transfer ownership of the TweenSet instance to the Animation instance.
            animation->Push(ts);
        }
    }
}

void PageBuilder::getAnimationEvents(const xml_node<> *node, TweenSet &tweens)
{
    xml_attribute<> const *durationXml = node->first_attribute("duration");
    std::string actionSetting;
    config_.getProperty(OPTION_ACTION, actionSetting);

    if (!durationXml) {
        LOG_ERROR("Layout", "Animation set tag missing \"duration\" attribute");
    } 
    else {
        for (xml_node<> const *animate = node->first_node("animate"); animate; animate = animate->next_sibling("animate")) {
            xml_attribute<> const *type = animate->first_attribute("type");
            xml_attribute<> const *from = animate->first_attribute("from");
            xml_attribute<> const *to = animate->first_attribute("to");
            xml_attribute<> const *algorithmXml = animate->first_attribute("algorithm");
            xml_attribute<> const *setting = animate->first_attribute("setting");
            xml_attribute<> const *playlist = animate->first_attribute("playlist");

            std::string animateType;
            if (type) {
                animateType = type->value();
            }

            if (!type) {
                LOG_ERROR("Layout", "Animate tag missing \"type\" attribute");
            } 
            else if (!to && (animateType != "nop" && animateType != "restart")) {
                LOG_ERROR("Layout", "Animate tag missing \"to\" attribute");
            } 
            else {
                // if in settings action="<something>" and the action has setting="<something>" then perform animation
                if (setting && setting->value() != actionSetting) {
                    continue;
                }

                float fromValue = 0.0f;
                bool fromDefined = true;
                if (from) {
                    std::string fromStr = from->value();
                    if (fromStr == "left" || fromStr == "top") {
                        fromValue = 0.0f;
                    } 
                    else if (fromStr == "center") {
                        fromValue = (animateType == "width") 
                            ? static_cast<float>(layoutWidth_) / 2
                            : static_cast<float>(layoutHeight_) / 2;
                    } 
                    else if (fromStr == "right" || fromStr == "stretch") {
                        fromValue = static_cast<float>(layoutWidth_);
                    } 
                    else if (fromStr == "bottom") {
                        fromValue = static_cast<float>(layoutHeight_);
                    } 
                    else if (fromStr.back() == '%') {
                        float percent = Utils::convertFloat(fromStr.substr(0, fromStr.size() - 1));
                        if (animateType == "width") {
                            fromValue = layoutWidth_ * (percent / 100.0f);
                        } else if (animateType == "height") {
                            fromValue = layoutHeight_ * (percent / 100.0f);
                        } else {
                            LOG_ERROR("Layout", "Invalid animateType for percentage calculation: " + animateType);
                            fromValue = 0.0f; // or another default/fallback value
                        }
                    } 
                    else {
                        fromValue = Utils::convertFloat(fromStr);
                    }
                } 
                else {
                    fromDefined = false;
                }

                float toValue = 0.0f;
                if (to) {
                    std::string toStr = to->value();
                    if (toStr == "left" || toStr == "top") {
                        toValue = 0.0f;
                    } 
                    else if (toStr == "center") {
                        toValue = (animateType == "width") 
                            ? static_cast<float>(layoutWidth_) / 2
                            : static_cast<float>(layoutHeight_) / 2;
                    } 
                    else if (toStr == "right" || toStr == "stretch") {
                        toValue = static_cast<float>(layoutWidth_);
                    } 
                    else if (toStr == "bottom") {
                        toValue = static_cast<float>(layoutHeight_);
                    } 
                    else if (toStr.back() == '%') {
                        float percent = Utils::convertFloat(toStr.substr(0, toStr.size() - 1));
                        if (animateType == "width") {
                            toValue = static_cast<float>(layoutWidth_) * (percent / 100.0f);
                        } else if (animateType == "height") {
                            toValue = static_cast<float>(layoutHeight_) * (percent / 100.0f);
                        } else {
                            LOG_ERROR("Layout", "Invalid animateType for percentage calculation: " + animateType);
                            toValue = 0.0f; // or another default/fallback value
                        }
                    } 
                    else {
                        toValue = Utils::convertFloat(toStr);
                    }
                }

                float durationValue = Utils::convertFloat(durationXml->value());

                TweenAlgorithm algorithm = LINEAR;
                TweenProperty property;

                if (algorithmXml) {
                    algorithm = Tween::getTweenType(algorithmXml->value());
                }

                if (Tween::getTweenProperty(animateType, property)) {
                    switch (property) {
                    case TWEEN_PROPERTY_WIDTH:
                    case TWEEN_PROPERTY_X:
                    case TWEEN_PROPERTY_X_OFFSET:
                    case TWEEN_PROPERTY_CONTAINER_X:
                    case TWEEN_PROPERTY_CONTAINER_WIDTH:
                        fromValue = getHorizontalAlignment(from, 0);
                        toValue = getHorizontalAlignment(to, 0);
                        break;

                        // x origin gets translated to a percent
                    case TWEEN_PROPERTY_X_ORIGIN:
                        fromValue = getHorizontalAlignment(from, 0) / layoutWidth_;
                        toValue = getHorizontalAlignment(to, 0) / layoutWidth_;
                        break;

                    case TWEEN_PROPERTY_HEIGHT:
                    case TWEEN_PROPERTY_Y:
                    case TWEEN_PROPERTY_Y_OFFSET:
                    case TWEEN_PROPERTY_FONT_SIZE:
                    case TWEEN_PROPERTY_CONTAINER_Y:
                    case TWEEN_PROPERTY_CONTAINER_HEIGHT:
                        fromValue = getVerticalAlignment(from, 0);
                        toValue = getVerticalAlignment(to, 0);
                        break;

                        // y origin gets translated to a percent
                    case TWEEN_PROPERTY_Y_ORIGIN:
                        fromValue = getVerticalAlignment(from, 0) / layoutHeight_;
                        toValue = getVerticalAlignment(to, 0) / layoutHeight_;
                        break;

                    case TWEEN_PROPERTY_MAX_WIDTH:
                    case TWEEN_PROPERTY_MAX_HEIGHT:
                        fromValue = getVerticalAlignment(from, FLT_MAX);
                        toValue   = getVerticalAlignment(to, FLT_MAX);
                        break;
                    default:
                        break;
                    }

                    // if in layout action has playlist="<current playlist name>" then perform action
                    std::string playlistFilter = playlist && playlist->value() ? playlist->value() : "";
                    auto t = std::make_unique<Tween>(property, algorithm, fromValue, toValue, durationValue, playlistFilter);
                    if (!fromDefined)
                        t->startDefined = false;
                    tweens.push(std::move(t));
                } 
                else {
                    std::stringstream ss;
                    ss << "Unsupported tween type attribute \"" << type->value() << "\"";
                    LOG_ERROR("Layout", ss.str());
                }
            }
        }
    }
}

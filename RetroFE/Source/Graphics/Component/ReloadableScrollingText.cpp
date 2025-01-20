#include "ReloadableScrollingText.h"
#include "../ViewInfo.h"
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../SDL.h"
#include "../Font.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <algorithm>

ReloadableScrollingText::ReloadableScrollingText(Configuration& config, bool systemMode, bool layoutMode, bool menuMode, std::string type, std::string textFormat, std::string singlePrefix, std::string singlePostfix, std::string pluralPrefix, std::string pluralPostfix, std::string alignment, Page& p, int displayOffset, FontManager* font, std::string direction, float scrollingSpeed, float startPosition, float startTime, float endTime, std::string location)
    : Component(p)
    , config_(config)
    , systemMode_(systemMode)
    , layoutMode_(layoutMode)
    , fontInst_(font)
    , type_(type)
    , textFormat_(textFormat)
    , singlePrefix_(singlePrefix)
    , singlePostfix_(singlePostfix)
    , pluralPrefix_(pluralPrefix)
    , pluralPostfix_(pluralPostfix)
    , alignment_(alignment)
    , direction_(direction)
    , location_(location)
    , scrollingSpeed_(scrollingSpeed)
    , startPosition_(startPosition)
    , currentPosition_(-startPosition)
    , startTime_(startTime)
    , waitStartTime_(startTime)
    , endTime_(endTime)
    , waitEndTime_(0.0f)
    , currentCollection_("")
    , displayOffset_(displayOffset)
    , needsUpdate_(true)
    , textWidth_(0.0f)
    , textHeight_(0.0f)
    , lastScale_(0.0f)
    , lastImageMaxWidth_(0.0f)
    , lastImageMaxHeight_(0.0f)
    , lastWriteTime_(std::filesystem::file_time_type::min())
    , intermediateTexture_(nullptr)
    , needsTextureUpdate_(true)
{
}

ReloadableScrollingText::~ReloadableScrollingText() = default;

bool ReloadableScrollingText::loadFileText(const std::string& filePath) {
    std::string absolutePath = Utils::combinePath(Configuration::absolutePath, filePath);
    std::filesystem::path file(absolutePath);
    std::filesystem::file_time_type currentWriteTime;

    auto roundToNearestSecond = [](std::filesystem::file_time_type ftt) {
        return std::chrono::time_point_cast<std::chrono::seconds>(ftt);
        };

    try {
        currentWriteTime = std::filesystem::last_write_time(file);
        currentWriteTime = roundToNearestSecond(currentWriteTime);
    }
    catch (const std::filesystem::filesystem_error& e) {
        LOG_ERROR("ReloadableScrollingText", "Failed to retrieve file modification time: " + std::string(e.what()));
        return false;
    }

    if (currentWriteTime == lastWriteTime_ && !text_.empty()) {
        return false;
    }

    lastWriteTime_ = currentWriteTime;

    std::ifstream fileStream(absolutePath);
    if (!fileStream.is_open()) {
        LOG_ERROR("ReloadableScrollingText", "Failed to open file: " + absolutePath);
        return false;
    }

    std::string line;
    text_.clear();

    while (std::getline(fileStream, line)) {
        if (direction_ == "horizontal" && !text_.empty()) {
            line = " " + line;
        }

        if (textFormat_ == "uppercase") {
            std::transform(line.begin(), line.end(), line.begin(), ::toupper);
        }
        else if (textFormat_ == "lowercase") {
            std::transform(line.begin(), line.end(), line.begin(), ::tolower);
        }

        text_.push_back(line);
    }

    fileStream.close();

    return true;
}

bool ReloadableScrollingText::update(float dt) {
    if (waitEndTime_ > 0) {
        waitEndTime_ -= dt;
    }
    else if (waitStartTime_ > 0) {
        waitStartTime_ -= dt;
    }
    else {
        if (direction_ == "horizontal") {
            currentPosition_ += scrollingSpeed_ * dt;
            if (startPosition_ == 0.0f && textWidth_ <= baseViewInfo.Width) {
                currentPosition_ = 0.0f;
            }
        }
        else if (direction_ == "vertical") {
            currentPosition_ += scrollingSpeed_ * dt;
        }
    }

    if (type_ == "file") {
        reloadTexture();
    }
    else if (newItemSelected || (newScrollItemSelected && getMenuScrollReload())) {
        reloadTexture();
        newItemSelected = false;
    }

    return Component::update(dt);
}

void ReloadableScrollingText::allocateGraphicsMemory() {
    Component::allocateGraphicsMemory();

    // Create intermediate texture after base allocation
    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (renderer) {
        float imageMaxWidth = (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
            ? baseViewInfo.Width : baseViewInfo.MaxWidth;
        float imageMaxHeight = (baseViewInfo.Height < baseViewInfo.MaxHeight && baseViewInfo.Height > 0)
            ? baseViewInfo.Height : baseViewInfo.MaxHeight;

        createIntermediateTexture(renderer, 
            static_cast<int>(std::ceil(imageMaxWidth)),
            static_cast<int>(std::ceil(imageMaxHeight)));
    }

    reloadTexture();
}

void ReloadableScrollingText::freeGraphicsMemory() {
    if (intermediateTexture_) {
        SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = nullptr;
    }
    Component::freeGraphicsMemory();
    text_.clear();
}

void ReloadableScrollingText::deInitializeFonts() {
    fontInst_->deInitialize();
}

void ReloadableScrollingText::initializeFonts() {
    fontInst_->initialize();
}

void ReloadableScrollingText::reloadTexture(bool resetScroll) {
    if (type_ == "file" && !location_.empty()) {
        bool fileChanged = loadFileText(location_);
        if (fileChanged) {
            resetScroll = true;
        }
        else {
            resetScroll = false;
        }
    }

    if (resetScroll) {
        if (direction_ == "horizontal") {
            currentPosition_ = -startPosition_;
        }
        else if (direction_ == "vertical") {
            currentPosition_ = -startPosition_;
        }

        waitStartTime_ = startTime_;
        waitEndTime_ = 0.0f;
    }

    text_.clear();

    if (type_ == "file" && !location_.empty()) {
        loadFileText(location_);
        return;
    }

    Item* selectedItem = page.getSelectedItem(displayOffset_);
    if (!selectedItem) {
        return;
    }

    config_.getProperty("currentCollection", currentCollection_);

    std::vector<std::string> names;

    names.push_back(selectedItem->name);
    names.push_back(selectedItem->fullTitle);

    if (!selectedItem->cloneof.empty()) {
        names.push_back(selectedItem->cloneof);
    }

    for (unsigned int n = 0; n < names.size() && text_.empty(); ++n) {
        std::string basename = names[n];
        Utils::replaceSlashesWithUnderscores(basename);

        if (systemMode_) {
            loadText(collectionName, type_, type_, "", true);
            if (text_.empty()) {
                loadText(selectedItem->collectionInfo->name, type_, type_, "", true);
            }
        }
        else {
            if (selectedItem->leaf) {
                loadText(collectionName, type_, basename, "", false);
                if (text_.empty()) {
                    loadText(selectedItem->collectionInfo->name, type_, basename, "", false);
                }
            }
            else {
                loadText(collectionName, type_, basename, "", false);
                if (text_.empty()) {
                    loadText(selectedItem->collectionInfo->name, type_, basename, "", false);
                }
                if (text_.empty()) {
                    loadText(selectedItem->name, type_, type_, "", true);
                }
            }
        }
    }

    if (text_.empty())
        loadText(selectedItem->filepath, type_, type_, selectedItem->filepath, false);

    if (text_.empty()) {
        std::stringstream ss;
        std::string text = "";
        if (type_ == "numberButtons") {
            text = selectedItem->numberButtons;
        }
        else if (type_ == "numberPlayers") {
            text = selectedItem->numberPlayers;
        }
        else if (type_ == "ctrlType") {
            text = selectedItem->ctrlType;
        }
        else if (type_ == "numberJoyWays") {
            text = selectedItem->joyWays;
        }
        else if (type_ == "rating") {
            text = selectedItem->rating;
        }
        else if (type_ == "score") {
            text = selectedItem->score;
        }
        else if (type_ == "year") {
            if (selectedItem->leaf)
                text = selectedItem->year;
            else
                (void)config_.getProperty("collections." + selectedItem->name + ".year", text);
        }
        else if (type_ == "title") {
            text = selectedItem->title;
        }
        else if (type_ == "developer") {
            text = selectedItem->developer;
            if (text.empty()) {
                text = selectedItem->manufacturer;
            }
        }
        else if (type_ == "manufacturer") {
            if (selectedItem->leaf)
                text = selectedItem->manufacturer;
            else
                (void)config_.getProperty("collections." + selectedItem->name + ".manufacturer", text);
        }
        else if (type_ == "genre") {
            if (selectedItem->leaf)
                text = selectedItem->genre;
            else
                (void)config_.getProperty("collections." + selectedItem->name + ".genre", text);
        }
        else if (type_.rfind("playlist", 0) == 0) {
            text = playlistName;
        }
        else if (type_ == "firstLetter") {
            text = selectedItem->fullTitle.at(0);
        }
        else if (type_ == "collectionName") {
            text = page.getCollectionName();
        }
        else if (type_ == "collectionSize") {
            if (page.getCollectionSize() == 0) {
                ss << singlePrefix_ << page.getCollectionSize() << pluralPostfix_;
            }
            else if (page.getCollectionSize() == 1) {
                ss << singlePrefix_ << page.getCollectionSize() << singlePostfix_;
            }
            else {
                ss << pluralPrefix_ << page.getCollectionSize() << pluralPostfix_;
            }
        }
        else if (type_ == "collectionIndex") {
            if (page.getSelectedIndex() == 0) {
                ss << singlePrefix_ << (page.getSelectedIndex() + 1) << pluralPostfix_;
            }
            else if (page.getSelectedIndex() == 1) {
                ss << singlePrefix_ << (page.getSelectedIndex() + 1) << singlePostfix_;
            }
            else {
                ss << pluralPrefix_ << (page.getSelectedIndex() + 1) << pluralPostfix_;
            }
        }
        else if (type_ == "collectionIndexSize") {
            if (page.getSelectedIndex() == 0) {
                ss << singlePrefix_ << (page.getSelectedIndex() + 1) << "/" << page.getCollectionSize() << pluralPostfix_;
            }
            else if (page.getSelectedIndex() == 1) {
                ss << singlePrefix_ << (page.getSelectedIndex() + 1) << "/" << page.getCollectionSize() << singlePostfix_;
            }
            else {
                ss << pluralPrefix_ << (page.getSelectedIndex() + 1) << "/" << page.getCollectionSize() << pluralPostfix_;
            }
        }
        else if (!selectedItem->leaf) {
            (void)config_.getProperty("collections." + selectedItem->name + "." + type_, text);
        }

        if (text == "0") {
            text = singlePrefix_ + text + pluralPostfix_;
        }
        else if (text == "1") {
            text = singlePrefix_ + text + singlePostfix_;
        }
        else if (!text.empty()) {
            text = pluralPrefix_ + text + pluralPostfix_;
        }

        if (!text.empty()) {
            if (textFormat_ == "uppercase") {
                std::transform(text.begin(), text.end(), text.begin(), ::toupper);
            }
            if (textFormat_ == "lowercase") {
                std::transform(text.begin(), text.end(), text.begin(), ::tolower);
            }
            ss << text;
            text_.push_back(ss.str());
        }
    }
    //needsUpdate_ = true;
}

void ReloadableScrollingText::loadText(std::string collection, std::string type, std::string basename, std::string filepath, bool systemMode) {
    std::string textPath = "";

    if (layoutMode_) {
        std::string layoutName;
        config_.getProperty("collections." + collection + ".layout", layoutName);
        if (layoutName.empty()) {
            config_.getProperty(OPTION_LAYOUT, layoutName);
        }
        textPath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", collection);
        if (systemMode)
            textPath = Utils::combinePath(textPath, "system_artwork");
        else
            textPath = Utils::combinePath(textPath, "medium_artwork", type);
    }
    else {
        config_.getMediaPropertyAbsolutePath(collection, type, systemMode, textPath);
    }
    if (!filepath.empty())
        textPath = filepath;

    textPath = Utils::combinePath(textPath, basename);
    textPath += ".txt";

    std::ifstream includeStream(textPath.c_str());

    if (!includeStream.good()) {
        return;
    }

    std::string line;

    while (std::getline(includeStream, line)) {
        if (direction_ == "horizontal" && !text_.empty()) {
            line = " " + line;
        }

        if (textFormat_ == "uppercase") {
            std::transform(line.begin(), line.end(), line.begin(), ::toupper);
        }
        if (textFormat_ == "lowercase") {
            std::transform(line.begin(), line.end(), line.begin(), ::tolower);
        }

        text_.push_back(line);
    }
}

void ReloadableScrollingText::draw() {
    Component::draw();

    if (text_.empty() || waitEndTime_ > 0.0f || baseViewInfo.Alpha <= 0.0f) {
        return;
    }

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    SDL_Texture* fontTexture = font ? font->getTexture() : nullptr;

    if (!fontTexture) {
        return;
    }

    float scale = baseViewInfo.FontSize / font->getHeight();
    float imageMaxWidth = (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;
    float imageMaxHeight = (baseViewInfo.Height < baseViewInfo.MaxHeight && baseViewInfo.Height > 0)
        ? baseViewInfo.Height : baseViewInfo.MaxHeight;

    // Check if we need to update our cache and possibly recreate the intermediate texture
    if (needsUpdate_ || lastScale_ != scale || lastImageMaxWidth_ != imageMaxWidth || lastImageMaxHeight_ != imageMaxHeight) {
        updateGlyphCache();

        // Recreate intermediate texture if dimensions changed
        SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
        if (renderer) {
            int texWidth = direction_ == "horizontal" 
                ? static_cast<int>(std::ceil(std::max(imageMaxWidth, textWidth_)))
                : static_cast<int>(std::ceil(imageMaxWidth));
            int texHeight = direction_ == "vertical"
                ? static_cast<int>(std::ceil(std::max(imageMaxHeight, textHeight_)))
                : static_cast<int>(std::ceil(imageMaxHeight));

            createIntermediateTexture(renderer, texWidth, texHeight);
            needsTextureUpdate_ = true;
        }
    }

    // If texture needs updating, render all glyphs to it
    if (needsTextureUpdate_ && intermediateTexture_) {
        SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
        SDL_Texture* originalTarget = SDL_GetRenderTarget(renderer);

        // Set intermediate texture as render target
        SDL_SetRenderTarget(renderer, intermediateTexture_);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);

        // Render all cached glyphs to the intermediate texture
        for (const auto& glyph : cachedGlyphs_) {
            SDL_FRect destRect = glyph.destRect;  // Use the cached positions directly
            SDL_RenderCopyF(renderer, fontTexture, &glyph.sourceRect, &destRect);
        }

        SDL_SetRenderTarget(renderer, originalTarget);
        needsTextureUpdate_ = false;
    }

    // Now handle the scrolling and display of the intermediate texture
    float xOrigin = baseViewInfo.XRelativeToOrigin();
    float yOrigin = baseViewInfo.YRelativeToOrigin();

    SDL_Rect srcRect;
    SDL_FRect destRect;
    destRect.x = xOrigin;
    destRect.y = yOrigin;
    destRect.w = imageMaxWidth;
    destRect.h = imageMaxHeight;

    if (direction_ == "horizontal") {
        float scrollPosition = currentPosition_;

        // Calculate source rectangle based on scroll position
        srcRect.x = scrollPosition < 0.0f ? 0 : static_cast<int>(scrollPosition);
        srcRect.y = 0;
        srcRect.w = static_cast<int>(imageMaxWidth);
        srcRect.h = static_cast<int>(imageMaxHeight);

        if (textWidth_ <= imageMaxWidth && startPosition_ == 0.0f) {
            currentPosition_ = 0.0f;
            srcRect.x = 0;
            waitStartTime_ = 0.0f;
            waitEndTime_ = 0.0f;
        }

        // Handle scroll reset
        if (currentPosition_ > textWidth_) {
            waitStartTime_ = startTime_;
            waitEndTime_ = endTime_;
            currentPosition_ = -startPosition_;
        }
    }
    else if (direction_ == "vertical") {
        float scrollPosition = currentPosition_;

        // Calculate source rectangle based on scroll position
        srcRect.x = 0;
        srcRect.y = scrollPosition < 0.0f ? 0 : static_cast<int>(scrollPosition);
        srcRect.w = static_cast<int>(imageMaxWidth);
        srcRect.h = static_cast<int>(imageMaxHeight);

        if (textHeight_ <= imageMaxHeight && startPosition_ == 0.0f) {
            currentPosition_ = 0.0f;
            srcRect.y = 0;
            waitStartTime_ = 0.0f;
            waitEndTime_ = 0.0f;
        }

        // Handle scroll reset
        if (currentPosition_ > textHeight_) {
            waitStartTime_ = startTime_;
            waitEndTime_ = endTime_;
            currentPosition_ = -startPosition_;
        }
    }

    // Render the intermediate texture with proper clipping
    SDL::renderCopyF(intermediateTexture_, baseViewInfo.Alpha, &srcRect, &destRect, baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
        page.getLayoutHeightByMonitor(baseViewInfo.Monitor));

    // Debug visualization
    //SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    //SDL_Rect debugRect = { static_cast<int>(destRect.x), static_cast<int>(destRect.y),
    //    static_cast<int>(destRect.w), static_cast<int>(destRect.h) };
    //SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    //SDL_RenderDrawRect(renderer, &debugRect);
}
bool ReloadableScrollingText::createIntermediateTexture(SDL_Renderer* renderer, int width, int height) {
    if (intermediateTexture_) {
        SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = nullptr;
    }

    intermediateTexture_ = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        width,
        height
    );

    if (!intermediateTexture_) {
        LOG_ERROR("ReloadableScrollingText", "Failed to create intermediate texture: " + std::string(SDL_GetError()));
        return false;
    }

    if (SDL_SetTextureBlendMode(intermediateTexture_, SDL_BLENDMODE_BLEND) != 0) {
        LOG_ERROR("ReloadableScrollingText", "Failed to set blend mode: " + std::string(SDL_GetError()));
        SDL_DestroyTexture(intermediateTexture_);
        intermediateTexture_ = nullptr;
        return false;
    }

    return true;
}

void ReloadableScrollingText::updateGlyphCache() {
    cachedGlyphs_.clear();
    textWidth_ = 0.0f;
    textHeight_ = 0.0f;

    FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
    if (!font) {
        return;
    }

    float scale = baseViewInfo.FontSize / font->getHeight();
    float imageMaxWidth = (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0)
        ? baseViewInfo.Width : baseViewInfo.MaxWidth;
    float imageMaxHeight = (baseViewInfo.Height < baseViewInfo.MaxHeight && baseViewInfo.Height > 0)
        ? baseViewInfo.Height : baseViewInfo.MaxHeight;

    lastScale_ = scale;
    lastImageMaxWidth_ = imageMaxWidth;
    lastImageMaxHeight_ = imageMaxHeight;

    float xPos = 0.0f;
    float yPos = 0.0f;

    if (direction_ == "horizontal") {
        for (const auto& line : text_) {
            for (char c : line) {
                FontManager::GlyphInfo glyph;
                if (font->getRect(c, glyph) && glyph.rect.h > 0) {
                    CachedGlyph cachedGlyph;
                    cachedGlyph.sourceRect = glyph.rect;
                    cachedGlyph.destRect.x = xPos;
                    cachedGlyph.destRect.y = 0;
                    cachedGlyph.destRect.w = static_cast<float>(glyph.rect.w) * scale;
                    cachedGlyph.destRect.h = static_cast<float>(glyph.rect.h) * scale;
                    cachedGlyph.advance = static_cast<float>(glyph.advance) * scale;

                    cachedGlyphs_.push_back(cachedGlyph);

                    xPos += static_cast<float>(glyph.advance) * scale;
                }
            }
        }
        textWidth_ = xPos;
    } else if (direction_ == "vertical") {
        std::vector<std::string> wrappedLines;

        for (const auto& line : text_) {
            std::istringstream iss(line);
            std::string word;
            std::string currentLine;
            float currentLineWidth = 0.0f;

            while (iss >> word) {
                float wordWidth = 0.0f;
                for (char c : word) {
                    FontManager::GlyphInfo glyph;
                    if (font->getRect(c, glyph)) {
                        wordWidth += static_cast<float>(glyph.advance) * scale;
                    }
                }
                if (!currentLine.empty()) {
                    FontManager::GlyphInfo spaceGlyph;
                    if (font->getRect(' ', spaceGlyph)) {
                        wordWidth += static_cast<float>(spaceGlyph.advance) * scale;
                    }
                }
                if (currentLineWidth + wordWidth > imageMaxWidth && !currentLine.empty()) {
                    wrappedLines.push_back(currentLine);
                    currentLine = word;
                    currentLineWidth = wordWidth;
                } else {
                    if (!currentLine.empty()) {
                        currentLine += ' ';
                    }
                    currentLine += word;
                    currentLineWidth += wordWidth;
                }
            }
            if (!currentLine.empty()) {
                wrappedLines.push_back(currentLine);
            }
        }

        for (const auto& line : wrappedLines) {
            // Calculate line width first - this is needed for all alignment types
            float lineWidth = 0.0f;
            for (char c : line) {
                FontManager::GlyphInfo glyph;
                if (font->getRect(c, glyph)) {
                    lineWidth += static_cast<float>(glyph.advance) * scale;
                }
            }

            // Set initial xPos based on alignment type
            if (alignment_ == "right") {
                xPos = imageMaxWidth - lineWidth;
            } else if (alignment_ == "centered") {
                xPos = (imageMaxWidth - lineWidth) / 2.0f;
            } else {
                // Left alignment or first position for justified
                xPos = 0.0f;
            }

            // Handle justified text specially
            if (alignment_ == "justified" && line != wrappedLines.back()) {
                float extraSpace = imageMaxWidth - lineWidth;
                std::string::size_type spaceCount = std::count(line.begin(), line.end(), ' ');
                float spaceWidth = (spaceCount > 0) ? extraSpace / spaceCount : 0.0f;
                size_t spaceIndex = 0;

                // Process each character in justified mode
                for (size_t i = 0; i < line.size(); ++i) {
                    FontManager::GlyphInfo glyph;
                    if (font->getRect(line[i], glyph) && glyph.rect.h > 0) {
                        CachedGlyph cachedGlyph;
                        cachedGlyph.sourceRect = glyph.rect;
                        cachedGlyph.destRect.x = xPos;
                        cachedGlyph.destRect.y = yPos;
                        cachedGlyph.destRect.w = static_cast<float>(glyph.rect.w) * scale;
                        cachedGlyph.destRect.h = static_cast<float>(glyph.rect.h) * scale;
                        cachedGlyph.advance = static_cast<float>(glyph.advance) * scale;

                        cachedGlyphs_.push_back(cachedGlyph);

                        xPos += static_cast<float>(glyph.advance) * scale;

                        // Add extra space after spaces in justified text
                        if (line[i] == ' ' && spaceIndex < spaceCount) {
                            xPos += spaceWidth;
                            ++spaceIndex;
                        }
                    }
                }
            } else {
                // Non-justified text (left, right, centered) or last line of justified text
                for (char c : line) {
                    FontManager::GlyphInfo glyph;
                    if (font->getRect(c, glyph) && glyph.rect.h > 0) {
                        CachedGlyph cachedGlyph;
                        cachedGlyph.sourceRect = glyph.rect;
                        cachedGlyph.destRect.x = xPos;
                        cachedGlyph.destRect.y = yPos;
                        cachedGlyph.destRect.w = static_cast<float>(glyph.rect.w) * scale;
                        cachedGlyph.destRect.h = static_cast<float>(glyph.rect.h) * scale;
                        cachedGlyph.advance = static_cast<float>(glyph.advance) * scale;

                        cachedGlyphs_.push_back(cachedGlyph);

                        xPos += static_cast<float>(glyph.advance) * scale;
                    }
                }
            }

            // Move to next line
            yPos += static_cast<float>(font->getHeight()) * scale;
        }
        textHeight_ = yPos;
    }

    needsUpdate_ = false;
    needsTextureUpdate_ = true;  // Signal that the intermediate texture needs updating
}
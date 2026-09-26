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

#include "ReloadableScrollingText.h"
#include "../ViewInfo.h"
#include "../../Database/Configuration.h"
#include "../../Database/GlobalOpts.h"
#include "../../Utility/Log.h"
#include "../../Utility/Utils.h"
#include "../../SDL.h"
#include "../Font.h"
#include "../TextEngineAtlas.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <algorithm>
#include <utility>


ReloadableScrollingText::ReloadableScrollingText(Configuration& config, bool systemMode, bool layoutMode, [[maybe_unused]] bool menuMode, std::string type, std::string textFormat, std::string singlePrefix, std::string singlePostfix, std::string pluralPrefix, std::string pluralPostfix, std::string alignment, Page& p, int displayOffset, FontManager* font, std::string direction, float scrollingSpeed, float startPosition, float startTime, float endTime, std::string location)
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
	, scrollingSpeed_(scrollingSpeed)
	, startPosition_(startPosition)
	, currentPosition_(-startPosition)
	, startTime_(startTime)
	, waitStartTime_(startTime)
	, endTime_(endTime)
	, waitEndTime_(0.0f)
	, currentCollection_("")
	, displayOffset_(displayOffset)
	, textWidth_(0)
	, location_(location) {
	text_.clear();
	lastWriteTime_ = std::filesystem::file_time_type::min();  // Initialize to the earliest possible time
}


ReloadableScrollingText::~ReloadableScrollingText() {
	ReloadableScrollingText::freeGraphicsMemory();
}

bool ReloadableScrollingText::loadFileText(const std::string& filePath) {
	std::string absolutePath = Utils::combinePath(Configuration::absolutePath, filePath);
	std::filesystem::path file(absolutePath);
	std::filesystem::file_time_type currentWriteTime;

	// Lambda to round the file time to the nearest second
	auto roundToNearestSecond = [](std::filesystem::file_time_type ftt) {
		return std::chrono::time_point_cast<std::chrono::seconds>(ftt);
		};

	try {
		currentWriteTime = std::filesystem::last_write_time(file);
		currentWriteTime = roundToNearestSecond(currentWriteTime);  // Round to nearest second
	}
	catch (const std::filesystem::filesystem_error& e) {
		LOG_ERROR("ReloadableScrollingText", "Failed to retrieve file modification time: " + std::string(e.what()));
		return false;  // Return false if there is an error
	}

	// Check if the file has been modified since the last read
	if (currentWriteTime == lastWriteTime_ && !text_.empty()) {
		// No change in file, skip update
		return false;  // File has not changed
	}

	// Store the current modification time
	lastWriteTime_ = currentWriteTime;

	// Reload the text from the file
	std::ifstream fileStream(absolutePath);
	if (!fileStream.is_open()) {
		LOG_ERROR("ReloadableScrollingText", "Failed to open file: " + absolutePath);
		return false;  // Return false if the file could not be opened
	}

	std::string line;
	text_.clear();  // Clear previous content
	shapedCacheDirty_ = true;

	while (std::getline(fileStream, line)) {
		if (direction_ == "horizontal" && !text_.empty()) {
			line = " " + line;  // Add space between lines for horizontal scrolling
		}

		// Apply text formatting (uppercase/lowercase)
		if (textFormat_ == "uppercase") {
			std::transform(line.begin(), line.end(), line.begin(), ::toupper);
		}
		else if (textFormat_ == "lowercase") {
			std::transform(line.begin(), line.end(), line.begin(), ::tolower);
		}

		text_.push_back(line);  // Add the line to the scrolling text vector
	}

	fileStream.close();

	return true;  // File was modified, return true
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

	// If the type is "file", always reload the text
	if (type_ == "file") {
		reloadTexture();
	}
	// For non-file types, use the default behavior
	else if (newItemSelected || (newScrollItemSelected && getMenuScrollReload())) {
		reloadTexture();  // Reset scroll position as usual for non-file types
		newItemSelected = false;
	}

	return Component::update(dt);
}

void ReloadableScrollingText::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	if (!fontInst_) {
		LOG_ERROR("ReloadableScrollingText", "Font instance is null during graphics memory allocation.");
		return;
	}
	reloadTexture();
}


void ReloadableScrollingText::freeGraphicsMemory() {
	Component::freeGraphicsMemory();
	text_.clear();
	shapedCacheDirty_ = true;
}


void ReloadableScrollingText::deInitializeFonts() {
	if (fontInst_) fontInst_->deInitialize();
}


void ReloadableScrollingText::initializeFonts() {
	if (fontInst_) fontInst_->initialize();
}


void ReloadableScrollingText::reloadTexture(bool resetScroll) {
	// If the type is "file", check if the file has changed
	const bool fileMode = type_ == "file" && !location_.empty();
	if (fileMode) {
		if (!loadFileText(location_)) return;
		resetScroll = true;
	}

	if (resetScroll) {
		// Reset scroll position
		if (direction_ == "horizontal") {
			currentPosition_ = -startPosition_;
		}
		else if (direction_ == "vertical") {
			currentPosition_ = -startPosition_;
		}

		// Reset start and end times to ensure proper wait timing after reload
		waitStartTime_ = startTime_;
		waitEndTime_ = 0.0f;  // Reset to zero when scroll restarts
	}

	if (fileMode) return;

	text_.clear();
	shapedCacheDirty_ = true;

	// Load the appropriate text content

	Item* selectedItem = page.getSelectedItem(displayOffset_);
	if (!selectedItem) {
		return;
	}

	config_.getProperty("currentCollection", currentCollection_);

	// build clone list
	std::vector<std::string> names;

	names.push_back(selectedItem->name);
	names.push_back(selectedItem->fullTitle);

	if (selectedItem->cloneof.length() > 0) {
		names.push_back(selectedItem->cloneof);
	}

	// Check for corresponding .txt files
	for (unsigned int n = 0; n < names.size() && text_.empty(); ++n) {

		std::string basename = names[n];

		Utils::replaceSlashesWithUnderscores(basename);

		if (systemMode_) {

			// check the master collection for the system artifact 
			loadText(collectionName, type_, type_, "", true);

			// check collection for the system artifact
			if (text_.empty()) {
				loadText(selectedItem->collectionInfo->name, type_, type_, "", true);
			}

		}
		else {
			// are we looking at a leaf or a submenu
			if (selectedItem->leaf) // item is a leaf
			{

				// check the master collection for the artifact 
				loadText(collectionName, type_, basename, "", false);

				// check the collection for the artifact
				if (text_.empty()) {
					loadText(selectedItem->collectionInfo->name, type_, basename, "", false);
				}
			}
			else // item is a submenu
			{

				// check the master collection for the artifact
				loadText(collectionName, type_, basename, "", false);

				// check the collection for the artifact
				if (text_.empty()) {
					loadText(selectedItem->collectionInfo->name, type_, basename, "", false);
				}

				// check the submenu collection for the system artifact
				if (text_.empty()) {
					loadText(selectedItem->name, type_, type_, "", true);
				}
			}
		}
	}

	// Check for thext in the roms directory
	if (text_.empty())
		loadText(selectedItem->filepath, type_, type_, selectedItem->filepath, false);

	// Check for supported fields if text is still empty
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
			if (selectedItem->leaf) // item is a leaf
				text = selectedItem->year;
			else // item is a collection
				(void)config_.getProperty("collections." + selectedItem->name + ".year", text);
		}
		else if (type_ == "title") {
			text = selectedItem->title;
		}
		else if (type_ == "developer") {
			text = selectedItem->developer;
			// Overwrite in case developer has not been specified
			if (text == "") {
				text = selectedItem->manufacturer;
			}
		}
		else if (type_ == "manufacturer") {
			if (selectedItem->leaf) // item is a leaf
				text = selectedItem->manufacturer;
			else // item is a collection
				(void)config_.getProperty("collections." + selectedItem->name + ".manufacturer", text);
		}
		else if (type_ == "genre") {
			if (selectedItem->leaf) // item is a leaf
				text = selectedItem->genre;
			else // item is a collection
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
		else if (!selectedItem->leaf) // item is not a leaf
		{
			(void)config_.getProperty("collections." + selectedItem->name + "." + type_, text);
		}

		if (text == "0") {
			text = singlePrefix_ + text + pluralPostfix_;
		}
		else if (text == "1") {
			text = singlePrefix_ + text + singlePostfix_;
		}
		else if (text != "") {
			text = pluralPrefix_ + text + pluralPostfix_;
		}

		if (text != "") {
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

}


void ReloadableScrollingText::loadText(std::string collection, std::string type, std::string basename, std::string filepath, bool systemMode) {

	std::string textPath = "";

	// check the system folder
	if (layoutMode_) {
		// check if collection's assets are in a different theme
		std::string layoutName;
		config_.getProperty("collections." + collection + ".layout", layoutName);
		if (layoutName == "") {
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
	if (filepath != "")
		textPath = filepath;

	textPath = Utils::combinePath(textPath, basename);

	textPath += ".txt";

	std::ifstream includeStream(textPath.c_str());

	if (!includeStream.good()) {
		return;
	}

	std::string line;

	while (std::getline(includeStream, line)) {

		// In horizontal scrolling direction, add a space before every line except the first.
		if (direction_ == "horizontal" && !text_.empty()) {
			line = " " + line;
		}

		// Reformat lines to uppercase or lowercase
		if (textFormat_ == "uppercase") {
			std::transform(line.begin(), line.end(), line.begin(), ::toupper);
		}
		if (textFormat_ == "lowercase") {
			std::transform(line.begin(), line.end(), line.begin(), ::tolower);
		}

		text_.push_back(line);

	}

	return;

}

void ReloadableScrollingText::draw() {
	Component::draw();


	FontManager* font = baseViewInfo.font ? baseViewInfo.font : fontInst_;
	if (!font || text_.empty() || waitEndTime_ > 0.0f || baseViewInfo.Alpha <= 0.0f) {
		return;
	}

	// --- Select the best MipLevel for the current render size ---
	const FontManager::MipLevel* mip = font->getMipLevelForHeight(baseViewInfo.FontSize);
	if (!mip || !mip->font) {
		// If no suitable mip is found, we cannot draw.
		return;
	}

	float imageMaxWidth = 0;
	float imageMaxHeight = 0;
	if (baseViewInfo.Width < baseViewInfo.MaxWidth && baseViewInfo.Width > 0) {
		imageMaxWidth = baseViewInfo.Width;
	}
	else {
		imageMaxWidth = baseViewInfo.MaxWidth;
	}
	if (baseViewInfo.Height < baseViewInfo.MaxHeight && baseViewInfo.Height > 0) {
		imageMaxHeight = baseViewInfo.Height;
	}
	else {
		imageMaxHeight = baseViewInfo.MaxHeight;
	}

	// The scale is now calculated relative to the chosen mip's height.
	const float mipHeight = static_cast<float>(mip->height);
	float scale = (mipHeight > 0.f) ? (static_cast<float>(baseViewInfo.FontSize) / mipHeight) : 1.0f;

	float xOrigin = baseViewInfo.XRelativeToOrigin();
	float yOrigin = baseViewInfo.YRelativeToOrigin();

	if (auto* engine = font->getTextEngineAtlas(mip)) {
			const int layoutW = page.getLayoutWidthByMonitor(baseViewInfo.Monitor);
			const int layoutH = page.getLayoutHeightByMonitor(baseViewInfo.Monitor);
			const float viewportW = imageMaxWidth > 0 ? imageMaxWidth : static_cast<float>(layoutW);
			const float viewportH = imageMaxHeight > 0 ? imageMaxHeight : static_cast<float>(layoutH);
			const SDL_FRect viewport{xOrigin, yOrigin, viewportW, viewportH};
			if (shapedCacheDirty_) {
				shapedHorizontalText_.clear();
				for (const auto& line : text_) shapedHorizontalText_ += line;
				shapedRows_.clear();
				shapedWrapWidth_ = -1.0f;
				shapedScale_ = -1.0f;
				shapedCacheDirty_ = false;
			}
			const bool fontChanged = shapedScale_ != scale ||
				shapedMipSize_ != mip->fontSize ||
				shapedFontGeneration_ != font->getResourceGeneration();
			if (direction_ == "horizontal") {
				if (fontChanged && !engine->measure(shapedHorizontalText_, scale,
					shapedHorizontalWidth_)) return;
				shapedScale_ = scale;
				shapedMipSize_ = mip->fontSize;
				shapedFontGeneration_ = font->getResourceGeneration();
				textWidth_ = shapedHorizontalWidth_;
				if (startPosition_ == 0.0f && shapedHorizontalWidth_ <= viewportW)
					currentPosition_ = 0.0f;
				if (currentPosition_ > shapedHorizontalWidth_) {
					waitStartTime_ = startTime_;
					waitEndTime_ = endTime_;
					currentPosition_ = -startPosition_;
				}
				if (engine->drawTransformed(shapedHorizontalText_,
					xOrigin - currentPosition_, yOrigin, scale, baseViewInfo.textColor,
					baseViewInfo, layoutW, layoutH, &viewport)) return;
			} else if (direction_ == "vertical") {
				if (fontChanged || shapedWrapWidth_ != viewportW) {
					shapedRows_.clear();
					auto pushRow = [&](const std::string& value, float width, bool justify) {
						ShapedRow row;
						row.text = value;
						row.width = width;
						row.justify = justify;
						if (justify) {
							std::istringstream words(value);
							std::string word;
							while (words >> word) {
								float wordWidth = 0.0f;
								engine->measure(word, scale, wordWidth);
								row.words.push_back(word);
								row.wordWidths.push_back(wordWidth);
							}
						}
						shapedRows_.push_back(std::move(row));
					};
					for (const auto& sourceLine : text_) {
						std::vector<std::string> lines;
						if (!engine->wrapLines(sourceLine, scale, viewportW, lines)) return;
						for (size_t i = 0; i < lines.size(); ++i) {
							float width = 0.0f;
							if (!engine->measure(lines[i], scale, width)) return;
							pushRow(lines[i], width, alignment_ == "justified" && i + 1 < lines.size());
						}
					}
					shapedWrapWidth_ = viewportW;
					shapedScale_ = scale;
					shapedMipSize_ = mip->fontSize;
					shapedFontGeneration_ = font->getResourceGeneration();
				}
				const float lineHeight = mipHeight * scale;
				const float totalHeight = shapedRows_.size() * lineHeight;
				if (startPosition_ == 0.0f && totalHeight <= viewportH) {
					currentPosition_ = 0.0f;
					waitStartTime_ = 0.0f;
					waitEndTime_ = 0.0f;
				}
				if (currentPosition_ > totalHeight) {
					waitStartTime_ = startTime_;
					waitEndTime_ = endTime_;
					currentPosition_ = -startPosition_;
				}
				bool drawn = true;
				for (size_t i = 0; i < shapedRows_.size(); ++i) {
					const auto& row = shapedRows_[i];
					const float rowY = yOrigin + i * lineHeight - currentPosition_;
					if (rowY + lineHeight <= yOrigin || rowY >= yOrigin + viewportH) continue;
					if (row.justify && row.words.size() > 1) {
						float wordWidthSum = 0.0f;
						for (float wordWidth : row.wordWidths) wordWidthSum += wordWidth;
						if (!drawn) break;
						const float gap = std::max(0.0f,
							(viewportW - wordWidthSum) / (row.words.size() - 1));
						float wordX = xOrigin;
						for (size_t wordIndex = 0; wordIndex < row.words.size(); ++wordIndex) {
							if (!engine->drawTransformed(row.words[wordIndex], wordX, rowY, scale,
								baseViewInfo.textColor, baseViewInfo, layoutW, layoutH,
								&viewport)) { drawn = false; break; }
							wordX += row.wordWidths[wordIndex] + gap;
						}
					} else if (!row.text.empty()) {
						float rowX = xOrigin;
						if (alignment_ == "right") rowX += viewportW - row.width;
						else if (alignment_ == "centered") rowX += (viewportW - row.width) * 0.5f;
						if (!engine->drawTransformed(row.text, rowX, rowY, scale,
							baseViewInfo.textColor, baseViewInfo, layoutW, layoutH,
							&viewport)) drawn = false;
					}
					if (!drawn) break;
				}
				if (drawn) return;
			}
	}
}

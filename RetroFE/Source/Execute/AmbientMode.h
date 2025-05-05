#pragma once

#include <filesystem>
#include <vector>
#include <algorithm>
#include <random>
#include <string>
#include <chrono>
#include "../SDL.h"
#include "SDL_image.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "../Control/UserInput.h"

class AmbientMode {
public:
    AmbientMode(UserInput& input, const std::string& basePath, int minutesPerImage)
        : input_(input), basePath_(basePath), minutesPerImage_(minutesPerImage) {}

    void activate();
    void display(std::string imageName, int screenNum);
    void displayImages(int imageIndex);

private:
    void populateImageFiles(std::string directory);
    UserInput& input_;
    std::string basePath_;    
    std::string ambientPath_;
    std::vector<std::string> imageFiles_;
    std::vector<std::string> marqueeImageFiles_;
    int minutesPerImage_;
};
#pragma once

#include <filesystem>
#include <vector>
#include <algorithm>
#include <random>
#include <string>
#include <chrono>
#include "../SDL.h"
#if __has_include(<SDL2/SDL_image.h>)
    #include <SDL2/SDL_image.h>
#elif __has_include(<SDL2_image/SDL_image.h>)
    #include <SDL2_image/SDL_image.h>
#else
    #error "Cannot find SDL_image header"
#endif
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "../Control/UserInput.h"

class AmbientMode {
public:
    AmbientMode(UserInput& input, const std::string& basePath, int minutesPerImage)
        : input_(input), basePath_(basePath), minutesPerImage_(minutesPerImage) {}

    void activate();
    

private:
    void populateImageFiles();
    std::string determineMarqueePath(int imageIndex);    
    void displayImages(SDL_Texture *currentImage, SDL_Texture *nextImage, float alphaOfFirstImage, int screenNum);
    UserInput& input_;
    std::string basePath_;    
    std::string ambientPath_;
    std::vector<std::string> imageFiles_;
    std::vector<std::string> marqueeImageFiles_;
    int minutesPerImage_;
    SDL_Texture* currentImage_ = nullptr;
    SDL_Texture* nextImage_ = nullptr;
    SDL_Texture* currentImageMarquee_ = nullptr;
    SDL_Texture* nextImageMarquee_ = nullptr;
};

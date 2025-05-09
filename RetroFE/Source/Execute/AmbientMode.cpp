#include "AmbientMode.h"
/*
This is the main class for "Ambient Mode".

## Why and What

The intent of Ambient Mode is to allow your arcade cabinet to assume a low-key presence in a room. For example, in a living room setting, you might not
want the cabinet to be a focal point all the time. Ambient Mode allows the cabinet to recede to the background without powering it all the way off.

When enabled:
* the "exit" controller combo button from the retrofe main menu will go to ambient mode instead of exiting retrofe.
* while in ambient mode, images from the "ambient" directory will be displayed on the main screen, and rotated periodically
* to exit ambient mode, the controller combo button OR the action button will return you to the main retrofe menu.

## Configuration

1) create a directory called "ambient" in the same directory as the RetroFE executable, and populate it with images. 
If you have a 2nd monitor, you should have at least one image name ending with "_marquee.png" (or any other common image extenion)

2) in settings.conf:

    controllerComboExit = false
    controllerComboAmbient = true
    ambientModeMinutesPerImage = 2 # OPTIONAL - how often to change to a new images. if left unspecified, default is 2 minutes

## Marquee display

If you have two monitors, the second monitor is assumed to be a marquee display. When a new image is displayed on the main screen,
the corresponding marquee image will be displayed if it exists, by looking for "*_marquee.ext". For example, if "sunset.png" is being displayed on 
the main screen, the system will look for "sunset_marquee.png" to display on the marquee. 
If no corresponding marquee image is found, a random marquee image is displayed instead.
*/

void AmbientMode::activate() {    

    // set member variables
    imageFiles_.clear();    
    marqueeImageFiles_.clear();
    ambientPath_ = Utils::combinePath(basePath_, "ambient");
    LOG_INFO("AmbientMode", "Activating Ambient mode with "+std::to_string(SDL::getScreenCount())+" screen(s). Path for images is: " + ambientPath_);

    // Ensure the directory exists
    if (!std::filesystem::is_directory(ambientPath_)) {
        LOG_ERROR("AmbientMode", "Ambient directory does not exist: " + ambientPath_);
        return;
    }

    // Get lists of image files and marquee image files into our member variables
    populateImageFiles();    

    if (imageFiles_.empty()) {
        LOG_ERROR("AmbientMode", "Ambient mode will not be launched, since there are no images for the main screen in " + ambientPath_);
        return;
    } else {
        LOG_INFO("AmbientMode", "There are " + std::to_string(imageFiles_.size()) + " images and " + std::to_string(marqueeImageFiles_.size()) + " marquee images in the ambient directory.");
    }

    // Shuffle the image files to randomize the order
    auto rng = std::default_random_engine(std::random_device{}());
    std::shuffle(std::begin(imageFiles_), std::end(imageFiles_), rng);
    
    input_.resetStates();
    SDL_Event e;    

    int fadeStartTime = 0;
    bool isFading = false; 
    int fadeDuration = 2000; // 2 second fade durationn. Could be made configurable in the future.
    float firstImageOpacity = 1.0f; // 1 = fully opaque; 0 = fully transparent
    auto lastChangeTime = std::chrono::steady_clock::now();
    int imageIndex = 0;
    
    SDL_Renderer* rendererMain = SDL::getRenderer(0); // Get the renderer for the main screen
    SDL_Renderer* rendererMarquee = SDL::getRenderer(1); // Get the renderer for the Marquee screen
    
    currentImage_ = loadTexture(rendererMain, imageFiles_[imageIndex].c_str());
    if (SDL::getScreenCount() > 1) {
        currentImageMarquee_ = loadTexture(rendererMarquee, determineMarqueePath(imageIndex).c_str());
    } 
    

    // Main loop for ambient mode
    while (true) {
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastChangeTime);
    
        if (!isFading) {                                    
            if (elapsedTime.count() >= minutesPerImage_ * 60 || 
                input_.keystate(UserInput::KeyCodeRight) || 
                input_.keystate(UserInput::KeyCodeLeft)) 
            {                 
                imageIndex = (imageIndex + 1) % imageFiles_.size(); // Increment the image index, wrapping around if necessary                
                nextImage_ = loadTexture(rendererMain, imageFiles_[imageIndex].c_str());
                if (SDL::getScreenCount() > 1) {
                    nextImageMarquee_ = loadTexture(rendererMarquee, determineMarqueePath(imageIndex).c_str());
                }      
                isFading = true;
                fadeStartTime = SDL_GetTicks(); // Record the start time of the fade                
                LOG_INFO("AmbientMode", "start fading to new image: " + imageFiles_[imageIndex]);
            }
        }

        // Handle fading
        if (isFading) {
            int currentFadeTime = SDL_GetTicks() - fadeStartTime;
            firstImageOpacity = 1.0f - static_cast<float>(currentFadeTime) / fadeDuration;
            firstImageOpacity < 0.0f ? firstImageOpacity = 0.0f : NULL; // Clamp opacity lower value to 0.0f
            
            // check if we're done fading
            if (firstImageOpacity == 0.0f) {
                lastChangeTime = currentTime; // Reset the timer                
                isFading = false; // Reset the fade state                
                
                SDL_DestroyTexture(currentImage_); // Destroy the old texture
                currentImage_ = nextImage_; // Set the currentImage pointer so it will will now render
                nextImage_ = nullptr; // Reset the next image texture
                
                SDL_DestroyTexture(currentImageMarquee_); // Destroy the old texture
                currentImageMarquee_ = nextImageMarquee_; // Set the currentImage so it will renter the next iteration
                nextImageMarquee_ = nullptr; // Reset the next image texture

                firstImageOpacity = 1.0f; 
                LOG_INFO("AmbientMode", "done fading "); 
            }
        } 
        
        // Display the current image (blended with the 2nd if needed) on the main screen
        displayImages(currentImage_, nextImage_, firstImageOpacity, 0); 
        // Display the current image (blended with the 2nd if needed) on the marquee screen
        if (SDL::getScreenCount() > 1) {
            displayImages(currentImageMarquee_, nextImageMarquee_, firstImageOpacity, 1); 
        }
        
        // Check events to see if it's time to exit ambient mode
        SDL_PollEvent(&e);
        input_.update(e);

        if (input_.keystate(UserInput::KeyCodeSelect) || 
                (input_.keystate(UserInput::KeyCodeQuitCombo1) && input_.keystate(UserInput::KeyCodeQuitCombo2))) {

            input_.resetStates();
            break; // by breaking, we will exit the ambient mode loop, exit this function, and cease to block the main thread, thereby returning the user to retrofe.
        }

        // little delay to avoid busy waiting
        SDL_Delay(16); // SDL_Delay takes milliseconds, so this results in ~60 FPS. There's not enough going on in this loop to conditionally reduce the delay.
    }

}

/*
The thing to know about this method is that nextImage_ CAN be a nullptr. That's the case when images NOT in the process of fading in/out.
If both images ARE provided, this method will render some blend between them, based on the firstImageOpacity value.

@firstImageOpacity: 0.0f = fully transparent, 1.0f = fully opaque
*/
void AmbientMode::displayImages(SDL_Texture* currentImage, SDL_Texture* nextImage, float firstImageOpacity, int screenNum) {
    // safety -- if the screen number is out of bounds, this call is a no-op
    if (screenNum + 1 > SDL::getScreenCount() ) {
        return; 
    }

    SDL_Renderer* renderer = SDL::getRenderer(screenNum);
    SDL_RenderClear(renderer); // Clear the screen

    // Set the alpha value for the first image
    SDL_SetTextureAlphaMod(currentImage, static_cast<Uint8>(firstImageOpacity * 255)); // Set the alpha value for the first image

    // Render the first image
    SDL_RenderCopy(renderer, currentImage, nullptr, nullptr);

    if (nextImage != nullptr) {
        float alphaOfSecondImage = 1.0f - firstImageOpacity; // Inverse of the first image's alpha
        SDL_SetTextureAlphaMod(nextImage, static_cast<Uint8>(alphaOfSecondImage * 255)); // Set the alpha value for the second image
        SDL_RenderCopy(renderer, nextImage, nullptr, nullptr);
    }

    SDL_RenderPresent(renderer); // Present the rendered frame
}
/*
The point of this mehtod is to decide which marquee image to display, given a specific image for the main screen.

 @imageIndex:  refers to the index of an image in the imageFiles_ vector (the main screen).
 @returns: the full path to some image, OR POSIBLY nullptr if we aren't doing marquees.
*/
std::string AmbientMode::determineMarqueePath(int imageIndex) {
    // for the main screen, just display the image by index
    std::string imageName = imageFiles_[imageIndex];
    std::string marqueeImageName;
    std::string marqueeImagePath = nullptr;
    
    // for the marquee screen, determine the corresponding marquee image if available (by naming convention), 
    // otherwise return a random marquee image.
    if (!marqueeImageFiles_.empty()) {
        std::filesystem::path path(imageName);
        std::string baseName = path.stem().string(); // Get the filename without extension
        std::string extension = path.extension().string(); // Get the file extension        
        marqueeImageName = baseName +"_marquee" + extension;
        marqueeImagePath = Utils::combinePath(ambientPath_, marqueeImageName);

        if (!std::filesystem::exists(marqueeImagePath)) {
            marqueeImageName = marqueeImageFiles_[std::rand() % marqueeImageFiles_.size()];                    
            marqueeImagePath = Utils::combinePath(ambientPath_, marqueeImageName);
            LOG_INFO("AmbientMode", "There is no matching ambient image for "+marqueeImagePath+". Displaying random marquee image: "+ marqueeImagePath);
        }
    }
    return marqueeImagePath; // Return the path to the marquee image
}


/*
This method modifies the members imageFiles_ and marqueeImageFiles_. It's intented to be called early, either in instantation or first use.
The vectors, once populated, will contain strings with full paths to image files.
*/
void AmbientMode::populateImageFiles()
{
    namespace fs = std::filesystem;

    // Supported image extensions
    const std::vector<std::string> imageExtensions = { ".png", ".jpg", ".jpeg" };

    // Iterate through the directory
    for (const auto& entry : fs::directory_iterator(ambientPath_)) {
        if (entry.is_regular_file()) {
            std::string extension = entry.path().extension().string();

            // Check if the file has a supported image extension
            if (std::find(imageExtensions.begin(), imageExtensions.end(), extension) != imageExtensions.end()) {
                std::string filenameWithoutExtension = entry.path().stem().string(); // Get the filename without extension
                std::string filenameWithExtension = entry.path().filename().string(); // Get the filename with extension

                // Check if the filename (without extension) ends with "marquee"
                if (filenameWithoutExtension.size() >= 8 && filenameWithoutExtension.substr(filenameWithoutExtension.size() - 8) == "_marquee") {
                    marqueeImageFiles_.push_back(entry.path().string());
                } else {
                    imageFiles_.push_back(entry.path().string()); // Store the full path of the image file
                }
            }
        }
    }
}

/* This is needed for image formats that do not support alpha channels (like JPEG).
Convert everything we load to a format that does support alpha channels.
*/
SDL_Texture* AmbientMode::loadTexture(SDL_Renderer* renderer, const std::string& imagePath) {
    // Load the image as a surface
    SDL_Surface* loadedSurface = IMG_Load(imagePath.c_str());
    if (!loadedSurface) {
        LOG_ERROR("AmbientMode", "Failed to load image: " + imagePath + " - " + IMG_GetError());
        return nullptr;
    }

    // Convert the surface to a format with an alpha channel (RGBA8888)
    SDL_Surface* surfaceWithAlpha = SDL_ConvertSurfaceFormat(loadedSurface, SDL_PIXELFORMAT_RGBA8888, 0);    
    SDL_FreeSurface(loadedSurface); // Free the original surface
    if (!surfaceWithAlpha) {
        LOG_ERROR("AmbientMode", "Failed to convert surface to RGBA8888: " + std::string(SDL_GetError()));
        return nullptr;
    }

    // Create a texture from the surface
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surfaceWithAlpha);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surfaceWithAlpha); // Free the converted surface
    if (!texture) {
        LOG_ERROR("AmbientMode", "Failed to create texture: " + std::string(SDL_GetError()));
    }

    return texture;
}
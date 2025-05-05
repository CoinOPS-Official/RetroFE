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
    ambientModeMinutesPerImage = 30 # OPTIONAL - how often to change to a new images. if left unspecified, default is 30 minutes

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
    LOG_INFO("AmbientMode", "Activating Ambient mode. Path for images is: " + ambientPath_);


    // Ensure the directory exists
    if (!std::filesystem::is_directory(ambientPath_)) {
        LOG_ERROR("AmbientMode", "Ambient directory does not exist: " + ambientPath_);
        return;
    }

    // Get lists of image files and marquee image files into our member variables
    populateImageFiles(ambientPath_);
    LOG_INFO("AmbientMode", "There are " + std::to_string(imageFiles_.size()) + " images and " + std::to_string(marqueeImageFiles_.size()) + " marquee images in the ambient directory.");

    if (imageFiles_.empty()) {
        LOG_ERROR("AmbientMode", "Ambient mode will not be launched, since there are no images for the main screen in " + ambientPath_);
        return;
    }

    // Shuffle the image files to randomize the order
    auto rng = std::default_random_engine(std::random_device{}());
    std::shuffle(std::begin(imageFiles_), std::end(imageFiles_), rng);
    
    input_.resetStates();
    SDL_Event e;    
    auto lastChangeTime = std::chrono::steady_clock::now();
    int imageIndex = 0;
    displayImages(imageIndex);

    // Main loop for ambient mode
    while (true) {
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastChangeTime);
        if (elapsedTime.count() >= minutesPerImage_ * 60) {
            // update images
            imageIndex = (imageIndex + 1) % imageFiles_.size();
            displayImages(imageIndex);
            lastChangeTime = currentTime; // Reset the timer
        }
        
        // Check events to see if it's time to exit ambient mode
        SDL_PollEvent(&e);
        input_.update(e);

        if (input_.keystate(UserInput::KeyCodeSelect) || 
                (input_.keystate(UserInput::KeyCodeQuitCombo1) && input_.keystate(UserInput::KeyCodeQuitCombo2))) {

            input_.resetStates();
            break; // by breaking, we will exit the ambient mode loop, exit this function, and cease to block the main thread, thereby returning the user to retrofe.
        }
        // you can also move through the images with left or right. Honestly, more useful for testing than anything else.
        if (input_.keystate(UserInput::KeyCodeRight) ) {
            imageIndex = (imageIndex + 1) % imageFiles_.size();
            displayImages(imageIndex);
            lastChangeTime = currentTime; // Reset the timer
            input_.resetStates();
        }
        if (input_.keystate(UserInput::KeyCodeLeft)) {
            imageIndex = (imageIndex - 1) % imageFiles_.size();
            displayImages(imageIndex);
            lastChangeTime = currentTime; // Reset the timer
            input_.resetStates();
        }        
        
        // little delay to avoid busy waiting
        SDL_Delay(10);
    }

}

// Takes care of displaying the images on both the main and marquee screens. 
// The imageIndex refers to the index of an image in the imageFiles_ vector (the main screen).
void AmbientMode::displayImages(int imageIndex) {
    // for the main screen, just display the image by index
    std::string imageName = Utils::combinePath(ambientPath_, imageFiles_[imageIndex]);
    display(imageName, 0); // Display on the first screen
    

    // for the marquee screen, display the corresponding marquee image if available (by naming convention), 
    // otherwise display a random marquee image.
    if (!marqueeImageFiles_.empty()) {    
        std::filesystem::path path(imageName);
        std::string baseName = path.stem().string(); // Get the filename without extension
        std::string extension = path.extension().string(); // Get the file extension        
        std::string marqueeImageName = baseName +"_marquee" + extension;
        std::string marqueeImagePath = Utils::combinePath(ambientPath_, marqueeImageName);

        if (std::filesystem::exists(marqueeImagePath)) {
            LOG_INFO("AmbientMode", "displaying corresponding marquee image: "+ marqueeImageName);
            display(marqueeImageName, 1); // Display on the second screen
        } else {            
            std::string randomMarqueeImage = marqueeImageFiles_[std::rand() % marqueeImageFiles_.size()];        
            LOG_INFO("AmbientMode", "displaying random marquee image: "+ randomMarqueeImage);
            display(randomMarqueeImage, 1); // Display on the second screen
        }
    }
}


// display a specific image (indentified by name only; it is assumed to be in the "ambient" directory) on the specified screen
void AmbientMode::display(std::string imageName, int screenNum) {
    std::string imagePath = Utils::combinePath(ambientPath_, imageName);
    
    SDL_LockMutex(SDL::getMutex());
    SDL_Renderer* renderer = SDL::getRenderer(screenNum);
    SDL_Surface* surface = IMG_Load(imagePath.c_str());
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    // Render the texture
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);

    SDL_UnlockMutex(SDL::getMutex());
}

void AmbientMode::populateImageFiles(std::string directoryPath) {
    namespace fs = std::filesystem;

    // Supported image extensions
    const std::vector<std::string> imageExtensions = { ".png", ".jpg", ".jpeg", ".bmp", ".gif" };

    // Iterate through the directory
    for (const auto& entry : fs::directory_iterator(directoryPath)) {
        if (entry.is_regular_file()) {
            std::string extension = entry.path().extension().string();

            // Check if the file has a supported image extension
            if (std::find(imageExtensions.begin(), imageExtensions.end(), extension) != imageExtensions.end()) {
                std::string filenameWithoutExtension = entry.path().stem().string(); // Get the filename without extension
                std::string filenameWithExtension = entry.path().filename().string(); // Get the filename with extension

                // Check if the filename (without extension) ends with "marquee"
                if (filenameWithoutExtension.size() >= 8 && filenameWithoutExtension.substr(filenameWithoutExtension.size() - 8) == "_marquee") {
                    marqueeImageFiles_.push_back(filenameWithExtension);
                } else {
                    imageFiles_.push_back(filenameWithExtension);
                }
            }
        }
    }
}

#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <SDL2_mixer/SDL_mixer.h>

#include "../Utility/Utils.h"
#include "../Database/Configuration.h"
#include "Player.h"

// User Defined
std::string INI_PATH = Utils::combinePath(Configuration::absolutePath, "config.ini");
std::string NOWPLAYING_PATH = Utils::combinePath(Configuration::absolutePath, "NowPlaying.txt");
const char* APP_TO_MONITOR = "WhatsApp";

// Settings variables
std::string FOLDER_TO_PLAY = Utils::combinePath(Configuration::absolutePath, "music");
char PLAYLIST_FILE_PATH[256];
int FADE_STEPS;
int FADE_STEPS_DURATION;
int INITIAL_VOL;
int PLAY_MUSIC_INGAME;
int INGAME_VOL;
int SHUFFLE;
int RESUME_NEXT_TRACK;
int KEYBOARD_CONTROL;
int GAMEPAD_CONTROL_FULL;
int GAMEPAD_CONTROL_SIMPLE;
int GAMEPAD_CONTROL_PAUSE;
int GAMEPAD_CONTROL_L1R1;
char KEY_PAUSE = '\''; // Default key for pausing playback
char KEY_NEXT = '.' ; // Default key for playing the next track
char KEY_PREV = ','; // Default key for playing the previous track
char KEY_VOL_UP = ']'; // Default key for increasing volume
char KEY_VOL_DOWN = '['; //Default key for increasing volume
int OSD;
int OSD_SCREEN;
int OSD_PERCENTFROMBOTTOM;

// Music Player Variables
Mix_Music *music = NULL;
int volume = 0;
int currentTrackIndex = 0;
int trackCount = 0;
char **trackList = NULL;

// SDL Variables
SDL_GameController *gamepad = NULL;

namespace MusicPlayer {

// Function to get song metadata
std::string readID3v1Tag(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        return "Error opening file\n";
    }

    file.seekg(-128, std::ios::end); // Seek to last 128 bytes
    ID3v1Tag tag;
    file.read(reinterpret_cast<char*>(&tag), sizeof(ID3v1Tag));

    if (std::string(tag.tag, 3) == "TAG") {
//        std::cout << "Title: " << std::string(tag.title, 30) << "\n";
//        std::cout << "Artist: " << std::string(tag.artist, 30) << "\n";
//        std::cout << "Album: " << std::string(tag.album, 30) << "\n";
//        std::cout << "Year: " << std::string(tag.year, 4) << "\n";
        
        // Create the NowPlaying string
        std::string nowPlaying = tag.artist + std::string(" - ") + tag.title;
        return nowPlaying;
    } else {
        return "No ID3v1 tag found\n";
    }
}

// Helper function to check if a file exists
int fileExists(const char *path) {
    FILE *file = fopen(path, "r");
    if (file) {
        fclose(file);
        return 1;
    } else {
        return 0;
    }
}

// Helper function to sort A-Z with qsort
int compareStrings(const void* a, const void* b) {
    const char* str1 = *(const char**)a;
    const char* str2 = *(const char**)b;
    return strcmp(str1, str2);
}

// Helper function to write current playing to a file
void writeNowPlayingToFile(const std::string& nowPlaying) {
    std::ofstream nowPlayingFile(NOWPLAYING_PATH);
    if (nowPlayingFile.is_open()) {
        nowPlayingFile << nowPlaying << std::endl;
        nowPlayingFile.close();
    } else {
        std::cout << "Failed to write to nowplaying.txt" << std::endl;
    }
}

// Helper function for parsing the ini
void stripQuotes(char *str) {
    int len = strlen(str);
    // Check if the string starts and ends with a quote and strip them
    if (len > 1 && str[0] == '"' && str[len - 1] == '"') {
        // Shift the string to remove the quotes
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0'; // Null terminate the string after removing the quotes
    }
    // Remove extra spaces around the value
    char *start = str;
    while (*start == ' ') start++;  // Remove leading spaces
    char *end = str + strlen(str) - 1;
    while (end > start && *end == ' ') end--;  // Remove trailing spaces
    *(end + 1) = '\0';  // Null terminate the string
}

// Function to parse the INI file
int loadSettingsFromFile(const std::string& filename) {
    FILE *file = fopen(filename.c_str(), "r");
    if (!file) {
        printf("Error opening INI file: %s\n", filename.c_str());
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        // Remove newline characters
        line[strcspn(line, "\n")] = 0;
        
        // Ignore empty lines and comments
        if (line[0] == '\0' || line[0] == ';') continue;

        // Parse key-value pairs
        else if (strchr(line, '=')) {
            char key[128], value[256];
            sscanf(line, "%127[^=]=%255[^\n]", key, value);

            // Remove quotes from the value if present
            stripQuotes(value);

            // Assign values to settings based on key
            if (strcmp(key, "FOLDER_TO_PLAY") == 0) {
                std::string FOLDER_TO_PLAY = value;
            } else if (strcmp(key, "PLAYLIST_FILE_PATH") == 0) {
                strncpy(PLAYLIST_FILE_PATH, value, sizeof(PLAYLIST_FILE_PATH) - 1);
            } else if (strcmp(key, "FADE_STEPS") == 0) {
                FADE_STEPS = atoi(value); // Convert to integer
            } else if (strcmp(key, "FADE_STEPS_DURATION") == 0) {
            FADE_STEPS_DURATION = atoi(value); // Convert to integer
            } else if (strcmp(key, "INITIAL_VOL") == 0) {
                INITIAL_VOL = atoi(value); // Convert to integer
            } else if (strcmp(key, "PLAY_MUSIC_INGAME") == 0) {
            PLAY_MUSIC_INGAME = atoi(value); // Convert to integer
            } else if (strcmp(key, "INGAME_VOL") == 0) {
                INGAME_VOL = atoi(value); // Convert to integer
            } else if (strcmp(key, "SHUFFLE") == 0) {
                SHUFFLE = atoi(value); // Convert to integer
            } else if (strcmp(key, "RESUME_NEXT_TRACK") == 0) {
                RESUME_NEXT_TRACK = atoi(value); // Convert to integer
            } else if (strcmp(key, "KEYBOARD_CONTROL") == 0) {
                KEYBOARD_CONTROL = atoi(value); // Convert to integer
            } else if (strcmp(key, "GAMEPAD_CONTROL_FULL") == 0) {
                GAMEPAD_CONTROL_FULL = atoi(value); // Convert to integer
            } else if (strcmp(key, "GAMEPAD_CONTROL_SIMPLE") == 0) {
                GAMEPAD_CONTROL_SIMPLE = atoi(value); // Convert to integer
            } else if (strcmp(key, "GAMEPAD_CONTROL_PAUSE") == 0) {
                GAMEPAD_CONTROL_PAUSE = atoi(value); // Convert to integer
            } else if (strcmp(key, "GAMEPAD_CONTROL_L1R1") == 0) {
                GAMEPAD_CONTROL_L1R1 = atoi(value); // Convert to integer
            } else if (strcmp(key, "KEY_PAUSE") == 0) {
                KEY_PAUSE = value[0];
            } else if (strcmp(key, "KEY_NEXT") == 0) {
                KEY_NEXT = value[0];
            } else if (strcmp(key, "KEY_PREV") == 0) {
                KEY_PREV = value[0];
            } else if (strcmp(key, "KEY_VOL_UP") == 0) {
                KEY_VOL_UP = value[0];
            } else if (strcmp(key, "KEY_VOL_DOWN") == 0) {
                KEY_VOL_DOWN = value[0];
            }
        }
    }
    fclose(file);
    return 0;
}

// Function to get music files from a directory
char **getMusicFiles(const std::string& path, int *trackCount) {
    DIR *dir = opendir(path.c_str());
    if (!dir) {
        perror("Could not open directory");
        return NULL;
    }

    struct dirent *entry;
    char **files = (char **)malloc(100 * sizeof(char *));
    *trackCount = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".mp3") || 
            strstr(entry->d_name, ".mp4") || 
            strstr(entry->d_name, ".wav") || 
            strstr(entry->d_name, ".wma") || 
            strstr(entry->d_name, ".flac") || 
            strstr(entry->d_name, ".ogg")) {

            // Allocate space for the full path of the file
            char *fullPath = (char *)malloc(strlen(path.c_str()) + strlen(entry->d_name) + 2);
            snprintf(fullPath, strlen(path.c_str()) + strlen(entry->d_name) + 2, "%s/%s", path.c_str(), entry->d_name);

            // Add the file to the list
            files[*trackCount] = fullPath;
            (*trackCount)++;
        }
    }
    closedir(dir);
    return files;
}

// Function to load and return tracks from an M3U playlist
char **loadM3UPlaylist(const char *path, int *trackCount){
    FILE *file = fopen(path, "r");
    if (!file) {
        perror("Failed to open M3U file");
        return NULL;
    }

    char line[1024];
    char **files = (char **)malloc(100 * sizeof(char *));
    *trackCount = 0;

    // Read each line of the M3U file
    while (fgets(line, sizeof(line), file)) {
        // Trim newline or carriage return characters
        line[strcspn(line, "\r\n")] = '\0';

        // Check if the line contains a valid audio file (check extensions)
        if (strstr(line, ".mp3") || strstr(line, ".mp4") || 
            strstr(line, ".wav") || strstr(line, ".wma") || 
            strstr(line, ".flac") || strstr(line, ".ogg")) {
            
            // Allocate space for the track and copy the path/filename
            files[*trackCount] = (char *)malloc(strlen(line) + 1);
            if (!files[*trackCount]) {
                perror("Memory allocation failure for track");
                fclose(file);
                return NULL;
            }
            strcpy(files[*trackCount], line);
            (*trackCount)++;
        }
    }
    fclose(file);
    return files;
}

// Function to fade volume
void fadeVolume(int startVolume, int endVolume, int steps, int stepDuration) {
    int volumeStep = (endVolume - startVolume) / steps;

    for (int i = 0; i < steps; i++) {
        int currentVolume = startVolume + (volumeStep * i);
        Mix_VolumeMusic(currentVolume);
        SDL_Delay(stepDuration); // Pause for the step duration
    }

    Mix_VolumeMusic(endVolume); // Ensure final volume is set
}

// Function to load and play music from a given track
void loadAndPlayMusic(int trackIndex) {
    if (music) {
        Mix_FreeMusic(music);
    }
    music = Mix_LoadMUS(trackList[trackIndex]);
    if (music) {
        int startVolume = 0;
        int endVolume = (INITIAL_VOL * 128) / 100;
        Mix_VolumeMusic(startVolume);
        Mix_PlayMusic(music, -1);
        fadeVolume(startVolume, endVolume, FADE_STEPS, FADE_STEPS_DURATION); // Fade-in effect

    } else {
        printf("LAP Error loading music: %s\n", Mix_GetError());
    }
}

// Function to check if an application is running
int isApplicationOpen(const char* appName) {
    char command[256];
    snprintf(command, sizeof(command), "pgrep -x '%s' > /dev/null", appName);
    return system(command) == 0;  // Returns 1 if the process is running, 0 otherwise
}

// Function that skips to start of next song if RESUME_NEXT_TRACK is true
void resumeNextTrack() {
    currentTrackIndex = (currentTrackIndex + 1) % trackCount;
    loadAndPlayMusic(currentTrackIndex);
}

// Function to toggle playback state with fadeVolume 
void toggleMusicWithFade(bool resume, int VOL) {
    if (resume) {
        int startVolume = 0;
        int endVolume = (VOL * 128) / 100;
        Mix_VolumeMusic(startVolume);
        if(RESUME_NEXT_TRACK) {
            resumeNextTrack();
        } else {
            Mix_ResumeMusic();
            fadeVolume(startVolume, endVolume, FADE_STEPS, FADE_STEPS_DURATION); // Fade-in effect
        }
    } else {
        int startVolume = Mix_VolumeMusic(-1); // Get current volume
        int endVolume = 0;                    
        fadeVolume(startVolume, endVolume, FADE_STEPS, FADE_STEPS_DURATION); // Fade-out effect
        Mix_PauseMusic();
    }
}

// Function to handle music playback state during the game
void handleMusicInGamePlayback() {
    if (!PLAY_MUSIC_INGAME && isApplicationOpen(APP_TO_MONITOR)) {
        // If PLAY_MUSIC_INGAME is false and isApplicationOpen is true, then we pause, this simulates handing RetroFE off to a game
        Mix_PauseMusic();
    }
    else if (PLAY_MUSIC_INGAME && isApplicationOpen(APP_TO_MONITOR)) {
        toggleMusicWithFade(true, INGAME_VOL);
    }
    else {
        // If PLAY_MUSIC_INGAME true or isApplicationOpen false we assume we're in RetroFE or we're in game and we don't need to do anything
            Mix_ResumeMusic();
        }
}

// Function that handles shuffle
void shuffle(char **trackList, int trackCount) {
    srand(time(NULL));  // Seed the random number generator
    for (int i = trackCount - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char *temp = trackList[i];
        trackList[i] = trackList[j];
        trackList[j] = temp;
    }
}

// Function to handle keyboard input
void handleKeyboardInput(SDL_Event *event) {
    if (event->type == SDL_KEYDOWN) {
        if (event->key.keysym.sym == SDL_GetKeyFromName(&KEY_VOL_UP)) {
            volume += 10;
            if (volume > 128) volume = 128; // Cap volume at maximum (128)
            Mix_VolumeMusic(volume);
        }

        if (event->key.keysym.sym == SDL_GetKeyFromName(&KEY_VOL_DOWN)) {
            volume -= 10;
            if (volume < 0) volume = 0; // Ensure volume doesn't go below 0
            Mix_VolumeMusic(volume);
        }

        if (event->key.keysym.sym == SDL_GetKeyFromName(&KEY_PAUSE)) {
            // Pause/Play music
            if (Mix_PlayingMusic()) {
                Mix_PauseMusic();
            } else { Mix_ResumeMusic(); }
        }

        if (event->key.keysym.sym == SDL_GetKeyFromName(&KEY_NEXT)) {
            // Next track
            currentTrackIndex = (currentTrackIndex + 1) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
        }

        if (event->key.keysym.sym == SDL_GetKeyFromName(&KEY_PREV)) {
            // Previous track
            currentTrackIndex = (currentTrackIndex - 1 + trackCount) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
        }
    }
}

// Function to handle gamepad input
void handleGamepadInput() {
    if (!gamepad) { return; }
    int deadzone = 16000; // Set the deadzone, -32000,32000 is max

    // GAMEPAD_CONTROL_FULL: Right Stick X-axis for Next/Prev, Y-axis for Volume, Right Stick Click for Pause
    if (GAMEPAD_CONTROL_FULL) {
        // Track navigation (Right Stick X-axis)
        static bool axisRightXHandled = false;
        int rightX = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
        if (!axisRightXHandled && rightX > deadzone) {
            currentTrackIndex = (currentTrackIndex + 1) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
            axisRightXHandled = true;
        } else if (!axisRightXHandled && rightX < -deadzone) {
            currentTrackIndex = (currentTrackIndex - 1 + trackCount) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
            axisRightXHandled = true;
        } else if (rightX > -deadzone && rightX < deadzone) {
            axisRightXHandled = false; // Reset once axis returns to neutral
        }

        // Volume control (Right Stick Y-axis)
        static bool axisRightYHandled = false;
        int rightY = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY);
        int volumeIncrement = 5;
        int currentVolume = Mix_VolumeMusic(-1);
        if (!axisRightYHandled && rightY < -deadzone) {
            // Right stick Y+
            currentVolume += volumeIncrement;
            if (currentVolume > 128) currentVolume = 128;
            Mix_VolumeMusic(currentVolume);
            axisRightYHandled = true;
        } else if (!axisRightYHandled && rightY > deadzone) {
            // Right stick Y-
            currentVolume -= volumeIncrement;
            if (currentVolume < 0) currentVolume = 0;
            Mix_VolumeMusic(currentVolume);
            axisRightYHandled = true;
        } else if (rightY > -deadzone && rightY < deadzone) {
            axisRightYHandled = false; // Reset once axis returns to neutral
        }

        // Play/Pause toggle (Right Stick Button)
        static bool buttonHandled = false;
        if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
            if (!buttonHandled) {
                if (Mix_PlayingMusic()) {
                    if (Mix_PausedMusic()) {
                        toggleMusicWithFade(true, INITIAL_VOL);
                    } else {
                        toggleMusicWithFade(false, INITIAL_VOL);
                    }
                } else {
                    // Optionally handle the "no music loaded" case
                }
                buttonHandled = true;
            }
        } else {
            buttonHandled = false; // Reset when button is released
        }
    }

    // GAMEPAD_CONTROL_SIMPLE: Right Stick X-axis for Next/Prev
    if (GAMEPAD_CONTROL_SIMPLE) {
        // Track navigation (Right Stick X-axis)
        static bool axisRightXHandled = false;
        int rightX = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
        if (!axisRightXHandled && rightX > deadzone) {
            currentTrackIndex = (currentTrackIndex + 1) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
            axisRightXHandled = true;
        } else if (!axisRightXHandled && rightX < -deadzone) {
            currentTrackIndex = (currentTrackIndex - 1 + trackCount) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
            axisRightXHandled = true;
        } else if (rightX > -deadzone && rightX < deadzone) {
            axisRightXHandled = false; // Reset once axis returns to neutral
        }
    }

    // GAMEPAD_CONTROL_PAUSE: Right Stick Click only for Pause/Resume
    if (GAMEPAD_CONTROL_PAUSE) {
        static bool buttonHandled = false;
        if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
            if (!buttonHandled) {
                if (Mix_PlayingMusic()) {
                    if (Mix_PausedMusic()) {
                        toggleMusicWithFade(true, INITIAL_VOL);
                    } else {
                        toggleMusicWithFade(false, INITIAL_VOL);
                    }
                } else {
                    // Optionally handle the "no music loaded" case
                }
                buttonHandled = true;
            }
        } else {
            buttonHandled = false; // Reset when button is released
        }
    }

    // GAMEPAD_CONTROL_L1R1: L1 for Previous Track, R1 for Next Track
    if (GAMEPAD_CONTROL_L1R1) {
        if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
            std::cout << "L1 button pressed!" << std::endl;
            currentTrackIndex = (currentTrackIndex - 1 + trackCount) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
        } else if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            std::cout << "R1 button pressed!" << std::endl;
            currentTrackIndex = (currentTrackIndex + 1) % trackCount;
            loadAndPlayMusic(currentTrackIndex);
        }
    }
}

// Function to init SDL
void init() {
    // Initialize SDL
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        exit(1);
    }

    // Open the first available game controller
    gamepad = SDL_GameControllerOpen(0);
    if (!gamepad) {
        printf("Controller not found!\n");
    }

    if (Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_WAVPACK | MIX_INIT_FLAC) == 0) {
        printf("Mix_Init failed: %s\n", Mix_GetError());
        exit(1);
    }

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        printf("Mix_OpenAudio failed: %s\n", Mix_GetError());
        exit(1);
    }
}

// Function to cleanup SDL when exiting
void cleanup() {
    SDL_GameControllerClose(gamepad);
    Mix_CloseAudio();
    Mix_Quit();
    SDL_Quit();
}

int MusicPlayer() {
    if (loadSettingsFromFile(INI_PATH) != 0) {
        printf("Failed to load settings from config.ini\n");
        return -1;  // Exit if settings can't be loaded
    }
    
    init();
    
    // Set initial volume
    if (INITIAL_VOL < 0) INITIAL_VOL = 0;
    if (INITIAL_VOL > 100) INITIAL_VOL = 100;
    Mix_VolumeMusic((INITIAL_VOL * 128) / 100);
    
    // Load music files
    if(fileExists(PLAYLIST_FILE_PATH))
    {
        trackList = loadM3UPlaylist(PLAYLIST_FILE_PATH, &trackCount);
    } else {
        trackList = getMusicFiles(FOLDER_TO_PLAY, &trackCount);
    }
    if (trackCount == 0) {
        printf("No music files found!\n");
        exit(1);
    }
    printf("Found %d music files\n", trackCount);
    
    // Sort trackList alphabetically
    qsort(trackList, trackCount, sizeof(char*), compareStrings);
    
    // Set shuffle
    if (SHUFFLE) {
        shuffle(trackList, trackCount); // Shuffle the index
        currentTrackIndex = rand() % trackCount; // Start on a random index
    }
    
    // Play the first song
    if (trackCount > 0) {
        loadAndPlayMusic(currentTrackIndex);
    }
    
    // Get the metadata for the first song
    std::string nowPlaying = readID3v1Tag(trackList[currentTrackIndex]);
    writeNowPlayingToFile(nowPlaying);
    
    // Main loop
    SDL_Event e;
    int quit = 0;
    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                MusicPlayer::handleKeyboardInput(&e);
            }
        }
        handleGamepadInput();
        handleMusicInGamePlayback();
        
        std::string newSongMetadata = readID3v1Tag(trackList[currentTrackIndex]);
        if (newSongMetadata != nowPlaying) {
            nowPlaying = newSongMetadata;
            writeNowPlayingToFile(nowPlaying);
        }
        
    }
    cleanup();
    return 0;
}

}

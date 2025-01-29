#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <string>
#include <SDL2/SDL.h>
#include <SDL2_mixer/SDL_mixer.h>

namespace MusicPlayer {

// Structures
struct ID3v1Tag {
    char tag[3];      // "TAG"
    char title[30];
    char artist[30];
    char album[30];
    char year[4];
    char comment[30];
    char genre;
};

// Function declarations
std::string readID3v1Tag(const std::string& filename);
int fileExists(const char *path);
int compareStrings(const void* a, const void* b);
void writeNowPlayingToFile(const std::string& nowPlaying);
void stripQuotes(char *str);
int loadSettingsFromFile(const std::string& filename);
char **getMusicFiles(const std::string& path, int *trackCount);
char **loadM3UPlaylist(const char *path, int *trackCount);
void fadeVolume(int startVolume, int endVolume, int steps, int stepDuration);
void loadAndPlayMusic(int trackIndex);
int isApplicationOpen(const char* appName);
void resumeNextTrack();
void toggleMusicWithFade(bool resume, int VOL);
void handleMusicInGamePlayback();
void shuffle(char **trackList, int trackCount);
void handleKeyboardInput(SDL_Event *event);
void handleGamepadInput();
void init();
int MusicPlayer();

}  

#endif // MUSIC_PLAYER_H

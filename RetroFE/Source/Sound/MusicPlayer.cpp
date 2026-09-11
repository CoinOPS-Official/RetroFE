/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "MusicPlayer.h"
#include "AudioBus.h"
#include "../Database/Configuration.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "../Graphics/Component/MusicPlayerComponent.h"

#include <filesystem>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cmath>
#include <chrono>
#include <thread>
#include <tuple>
#include <cctype>

namespace fs = std::filesystem;

// -------------------------------------------------------------------------
// Anonymous Namespace: Helper Functions
// -------------------------------------------------------------------------
namespace {
    inline int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

    inline int pctTo128(int pct) {
        pct = clampInt(pct, 0, 100);
        return static_cast<int>((pct / 100.0f) * 128 + 0.5f);
    }

    inline int vol128ToPct(int v128) {
        v128 = clampInt(v128, 0, 128);
        return static_cast<int>((v128 / static_cast<float>(128)) * 100.0f + 0.5f);
    }

    // Convert syncsafe integer (for ID3v2.4)
    uint32_t syncsafe_to_int(const uint8_t* buf) {
        return ((buf[0] & 0x7f) << 21) | ((buf[1] & 0x7f) << 14) | ((buf[2] & 0x7f) << 7) | (buf[3] & 0x7f);
    }

    // Best-effort: treat encoding 0 as "bytes", encoding 3 as UTF-8, encoding 1/2 as UTF-16.
    std::string read_id3v2_text_frame(const char* data, size_t size) {
        if (size < 2) return "";
        uint8_t encoding = static_cast<uint8_t>(data[0]);

        if (encoding == 0 || encoding == 3) { // ISO-8859-1 (best-effort passthrough) or UTF-8
            std::string value(data + 1, size - 1);
            size_t end = value.find_last_not_of(" \0");
            value = (end != std::string::npos) ? value.substr(0, end + 1) : "";
            return value;
        }
        else if (encoding == 1 || encoding == 2) { // UTF-16 (with/without BOM)
            if (size < 4) return "";
            const unsigned char b1 = static_cast<unsigned char>(data[1]);
            const unsigned char b2 = static_cast<unsigned char>(data[2]);
            const bool bigEndian = (b1 == 0xFE && b2 == 0xFF);
            size_t offset = 3; // encoding byte + BOM (2 bytes)
            size_t len = size - offset;
            if (len % 2 != 0) --len;

            std::wstring wstr;
            wstr.reserve(len / 2);

            for (size_t i = 0; i < len; i += 2) {
                wchar_t ch;
                if (bigEndian) {
                    ch = (static_cast<unsigned char>(data[offset + i]) << 8)
                        | static_cast<unsigned char>(data[offset + i + 1]);
                }
                else {
                    ch = (static_cast<unsigned char>(data[offset + i + 1]) << 8)
                        | static_cast<unsigned char>(data[offset + i]);
                }
                if (ch == 0) break;
                wstr.push_back(ch);
            }

            std::string value = Utils::wstringToString(wstr); // assume Utils returns UTF-8
            size_t end = value.find_last_not_of(" \0");
            return (end != std::string::npos) ? value.substr(0, end + 1) : "";
        }

        return "";
    }

    inline fs::path pathFromUtf8(const std::string& s) {
        // fs::u8path expects UTF-8 and produces native path (wide on Windows).
        return fs::u8path(s);
    }

    inline std::string toUtf8String(const fs::path& p) {
        // p.u8string() now returns std::u8string (char8_t)
        auto u8str = p.u8string();
        return std::string(u8str.begin(), u8str.end());
    }

    inline void trimCR(std::string& s) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
    }

    inline void stripUtf8Bom(std::string& s) {
        if (s.size() >= 3 &&
            static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF) {
            s.erase(0, 3);
        }
    }

    // Helper to extract album art raw bytes from ID3 tags
    bool extractAlbumArtFromFile(const fs::path& filePath, std::vector<unsigned char>& albumArtData) {
        albumArtData.clear();

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return false;

        file.seekg(0, std::ios::end);
        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        if (fileSize < 10) return false;

        char header[10];
        file.read(header, 10);
        if (std::memcmp(header, "ID3", 3) != 0) return false;

        const int majorVersion = static_cast<unsigned char>(header[3]);
        int tagSize = 0;
        for (int i = 0; i < 4; ++i) tagSize = (tagSize << 7) | (header[6 + i] & 0x7F);

        if (tagSize <= 0 || tagSize > fileSize - 10) return false;

        const std::streamoff tagEnd = 10 + tagSize;

        while (file && file.tellg() < tagEnd) {
            if ((tagEnd - file.tellg()) < 10) break;

            char frameHeader[10];
            file.read(frameHeader, 10);
            if (!file) break;

            // Validate Frame ID (A-Z, 0-9) across 4 chars
            for (int i = 0; i < 4; ++i) {
                const unsigned char c = static_cast<unsigned char>(frameHeader[i]);
                if (!std::isalnum(c)) return false;
            }

            const std::string frameID(frameHeader, 4);
            int frameSize = 0;

            if (majorVersion >= 4) { // Syncsafe
                for (int i = 0; i < 4; ++i) frameSize = (frameSize << 7) | (frameHeader[4 + i] & 0x7F);
            }
            else {
                frameSize = (static_cast<unsigned char>(frameHeader[4]) << 24) |
                    (static_cast<unsigned char>(frameHeader[5]) << 16) |
                    (static_cast<unsigned char>(frameHeader[6]) << 8) |
                    (static_cast<unsigned char>(frameHeader[7]));
            }

            if (frameSize <= 0 || frameSize > (tagEnd - file.tellg())) break;

            if (frameID == "APIC") {
                std::vector<char> buffer(static_cast<size_t>(frameSize));
                file.read(buffer.data(), frameSize);
                if (!file) break;

                size_t offset = 1; // encoding

                // MIME (null-terminated)
                while (offset < buffer.size() && buffer[offset] != 0) offset++;
                offset++;
                if (offset >= buffer.size()) break;

                const unsigned char pictureType = static_cast<unsigned char>(buffer[offset++]);

                // Allow Front Cover (3) or Other (0)
                if (pictureType != 0x03 && pictureType != 0x00) continue;

                // Description: encoding-dependent termination.
                // This is still simplified, but at least respects UTF-16 width.
                const unsigned char enc = static_cast<unsigned char>(buffer[0]);
                if (enc == 1 || enc == 2) {
                    // UTF-16: terminator is 0x00 0x00
                    while (offset + 1 < buffer.size()) {
                        if (buffer[offset] == 0 && buffer[offset + 1] == 0) { offset += 2; break; }
                        offset += 2;
                    }
                }
                else {
                    // 8-bit: single 0x00
                    while (offset < buffer.size() && buffer[offset] != 0) offset++;
                    if (offset < buffer.size()) offset++;
                }

                if (offset < buffer.size()) {
                    albumArtData.assign(reinterpret_cast<unsigned char*>(buffer.data() + offset),
                        reinterpret_cast<unsigned char*>(buffer.data() + buffer.size()));
                    return true;
                }
            }
            else {
                file.seekg(frameSize, std::ios::cur);
            }
        }

        return false;
    }

    inline fs::path normalizeForCompare(const fs::path& p) {
        // Cheap + stable: normalize separators and dot segments; do NOT hit filesystem.
        return p.lexically_normal();
    }
} // namespace

// -------------------------------------------------------------------------
// MusicPlayer Implementation
// -------------------------------------------------------------------------

MusicPlayer* MusicPlayer::instance_ = nullptr;

MusicPlayer* MusicPlayer::getInstance() {
    if (!instance_) instance_ = new MusicPlayer();
    return instance_;
}

MusicPlayer::MusicPlayer()
    : config_(nullptr)
    , currentMusic_(nullptr)
    , currentShufflePos_(-1)
    , playbackState_(PlaybackState::NONE)
    , currentIndex_(-1)
    , volume_(128)
    , logicalVolume_(128)
    , previousLogicalVolume_(128)
    , loopMode_(false)
    , shuffleMode_(false)
    , hasStartedPlaying_(false)
    , fadeMs_(1500)
    , savedTrackIndex_(-1)
    , savedPosition_(0.0)
    , lastVolumeChangeTime_(0)
    , volumeChangeIntervalMs_(0)
    , volumeFadeDuration_(0)
    , volumeFadeStartVal_(0)
    , volumeFadeTargetVal_(0)
    , buttonPressed_(false)
    , isShuttingDown_(false)
    , hasActiveVisualizers_(false)
    , audioChannels_(2)
    , audioSampleRate_(44100)
    , hasVuMeter_(false)
    , sampleSize_(sizeof(float)) {

    uint64_t seed = SDL_GetTicks();
    std::seed_seq seq{
        static_cast<uint32_t>(seed & 0xFFFFFFFF),
        static_cast<uint32_t>((seed >> 32) & 0xFFFFFFFF)
    };
    rng_.seed(seq);

    audioLevels_.resize(audioChannels_, 0.0f);
}

MusicPlayer::~MusicPlayer() {
    shutdown();
}

void MusicPlayer::shutdown() {
    if (isShuttingDown_.exchange(true, std::memory_order_acq_rel)) return;

    LOG_INFO("MusicPlayer", "Shutting down music player");

    const bool mixerIsOpen = musicTrack_ != nullptr;

    int startVol = -1;
    if (mixerIsOpen) {
        startVol = musicVolume(-1);

        if ((musicTrack_ && (MIX_TrackPlaying(musicTrack_) || MIX_TrackPaused(musicTrack_)))) {
            int steps = 20;
            int stepMs = (fadeMs_ > 0) ? (fadeMs_ / steps) : 0;
            if (stepMs < 10) { steps = 1; stepMs = 0; }

            for (int i = 0; i <= steps; ++i) {
                if (!(musicTrack_ && (MIX_TrackPlaying(musicTrack_) || MIX_TrackPaused(musicTrack_)))) break;
                float t = static_cast<float>(i) / static_cast<float>(steps);
                musicVolume(static_cast<int>(startVol * (1.0f - t)));
                if (stepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
            }
        }

        haltTrack();

        if (startVol >= 0) musicVolume(startVol);
    }

    if (currentMusic_) {
        haltTrack();
        if (musicTrack_) MIX_SetTrackAudio(musicTrack_, nullptr);
        MIX_DestroyAudio(currentMusic_);
        currentMusic_ = nullptr;
    }

    releaseAudio();
    musicFiles_.clear();
    musicNames_.clear();
    trackMetadata_.clear();
    currentIndex_ = -1;
    lastCheckedTrackPath_.clear();
}

bool MusicPlayer::initialize(Configuration& config) {
    config_ = &config;
    isShuttingDown_.store(false, std::memory_order_release);

    int configVolume = 100;
    if (config.getProperty("musicPlayer.volume", configVolume)) {
        configVolume = std::clamp(configVolume, 0, 100);
    }

    logicalVolume_ = pctTo128(configVolume);
    setLogicalVolume(logicalVolume_);

    if (!ensureAudio()) return false;

    bool configLoop = false;
    config.getProperty("musicPlayer.loop", configLoop);
    loopMode_ = configLoop;

    bool configShuffle = false;
    config.getProperty("musicPlayer.shuffle", configShuffle);
    shuffleMode_ = configShuffle;

    int configFadeMs = 1500;
    config.getProperty("musicPlayer.fadeMs", configFadeMs);
    fadeMs_ = std::max(0, configFadeMs);

    int configVolumeDelay = 0;
    if (config.getProperty("musicPlayer.volumeDelay", configVolumeDelay)) {
        volumeChangeIntervalMs_ = std::clamp(configVolumeDelay, 0, 50);
    }

    std::string m3uPlaylist;
    if (config.getProperty("musicPlayer.m3uplaylist", m3uPlaylist)) {
        fs::path p = pathFromUtf8(m3uPlaylist);
        if (!p.is_absolute()) {
            p = pathFromUtf8(Utils::combinePath(Configuration::absolutePath, m3uPlaylist));
        }
        if (loadM3UPlaylist(toUtf8String(p))) {
            LOG_INFO("MusicPlayer", "Initialized with M3U playlist: " + toUtf8String(p));
        }
        else {
            loadMusicFolderFromConfig();
        }
    }
    else {
        loadMusicFolderFromConfig();
    }

    return true;
}

void MusicPlayer::reinitialize() {
    LOG_INFO("MusicPlayer", "Re-initializing hooks after SDL cycle.");
    if (!ensureAudio()) return;
    musicVolume(volume_.load(std::memory_order_relaxed));
}

void MusicPlayer::pump() {
    if (!musicTrack_) return;
    if (isShuttingDown_.load(std::memory_order_relaxed)) return;

    const auto ev = finishEvent_.exchange(FinishEvent::None, std::memory_order_acq_rel);
    if (ev == FinishEvent::NaturalEnd) {
        LOG_INFO("MusicPlayer", "Track finished: " + getCurrentTrackName());
        if (!loopMode_) nextTrack();
    }

    if (musicFade_.active) {
        const uint64_t now = SDL_GetTicks();
        const uint64_t elapsed = now - musicFade_.startTimeMs;
        int currentLogical = 0;

        if (musicFade_.durationMs <= 0 || elapsed >= static_cast<uint64_t>(musicFade_.durationMs)) {
            currentLogical = musicFade_.targetVol;
        }
        else {
            const float t = static_cast<float>(elapsed) / static_cast<float>(musicFade_.durationMs);
            currentLogical = static_cast<int>(musicFade_.startVol + (musicFade_.targetVol - musicFade_.startVol) * t);
        }

        int rawVolume = applyVolumeCurve(currentLogical);
        musicVolume(rawVolume);
        volume_.store(rawVolume, std::memory_order_relaxed);

        if (musicFade_.durationMs <= 0 || elapsed >= static_cast<uint64_t>(musicFade_.durationMs)) {
            if (musicFade_.fadingOut) {
                const FinishEvent action = musicFade_.finishAction;
                const int idx = musicFade_.finishIndex;
                const double seekPos = musicFade_.finishSeekPos;
                const int fadeInMs = musicFade_.fadeInMs;

                musicFade_.active = false;

                switch (action) {
                    case FinishEvent::PauseAfterFade:
                    MIX_PauseTrack(musicTrack_);
                    musicVolume(0);
                    setPlaybackState(PlaybackState::PAUSED);
                    break;

                    case FinishEvent::StopAfterFade:
                    haltTrack();
                    musicVolume(applyVolumeCurve(getLogicalVolume()));
                    setPlaybackState(PlaybackState::NONE);
                    break;

                    case FinishEvent::TrackChangeAfterFade:
                    haltTrack();
                    musicVolume(0);
                    if (playMusic(idx, 0, seekPos)) {
                        beginFadeInToSteadyVolume(fadeInMs);
                    }
                    else {
                        musicVolume(applyVolumeCurve(getLogicalVolume()));
                    }
                    break;

                    default:
                    break;
                }
            }
            else {
                musicFade_.active = false;
            }
        }

        return;
    }

    if (isVolumeFading_) {
        uint64_t now = SDL_GetTicks();
        uint64_t elapsed = now - volumeFadeStartTime_;

        if (elapsed >= static_cast<uint64_t>(volumeFadeDuration_)) {
            int finalLogical = volumeFadeTargetVal_;
            int finalRaw = applyVolumeCurve(finalLogical);
            musicVolume(finalRaw);
            volume_.store(finalRaw);
            logicalVolume_.store(finalLogical);
            isVolumeFading_ = false;
        }
        else {
            float t = static_cast<float>(elapsed) / static_cast<float>(volumeFadeDuration_);
            int currentLogical = static_cast<int>(volumeFadeStartVal_ + (volumeFadeTargetVal_ - volumeFadeStartVal_) * t);
            int currentRaw = applyVolumeCurve(currentLogical);
            musicVolume(currentRaw);
            volume_.store(currentRaw);
            logicalVolume_.store(currentLogical);
        }
    }
}

// -------------------------------------------------------------------------
// Game Launch / State Hooks
// -------------------------------------------------------------------------

void MusicPlayer::onGameLaunchStart() {
    if (!config_) return;

    savedTrackIndex_ = getCurrentTrackIndex();
    savedPosition_ = saveCurrentMusicPosition();

    bool playInGame = false;
    config_->getProperty("musicPlayer.playInGame", playInGame);

    previousLogicalVolume_.store(getLogicalVolume(), std::memory_order_relaxed);

    if (playInGame) {
        int playInGameVolPct = -1;
        if (config_->getProperty("musicPlayer.playInGameVol", playInGameVolPct) &&
            playInGameVolPct >= 0 && playInGameVolPct <= 100) {
            int targetLogical = pctTo128(playInGameVolPct);
            if (getLogicalVolume() > targetLogical) fadeToVolume(targetLogical);
        }
    }
    else {
        pauseMusic();
    }
}

void MusicPlayer::onGameLaunchEnd(bool wasSdlUnloaded) {
    if (!config_) return;

    // Cancel any ongoing fade to ensure volume restoration
    cancelFade();

    bool playInGame = false;
    config_->getProperty("musicPlayer.playInGame", playInGame);

    if (playInGame) {
        // Fade back to previous volume if different
        if (getLogicalVolume() != previousLogicalVolume_.load(std::memory_order_relaxed)) {
            fadeBackToPreviousVolume();
        }
    }
    else {
        if (wasSdlUnloaded) {
            if (savedTrackIndex_ != -1) playMusic(savedTrackIndex_, -1, savedPosition_);
        }
        else {
            resumeMusic();
        }
    }
}

// -------------------------------------------------------------------------
// File Loading
// -------------------------------------------------------------------------

void MusicPlayer::loadMusicFolderFromConfig() {
    fs::path p;

    std::string musicFolderUtf8;
    if (config_ && config_->getProperty("musicPlayer.folder", musicFolderUtf8) && !musicFolderUtf8.empty()) {
        p = pathFromUtf8(musicFolderUtf8);

        if (!p.is_absolute()) {
            // Resolve relative against RetroFE base path
            p = pathFromUtf8(Configuration::absolutePath) / p;
        }
    }
    else {
        p = pathFromUtf8(Configuration::absolutePath) / pathFromUtf8("music");
    }

    p = p.lexically_normal();

    LOG_INFO("MusicPlayer", "musicPlayer.folder resolved to: " + toUtf8String(p));
    loadMusicFolder(toUtf8String(p));
}


bool MusicPlayer::loadMusicFolder(const std::string& folderPathUtf8) {
    musicFiles_.clear();
    musicNames_.clear();
    trackMetadata_.clear();

    fs::path folderPath = pathFromUtf8(folderPathUtf8);

    LOG_INFO("MusicPlayer", "Loading music from: " + toUtf8String(folderPath));

    if (!fs::exists(folderPath)) return false;

    std::vector<std::tuple<fs::path, std::string, TrackMetadata>> musicEntries;

    try {
        for (const auto& entry : fs::directory_iterator(folderPath)) {
            if (!entry.is_regular_file()) continue;

            const fs::path p = entry.path();
            if (!isValidAudioFile(p)) continue;

            TrackMetadata metadata;
            readTrackMetadata(p, metadata);

            // Store display name as UTF-8.
            musicEntries.emplace_back(p, toUtf8String(p.filename()), metadata);
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("MusicPlayer", "Scan error: " + std::string(e.what()));
        return false;
    }

    std::sort(musicEntries.begin(), musicEntries.end(),
        [](const auto& a, const auto& b) { return std::get<1>(a) < std::get<1>(b); });

    for (const auto& e : musicEntries) {
        musicFiles_.push_back(std::get<0>(e));
        musicNames_.push_back(std::get<1>(e));
        trackMetadata_.push_back(std::get<2>(e));
    }

    LOG_INFO("MusicPlayer", "Found " + std::to_string(musicFiles_.size()) + " files");
    return !musicFiles_.empty();
}

bool MusicPlayer::loadM3UPlaylist(const std::string& playlistPathUtf8) {
    musicFiles_.clear();
    musicNames_.clear();
    trackMetadata_.clear();

    if (!parseM3UFile(pathFromUtf8(playlistPathUtf8))) return false;

    LOG_INFO("MusicPlayer", "M3U Loaded: " + std::to_string(musicFiles_.size()) + " files");
    return !musicFiles_.empty();
}

bool MusicPlayer::parseM3UFile(const fs::path& playlistPath) {
    if (!fs::exists(playlistPath)) return false;

    std::ifstream playlistFile(playlistPath, std::ios::binary);
    if (!playlistFile.is_open()) return false;

    const fs::path playlistDir = playlistPath.parent_path();

    std::string line;
    bool firstLine = true;

    std::vector<std::tuple<fs::path, std::string, TrackMetadata>> musicEntries;

    while (std::getline(playlistFile, line)) {
        trimCR(line);
        if (firstLine) { stripUtf8Bom(line); firstLine = false; }

        if (line.empty() || line[0] == '#') continue;

        // Interpret M3U text as UTF-8.
        fs::path trackPath = pathFromUtf8(line);

        if (!trackPath.is_absolute()) trackPath = playlistDir / trackPath;

        if (fs::exists(trackPath) && isValidAudioFile(trackPath)) {
            TrackMetadata metadata;
            readTrackMetadata(trackPath, metadata);
            musicEntries.emplace_back(trackPath, toUtf8String(trackPath.filename()), metadata);
        }
    }

    std::sort(musicEntries.begin(), musicEntries.end(),
        [](const auto& a, const auto& b) { return std::get<1>(a) < std::get<1>(b); });

    for (const auto& e : musicEntries) {
        musicFiles_.push_back(std::get<0>(e));
        musicNames_.push_back(std::get<1>(e));
        trackMetadata_.push_back(std::get<2>(e));
    }

    return true;
}

bool MusicPlayer::isValidAudioFile(const fs::path& filePath) const {
    std::string ext = toUtf8String(filePath.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".mod");
}

void MusicPlayer::loadTrack(int index) {
    if (!ensureAudio()) return;
    if (currentMusic_) {
        haltTrack();
        if (musicTrack_) MIX_SetTrackAudio(musicTrack_, nullptr);
        MIX_DestroyAudio(currentMusic_);
        currentMusic_ = nullptr;
    }

    if (index < 0 || index >= static_cast<int>(musicFiles_.size())) {
        currentIndex_ = -1;
        return;
    }

    // SDL expects UTF-8 paths; fs::path stays native, convert at the boundary.
    const std::string pathUtf8 = toUtf8String(musicFiles_[index]);
    currentMusic_ = MIX_LoadAudio(AudioBus::instance().mixer(), pathUtf8.c_str(), false);

    if (!currentMusic_) {
        LOG_ERROR("MusicPlayer", "Load failed: " + pathUtf8 + " " + SDL_GetError());
        currentIndex_ = -1;
        return;
    }

    currentIndex_ = index;
    LOG_INFO("MusicPlayer", "Loaded: " + musicNames_[index]);
}

// -------------------------------------------------------------------------
// Metadata Parsing
// -------------------------------------------------------------------------

bool MusicPlayer::readTrackMetadata(const fs::path& filePath, TrackMetadata& metadata) const {
    // 1) ID3v2
    std::ifstream file(filePath, std::ios::binary);
    bool tagFound = false;

    if (file) {
        char header[10];
        file.read(header, 10);
        if (file.gcount() == 10 && std::memcmp(header, "ID3", 3) == 0) {
            const int version = header[3];
            const uint32_t tag_size = syncsafe_to_int(reinterpret_cast<uint8_t*>(header + 6));
            uint32_t bytesRead = 0;

            while (bytesRead < tag_size) {
                char frame_header[10];
                file.read(frame_header, 10);
                if (file.gcount() != 10 || frame_header[0] == 0) break;

                const std::string frame_id(frame_header, 4);

                uint32_t frame_size = 0;
                if (version == 4) {
                    frame_size = syncsafe_to_int(reinterpret_cast<uint8_t*>(frame_header + 4));
                }
                else {
                    frame_size =
                        (static_cast<uint8_t>(frame_header[4]) << 24) |
                        (static_cast<uint8_t>(frame_header[5]) << 16) |
                        (static_cast<uint8_t>(frame_header[6]) << 8) |
                        (static_cast<uint8_t>(frame_header[7]));
                }

                if (frame_size == 0 || frame_size > 1024 * 1024) break;

                std::vector<char> frame_data(frame_size);
                file.read(frame_data.data(), frame_size);
                if (!file) break;

                if (frame_id == "TIT2") metadata.title = read_id3v2_text_frame(frame_data.data(), frame_size);
                else if (frame_id == "TPE1") metadata.artist = read_id3v2_text_frame(frame_data.data(), frame_size);
                else if (frame_id == "TALB") metadata.album = read_id3v2_text_frame(frame_data.data(), frame_size);
                else if (frame_id == "TYER" || frame_id == "TDRC") metadata.year = read_id3v2_text_frame(frame_data.data(), frame_size);
                else if (frame_id == "TRCK") metadata.trackNumber = std::atoi(read_id3v2_text_frame(frame_data.data(), frame_size).c_str());
                else if (frame_id == "TCON") metadata.genre = read_id3v2_text_frame(frame_data.data(), frame_size);
                else if (frame_id == "COMM") metadata.comment = "[comment]";

                bytesRead += 10 + frame_size;
            }

            tagFound = !metadata.title.empty();
        }
    }

    // 2) ID3v1 (bytes; best-effort)
    if (!tagFound) {
        std::ifstream filev1(filePath, std::ios::binary);
        if (filev1) {
            filev1.seekg(-128, std::ios::end);
            char tag[128] = { 0 };
            filev1.read(tag, 128);
            if (filev1.gcount() == 128 && std::string(tag, 3) == "TAG") {
                metadata.title = Utils::trim(std::string(tag + 3, 30));
                metadata.artist = Utils::trim(std::string(tag + 33, 30));
                metadata.album = Utils::trim(std::string(tag + 63, 30));
                metadata.year = Utils::trim(std::string(tag + 93, 4));
                metadata.trackNumber = (tag[125] == 0) ? static_cast<unsigned char>(tag[126]) : 0;
                tagFound = !metadata.title.empty();
            }
        }
    }

    // 3) Fallback to filename
    if (!tagFound) {
        std::string fileName = toUtf8String(filePath.filename());
        size_t lastDot = fileName.find_last_of('.');
        metadata.title = (lastDot != std::string::npos) ? fileName.substr(0, lastDot) : fileName;

        size_t dashPos = metadata.title.find(" - ");
        if (dashPos != std::string::npos) {
            metadata.artist = metadata.title.substr(0, dashPos);
            metadata.title = metadata.title.substr(dashPos + 3);
        }
    }

    return true;
}

// -------------------------------------------------------------------------
// Playback Logic
// -------------------------------------------------------------------------

bool MusicPlayer::playMusic(int index, int customFadeMs, double position) {
    if (musicFiles_.empty()) {
        LOG_ERROR("MusicPlayer", "playMusic called with empty playlist.");
        return false;
    }

    const int useFadeMs = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

    if (index == -1) {
        if (currentIndex_ >= 0) {
            index = currentIndex_;
        }
        else if (shuffleMode_) {
            if (shuffledIndices_.empty()) setShuffle(true);
            if (shuffledIndices_.empty() || currentShufflePos_ < 0 ||
                currentShufflePos_ >= static_cast<int>(shuffledIndices_.size())) {
                LOG_ERROR("MusicPlayer", "Shuffle enabled but no shuffled indices available.");
                return false;
            }
            index = shuffledIndices_[currentShufflePos_];
        }
        else {
            index = 0;
        }
    }

    if (index < 0 || index >= static_cast<int>(musicFiles_.size())) {
        LOG_ERROR("MusicPlayer",
            "Invalid track index. index=" + std::to_string(index) +
            " trackCount=" + std::to_string(musicFiles_.size()) +
            " shuffleMode=" + std::to_string(shuffleMode_) +
            " shuffledCount=" + std::to_string(shuffledIndices_.size()) +
            " shufflePos=" + std::to_string(currentShufflePos_) +
            " currentIndex=" + std::to_string(currentIndex_));
        return false;
    }

    if (musicFade_.active) return false;

    if ((musicTrack_ && (MIX_TrackPlaying(musicTrack_) || MIX_TrackPaused(musicTrack_))) || (musicTrack_ && MIX_TrackPaused(musicTrack_))) {
        if (useFadeMs > 0) {
            beginFadeOutToAction(FinishEvent::TrackChangeAfterFade, index, position, useFadeMs, useFadeMs);
            return true;
        }
        haltTrack();
    }

    loadTrack(index);
    if (!currentMusic_) return false;

    if (shuffleMode_) {
        auto it = std::find(shuffledIndices_.begin(), shuffledIndices_.end(), index);
        if (it != shuffledIndices_.end())
            currentShufflePos_ = static_cast<int>(std::distance(shuffledIndices_.begin(), it));
        else
            setShuffle(true);
    }

    if (musicVolume(-1) != 0) musicVolume(steadyMusicVolume());

    if (!startTrack(position)) {
        LOG_ERROR("MusicPlayer", "Play error: " + std::string(SDL_GetError()));
        return false;
    }



    setPlaybackState(PlaybackState::PLAYING);
    hasStartedPlaying_ = true;
    LOG_INFO("MusicPlayer", "Now playing: " + getFormattedTrackInfo(index));
    return true;
}

bool MusicPlayer::pauseMusic(int customFadeMs) {
    if (!isPlaying() || isFading()) return false;
    int useFade = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

    if (useFade > 0) {
        beginFadeOutToAction(FinishEvent::PauseAfterFade, -1, -1, useFade, 0);
        return true;
    }

    MIX_PauseTrack(musicTrack_);
    setPlaybackState(PlaybackState::PAUSED);
    return true;
}

bool MusicPlayer::resumeMusic(int customFadeMs) {
    if (!isPaused() || isFading()) return false;
    int useFade = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

    musicVolume(0);
    MIX_ResumeTrack(musicTrack_);
    setPlaybackState(PlaybackState::PLAYING);

    if (useFade > 0) beginFadeInToSteadyVolume(useFade);
    else musicVolume(steadyMusicVolume());

    return true;
}

bool MusicPlayer::stopMusic(int customFadeMs) {
    if (!(musicTrack_ && (MIX_TrackPlaying(musicTrack_) || MIX_TrackPaused(musicTrack_))) && !(musicTrack_ && MIX_TrackPaused(musicTrack_))) return false;
    if (isFading()) return false;
    int useFade = (customFadeMs < 0) ? fadeMs_ : customFadeMs;

    if (useFade > 0 && !isShuttingDown_) {
        beginFadeOutToAction(FinishEvent::StopAfterFade, -1, -1, useFade, 0);
        return true;
    }
    haltTrack();
    musicVolume(steadyMusicVolume());
    setPlaybackState(PlaybackState::NONE);
    return true;
}

bool MusicPlayer::nextTrack(int customFadeMs) {
    if (musicFiles_.empty() || isFading()) return false;
    setPlaybackState(PlaybackState::NEXT);
    return playMusic(getNextTrackIndex(), customFadeMs);
}

int MusicPlayer::getNextTrackIndex() {
    if (shuffleMode_ && !shuffledIndices_.empty()) {
        currentShufflePos_ = (currentShufflePos_ + 1) % static_cast<int>(shuffledIndices_.size());
        return shuffledIndices_[currentShufflePos_];
    }
    return (currentIndex_ + 1) % static_cast<int>(musicFiles_.size());
}

bool MusicPlayer::previousTrack(int customFadeMs) {
    if (musicFiles_.empty() || isFading()) return false;

    int prevIndex;
    if (shuffleMode_ && !shuffledIndices_.empty()) {
        currentShufflePos_ = (currentShufflePos_ - 1 + static_cast<int>(shuffledIndices_.size())) % static_cast<int>(shuffledIndices_.size());
        prevIndex = shuffledIndices_[currentShufflePos_];
    }
    else {
        prevIndex = (currentIndex_ - 1 + static_cast<int>(musicFiles_.size())) % static_cast<int>(musicFiles_.size());
    }

    setPlaybackState(PlaybackState::PREVIOUS);
    return playMusic(prevIndex, customFadeMs);
}

// -------------------------------------------------------------------------
// Volume Control
// -------------------------------------------------------------------------

void MusicPlayer::changeVolume(bool increase) {
    Uint64 now = SDL_GetTicks();
    if (now - lastVolumeChangeTime_ < volumeChangeIntervalMs_) return;
    lastVolumeChangeTime_ = now;

    int current = getLogicalVolume();
    int next = increase ? std::min(128, current + 1) : std::max(0, current - 1);
    setLogicalVolume(next);
    setButtonPressed(true);
}

void MusicPlayer::setVolume(int newVolume) {
    isVolumeFading_ = false;
    musicFade_.active = false;

    int v = std::clamp(newVolume, 0, 128);
    volume_.store(v);
    musicVolume(v);
    logicalVolume_.store(v);

    if (config_) config_->setProperty("musicPlayer.volume", vol128ToPct(v));
}

void MusicPlayer::setLogicalVolume(int v) {
    isVolumeFading_ = false;
    musicFade_.active = false;

    v = std::clamp(v, 0, 128);
    logicalVolume_.store(v);
    if (config_) config_->setProperty("musicPlayer.volume", vol128ToPct(v));

    int raw = applyVolumeCurve(v);
    volume_.store(raw);
    musicVolume(raw);
}

void MusicPlayer::fadeToVolume(int targetLogical, int customFadeMs) {
    if (musicFade_.active) return;

    int duration = (customFadeMs >= 0) ? customFadeMs : fadeMs_;
    targetLogical = std::clamp(targetLogical, 0, 128);
    previousLogicalVolume_.store(getLogicalVolume());

    if (getLogicalVolume() == targetLogical || duration <= 0) {
        setLogicalVolume(targetLogical);
        return;
    }

    volumeFadeStartVal_ = getLogicalVolume();
    volumeFadeTargetVal_ = targetLogical;
    volumeFadeDuration_ = duration;
    volumeFadeStartTime_ = SDL_GetTicks();
    isVolumeFading_ = true;
}

void MusicPlayer::fadeBackToPreviousVolume() {
    fadeToVolume(previousLogicalVolume_.load(), fadeMs_);
}

int MusicPlayer::applyVolumeCurve(int v) const {
    v = std::clamp(v, 0, 128);
    if (v == 0) return 0;

    float norm = static_cast<float>(v) / 128.0f;
    float gain = std::pow(10.0f, (norm * 40.0f - 40.0f) / 20.0f);
    return std::clamp(static_cast<int>(gain * 128 + 0.5f), 0, 128);
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

void SDLCALL MusicPlayer::musicFinishedCallback(void* userdata, MIX_Track*) {
    auto* self = static_cast<MusicPlayer*>(userdata);
    if (!self->isShuttingDown_.load(std::memory_order_relaxed))
        self->finishEvent_.store(FinishEvent::NaturalEnd, std::memory_order_release);
}

bool MusicPlayer::ensureAudio() {
    auto& bus = AudioBus::instance();
    if (!bus.initialize()) return false;
    if (!musicTrack_) {
        musicTrack_ = MIX_CreateTrack(bus.mixer());
        if (!musicTrack_) return false;
        MIX_SetTrackStoppedCallback(musicTrack_, musicFinishedCallback, this);
        musicVolume(volume_.load());
        audioChannels_ = bus.dev_channels();
        audioSampleRate_ = bus.dev_rate();
        sampleSize_ = sizeof(float);
        audioLevels_.assign(audioChannels_, 0.0f);
    }
    bus.setMusicPlayer(this);
    return true;
}

void MusicPlayer::haltTrack() {
    if (!musicTrack_) return;
    // Explicit stops must never advance the playlist, including already-ended tracks.
    MIX_SetTrackStoppedCallback(musicTrack_, nullptr, nullptr);
    MIX_StopTrack(musicTrack_, 0);
    MIX_SetTrackStoppedCallback(musicTrack_, musicFinishedCallback, this);
    finishEvent_.store(FinishEvent::None, std::memory_order_release);
}

int MusicPlayer::musicVolume(int value) {
    const int previous = musicTrack_ ? static_cast<int>(MIX_GetTrackGain(musicTrack_) * 128 + 0.5f) : volume_.load();
    if (value >= 0 && musicTrack_) MIX_SetTrackGain(musicTrack_, std::clamp(value, 0, 128) / 128.0f);
    return previous;
}

bool MusicPlayer::startTrack(double position) {
    if (!musicTrack_ || !currentMusic_ || !MIX_SetTrackAudio(musicTrack_, currentMusic_)) return false;
    const auto options = SDL_CreateProperties();
    if (!options) return false;
    bool ok = SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, loopMode_ ? -1 : 0);
    if (position > 0 && std::isfinite(position))
        ok = ok && SDL_SetNumberProperty(options, MIX_PROP_PLAY_START_MILLISECOND_NUMBER, static_cast<Sint64>(position * 1000));
    ok = ok && MIX_PlayTrack(musicTrack_, options);
    SDL_DestroyProperties(options);
    return ok;
}

void MusicPlayer::releaseAudio() {
    AudioBus::instance().setMusicPlayer(nullptr);
    if (musicTrack_) MIX_DestroyTrack(musicTrack_);
    musicTrack_ = nullptr;
    if (currentMusic_) MIX_DestroyAudio(currentMusic_);
    currentMusic_ = nullptr;
    finishEvent_.store(FinishEvent::None, std::memory_order_release);
    cancelFade();
}

void MusicPlayer::beginFadeOutToAction(FinishEvent action, int index, double seekPos, int fadeOutMs, int fadeInMs) {
    isVolumeFading_ = false;
    int duration = std::max(0, fadeOutMs);

    musicFade_.active = true;
    musicFade_.fadingOut = true;
    musicFade_.startTimeMs = SDL_GetTicks();
    musicFade_.durationMs = duration;
    musicFade_.startVol = getLogicalVolume();
    musicFade_.targetVol = 0;
    musicFade_.finishAction = action;
    musicFade_.finishIndex = index;
    musicFade_.finishSeekPos = seekPos;
    musicFade_.fadeInMs = std::max(0, fadeInMs);
}

void MusicPlayer::beginFadeInToSteadyVolume(int fadeInMs) {
    isVolumeFading_ = false;
    int duration = std::max(0, fadeInMs);
    int target = getLogicalVolume();

    musicFade_.active = true;
    musicFade_.fadingOut = false;
    musicFade_.startTimeMs = SDL_GetTicks();
    musicFade_.durationMs = duration;
    musicFade_.startVol = 0;
    musicFade_.targetVol = target;
}

int MusicPlayer::steadyMusicVolume() const {
    return applyVolumeCurve(logicalVolume_.load());
}

// -------------------------------------------------------------------------
// Audio Visuals & Listeners
// -------------------------------------------------------------------------

void MusicPlayer::addVisualizerListener(MusicPlayerComponent* listener) {
    std::lock_guard<std::mutex> lock(visualizerMutex_);

    audioChannels_ = AudioBus::instance().dev_channels();
    audioSampleRate_ = AudioBus::instance().dev_rate();
    sampleSize_ = sizeof(float);
    audioLevels_.resize(audioChannels_, 0.0f);

    if (std::find(visualizerListeners_.begin(), visualizerListeners_.end(), listener) == visualizerListeners_.end()) {
        visualizerListeners_.push_back(listener);
        hasActiveVisualizers_ = true;
    }
}

void MusicPlayer::removeVisualizerListener(MusicPlayerComponent* listener) {
    std::lock_guard<std::mutex> lock(visualizerMutex_);
    auto it = std::remove(visualizerListeners_.begin(), visualizerListeners_.end(), listener);
    visualizerListeners_.erase(it, visualizerListeners_.end());
    hasActiveVisualizers_ = !visualizerListeners_.empty();
}

void MusicPlayer::processAudioData(Uint8* stream, int len) {
    if (!hasActiveVisualizers_ || !stream || len <= 0) return;

    {
        std::lock_guard<std::mutex> lock(visualizerMutex_);
        for (auto* listener : visualizerListeners_) {
            if (listener) listener->onPcmDataReceived(stream, len);
        }
    }

    if (hasVuMeter_) {
        std::fill(audioLevels_.begin(), audioLevels_.end(), 0.0f);

        const int samplesPerChannel = len / (sampleSize_ * audioChannels_);
        if (samplesPerChannel <= 0) return;

        for (int ch = 0; ch < audioChannels_; ++ch) {
            float sum = 0.0f;

            for (int i = 0; i < samplesPerChannel; ++i) {
                const int pos = (i * audioChannels_ + ch) * sampleSize_;
                if (pos + sampleSize_ > len) break;

                float val = 0.0f;

                if (sampleSize_ == 1) {
                    val = (static_cast<float>(stream[pos]) - 128.0f) / 128.0f;
                }
                else if (sampleSize_ == 2) {
                    Sint16 s = 0;
                    std::memcpy(&s, stream + pos, sizeof(Sint16));
                    val = static_cast<float>(s) / 32768.0f;
                }
                else if (sampleSize_ == 4) {
                    float f = 0.0f;
                    std::memcpy(&f, stream + pos, sizeof(float));
                    val = f;
                }

                sum += val * val;
            }

            audioLevels_[ch] = std::min(1.0f, std::sqrt(sum / samplesPerChannel));
        }
    }
}

// -------------------------------------------------------------------------
// Getters / Setters / Info
// -------------------------------------------------------------------------

bool MusicPlayer::setShuffle(bool shuffle) {
    shuffleMode_ = shuffle;
    shuffledIndices_.clear();

    if (shuffleMode_) {
        for (int i = 0; i < static_cast<int>(musicFiles_.size()); i++) shuffledIndices_.push_back(i);
        std::shuffle(shuffledIndices_.begin(), shuffledIndices_.end(), rng_);

        if (currentIndex_ >= 0) {
            auto it = std::find(shuffledIndices_.begin(), shuffledIndices_.end(), currentIndex_);
            currentShufflePos_ = (it != shuffledIndices_.end())
                ? static_cast<int>(std::distance(shuffledIndices_.begin(), it))
                : 0;
        }
        else {
            currentShufflePos_ = 0;
        }
    }
    else {
        currentShufflePos_ = -1;
    }

    if (config_) config_->setProperty("musicPlayer.shuffle", shuffleMode_);
    return true;
}

void MusicPlayer::setLoop(bool loop) {
    loopMode_ = loop;
    if (isPlaying() && currentMusic_) {
        haltTrack();
        startTrack();
    }
    if (config_) config_->setProperty("musicPlayer.loop", loopMode_);
}

bool MusicPlayer::getLoop() const { return loopMode_; }
bool MusicPlayer::getShuffle() const { return shuffleMode_; }
bool MusicPlayer::shuffle() { return playMusic(-1); }

int MusicPlayer::getCurrentTrackIndex() const { return currentIndex_; }
int MusicPlayer::getTrackCount() const { return static_cast<int>(musicFiles_.size()); }

std::filesystem::path MusicPlayer::getCurrentTrackPath() const {
    return (currentIndex_ >= 0) ? musicFiles_[currentIndex_] : fs::path();
}

std::string MusicPlayer::getCurrentTrackName() const {
    return (currentIndex_ >= 0) ? musicNames_[currentIndex_] : "";
}

std::string MusicPlayer::getCurrentTrackNameWithoutExtension() const {
    std::string name = getCurrentTrackName();
    size_t lastDot = name.find_last_of('.');
    return (lastDot != std::string::npos) ? name.substr(0, lastDot) : name;
}

const MusicPlayer::TrackMetadata& MusicPlayer::getCurrentTrackMetadata() const {
    return getTrackMetadata(currentIndex_);
}

const MusicPlayer::TrackMetadata& MusicPlayer::getTrackMetadata(int index) const {
    static TrackMetadata empty;
    if (index >= 0 && index < static_cast<int>(trackMetadata_.size())) return trackMetadata_[index];
    return empty;
}

size_t MusicPlayer::getTrackMetadataCount() const { return trackMetadata_.size(); }

std::string MusicPlayer::getCurrentTitle() const { return getCurrentTrackMetadata().title; }
std::string MusicPlayer::getCurrentArtist() const { return getCurrentTrackMetadata().artist; }
std::string MusicPlayer::getCurrentAlbum() const { return getCurrentTrackMetadata().album; }
std::string MusicPlayer::getCurrentYear() const { return getCurrentTrackMetadata().year; }
std::string MusicPlayer::getCurrentGenre() const { return getCurrentTrackMetadata().genre; }
std::string MusicPlayer::getCurrentComment() const { return getCurrentTrackMetadata().comment; }
int MusicPlayer::getCurrentTrackNumber() const { return getCurrentTrackMetadata().trackNumber; }

std::string MusicPlayer::getFormattedTrackInfo(int index) const {
    if (index == -1) index = currentIndex_;
    const auto& meta = getTrackMetadata(index);
    if (meta.title.empty()) return (index >= 0) ? musicNames_[index] : "";
    return meta.artist.empty() ? meta.title : (meta.title + " - " + meta.artist);
}

std::string MusicPlayer::getTrackArtist(int index) const { return getTrackMetadata(index).artist; }
std::string MusicPlayer::getTrackAlbum(int index) const { return getTrackMetadata(index).album; }

bool MusicPlayer::isPlaying() const { return (musicTrack_ && (MIX_TrackPlaying(musicTrack_) || MIX_TrackPaused(musicTrack_))) == 1 && !(musicTrack_ && MIX_TrackPaused(musicTrack_)); }
bool MusicPlayer::isPaused() const { return (musicTrack_ && MIX_TrackPaused(musicTrack_)) == 1; }
bool MusicPlayer::hasStartedPlaying() const { return hasStartedPlaying_; }
bool MusicPlayer::isFading() const { return musicFade_.active || isVolumeFading_; }

bool MusicPlayer::hasTrackChanged() {
    const fs::path current = (currentIndex_ >= 0) ? normalizeForCompare(musicFiles_[currentIndex_]) : fs::path();
    bool changed = !current.empty() && (current != lastCheckedTrackPath_);
    if (changed) lastCheckedTrackPath_ = current;
    return changed;
}

bool MusicPlayer::isPlayingNewTrack() { return isPlaying() && hasTrackChanged(); }

double MusicPlayer::saveCurrentMusicPosition() {
    if (!currentMusic_) return 0.0;
    return getCurrent();
}

double MusicPlayer::getCurrent() {
    if (!musicTrack_ || !currentMusic_) return -1.0;
    const auto frames = MIX_GetTrackPlaybackPosition(musicTrack_);
    return frames < 0 ? -1.0 : MIX_TrackFramesToMS(musicTrack_, frames) / 1000.0;
}
double MusicPlayer::getDuration() {
    if (!currentMusic_) return -1.0;
    const auto frames = MIX_GetAudioDuration(currentMusic_);
    return frames < 0 ? -1.0 : MIX_AudioFramesToMS(currentMusic_, frames) / 1000.0;
}

std::pair<int, int> MusicPlayer::getCurrentAndDurationSec() {
    if (!currentMusic_) return { -1, -1 };
    return {
        static_cast<int>(getCurrent()),
        static_cast<int>(getDuration())
    };
}

bool MusicPlayer::getAlbumArt(int trackIndex, std::vector<unsigned char>& albumArtData) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(musicFiles_.size())) return false;
    return extractAlbumArtFromFile(musicFiles_[trackIndex], albumArtData);
}

bool MusicPlayer::getButtonPressed() { return buttonPressed_; }
void MusicPlayer::setButtonPressed(bool b) { buttonPressed_ = b; }
int MusicPlayer::getLogicalVolume() { return logicalVolume_.load(); }
int MusicPlayer::getVolume() const { return volume_.load(); }
int MusicPlayer::getSampleSize() const { return sampleSize_; }

void MusicPlayer::cancelFade() {
    if (musicFade_.active) {
        musicFade_.active = false;
        setLogicalVolume(previousLogicalVolume_.load(std::memory_order_relaxed));
    }
    if (isVolumeFading_) {
        isVolumeFading_ = false;
        setLogicalVolume(previousLogicalVolume_.load(std::memory_order_relaxed));
    }
}
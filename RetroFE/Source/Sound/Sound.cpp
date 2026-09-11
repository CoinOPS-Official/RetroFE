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

#include "Sound.h"
#include "AudioBus.h"
#include <algorithm>

#include "../Utility/Log.h"

Sound::Sound(std::string file, std::string altfile)
    : file_(file)
    , chunk_(NULL)
{
    if(file_ != "" && !allocate())
    {
        file_ = altfile;
        if (file_ != "" && !allocate())
        {
            LOG_WARNING("Sound", "Cannot load " + file_);
        }
    }
}

Sound::~Sound() { free(); }

void Sound::play() {
    auto& bus = AudioBus::instance();
    if (!chunk_ || !bus.initialize()) return;
    if (generation_ != bus.generation()) {
        voices_.clear(); // The previous mixer destroyed its tracks.
        generation_ = bus.generation();
    }
    MIX_Track* voice = nullptr;
    for (auto* track : voices_) {
        if (!MIX_TrackPlaying(track)) { voice = track; break; }
    }
    if (!voice) {
        voice = MIX_CreateTrack(bus.mixer());
        if (!voice) return;
        voices_.push_back(voice);
    }
    if (!MIX_SetTrackAudio(voice, chunk_) || !MIX_PlayTrack(voice, 0))
        LOG_WARNING("Sound", std::string("Cannot play ") + file_ + ": " + SDL_GetError());
}

bool Sound::free() {
    auto& bus = AudioBus::instance();
    if (generation_ == bus.generation())
        for (auto* voice : voices_) MIX_DestroyTrack(voice);
    voices_.clear();
    if (chunk_) MIX_DestroyAudio(chunk_);
    chunk_ = nullptr;
    return true;
}

bool Sound::allocate() {
    if (!chunk_ && AudioBus::instance().initialize())
        chunk_ = MIX_LoadAudio(AudioBus::instance().mixer(), file_.c_str(), true);
    return chunk_ != nullptr;
}

bool Sound::isPlaying() {
    if (generation_ != AudioBus::instance().generation()) return false;
    return std::any_of(voices_.begin(), voices_.end(), [](MIX_Track* t) { return MIX_TrackPlaying(t); });
}

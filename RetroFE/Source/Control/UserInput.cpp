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

#include "UserInput.h"
#include "../Database/Configuration.h"
#include "../Database/GlobalOpts.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "GameControllerAxisHandler.h"
#include "GameControllerButtonHandler.h"
#include "JoyAxisHandler.h"
#include "JoyButtonHandler.h"
#include "JoyHatHandler.h"
#include "KeyboardHandler.h"
#include "MouseButtonHandler.h"

UserInput::UserInput(Configuration &c)
    : config_(c)
{
    for(unsigned int i = 0; i < KeyCodeMax; ++i) {
        currentKeyState_[i] = false;
        lastKeyState_[i] = false;
        lastInputTime_[i] = 0;
    }
    for ( unsigned int i = 0; i < cMaxJoy; i++ ) {
        joysticks_[i] = 0;
        gameControllers_[i] = 0;
    }
}

UserInput::~UserInput()
{
    for (unsigned int i = 0; i < keyHandlers_.size(); ++i) {
        if (keyHandlers_[i].first) {
            delete keyHandlers_[i].first;
        }
    }
}

bool UserInput::initialize()
{
    sdlGameController_ = false;
    config_.getProperty(OPTION_SDLGAMECONTROLLER, sdlGameController_);

    if (sdlGameController_) {
        LOG_INFO("Input", "SDL GameController mode enabled");

        // Get the directory where the executable is located
        const char* basePath = SDL_GetBasePath();
        if (basePath) {
            std::string dbPath = std::string(basePath) + "gamecontrollerdb.txt";


            int mappingsAdded = SDL_AddGamepadMappingsFromFile(dbPath.c_str());

            if (mappingsAdded >= 0) {
                LOG_INFO("Input", "Loaded " << mappingsAdded << " controller mappings from: " << dbPath);
            }
            else {
                // Not found is often fine as SDL has internal defaults, but useful to log
                LOG_INFO("Input", "Optional gamecontrollerdb.txt not loaded: " << SDL_GetError());
            }
        }
        else {
            LOG_ERROR("Input", "Could not determine base path for gamecontrollerdb.txt");
        }
    }

    // Optional keys
    MapKey("playlistUp", KeyCodePlaylistUp, false );
    MapKey("playlistDown", KeyCodePlaylistDown, false );
    MapKey("playlistLeft", KeyCodePlaylistLeft, false );
    MapKey("playlistRight", KeyCodePlaylistRight, false );
    MapKey("collectionUp", KeyCodeCollectionUp, false );
    MapKey("collectionDown", KeyCodeCollectionDown, false );
    MapKey("collectionLeft", KeyCodeCollectionLeft, false );
    MapKey("collectionRight", KeyCodeCollectionRight, false );
    MapKey("pageDown", KeyCodePageDown, false );
    MapKey("pageUp", KeyCodePageUp, false );
    MapKey("letterDown", KeyCodeLetterDown, false);
    MapKey("letterUp", KeyCodeLetterUp, false);
    MapKey("favPlaylist", KeyCodeFavPlaylist, false);
    MapKey("nextPlaylist", KeyCodeNextPlaylist, false);
    MapKey("prevPlaylist", KeyCodePrevPlaylist, false);
    MapKey("cyclePlaylist", KeyCodeCyclePlaylist, false);
    MapKey("nextCyclePlaylist", KeyCodeNextCyclePlaylist, false);
    MapKey("prevCyclePlaylist", KeyCodePrevCyclePlaylist, false);
    MapKey("addPlaylist", KeyCodeAddPlaylist, false);
    MapKey("removePlaylist", KeyCodeRemovePlaylist, false);
    MapKey("togglePlaylist", KeyCodeTogglePlaylist, false);
    MapKey("random", KeyCodeRandom, false);
    MapKey("menu", KeyCodeMenu, false);
    MapKey("reboot", KeyCodeReboot, false);
    MapKey("saveFirstPlaylist", KeyCodeSaveFirstPlaylist, false);
    MapKey("kiosk", KeyCodeKisok, false);
	MapKey("showFps", KeyCodeShowFps, false);
    MapKey("cycleCollection", KeyCodeCycleCollection, false);
    MapKey("prevCycleCollection", KeyCodePrevCycleCollection, false);
    MapKey("toggleGameInfo", KeyCodeToggleGameInfo, false);
    MapKey("toggleCollectionInfo", KeyCodeToggleCollectionInfo, false);
    MapKey("toggleBuildInfo", KeyCodeToggleBuildInfo, false);
    MapKey("settings", KeyCodeSettings, false);
	MapKey("quickPlaylist", KeyCodeQuickList, false);
    MapKey("musicPlayer.playPause", KeyCodeMusicPlayPause, false);
    MapKey("musicPlayer.next", KeyCodeMusicNext, false);
    MapKey("musicPlayer.prev", KeyCodeMusicPrev, false);
    MapKey("musicPlayer.volUp", KeyCodeMusicVolumeUp, false);
    MapKey("musicPlayer.volDown", KeyCodeMusicVolumeDown, false);
    
    std::string jbKey;
    if(config_.getProperty(OPTION_JUKEBOX, jbKey)) {
        MapKey("jbFastForward1m", KeyCodeSkipForward, false);
        MapKey("jbFastRewind1m", KeyCodeSkipBackward, false);
        MapKey("jbFastForward5p", KeyCodeSkipForwardp, false);
        MapKey("jbFastRewind5p", KeyCodeSkipBackwardp, false);
        MapKey("jbPause", KeyCodePause, false);
        MapKey("jbRestart", KeyCodeRestart, false);
    }
    
	MapKeyCombo("quitCombo", KeyCodeQuitCombo1, KeyCodeQuitCombo2, false);
    MapKeyCombo("settingsCombo", KeyCodeSettingsCombo1, KeyCodeSettingsCombo2, false);
    MapKeyCombo("gameInfoCombo", KeyCodeGameInfoCombo1, KeyCodeGameInfoCombo2, false);
    MapKeyCombo("collectionInfoCombo", KeyCodeCollectionInfoCombo1, KeyCodeCollectionInfoCombo2, false);
    MapKeyCombo("buildInfoCombo", KeyCodeBuildInfoCombo1, KeyCodeBuildInfoCombo2, false);

    bool retVal = true;
    
    // At least have controls for either a vertical or horizontal menu
    if(!MapKey("up", KeyCodeUp)) { retVal = MapKey("left", KeyCodeUp) && retVal; }
    if(!MapKey("left", KeyCodeLeft)) { retVal = MapKey("up", KeyCodeLeft) && retVal; }
    if(!MapKey("down", KeyCodeDown)) { retVal = MapKey("right", KeyCodeDown ) && retVal; }
    if(!MapKey("right", KeyCodeRight )) { retVal = MapKey("down", KeyCodeRight ) && retVal; }
    
    // These keys are mandatory
    retVal = MapKey("select", KeyCodeSelect) && retVal;
    retVal = MapKey("back",   KeyCodeBack) && retVal;
    retVal = MapKey("quit",   KeyCodeQuit) && retVal;
   

    return retVal;
}

bool UserInput::MapKey(const std::string& keyDescription, KeyCode_E key)
{
    return MapKey(keyDescription, key, true);
}

bool UserInput::MapKey(const std::string& keyDescription, KeyCode_E key, bool required)
{
    std::string description;

    std::string configKey = "controls." + keyDescription;

    if (!config_.getProperty(configKey, description)) {
        if (required) {
            LOG_ERROR("Input", "Missing required property: " + configKey);
        }
        else {
            LOG_INFO("Input", "Missing optional property: " + configKey);
        }
        return false;
    }
    std::istringstream ss(description);
    std::string token;
    bool success = true;

    while (std::getline(ss, token, ',')) {
        token = Configuration::trimEnds(token);
        if (token == "" && description != "") // Allow "," as input key
            token = ",";

        if (!HandleInputMapping(token, key, configKey)) {
            success = false;
        }
    }

    return success;
}

bool UserInput::HandleInputMapping(const std::string& token, KeyCode_E key, const std::string& configKey) {
    SDL_Scancode scanCode = SDL_GetScancodeFromName(token.c_str());
    bool found = false;

    if (scanCode != SDL_SCANCODE_UNKNOWN) {
        LOG_INFO("Input", "Binding key " + configKey);
        keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new KeyboardHandler(scanCode), key));
        found = true;
    }
    else {
        std::string tokenLowered = Utils::toLower(token);

        if (tokenLowered.find("mouse") == 0) {
            std::string mousedesc = Utils::replace(Utils::toLower(token), "mouse", "");
            if (mousedesc.find("button") == 0) {
                int button = 0;
                std::stringstream ss;
                mousedesc = Utils::replace(mousedesc, "button", "");
                if (mousedesc == "left") button = SDL_BUTTON_LEFT;
                else if (mousedesc == "middle") button = SDL_BUTTON_MIDDLE;
                else if (mousedesc == "right") button = SDL_BUTTON_RIGHT;
                else if (mousedesc == "x1") button = SDL_BUTTON_X1;
                else if (mousedesc == "x2") button = SDL_BUTTON_X2;

                keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new MouseButtonHandler(button), key));
                LOG_INFO("Input", "Binding mouse button " + ss.str());
                found = true;
            }
        }
        else if (tokenLowered.find("joy") == 0) {
            std::string joydesc = Utils::replace(Utils::toLower(token), "joy", "");
            int joynum;
            if (isdigit(joydesc.at(0))) {
                std::stringstream ssjoy;
                ssjoy << joydesc.at(0);
                ssjoy >> joynum;
                joydesc = joydesc.erase(0, 1);
            }
            else {
                joynum = -1;
            }

            if (joydesc.find("button") == 0) {
                unsigned int button;
                std::stringstream ss;
                ss << Utils::replace(joydesc, "button", "");
                ss >> button;
                if (sdlGameController_) {
                    keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new GameControllerButtonHandler(joynum, static_cast<SDL_GamepadButton>(button)), key));
                    LOG_INFO("Input", "Binding game controller button " + ss.str());
                }
                else {
                    keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new JoyButtonHandler(joynum, button), key));
                    LOG_INFO("Input", "Binding joypad button " + ss.str());
                }
                found = true;
            }
            else if (joydesc.find("hat") == 0) {
                joydesc = Utils::replace(joydesc, "hat", "");
                std::stringstream sshat;
                sshat << joydesc.at(0);
                int hatnum;
                sshat >> hatnum;
                joydesc = joydesc.erase(0, 1);

                if (sdlGameController_) {
                    // In game controller mode, D-pad directions map to individual buttons
                    SDL_GamepadButton dpadButton = SDL_GAMEPAD_BUTTON_INVALID;
                    if (joydesc == "up")         dpadButton = SDL_GAMEPAD_BUTTON_DPAD_UP;
                    else if (joydesc == "down")  dpadButton = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
                    else if (joydesc == "left")  dpadButton = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
                    else if (joydesc == "right") dpadButton = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;

                    if (dpadButton != SDL_GAMEPAD_BUTTON_INVALID) {
                        keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new GameControllerButtonHandler(joynum, dpadButton), key));
                        LOG_INFO("Input", "Binding game controller D-pad " + joydesc);
                        found = true;
                    }
                    else {
                        LOG_ERROR("Input", "Unsupported hat direction '" + joydesc + "' in GameController mode for " + configKey + ". Use up/down/left/right.");
                    }
                }
                else {
                    Uint8 hat = 0;
                    if (joydesc == "leftup") hat = SDL_HAT_LEFTUP;
                    else if (joydesc == "left") hat = SDL_HAT_LEFT;
                    else if (joydesc == "leftdown") hat = SDL_HAT_LEFTDOWN;
                    else if (joydesc == "up") hat = SDL_HAT_UP;
                    //else if(joydesc == "centered") hat = SDL_HAT_CENTERED;
                    else if (joydesc == "down") hat = SDL_HAT_DOWN;
                    else if (joydesc == "rightup") hat = SDL_HAT_RIGHTUP;
                    else if (joydesc == "right") hat = SDL_HAT_RIGHT;
                    else if (joydesc == "rightdown") hat = SDL_HAT_RIGHTDOWN;

                    keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new JoyHatHandler(joynum, hatnum, hat), key));
                    LOG_INFO("Input", "Binding joypad hat " + joydesc);
                    found = true;
                }
            }
            else if (joydesc.find("axis") == 0) {
                // string is now axis0+
                unsigned int axis;
                Sint16       min = 0;
                Sint16       max = 0;
                int          deadZone;

                joydesc = Utils::replace(joydesc, "axis", "");

                if (!config_.getProperty("controls.deadZone", deadZone)) {
                    deadZone = 3;
                }

                // string is now 0+
                if (joydesc.find("-") != std::string::npos) {
                    min = -32768;
                    max = -32768 / 100 * deadZone;
                    joydesc = Utils::replace(joydesc, "-", "");
                }
                else if (joydesc.find("+") != std::string::npos) {
                    min = 32767 / 100 * deadZone;
                    max = 32767;
                    joydesc = Utils::replace(joydesc, "+", "");
                }

                // string is now just the axis number
                std::stringstream ss;
                ss << joydesc;
                ss >> axis;
                if (sdlGameController_) {
                    LOG_INFO("Input", "Binding game controller axis " + ss.str());
                    keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new GameControllerAxisHandler(joynum, static_cast<SDL_GamepadAxis>(axis), min, max), key));
                }
                else {
                    LOG_INFO("Input", "Binding joypad axis " + ss.str());
                    keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new JoyAxisHandler(joynum, axis, min, max), key));
                }
                found = true;
            }
        }
else if (tokenLowered.find("gamepad") == 0) {
    std::string padDesc = Utils::replace(tokenLowered, "gamepad", "");
    int joynum = -1;

    // Check if a specific controller index was provided (e.g., gamepad0a)
    if (!padDesc.empty() && isdigit(padDesc.at(0))) {
        std::stringstream ssjoy;
        ssjoy << padDesc.at(0);
        ssjoy >> joynum;
        padDesc = padDesc.erase(0, 1);
    }

    if (sdlGameController_) {
        bool isAxis = false;
        Sint16 min = 0;
        Sint16 max = 0;
        int deadZone;

        if (!config_.getProperty("controls.deadZone", deadZone)) {
            deadZone = 3;
        }

        std::string axisDesc = padDesc;

        // If it has a + or -, it's an analog axis direction
        if (axisDesc.find("-") != std::string::npos) {
            isAxis = true;
            min = -32768;
            max = -32768 / 100 * deadZone;
            axisDesc = Utils::replace(axisDesc, "-", "");
        }
        else if (axisDesc.find("+") != std::string::npos) {
            isAxis = true;
            min = 32767 / 100 * deadZone;
            max = 32767;
            axisDesc = Utils::replace(axisDesc, "+", "");
        }

        if (isAxis) {
            SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(axisDesc.c_str());

            if (axis != SDL_GAMEPAD_AXIS_INVALID) {
                keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new GameControllerAxisHandler(joynum, axis, min, max), key));
                LOG_INFO("Input", "Binding game controller semantic axis: " + padDesc);
                found = true;
            }
            else {
                LOG_ERROR("Input", "Invalid GameController axis name: " + padDesc);
            }
        }
        else {
            // No + or - found, so it must be a button
            SDL_GamepadButton button = SDL_GetGamepadButtonFromString(padDesc.c_str());

            if (button != SDL_GAMEPAD_BUTTON_INVALID) {
                keyHandlers_.push_back(std::pair<InputHandler*, KeyCode_E>(new GameControllerButtonHandler(joynum, button), key));
                LOG_INFO("Input", "Binding game controller semantic button: " + padDesc);
                found = true;
            }
            else {
                LOG_ERROR("Input", "Invalid GameController button name: " + padDesc);
            }
        }
    }
    else {
        LOG_WARNING("Input", "Cannot map 'gamepad' string because SDL GameController mode is disabled.");
    }
        }
    }

    if (!found) {
        LOG_ERROR("Input", "Unsupported property value for " + configKey + "(" + token + "). See Documentation/Keycodes.txt for valid inputs");
    }

    return found;
}

bool UserInput::MapKeyCombo(const std::string& keyDescription, KeyCode_E key1, KeyCode_E key2, bool required) {
    std::string description;

    std::string configKey = "controls." + keyDescription;
    if (!config_.getProperty(configKey, description)) {
        if (required) {
            LOG_ERROR("Input", "Missing required combo property: " + configKey);
        }
        else {
            LOG_INFO("Input", "Missing optional combo property: " + configKey);
        }
        return false;
    }

    std::istringstream ss(description);
    std::string token;
    bool success = true;
    bool firstKeyMapped = false;

    while (std::getline(ss, token, ',')) {
        token = Configuration::trimEnds(token);
        if (token.empty() && !description.empty()) // Allow "," as input key
            token = ",";

        if (!firstKeyMapped) {
            if (!HandleInputMapping(token, key1, configKey)) {
                success = false;
            }
            firstKeyMapped = true;
        }
        else {
            if (!HandleInputMapping(token, key2, configKey)) {
                success = false;
            }
            break; // Only two keys are expected for a combo
        }
    }

    return success;
}

void UserInput::resetStates()
{
    for (unsigned int i = 0; i < keyHandlers_.size(); ++i) {
        if (keyHandlers_[i].first) {
            keyHandlers_[i].first->reset();
        }
        currentKeyState_[keyHandlers_[i].second] = false;
        lastKeyState_[keyHandlers_[i].second] = false;
    }
}


bool UserInput::update(SDL_Event& event) {
    if (!SDL_IsMainThread()) {
        LOG_ERROR("Input", "UserInput::update must run on the SDL main thread.");
        return false;
    }
    // Handlers use zero-based configuration slots; never rewrite the caller's instance IDs.
    SDL_Event e = event;
    if (sdlGameController_) {
        // Handle adding a game controller
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
            for (auto id : gameControllers_) if (id == e.gdevice.which) return false;
            SDL_Gamepad* controller = SDL_OpenGamepad(e.gdevice.which);
            if (!controller) {
                LOG_ERROR("Input", "Failed to open game controller: " << SDL_GetError());
            } else {
                SDL_JoystickID id = SDL_GetJoystickID(SDL_GetGamepadJoystick(controller));
                bool added = false;
                for (unsigned int i = 0; i < cMaxJoy; i++) {
                    if (gameControllers_[i] == 0) {
                        gameControllers_[i] = id;
                        LOG_INFO("Input", "Game controller connected, assigned to slot " << i);
                        added = true;
                        break;
                    }
                }
                if (!added) {
                    LOG_WARNING("Input", "Maximum number of game controllers (" << cMaxJoy << ") reached; new controller ignored");
                    SDL_CloseGamepad(controller);
                }
            }
        }

        // Handle removing a game controller
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
            bool owned = false;
            for (unsigned int i = 0; i < cMaxJoy; i++) {
                if (gameControllers_[i] == e.gdevice.which) {
                    owned = true;
                    gameControllers_[i] = 0;
                    LOG_INFO("Input", "Game controller disconnected from slot " << i);
                    break;
                }
            }
            SDL_Gamepad* controller = SDL_GetGamepadFromID(e.gdevice.which);
            if (owned && controller) {
                SDL_CloseGamepad(controller);
            }
            // Reset all handler pressed states so no input remains "stuck" after disconnect
            resetStates();
        }

        // Remap game controller events: replace instance ID with slot index
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP   ||
            e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
            e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            SDL_JoystickID& which = e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
                ? e.gaxis.which : e.gbutton.which;
            unsigned int slot = 0;
            while (slot < cMaxJoy && gameControllers_[slot] != which) ++slot;
            if (!which || slot == cMaxJoy) return false;
            which = slot;
        }
    } else {
        // Handle adding a joystick
        if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
            for (auto id : joysticks_) if (id == e.jdevice.which) return false;
            SDL_Joystick* joy = SDL_OpenJoystick(e.jdevice.which);
            if (!joy) {
                LOG_ERROR("Input", "Failed to open joystick: " << SDL_GetError());
            } else {
                SDL_JoystickID id = SDL_GetJoystickID(joy);
                bool added = false;
                for (unsigned int i = 0; i < cMaxJoy; i++) {
                    if (joysticks_[i] == 0) {
                        joysticks_[i] = id;
                        LOG_INFO("Input", "Joystick connected, assigned to slot " << i);
                        added = true;
                        break;
                    }
                }
                if (!added) {
                    LOG_WARNING("Input", "Maximum number of joysticks (" << cMaxJoy << ") reached; new joystick ignored");
                    SDL_CloseJoystick(joy);
                }
            }
        }

        // Handle removing a joystick
        if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
            bool owned = false;
            for (unsigned int i = 0; i < cMaxJoy; i++) {
                if (joysticks_[i] == e.jdevice.which) {
                    owned = true;
                    joysticks_[i] = 0;
                    LOG_INFO("Input", "Joystick disconnected from slot " << i);
                    break;
                }
            }
            SDL_Joystick* joy = SDL_GetJoystickFromID(e.jdevice.which);
            if (owned && joy) {
                SDL_CloseJoystick(joy);
            }
            // Reset all handler pressed states so no input remains "stuck" after disconnect
            resetStates();
        }

        // Remap joystick events
        if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION ||
            e.type == SDL_EVENT_JOYSTICK_BUTTON_UP ||
            e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ||
            e.type == SDL_EVENT_JOYSTICK_HAT_MOTION) {
            SDL_JoystickID& which = e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION
                ? e.jaxis.which : (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION
                    ? e.jhat.which : e.jbutton.which);
            unsigned int slot = 0;
            while (slot < cMaxJoy && joysticks_[slot] != which) ++slot;
            if (!which || slot == cMaxJoy) return false;
            which = slot;
        }
    }

    bool event_handled = false;
    for (unsigned int i = 0; i < keyHandlers_.size(); ++i) {
        InputHandler* h = keyHandlers_[i].first;
        if (h) {
            if (h->update(e)) event_handled = true;
        }
    }

    return event_handled;
}

bool UserInput::keystate(KeyCode_E code) const
{
    return currentKeyState_[code];
}

bool UserInput::lastKeyPressed(KeyCode_E code) const
{
    if (lastKeyState_[code]) {
        return true;
    }
    return false;
}

bool UserInput::newKeyPressed(KeyCode_E code) const
{
    return currentKeyState_[code] && !lastKeyState_[code];
}


void UserInput::clearJoysticks( )
{
    for ( unsigned int i = 0; i < cMaxJoy; i++ ) {
        joysticks_[i] = 0;
        gameControllers_[i] = 0;
    }
}


void UserInput::reconfigure()
{
    LOG_INFO("Input", "Reconfigure Inputs");

    for (unsigned int i = 0; i < keyHandlers_.size(); ++i) {
        if (keyHandlers_[i].first) {
            delete keyHandlers_[i].first;
        }
    }
    keyHandlers_.clear();
    initialize( );
}


void UserInput::updateKeystate() {
    // First, prepare the state for the new frame.
    memcpy(lastKeyState_, currentKeyState_, sizeof(lastKeyState_));
    memset(currentKeyState_, 0, sizeof(currentKeyState_));

    // Now, poll all handlers to aggregate the final state for this frame.
    for (unsigned int i = 0; i < keyHandlers_.size(); ++i) {
        InputHandler* h = keyHandlers_[i].first;
        if (h) {
            h->updateKeystate(); // For polling-based inputs like touch
            currentKeyState_[keyHandlers_[i].second] |= h->pressed();
        }
    }
    Uint64 now = SDL_GetTicks();
    for (unsigned int i = 0; i < KeyCodeMax; ++i) {
        // Is this a NEW press? (Hardware says true, but last frame was false)
        if (currentKeyState_[i] && !lastKeyState_[i]) {

            if (now - lastInputTime_[i] < DEBOUNCE_MS) {
                // BOUNCE DETECTED! 
                // The hardware flickered. Instead of dropping the input (which breaks holds),
                // we trick the engine into thinking the button was never released.
                lastKeyState_[i] = true;
            }
            else {
                // VALID NEW PRESS!
                // Anchor the timestamp so no other presses can happen for 100ms.
                lastInputTime_[i] = now;
            }
        }
    }
}
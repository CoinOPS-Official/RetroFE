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

#include "Launcher.h"
#include "../RetroFE.h"
#include "../Collection/Item.h"
#include "../Utility/Log.h"
#include "../Database/Configuration.h"
#include "../Utility/Utils.h"
#include "../RetroFE.h"
#include "../SDL.h"
#include "../Database/GlobalOpts.h"
#include "../Collection/CollectionInfoBuilder.h"
#include "../Database/HiScores.h"
#include <cstdlib>
#include <locale>
#include <sstream>
#include <fstream>
#include "../Graphics/Page.h"
#include <thread>
#include <atomic>
#include <filesystem>
#ifdef WIN32
#include <Windows.h>
#pragma comment(lib, "Xinput.lib")
#include <Xinput.h>
#include <cstring>
#include "PacDrive.h"
#include "StdAfx.h"
#include <tlhelp32.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <libusb-1.0/libusb.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <cstring>
#include <signal.h>
#include <iostream>
#endif
# if defined(__linux__)
#include <libevdev-1.0/libevdev/libevdev.h>
#include <libevdev-1.0/libevdev/libevdev-uinput.h>
#endif

namespace fs = std::filesystem;

Launcher::Launcher(Configuration& c, RetroFE& retroFe)
	: config_(c),
	retroFeInstance_(retroFe)
{
}

#if defined(__linux__) || defined(__APPLE__)

// Function to set ServoStik mode
bool SetServoStikMode(bool fourWay) {
	LOG_INFO("ServoStik", "Attempting to set ServoStik mode to " + std::string(fourWay ? "4-way" : "8-way"));

	libusb_context* ctx = NULL;
	int ret = libusb_init(&ctx);
	if (ret < 0) {
		LOG_ERROR("ServoStik", "libusb_init failed: " + std::string(libusb_error_name(ret)));
		return false;
	}

	libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, 0xD209, 0x1700);
	if (!handle) {
		LOG_ERROR("ServoStik", "Failed to open ServoStik device.");
		libusb_exit(ctx);
		return false;
	}

	// Detach kernel driver
	if (libusb_kernel_driver_active(handle, 0) == 1) {
		ret = libusb_detach_kernel_driver(handle, 0);
		if (ret < 0) {
			LOG_ERROR("ServoStik", "Failed to detach kernel driver: " + std::string(libusb_error_name(ret)));
			libusb_close(handle);
			libusb_exit(ctx);
			return false;
		}
	}

	// Claim the interface
	ret = libusb_claim_interface(handle, 0);
	if (ret < 0) {
		LOG_ERROR("ServoStik", "libusb_claim_interface failed: " + std::string(libusb_error_name(ret)));
		libusb_close(handle);
		libusb_exit(ctx);
		return false;
	}

	LOG_INFO("ServoStik", "Interface 0 claimed successfully.");

	unsigned char mesg[4] = { 0x00, 0xdd, 0x00, static_cast<unsigned char>(fourWay ? 0x00 : 0x01) };
	LOG_INFO("ServoStik", "Sending command: {0x00, 0xDD, 0x00, " + std::to_string((int)mesg[3]) + "}");

	for (int i = 0; i < 2; ++i) {
		ret = libusb_control_transfer(handle,
			0x21,  // Request type
			9,     // Request
			0x0200, // Value
			0,      // Index
			mesg,    // Data
			4,       // Length
			2000);   // Timeout (ms)
		if (ret < 0) {
			LOG_ERROR("ServoStik", "libusb_control_transfer failed on attempt " + std::to_string(i + 1) + ": " + std::string(libusb_error_name(ret)));
			libusb_release_interface(handle, 0);
			libusb_close(handle);
			libusb_exit(ctx);
			return false;
		}
		else {
			LOG_INFO("ServoStik", "Control transfer successful on attempt " + std::to_string(i + 1));
		}
	}

	libusb_release_interface(handle, 0);
	libusb_close(handle);
	libusb_exit(ctx);

	LOG_INFO("ServoStik", "ServoStik mode set successfully.");
	return true;
}

bool SetServoStik4Way() {
	return SetServoStikMode(true);
}

bool SetServoStik8Way() {
	return SetServoStikMode(false);
}
#endif

#if defined (__linux__)
std::vector<std::string> getInputDevices() {
	std::vector<std::string> devicePaths;
	const std::string inputDir = "/dev/input/";

	try {
		for (const auto& entry : std::filesystem::directory_iterator(inputDir)) {
			const std::string devicePath = entry.path().string();

			// Only consider character device files with "event" in their name
			if (!std::filesystem::is_character_file(entry) || devicePath.find("event") == std::string::npos) {
				continue;
			}

			// Open the device to check if it supports EV_KEY events
			int fd = open(devicePath.c_str(), O_RDONLY | O_NONBLOCK);
			if (fd < 0) {
				LOG_WARNING("InputDetection", "Failed to open device: " + devicePath);
				continue;
			}

			unsigned long evBitmask[(EV_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };
			if (ioctl(fd, EVIOCGBIT(0, sizeof(evBitmask)), evBitmask) >= 0) {
				if (evBitmask[0] & (1 << EV_KEY)) { // Check for EV_KEY support
					devicePaths.push_back(devicePath);
					LOG_DEBUG("InputDetection", "Added valid input device: " + devicePath);
				}
			}
			else {
				LOG_WARNING("InputDetection", "Failed to get event bits for device: " + devicePath);
			}
			close(fd);
		}
	}
	catch (const std::exception& e) {
		LOG_ERROR("InputDetection", "Error scanning input directory: " + std::string(e.what()));
	}

	return devicePaths;
}

bool detectInput(const std::vector<std::string>& devices) {
	for (const auto& devicePath : devices) {
		int fd = open(devicePath.c_str(), O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			LOG_WARNING("InputDetection", "Failed to open device: " + devicePath);
			continue;
		}

		struct libevdev* dev = nullptr;
		if (libevdev_new_from_fd(fd, &dev) < 0) {
			LOG_WARNING("InputDetection", "Failed to initialize device: " + devicePath);
			close(fd);
			continue;
		}

		struct input_event ev;
		auto startTime = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - startTime < std::chrono::milliseconds(100)) {
			int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
			if (rc == 0) {
				LOG_DEBUG("InputDetection", "Event type: " + std::to_string(ev.type) +
					", Code: " + std::to_string(ev.code) +
					", Value: " + std::to_string(ev.value));
				if (ev.type == EV_KEY && ev.value == 1) { // Key/button press
					LOG_INFO("InputDetection", "Key/button press detected on device: " + devicePath +
						", Code: " + std::to_string(ev.code));
					libevdev_free(dev);
					close(fd);
					return true;
				}
			}
			else if (rc == -EAGAIN) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Brief pause for non-blocking mode
			}
			else {
				LOG_WARNING("InputDetection", "Error reading from device: " + devicePath);
				break;
			}
		}

		libevdev_free(dev);
		close(fd);
	}
	return false; // No input detected
}
#endif

#if defined(__APPLE__)
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CoreFoundation/CoreFoundation.h>
#include <vector>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>

std::vector<std::string> getInputDevices() {
    std::vector<std::string> devicePaths;
    IOHIDManagerRef hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);

    if (hidManager == nullptr) {
        std::cerr << "Failed to create HID Manager" << std::endl;
        return devicePaths;
    }

    // Set up the device matching criteria to find all input devices
    CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOHIDDeviceKey);
    IOHIDManagerSetDeviceMatching(hidManager, matchingDict);

    IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone);

    // Get the list of devices
    CFSetRef devices = IOHIDManagerCopyDevices(hidManager);
    if (devices == nullptr) {
        std::cerr << "No input devices found." << std::endl;
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        CFRelease(hidManager);
        return devicePaths;
    }

    // Iterate over devices and check if they support key events (buttons/keys)
    CFIndex deviceCount = CFSetGetCount(devices);
    CFTypeRef *deviceArray = new CFTypeRef[deviceCount];
    CFSetGetValues(devices, deviceArray);

    for (CFIndex i = 0; i < deviceCount; ++i) {
        IOHIDDeviceRef device = (IOHIDDeviceRef)deviceArray[i];

        // Check if the device supports keys
        if (IOHIDDeviceConformsTo(device, kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard)) {
            // Get the product name (property)
            CFTypeRef productNameRef = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));

            // Ensure the property is of type CFStringRef before using it
            if (productNameRef && CFGetTypeID(productNameRef) == CFStringGetTypeID()) {
                CFStringRef productName = (CFStringRef)productNameRef;

                // Convert CFStringRef to std::string
                char buffer[256];
                if (CFStringGetCString(productName, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
                    devicePaths.push_back(std::string(buffer));
                    std::cout << "Added valid input device: " << buffer << std::endl;
                }
            }
        }
    }

    delete[] deviceArray;
    CFRelease(devices);
    IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
    CFRelease(hidManager);
    return devicePaths;
}

bool detectInput(const std::vector<std::string>& devices) {
    for (const auto& devicePath : devices) {
        IOHIDManagerRef hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (hidManager == nullptr) {
            std::cerr << "Failed to create HID Manager for device " << devicePath << std::endl;
            continue;
        }

        // Set up the device matching criteria (similar to getInputDevices)
        CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOHIDDeviceKey);
        IOHIDManagerSetDeviceMatching(hidManager, matchingDict);

        IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone);

        CFSetRef devicesSet = IOHIDManagerCopyDevices(hidManager);
        if (devicesSet == nullptr) {
            std::cerr << "No devices found." << std::endl;
            IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
            CFRelease(hidManager);
            continue;
        }

        CFIndex deviceCount = CFSetGetCount(devicesSet);
        CFTypeRef *deviceArray = new CFTypeRef[deviceCount];
        CFSetGetValues(devicesSet, deviceArray);

        // Iterate over devices to check if the desired input event is triggered
        for (CFIndex i = 0; i < deviceCount; ++i) {
            IOHIDDeviceRef device = (IOHIDDeviceRef)deviceArray[i];

            // Check if the device supports keys
            if (IOHIDDeviceConformsTo(device, kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard)) {

                // Get the list of elements for the device
                CFArrayRef elements = IOHIDDeviceCopyMatchingElements(device, nullptr, kIOHIDOptionsTypeNone);
                if (elements) {
                    CFIndex elementCount = CFArrayGetCount(elements);

                    for (CFIndex j = 0; j < elementCount; ++j) {
                        IOHIDElementRef element = (IOHIDElementRef)CFArrayGetValueAtIndex(elements, j);

                        // Check if the element represents a key
                        if (IOHIDElementGetUsagePage(element) == kHIDPage_Button &&
                            IOHIDElementGetUsage(element) >= kHIDUsage_Button_1 &&
                            IOHIDElementGetUsage(element) <= kHIDUsage_Button_128) {
                            
                            // Create an IOHIDValueRef to store the value of the element
                            IOHIDValueRef value = nullptr;
                            IOReturn result = IOHIDDeviceGetValue(device, element, &value);

                            if (result == kIOReturnSuccess && value != nullptr) {
                                uint8_t keyValue = 0;
                                // Extract the value (0 or 1 for key press/release)
                                keyValue = (uint8_t)IOHIDValueGetIntegerValue(value);

                                if (keyValue) {
                                    std::cout << "Key press detected on device: " << devicePath << std::endl;
                                    CFRelease(elements);
                                    IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
                                    CFRelease(hidManager);
                                    return true;
                                }
                                CFRelease(value);  // Always release the IOHIDValueRef
                            } else {
                                std::cerr << "Failed to get value for element" << std::endl;
                            }
                        }
                    }
                    CFRelease(elements);
                }
            }
        }

        delete[] deviceArray;
        CFRelease(devicesSet);
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        CFRelease(hidManager);
    }
    return false; // No input detected
}

#endif

std::string replaceVariables(std::string str,
	const std::string& itemFilePath,
	const std::string& itemName,
	const std::string& itemFilename,
	const std::string& itemDirectory,
	const std::string& itemCollectionName)
{
	str = Utils::replace(str, "%ITEM_FILEPATH%", itemFilePath);
	str = Utils::replace(str, "%ITEM_NAME%", itemName);
	str = Utils::replace(str, "%ITEM_FILENAME%", itemFilename);
	str = Utils::replace(str, "%ITEM_DIRECTORY%", itemDirectory);
	str = Utils::replace(str, "%ITEM_COLLECTION_NAME%", itemCollectionName);
	str = Utils::replace(str, "%RETROFE_PATH%", Configuration::absolutePath);
	str = Utils::replace(str, "%COLLECTION_PATH%", Utils::combinePath(Configuration::absolutePath, "collections", itemCollectionName));
#ifdef WIN32
	str = Utils::replace(str, "%RETROFE_EXEC_PATH%", Utils::combinePath(Configuration::absolutePath, "retrofe", "RetroFE.exe"));
	const char* comspec = std::getenv("COMSPEC");
	if (comspec) {
		str = Utils::replace(str, "%CMD%", std::string(comspec));
	}
#else
	str = Utils::replace(str, "%RETROFE_EXEC_PATH%", Utils::combinePath(Configuration::absolutePath, "RetroFE"));
#endif

	return str;
}

#ifdef WIN32
// Utility function to terminate a process and all its child processes
void TerminateProcessAndChildren(DWORD processId) {
	LOG_INFO("Launcher", "Terminating process ID: " + std::to_string(processId));

	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE) {
		LOG_WARNING("Launcher", "Failed to create snapshot for process termination.");
		return;
	}

	// First find all child processes
	std::vector<DWORD> childPids;
	PROCESSENTRY32 pe32{};
	pe32.dwSize = sizeof(PROCESSENTRY32);

	if (Process32First(hSnap, &pe32)) {
		do {
			if (pe32.th32ParentProcessID == processId) {
				childPids.push_back(pe32.th32ProcessID);
			}
		} while (Process32Next(hSnap, &pe32));
	}

	CloseHandle(hSnap);

	// Terminate children first
	for (DWORD childPid : childPids) {
		TerminateProcessAndChildren(childPid);
	}

	// Now terminate the main process
	HANDLE hProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
	if (hProcess != nullptr) {
		if (TerminateProcess(hProcess, 1)) {
			// Wait for process to exit with timeout
			WaitForSingleObject(hProcess, 2000); // 2 second timeout
		}
		CloseHandle(hProcess);
	}
}
#endif

bool Launcher::run(std::string collection, Item* collectionItem, Page* currentPage, bool isAttractMode) {
	// Step 1: Determine launcher name (with potential per-item override)
	std::string launcherName = collectionItem->collectionInfo->launcher;
	std::string launcherFile = Utils::combinePath(Configuration::absolutePath, "collections", collection, "launchers", collectionItem->name + ".conf");

	if (std::ifstream launcherStream(launcherFile); launcherStream.good()) {
		std::string line;
		if (std::getline(launcherStream, line)) {
			std::string localLauncherKey = "localLaunchers." + collection + "." + line;
			launcherName = config_.propertyPrefixExists(localLauncherKey) ? (collection + "." + line) : line;
			LOG_INFO("Launcher", "Using per-item launcher override: " + launcherName);
		}
	}

	// Check for collection-specific launcher if no override
	if (launcherName == collectionItem->collectionInfo->launcher) {
		std::string collectionLauncherKey = "collectionLaunchers." + collection;
		if (config_.propertyPrefixExists(collectionLauncherKey)) {
			launcherName = collectionItem->collectionInfo->name;
			LOG_INFO("Launcher", "Using collection-specific launcher: " + launcherName);
		}
	}

	// Step 2: Retrieve executable, extensions, and directory
	std::string executablePath, selectedItemsDirectory, selectedItemsPath, extensionstr, matchedExtension, args;

	if (!launcherExecutable(executablePath, launcherName)) {
		LOG_ERROR("Launcher", "Launcher executable not found for: " + launcherName);
		return false;
	}
	if (!extensions(extensionstr, collection)) {
		LOG_ERROR("Launcher", "No file extensions configured for collection: " + collection);
		return false;
	}
	if (!collectionDirectory(selectedItemsDirectory, collection)) {
		LOG_ERROR("Launcher", "No valid directory found for collection: " + collection);
		return false;
	}

	launcherArgs(args, launcherName);

	// Override directory if filepath is provided in the item
	if (!collectionItem->filepath.empty()) {
		selectedItemsDirectory = collectionItem->filepath;
		LOG_DEBUG("LauncherDebug", "Using filepath from item: " + selectedItemsDirectory);
	}

	// Step 3: Find the item file based on provided or derived names
	if (collectionItem->file.empty()) {
		findFile(selectedItemsPath, matchedExtension, selectedItemsDirectory, collectionItem->name, extensionstr);
	}
	else {
		findFile(selectedItemsPath, matchedExtension, selectedItemsDirectory, collectionItem->file, extensionstr);
	}

	LOG_DEBUG("LauncherDebug", "File selected: " + selectedItemsPath);
	LOG_DEBUG("LauncherDebug", "Arguments before replacement: " + args);

	// Step 4: Substitute variables in args and executable path
	args = replaceVariables(args, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
	executablePath = replaceVariables(executablePath, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);

	LOG_INFO("Launcher", "Final executable path: " + executablePath);
	LOG_INFO("Launcher", "Arguments after replacement: " + args);

	// Step 5: Determine the current working directory
	std::string currentDirectoryKey = "launchers." + launcherName + ".currentDirectory";
	std::string currentDirectory = Utils::getDirectory(executablePath);
	config_.getProperty(currentDirectoryKey, currentDirectory);

	currentDirectory = replaceVariables(currentDirectory, selectedItemsPath, collectionItem->name, Utils::getFileName(selectedItemsPath), selectedItemsDirectory, collection);
	LOG_DEBUG("LauncherDebug", "Final working directory: " + currentDirectory);

	// Step 6: Execute the command
	if (!execute(executablePath, args, currentDirectory, true, currentPage, isAttractMode, collectionItem)) {
		LOG_ERROR("Launcher", "Execution failed for: " + executablePath);
		return false;
	}

	// Step 7: Check for reboot configuration
	bool reboot = false;
	config_.getProperty("launchers." + launcherName + ".reboot", reboot);

	LOG_INFO("Launcher", "Execution completed for: " + executablePath + " with reboot flag: " + std::to_string(reboot));
	return reboot;
}

void Launcher::startScript()
{
#ifdef WIN32
	std::string exe = Utils::combinePath(Configuration::absolutePath, "start.bat");
#else
	std::string exe = Utils::combinePath(Configuration::absolutePath, "start.sh");
#endif
	if (fs::exists(exe)) {
		simpleExecute(exe, "", Configuration::absolutePath, false);
	}
}

void Launcher::exitScript()
{
#ifdef WIN32
	std::string exe = Utils::combinePath(Configuration::absolutePath, "exit.bat");
#else
	std::string exe = Utils::combinePath(Configuration::absolutePath, "exit.sh");
#endif
	if (fs::exists(exe)) {
		simpleExecute(exe, "", Configuration::absolutePath, false);
	}
}

void Launcher::LEDBlinky(int command, std::string collection, Item* collectionItem)
{
	std::string LEDBlinkyDirectory = "";
	config_.getProperty(OPTION_LEDBLINKYDIRECTORY, LEDBlinkyDirectory);
	if (LEDBlinkyDirectory == "") {
		return;
	}
	std::string exe = Utils::combinePath(LEDBlinkyDirectory, "LEDBlinky.exe");
	// Check if the LEDBlinky.exe file exists
	if (!std::filesystem::exists(exe)) {
		return; // Exit early if the file does not exist
	}
	std::string args = std::to_string(command);
	bool wait = false;
	if (command == 2)
		wait = true;
	if (command == 8) {
		std::string launcherName = collectionItem->collectionInfo->launcher;
		std::string launcherFile = Utils::combinePath(Configuration::absolutePath, "collections", collectionItem->collectionInfo->name, "launchers", collectionItem->name + ".conf");
		if (std::ifstream launcherStream(launcherFile.c_str()); launcherStream.good()) // Launcher file found
		{
			std::string line;
			if (std::getline(launcherStream, line)) // Launcher found
			{
				launcherName = line;
			}
		}
		launcherName = Utils::toLower(launcherName);
		std::string emulator = collection;
		config_.getProperty("launchers." + launcherName + ".LEDBlinkyEmulator", emulator);
		args = args + " \"" + emulator + "\"";
	}
	if (command == 3 || command == 9) {
		std::string launcherName = collectionItem->collectionInfo->launcher;
		std::string launcherFile = Utils::combinePath(Configuration::absolutePath, "collections", collectionItem->collectionInfo->name, "launchers", collectionItem->name + ".conf");
		if (std::ifstream launcherStream(launcherFile.c_str()); launcherStream.good()) // Launcher file found
		{
			std::string line;
			if (std::getline(launcherStream, line)) // Launcher found
			{
				launcherName = line;
			}
		}
		launcherName = Utils::toLower(launcherName);
		std::string emulator = launcherName;
		config_.getProperty("launchers." + launcherName + ".LEDBlinkyEmulator", emulator);
		args = args + " \"" + collectionItem->name + "\" \"" + emulator + "\"";
		if (emulator == "")
			return;
	}
	if (LEDBlinkyDirectory != "" && !simpleExecute(exe, args, LEDBlinkyDirectory, wait)) {
		LOG_WARNING("LEDBlinky", "Failed to launch.");
	}
	return;
}

bool Launcher::simpleExecute(std::string executable, std::string args, std::string currentDirectory, bool wait, Page* currentPage)
{
	bool retVal = false;
	std::string executionString = "\"" + executable + "\" " + args;

	LOG_INFO("Launcher", "Attempting to launch: " + executionString);
	LOG_INFO("Launcher", "     from within folder: " + currentDirectory);

#ifdef WIN32
	STARTUPINFO startupInfo;
	PROCESS_INFORMATION processInfo;
	char applicationName[2048];
	char currDir[2048];
	memset(&applicationName, 0, sizeof(applicationName));
	memset(&startupInfo, 0, sizeof(startupInfo));
	memset(&processInfo, 0, sizeof(processInfo));
	strncpy(applicationName, executionString.c_str(), sizeof(applicationName));
	strncpy(currDir, currentDirectory.c_str(), sizeof(currDir));
	startupInfo.dwFlags = STARTF_USESTDHANDLES;
	startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startupInfo.wShowWindow = SW_SHOWDEFAULT;

	if (!CreateProcess(nullptr, applicationName, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, currDir, &startupInfo, &processInfo))
#else
	const std::size_t last_slash_idx = executable.rfind(Utils::pathSeparator);
	if (last_slash_idx != std::string::npos) {
		std::string applicationName = executable.substr(last_slash_idx + 1);
		executionString = "cd \"" + currentDirectory + "\" && exec \"./" + applicationName + "\" " + args;
	}
	if (system(executionString.c_str()) != 0)
#endif
	{
		LOG_WARNING("Launcher", "Failed to run: " + executable);
	}

	else
	{
#ifdef WIN32
		if (wait) {
			while (WAIT_OBJECT_0 != MsgWaitForMultipleObjects(1, &processInfo.hProcess, FALSE, INFINITE, QS_ALLINPUT)) {
				MSG msg;
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
					DispatchMessage(&msg);
				}
			}
		}

		// result = GetExitCodeProcess(processInfo.hProcess, &exitCode);
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);
#endif
		retVal = true;
	}

	LOG_INFO("Launcher", "Completed");

	return retVal;
}

bool Launcher::execute(std::string executable, std::string args, std::string currentDirectory, bool wait, Page* currentPage, bool isAttractMode, Item* collectionItem) {
	bool retVal = false;
	std::string executionString = "\"" + executable + "\" " + args;

	LOG_INFO("Launcher", "Attempting to launch: " + executionString);
	LOG_INFO("Launcher", "     from within folder: " + currentDirectory);

	std::atomic<bool> stop_thread = true;
	std::thread proc_thread;
	bool multiple_display = SDL_GetNumVideoDisplays() > 1;
	bool animateDuringGame = true;
	config_.getProperty(OPTION_ANIMATEDURINGGAME, animateDuringGame);

	if (animateDuringGame && multiple_display && currentPage != nullptr) {
		stop_thread = false;
		proc_thread = std::thread([this, &stop_thread, currentPage]() {
			this->keepRendering(std::ref(stop_thread), *currentPage);
			});
	}

	// Variables to measure gameplay time
	std::chrono::time_point<std::chrono::steady_clock> startTime;
	std::chrono::time_point<std::chrono::steady_clock> endTime;

	// Start timing if not in attract mode
	if (!isAttractMode) {
		startTime = std::chrono::steady_clock::now();
	}

#ifdef WIN32

	// Ensure executable and currentDirectory are absolute paths
	std::filesystem::current_path(Configuration::absolutePath);
	std::filesystem::path exePath(executable);
	if (!exePath.is_absolute()) {
		exePath = std::filesystem::absolute(exePath);
	}

	std::filesystem::path currDir(currentDirectory);
	if (!currDir.is_absolute()) {
		currDir = std::filesystem::absolute(currDir);
	}

	std::string exePathStr = exePath.string();
	std::string currDirStr = currDir.string();

	// Log final paths after any necessary conversions to absolute paths
	LOG_INFO("Launcher", "Final absolute executable path: " + exePathStr);
	LOG_INFO("Launcher", "Final absolute current directory: " + currDirStr);

	// Lambda to check if a window is in fullscreen mode
	auto isFullscreenWindow = [](HWND hwnd) {
		RECT appBounds;
		if (!GetWindowRect(hwnd, &appBounds)) return false;

		HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		if (!hMonitor) return false;

		MONITORINFO monitorInfo{};
		monitorInfo.cbSize = sizeof(MONITORINFO);
		if (!GetMonitorInfo(hMonitor, &monitorInfo)) return false;

		return (appBounds.bottom - appBounds.top) == (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top) &&
			(appBounds.right - appBounds.left) == (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left);
		};

	HANDLE hLaunchedProcess = nullptr;
	DWORD launchedProcessId = 0;
	bool handleObtained = false;

	// Lower priority before launching the process
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

	// Check if launching an executable
	if (exePath.extension() == ".exe") {
		STARTUPINFOA startupInfo{};
		PROCESS_INFORMATION processInfo{};
		startupInfo.cb = sizeof(startupInfo);
		startupInfo.dwFlags = STARTF_USESHOWWINDOW;
		startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startupInfo.wShowWindow = SW_SHOWDEFAULT;

		// Construct command line
		std::string commandLine = "\"" + exePathStr + "\"";
		if (!args.empty()) {
			commandLine += " " + args;
		}

		LOG_INFO("Launcher", "Command line to be executed: " + commandLine);

		if (CreateProcessA(
			nullptr,
			&commandLine[0],          // Mutable command line
			nullptr,                  // Process security attributes
			nullptr,                  // Thread security attributes
			FALSE,                    // Inherit handles
			CREATE_NO_WINDOW,         // Creation flags
			nullptr,                  // Use parent's environment block
			currDirStr.c_str(),       // Current directory
			&startupInfo,             // Startup information
			&processInfo)) {          // Process information
			hLaunchedProcess = processInfo.hProcess;
			launchedProcessId = processInfo.dwProcessId;
			handleObtained = true;
			CloseHandle(processInfo.hThread);
			LOG_INFO("Launcher", "Process launched successfully with handle obtained.");
		}
		else {
			LOG_ERROR("Launcher", "Failed to launch executable: " + exePathStr + " with error code: " + std::to_string(GetLastError()));
		}
	}
	// Use ShellExecuteEx for non-executable files
	else {
		SHELLEXECUTEINFOA shExInfo = { 0 };
		shExInfo.cbSize = sizeof(SHELLEXECUTEINFOA);
		shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
		shExInfo.hwnd = nullptr;
		shExInfo.lpVerb = "open";
		shExInfo.lpFile = exePathStr.c_str();
		shExInfo.lpParameters = args.empty() ? nullptr : args.c_str();
		shExInfo.lpDirectory = currDirStr.c_str();
		shExInfo.nShow = SW_SHOWNORMAL;
		shExInfo.hInstApp = nullptr;

		if (ShellExecuteExA(&shExInfo)) {
			hLaunchedProcess = shExInfo.hProcess;
			if (hLaunchedProcess) {
				launchedProcessId = GetProcessId(hLaunchedProcess);
				handleObtained = true;
				LOG_INFO("Launcher", "ShellExecuteEx successfully launched: " + exePathStr);
			}
		}
		else {
			LOG_WARNING("Launcher", "ShellExecuteEx failed to launch: " + executable + " with error code: " + std::to_string(GetLastError()));
		}
	}

	// Fullscreen detection if process handle was not obtained
	if (!handleObtained) {
		auto start = std::chrono::high_resolution_clock::now();
		HWND hwndFullscreen = nullptr;

		LOG_INFO("Launcher", "Entering fullscreen detection phase.");

		while (true) {
			HWND hwnd = GetForegroundWindow();
			if (hwnd != nullptr) {
				DWORD windowProcessId;
				GetWindowThreadProcessId(hwnd, &windowProcessId);
				if (windowProcessId != GetCurrentProcessId() && isFullscreenWindow(hwnd)) {
					hwndFullscreen = hwnd;
					break;
				}
			}

			auto now = std::chrono::high_resolution_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
			if (elapsed > 40000) {
				LOG_WARNING("Launcher", "Timeout while waiting for fullscreen window detection.");
				break;
			}
			Sleep(500);
		}

		if (hwndFullscreen) {
			DWORD gameProcessId;
			GetWindowThreadProcessId(hwndFullscreen, &gameProcessId);
			hLaunchedProcess = OpenProcess(SYNCHRONIZE, FALSE, gameProcessId);
			if (hLaunchedProcess) {
				launchedProcessId = gameProcessId;
				handleObtained = true;
				LOG_INFO("Launcher", "Fullscreen process detected and handle obtained.");
			}
		}
	}

	// Monitoring the process
	if (handleObtained) {
		bool is4waySet = false;
		bool isServoStikEnabled = false;
		config_.getProperty(OPTION_SERVOSTIKENABLED, isServoStikEnabled);
		if (!isAttractMode && currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos && isServoStikEnabled) {
			if (!PacSetServoStik4Way()) {
				LOG_ERROR("RetroFE", "Failed to set ServoStik to 4-way mode");
			}
			else {
				LOG_INFO("RetroFE", "Setting ServoStik to 4-way mode");
				is4waySet = true;
			}
		}
		if (isAttractMode) {
			int attractModeLaunchRunTime = 30;
			config_.getProperty(OPTION_ATTRACTMODELAUNCHRUNTIME, attractModeLaunchRunTime);
			auto start = std::chrono::high_resolution_clock::now();
			bool userInputDetected = false;
			bool processTerminated = false;

			// For tracking first input
			bool firstInputWasExitCommand = false;
			bool anyInputRegistered = false;

			// Variables for simultaneous button detection
			bool startButtonDown = false;
			bool backButtonDown = false;
			auto startPressTime = std::chrono::high_resolution_clock::now();
			auto backPressTime = std::chrono::high_resolution_clock::now();
			const int simultaneousThresholdMs = 200; // Consider buttons pressed simultaneously if within 200ms

			// Store process ID for window detection
			HWND gameWindow = nullptr;

			// Give the process a moment to create its window
			Sleep(500);

			// Function to find the game window based on process ID
			auto findGameWindow = [launchedProcessId]() -> HWND {
				struct EnumData {
					DWORD processId;
					HWND result;
				};

				EnumData data = { launchedProcessId, nullptr };

				EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
					EnumData* data = reinterpret_cast<EnumData*>(lParam);
					DWORD windowProcessId = 0;
					GetWindowThreadProcessId(hwnd, &windowProcessId);

					if (windowProcessId == data->processId && IsWindowVisible(hwnd)) {
						data->result = hwnd;
						return FALSE; // Stop enumeration, we found it
					}
					return TRUE; // Continue enumeration
					}, reinterpret_cast<LPARAM>(&data));

				return data.result;
				};

			// First attempt to find the game window
			gameWindow = findGameWindow();
			if (gameWindow) {
				LOG_DEBUG("Launcher", "Found game window handle: 0x" + std::to_string(reinterpret_cast<uintptr_t>(gameWindow)));
			}

			// Combined input detection function that checks for all input types and special cases
			auto checkInputs = [&]() -> bool {
				bool inputDetected = false;
				auto currentTime = std::chrono::high_resolution_clock::now();

				// Check ESC key first (immediate exit command)
				if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
					// If this is the first input, mark it as an exit command
					if (!anyInputRegistered) {
						firstInputWasExitCommand = true;
						LOG_INFO("Launcher", "ESC key pressed - game will not be added to last played");
					}
					else {
						LOG_DEBUG("Launcher", "ESC key pressed");
					}

					anyInputRegistered = true;
					inputDetected = true;
				}

				// Check controller for START+BACK combo
				XINPUT_STATE state;
				ZeroMemory(&state, sizeof(XINPUT_STATE));
				if (XInputGetState(0, &state) == ERROR_SUCCESS) {
					// Check START button state change
					bool startCurrentlyDown = (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
					if (startCurrentlyDown != startButtonDown) {
						startButtonDown = startCurrentlyDown;
						if (startButtonDown) {
							startPressTime = currentTime;
							LOG_DEBUG("Launcher", "START button pressed");
						}
					}

					// Check BACK button state change
					bool backCurrentlyDown = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
					if (backCurrentlyDown != backButtonDown) {
						backButtonDown = backCurrentlyDown;
						if (backButtonDown) {
							backPressTime = currentTime;
							LOG_DEBUG("Launcher", "BACK button pressed");
						}
					}

					// If both buttons are pressed, check if they were pressed simultaneously
					if (startButtonDown && backButtonDown) {
						auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
							std::max(startPressTime, backPressTime) - std::min(startPressTime, backPressTime)
						).count();

						if (timeDiff <= simultaneousThresholdMs) {
							// If this is the first input, flag it
							if (!anyInputRegistered) {
								firstInputWasExitCommand = true;
								LOG_INFO("Launcher", "START+BACK combo detected (within " +
									std::to_string(timeDiff) + "ms) - game will not be added to last played");
							}
							else {
								LOG_DEBUG("Launcher", "START+BACK combo detected (within " + std::to_string(timeDiff) + "ms)");
							}

							inputDetected = true;
						}
					}

					// Check any other controller buttons (individual START or BACK are included here)
					if (state.Gamepad.wButtons != 0 && !inputDetected) {
						if (!anyInputRegistered) {
							LOG_INFO("Launcher", "Controller input - game will be added to last played");
						}
						else {
							LOG_DEBUG("Launcher", "Controller input detected");
						}
						anyInputRegistered = true;
						inputDetected = true;
					}
				}

				// Check other keyboard keys if no input detected yet
				if (!inputDetected) {
					for (int virtualKey : {
						// Letters A-Z
						0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
							0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54,
							0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, // A-Z
							// Numbers 0-9
							0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, // 0-9
							// Arrow keys
							VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
							// Other common keys (excluding ESC which was already checked)
							VK_RETURN, VK_SPACE, VK_TAB, VK_BACK,
							// Modifier keys
							VK_SHIFT, VK_CONTROL, VK_MENU // Shift, Ctrl, Alt
					}) {
						if (GetAsyncKeyState(virtualKey) & 0x8000) {
							if (!anyInputRegistered) {
								LOG_INFO("Launcher", "Keyboard input - game will be added to last played");
							}
							else {
								LOG_DEBUG("Launcher", "Keyboard input detected");
							}
							anyInputRegistered = true;
							inputDetected = true;
							break;
						}
					}
				}

				return inputDetected;
				};

			// Window check counter to reduce frequency of checks
			int windowCheckCounter = 0;

			while (true) {
				// Process window messages
				MSG msg;
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
					DispatchMessage(&msg);
				}

				// Check for any input (keyboard, controller, or special combos)
				if (checkInputs()) {
					LOG_INFO("Launcher", "User interrupted attract mode - waiting for game to exit");
					userInputDetected = true;
					if (!firstInputWasExitCommand) {  // Normal input (gameplay intended)
						if (currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos && isServoStikEnabled) {
							if (!PacSetServoStik4Way()) {
								LOG_ERROR("RetroFE", "Failed to set ServoStik to 4-way mode after normal input");
							}
							else {
								LOG_INFO("RetroFE", "Setting ServoStik to 4-way mode after normal input");
								is4waySet = true;
							}
						}
					}
					break;
				}

				// Check if the window still exists (do this every 5 loops to reduce overhead)
				if (++windowCheckCounter % 5 == 0) {
					if (gameWindow) {
						if (!IsWindow(gameWindow)) {
							LOG_INFO("Launcher", "Game window closed - process is terminating");
							processTerminated = true;
							break;
						}
					}
					else {
						// Try to find the window if we don't have it yet
						gameWindow = findGameWindow();
						if (gameWindow) {
							LOG_DEBUG("Launcher", "Found game window handle: 0x" +
								std::to_string(reinterpret_cast<uintptr_t>(gameWindow)));
						}
					}
				}

				// Check if the launched process has exited (use this as a backup method)
				DWORD exitCode;
				if (!GetExitCodeProcess(hLaunchedProcess, &exitCode)) {
					DWORD error = GetLastError();
					LOG_WARNING("Launcher", "Failed to get process exit code, error: " + std::to_string(error));
					processTerminated = true;
					break;
				}

				if (exitCode != STILL_ACTIVE) {
					LOG_INFO("Launcher", "Process terminated with exit code: " + std::to_string(exitCode));
					processTerminated = true;
					break;
				}

				// Check if the timeout has been reached
				auto now = std::chrono::high_resolution_clock::now();
				auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
				if (elapsed >= attractModeLaunchRunTime) {
					LOG_INFO("Launcher", "Attract mode timeout reached - terminating game");
					break;
				}

				// Sleep to prevent high CPU usage
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}

			if (userInputDetected) {
				bool shouldAddToLastPlayed = !firstInputWasExitCommand;

				// Wait indefinitely for the process to exit
				while (WAIT_OBJECT_0 != MsgWaitForMultipleObjects(1, &hLaunchedProcess, FALSE, INFINITE, QS_ALLINPUT)) {
					MSG msg;
					while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
						DispatchMessage(&msg);
					}

					// Also check window existence as a backup during this wait
					if (gameWindow && !IsWindow(gameWindow)) {
						LOG_DEBUG("Launcher", "Game window closed while waiting in user input mode");
						break; // Exit the wait loop since the window is gone
					}
				}

				// Only add to last played if first input wasn't an exit command
				if (shouldAddToLastPlayed) {
					LOG_INFO("Launcher", "Adding game to last played");

					// Add to last played
					CollectionInfoBuilder cib(config_, *retroFeInstance_.getMetaDb());
					std::string lastPlayedSkipCollection = "";
					int size = 0;
					config_.getProperty(OPTION_LASTPLAYEDSKIPCOLLECTION, lastPlayedSkipCollection);
					config_.getProperty(OPTION_LASTPLAYEDSIZE, size);

					if (lastPlayedSkipCollection != "") {
						// Check if collection should be skipped
						std::stringstream ss(lastPlayedSkipCollection);
						std::string collection = "";
						bool updateLastPlayed = true;
						while (ss.good()) {
							getline(ss, collection, ',');
							if (collectionItem->collectionInfo->name == collection) {
								updateLastPlayed = false;
								break;
							}
						}

						// Update last played collection if needed
						if (updateLastPlayed) {
							cib.updateLastPlayedPlaylist(currentPage->getCollection(), collectionItem, size);
						}
					}
				}
			}
			else if (!processTerminated) {
				// Timer elapsed case (process still running)
				if (launchedProcessId != 0) {
					TerminateProcessAndChildren(launchedProcessId);
				}
			}
		}
		else if (wait) {
			LOG_INFO("Launcher", "Waiting for launched process to complete.");
			while (WAIT_OBJECT_0 != MsgWaitForMultipleObjects(1, &hLaunchedProcess, FALSE, INFINITE, QS_ALLINPUT)) {
				MSG msg;
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
					DispatchMessage(&msg);
				}
			}
			LOG_INFO("Launcher", "Process completed.");
		}

		// Restore ServoStik if needed
		if (is4waySet) {
			if (!PacSetServoStik8Way()) { // return to 8-way if 4-way switch occurred
				LOG_ERROR("RetroFE", "Failed to return ServoStik to 8-way mode");
			}
			else {
				LOG_INFO("RetroFE", "Returned ServoStik to 8-way mode");
			}
		}

		// Always clean up handles
		if (hLaunchedProcess != nullptr) {
			CloseHandle(hLaunchedProcess);
			hLaunchedProcess = nullptr;
		}
		retVal = true;
	}
	else {
		LOG_WARNING("Launcher", "No handle was obtained; process monitoring will not occur.");
	}

	bool highPriority = false;
	config_.getProperty("OPTION_HIGHPRIORITY", highPriority);
	SetPriorityClass(GetCurrentProcess(), highPriority ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);

#else
	// Unix/Linux-specific execution logic

	bool is4waySet = false;
	bool isServoStikEnabled = false;

	config_.getProperty(OPTION_SERVOSTIKENABLED, isServoStikEnabled);

	pid_t pid = fork();

	if (pid == -1) {
		// Fork failed
		LOG_ERROR("Launcher", "Failed to fork a new process.");
		return false;
	}
	else if (pid == 0) {
		// Child process: Change directory and execute command
		if (!currentDirectory.empty() && chdir(currentDirectory.c_str()) != 0) {
			LOG_ERROR("Launcher", "Failed to change directory to: " + currentDirectory);
			exit(EXIT_FAILURE);
		}

		std::vector<std::string> argVector;
		std::istringstream argsStream(args);
		std::string arg;
		while (argsStream >> std::quoted(arg)) {
			argVector.push_back(arg);
		}

		std::vector<char*> execArgs;
		execArgs.push_back(const_cast<char*>(executable.c_str()));
		for (auto& arg : argVector) {
			execArgs.push_back(const_cast<char*>(arg.c_str()));
		}
		execArgs.push_back(nullptr);

		execvp(executable.c_str(), execArgs.data());
		LOG_ERROR("Launcher", "Failed to execute: " + executable + " with arguments: " + args);
		exit(EXIT_FAILURE);
	}
	else {
		std::thread servoThread;
		int status;
		int attractModeLaunchRunTime = 30;

		config_.getProperty(OPTION_ATTRACTMODELAUNCHRUNTIME, attractModeLaunchRunTime);
		// Non-attract mode: Perform ServoStik check immediately after successful launch
		if (!isAttractMode) {
			LOG_INFO("Launcher", "Waiting for launched item to exit.");

			if (isServoStikEnabled) {
				// Spawn ServoStik thread only if needed
				servoThread = std::thread([&]() {
					// Give the process a moment to start
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

					// Use kill with signal 0 to check if process is running
					if (kill(pid, 0) == 0) {  // Process is running
						if (currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos) {
							if (!SetServoStik4Way()) {
								LOG_ERROR("RetroFE", "Failed to set ServoStik to 4-way mode");
							}
							else {
								LOG_INFO("RetroFE", "Setting ServoStik to 4-way mode");
								is4waySet = true;
							}
						}
					}
					});
			}

			// Main process waits here
			waitpid(pid, &status, 0);  // This will block until the child exits
		}
		else {
			// Attract mode logic
			auto startTime = std::chrono::steady_clock::now();
			std::vector<std::string> inputDevices = getInputDevices();
			bool timerStopped = false; // Flag to indicate if the timer has been stopped due to user input

			while (true) {
				// Check if the child process has exited
				int waitStatus;
				if (waitpid(pid, &waitStatus, WNOHANG) > 0) {
					LOG_INFO("Launcher", "Launched process has terminated.");
					break; // Exit the loop if the process has exited
				}

				// Check for user input
				if (!timerStopped && detectInput(inputDevices)) {
					LOG_INFO("Launcher", "User input detected. Stopping attract mode timer.");

					// Perform ServoStik check if necessary
					if (currentPage->getSelectedItem()->ctrlType.find("4") != std::string::npos && isServoStikEnabled) {
						if (!SetServoStik4Way()) {
							LOG_ERROR("RetroFE", "Failed to set ServoStik to 4-way mode");
						}
						else {
							LOG_INFO("RetroFE", "Setting ServoStik to 4-way mode");
							is4waySet = true;
						}
					}



					timerStopped = true; // Stop the timer after input is detected
					// Add to last played if user interrupted during attract mode
					CollectionInfoBuilder cib(config_, *retroFeInstance_.getMetaDb());
					std::string lastPlayedSkipCollection = "";
					int size = 0;
					config_.getProperty(OPTION_LASTPLAYEDSKIPCOLLECTION, lastPlayedSkipCollection);
					config_.getProperty(OPTION_LASTPLAYEDSIZE, size);

					if (lastPlayedSkipCollection != "")
					{
						// see if any of the comma seperated match current collection
						std::stringstream ss(lastPlayedSkipCollection);
						std::string collection = "";
						bool updateLastPlayed = true;
						while (ss.good())
						{
							getline(ss, collection, ',');
							// Check if the current collection matches any collection in lastPlayedSkipCollection
							if (collectionItem->collectionInfo->name == collection)
							{
								updateLastPlayed = false;
								break; // No need to check further, as we found a match
							}
						}
						// Update last played collection if not found in the skip collection
						if (updateLastPlayed)
						{
							cib.updateLastPlayedPlaylist(currentPage->getCollection(), collectionItem, size);
							//currentPage_->updateReloadables(0);
						}
					}
				}

				// Check if the timer has elapsed (only if the timer is not stopped)
				if (!timerStopped) {
					auto now = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count() >= attractModeLaunchRunTime) {
						LOG_INFO("Launcher", "Attract Mode timeout reached, terminating game.");
						kill(pid, SIGKILL); // Terminate the launched item
						waitpid(pid, &waitStatus, 0); // Wait for the process to terminate
						break; // Exit the loop after timeout
					}
				}

				// Sleep briefly to prevent high CPU usage
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		// Ensure ServoStik thread completes if it was spawned
		if (servoThread.joinable()) {
			servoThread.join();
		}

		// Restore ServoStik to 8-way mode if it was changed
		if (is4waySet) {
			if (!SetServoStik8Way()) {
				LOG_ERROR("RetroFE", "Failed to return ServoStik to 8-way mode");
			}
			else {
				LOG_INFO("RetroFE", "Returned ServoStik to 8-way mode");
			}
		}

		if (WIFEXITED(status)) {
			retVal = WEXITSTATUS(status) == 0;
			LOG_INFO("Launcher", "Executable " + std::string(retVal ? "ran successfully." : "failed with status: " + std::to_string(WEXITSTATUS(status))));
		}
		else if (WIFSIGNALED(status)) {
			int signal = WTERMSIG(status);
			LOG_INFO("Launcher", "Child process was terminated by signal: " + std::to_string(signal) + ". Treated as normal termination.");
			retVal = true; // Treat killed processes as successfully terminated
		}
		else {
			LOG_WARNING("Launcher", "Child process did not terminate normally.");
			retVal = false;
		}

	}
#endif

	// Cleanup for animation thread if started
	if (multiple_display && !stop_thread) {
		stop_thread = true;
		proc_thread.join();
	}

	if (!isAttractMode) {
		endTime = std::chrono::steady_clock::now();
		double gameplayDuration = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count());
		LOG_INFO("Launcher", "Gameplay time recorded: " + std::to_string(gameplayDuration) + " seconds.");

		// Update timeSpent and save it
		if (collectionItem != nullptr) {
			CollectionInfoBuilder cib(config_, *retroFeInstance_.getMetaDb());
			cib.updateTimeSpent(collectionItem, gameplayDuration);
		}
	}
	if (executable.find("mame") != std::string::npos) {
		HiScores::getInstance().runHi2TxtAsync(collectionItem->name);
	}

	LOG_INFO("Launcher", "Completed execution for: " + executionString);
	return retVal;
}

void Launcher::keepRendering(std::atomic<bool>& stop_thread, Page& currentPage) const
{
	float lastTime = 0;
	float currentTime = 0;
	float deltaTime = 0;
	double sleepTime;
	double fpsTime = 1000.0 / static_cast<double>(60);

	while (!stop_thread) {
		lastTime = currentTime;
		currentTime = static_cast<float>(SDL_GetTicks64()) / 1000;

		if (currentTime < lastTime) {
			currentTime = lastTime;
		}

		deltaTime = currentTime - lastTime;
		sleepTime = fpsTime - deltaTime * 1000;

		if (sleepTime > 0 && sleepTime < 1000) {
			SDL_Delay(static_cast<unsigned int>(sleepTime));
		}
		currentPage.update(float(0));
		SDL_LockMutex(SDL::getMutex());

		// start on secondary monitor
		// todo support future main screen swap
		for (int i = 1; i < SDL::getScreenCount(); ++i) {
			SDL_SetRenderTarget(SDL::getRenderer(i), SDL::getRenderTarget(i));
			SDL_SetRenderDrawColor(SDL::getRenderer(i), 0x0, 0x0, 0x0, 0xFF);
			SDL_RenderClear(SDL::getRenderer(i));
		

		currentPage.draw(i);

			SDL_SetRenderTarget(SDL::getRenderer(i), nullptr);

			// Render the texture onto the screen
			SDL_RenderCopy(SDL::getRenderer(i), SDL::getRenderTarget(i), nullptr, nullptr);

			// Present the final result to the screen
			SDL_RenderPresent(SDL::getRenderer(i));
		}

		SDL_UnlockMutex(SDL::getMutex());
	}
}

bool Launcher::launcherName(std::string& launcherName, std::string collection)
{
	// find the launcher for the particular item 
	if (std::string launcherKey = "collections." + collection + ".launcher"; !config_.getProperty(launcherKey, launcherName)) {
		std::stringstream ss;

		ss << "Launch failed. Could not find a configured launcher for collection \""
			<< collection
			<< "\" (could not find a property for \""
			<< launcherKey
			<< "\")";

		LOG_ERROR("Launcher", ss.str());

		return false;
	}

	std::stringstream ss;
	ss << "collections."
		<< collection
		<< " is configured to use launchers."
		<< launcherName
		<< "\"";

	LOG_DEBUG("Launcher", ss.str());

	return true;
}

bool Launcher::launcherExecutable(std::string& executable, std::string launcherName) {
	// Try with the localLauncher prefix
	std::string executableKey = "localLaunchers." + launcherName + ".executable";
	if (!config_.getProperty(executableKey, executable)) {
		// Try with the collectionLauncher prefix
		executableKey = "collectionLaunchers." + launcherName + ".executable";
		if (!config_.getProperty(executableKey, executable)) {
			// Finally, try with the global launcher prefix
			executableKey = "launchers." + launcherName + ".executable";
			if (!config_.getProperty(executableKey, executable)) {
				LOG_ERROR("Launcher", "No launcher found for: " + executableKey);
				return false;
			}
		}
	}
	return true;
}

bool Launcher::launcherArgs(std::string& args, std::string launcherName) {
	// Try with the localLauncher prefix
	std::string argsKey = "localLaunchers." + launcherName + ".arguments";
	if (!config_.getProperty(argsKey, args)) {
		// Try with the collectionLauncher prefix
		argsKey = "collectionLaunchers." + launcherName + ".arguments";
		if (!config_.getProperty(argsKey, args)) {
			// Finally, try with the global launcher prefix
			argsKey = "launchers." + launcherName + ".arguments";
			if (!config_.getProperty(argsKey, args)) {
				LOG_WARNING("Launcher", "No arguments specified for: " + argsKey);
				args.clear(); // Ensure args is empty if not found
			}
		}
	}
	return true;
}

bool Launcher::extensions(std::string& extensions, std::string collection)
{
	if (std::string extensionsKey = "collections." + collection + ".list.extensions"; !config_.getProperty(extensionsKey, extensions)) {
		LOG_ERROR("Launcher", "No extensions specified for: " + extensionsKey);
		return false;
	}

	extensions = Utils::replace(extensions, " ", "");
	extensions = Utils::replace(extensions, ".", "");

	return true;
}

bool Launcher::collectionDirectory(std::string& directory, std::string collection)
{
	std::string itemsPathValue;
	std::string mergedCollectionName;

	// find the items path folder (i.e. ROM path)
	config_.getCollectionAbsolutePath(collection, itemsPathValue);
	directory += itemsPathValue + Utils::pathSeparator;

	return true;
}

bool Launcher::findFile(std::string& foundFilePath, std::string& foundFilename, const std::string& directory, const std::string& filenameWithoutExtension, const std::string& extensions) {
	bool fileFound = false;
	std::stringstream ss(extensions);
	std::string extension;

	while (std::getline(ss, extension, ',')) {
		fs::path filePath = fs::path(directory) / (filenameWithoutExtension + "." + extension);

		if (fs::exists(filePath)) {
			foundFilePath = fs::absolute(filePath).string();
			foundFilename = extension;
			fileFound = true;
			LOG_INFO("Launcher", "File found: " + foundFilePath + " with extension: ." + extension);
			break; // Exit the loop once the file is found
		}
	}

	if (!fileFound) {
		LOG_ERROR("Launcher", "No matching files found for \"" + filenameWithoutExtension + "\" in directory \"" + directory + "\" with extensions: " + extensions);
	}

	return fileFound;
}


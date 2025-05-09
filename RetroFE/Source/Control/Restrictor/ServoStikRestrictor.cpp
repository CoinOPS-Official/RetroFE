#include "ServoStikRestrictor.h"
#include "../../Utility/Log.h"
#ifdef WIN32
#include "PacDrive.h"
#else
#include <thread>
#include <chrono>
#include <libusb.h>
#endif

static constexpr char COMPONENT[] = "ServoStik";

#ifdef _WIN32

ServoStikRestrictor::ServoStikRestrictor(uint16_t vid, uint16_t pid)
    : vid_(vid), pid_(pid), initialized_(false) {}

ServoStikRestrictor::~ServoStikRestrictor() {}

bool ServoStikRestrictor::initialize() {
    LOG_INFO(COMPONENT, "Attempting to initialize ServoStik restrictor...");
    initialized_ = (PacInitialize() != 0);
    if (initialized_) {
        LOG_INFO(COMPONENT, "ServoStik restrictor detected and initialized.");
    }
    else {
		LOG_INFO(COMPONENT, "No ServoStik device found.");
	}
    return initialized_;
}

bool ServoStikRestrictor::setWay(int way) {
    if (!initialized_ || (way != 4 && way != 8)) return false;
    return (way == 4) ? PacSetServoStik4Way() : PacSetServoStik8Way();
}

std::optional<int> ServoStikRestrictor::getWay() {
    return std::nullopt;
}

bool ServoStikRestrictor::isPresent() {
    return PacInitialize() != 0;
}

#else

ServoStikRestrictor::ServoStikRestrictor(uint16_t vid, uint16_t pid)
    : vid_(vid), pid_(pid), ctx_(nullptr), handle_(nullptr) {}

ServoStikRestrictor::~ServoStikRestrictor() {
    if (handle_) {
        libusb_release_interface(handle_, 0);
        libusb_close(handle_);
    }
    if (ctx_) libusb_exit(ctx_);
}

bool ServoStikRestrictor::initialize() {
    LOG_INFO(COMPONENT, "Attempting to initialize ServoStik restrictor...");

    if (libusb_init(&ctx_) != 0) {
        LOG_ERROR(COMPONENT, "libusb_init failed.");
        return false;
    }

    handle_ = libusb_open_device_with_vid_pid(ctx_, vid_, pid_);
    if (!handle_) {
        LOG_INFO(COMPONENT, "No ServoStik device found.");
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    if (libusb_kernel_driver_active(handle_, 0) == 1) {
        int detachResult = libusb_detach_kernel_driver(handle_, 0);
        if (detachResult < 0) {
            LOG_ERROR(COMPONENT, "Failed to detach kernel driver: " + std::string(libusb_error_name(detachResult)));
            libusb_close(handle_);
            libusb_exit(ctx_);
            handle_ = nullptr;
            ctx_ = nullptr;
            return false;
        }
    }

    int claimResult = libusb_claim_interface(handle_, 0);
    if (claimResult < 0) {
        LOG_ERROR(COMPONENT, "libusb_claim_interface failed: " + std::string(libusb_error_name(claimResult)));
        libusb_close(handle_);
        libusb_exit(ctx_);
        handle_ = nullptr;
        ctx_ = nullptr;
        return false;
    }

    LOG_INFO(COMPONENT, "ServoStik restrictor detected and initialized.");
    return true;
}

bool ServoStikRestrictor::setWay(int way) {
    if (!handle_ || (way != 4 && way != 8)) {
        LOG_WARNING(COMPONENT, "Invalid handle or mode in setWay(" + std::to_string(way) + ")");
        return false;
    }

    unsigned char msg[4] = { 0x00, 0xDD, 0x00, static_cast<unsigned char>(way == 4 ? 0x00 : 0x01) };
    LOG_INFO(COMPONENT, "Sending command: {0x00, 0xDD, 0x00, " + std::to_string(msg[3]) + "}");

    for (int i = 0; i < 2; ++i) {
        int ret = libusb_control_transfer(handle_, 0x21, 9, 0x0200, 0, msg, sizeof(msg), 2000);
        if (ret < 0) {
            LOG_ERROR(COMPONENT, "libusb_control_transfer failed on attempt " + std::to_string(i + 1) + ": " + std::string(libusb_error_name(ret)));
        }
        else {
            LOG_INFO(COMPONENT, "Control transfer successful on attempt " + std::to_string(i + 1));
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return false;
}

std::optional<int> ServoStikRestrictor::getWay() {
    return std::nullopt;
}

bool ServoStikRestrictor::isPresent() {
    libusb_context* ctx = nullptr;
    bool result = false;
    if (libusb_init(&ctx) != 0) return false;

    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, 0xD209, 0x1700);
    if (h) {
        result = true;
        libusb_close(h);
    }

    libusb_exit(ctx);
    return result;
}

#endif

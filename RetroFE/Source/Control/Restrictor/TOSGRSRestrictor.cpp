#include "TOSGRSRestrictor.h"
#include "libserialport.h"
#include "../../Utility/Log.h"
#include <sstream>
#include <iomanip>
#include <memory>

static constexpr char COMPONENT[] = "TOSGRS";

TOSGRSRestrictor::TOSGRSRestrictor(uint16_t vid, uint16_t pid)
  : vid_(vid), pid_(pid), port_(nullptr) {}

TOSGRSRestrictor::~TOSGRSRestrictor() {
    if (port_) {
        sp_close(port_);
        sp_free_port(port_);
    }
}

bool TOSGRSRestrictor::initialize() {
    LOG_INFO(COMPONENT, "Attempting to initialize TOS GRS restrictor...");

    port_ = findPort(vid_, pid_);
    if (!port_) {
        LOG_INFO(COMPONENT, "No GRS device found");
        return false;
    }

    if (sp_open(port_, SP_MODE_READ_WRITE) != SP_OK) {
        return false;
    }

    if (sp_set_baudrate(port_, 115200) != SP_OK ||
        sp_set_bits(port_, 8) != SP_OK ||
        sp_set_parity(port_, SP_PARITY_NONE) != SP_OK ||
        sp_set_stopbits(port_, 1) != SP_OK ||
        sp_set_flowcontrol(port_, SP_FLOWCONTROL_NONE) != SP_OK) {
        sp_close(port_);
        return false;
    }

    LOG_INFO(COMPONENT, "TOS GRS restrictor detected and initialized.");
    return true;
}

bool TOSGRSRestrictor::setWay(int way) {
    if (!port_ || (way != 4 && way != 8)) return false;
    auto current = getWay();
    if (current && *current == way) return true;
    return sendCmd("setway,all," + std::to_string(way)) != "err";
}


std::optional<int> TOSGRSRestrictor::getWay() {
    if (!port_) return std::nullopt;
    auto r = sendCmd("getway,1");
    if (r.size() && (r[0] == '4' || r[0] == '8')) return r[0] - '0';
    return std::nullopt;
}

std::string TOSGRSRestrictor::sendCmd(const std::string& cmd) {
    sp_flush(port_, SP_BUF_INPUT); // Clear any stale input

    // Ensure command ends with '\r'
    std::string command = cmd;
    if (command.empty() || command.back() != '\r') {
        command += '\r';
    }

    // Write command
    if (sp_blocking_write(port_, command.c_str(), command.size(), 100) != (int)command.size()) {
        LOG_ERROR(COMPONENT, "Failed to write command to device.");
        return "err";
    }

    // Increased wait time to 250ms (was 100ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // Try to read (poll for up to 1000ms - was 500ms)
    char buf[128];
    int n = 0;
    int retries = 100;  // 100 retries * 10ms = 1000ms (was 50 retries)
    while (retries-- > 0) {
        n = sp_nonblocking_read(port_, buf, sizeof(buf));
        if (n > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (n <= 0) {
        LOG_ERROR(COMPONENT, "Timeout waiting for device response");

        // Add an additional retry after a delay
        LOG_INFO(COMPONENT, "Retrying read after additional delay");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        n = sp_nonblocking_read(port_, buf, sizeof(buf));
        if (n <= 0) {
            return "err";
        }
    }

    // Construct string and trim only trailing CR/LF, not all whitespace
    std::string response(buf, n);
    while (!response.empty() && (response.back() == '\r' || response.back() == '\n')) response.pop_back();

    // Also log ASCII for clarity
    LOG_INFO(COMPONENT, "Received ASCII: \"" + response + "\"");

    return response.empty() ? "err" : response;
}

sp_port* TOSGRSRestrictor::findPort(uint16_t vid, uint16_t pid) {
    sp_port** ports = nullptr;
    if (sp_list_ports(&ports) != SP_OK) return nullptr;

    sp_port* found = nullptr;

    for (int i = 0; ports[i]; ++i) {
        int v = 0, p = 0;
        if (sp_get_port_usb_vid_pid(ports[i], &v, &p) == SP_OK && v == vid && p == pid) {
            found = ports[i]; // no need to copy, just return the pointer
            break;
        }
    }

    sp_port* result = nullptr;
    if (found) sp_copy_port(found, &result);

    sp_free_port_list(ports);
    return result;
}

bool TOSGRSRestrictor::isPresent() {
    return TOSGRSRestrictor().findPort(0x2341, 0x8036) != nullptr;
}

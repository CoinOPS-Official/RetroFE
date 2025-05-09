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

    if (sp_set_baudrate(port_, 9600) != SP_OK ||
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
    sp_flush(port_, SP_BUF_INPUT);
    if (sp_blocking_write(port_, cmd.c_str(), cmd.size(), 100) != (int)cmd.size()) return "err";
    std::string r;
    char buf[64];
    int total = 0;
    while (total < 63) {
        int n = sp_blocking_read(port_, buf, 63, 100);
        if (n <= 0) break;
        buf[n] = '\0';
        r.append(buf, n);
        total += n;
        if (r.find('\n') != std::string::npos) break;
    }
    while (!r.empty() && (r.back() == '\r' || r.back() == '\n')) r.pop_back();
    return r.empty() ? "err" : r;
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

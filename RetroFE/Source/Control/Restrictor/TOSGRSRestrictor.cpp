#include "TOSGRSRestrictor.h"
#include "libserialport.h"
#include "../../Utility/Log.h"
#include <sstream>
#include <iomanip>
#include <memory>
#include <thread>

static constexpr char COMPONENT[] = "TOSGRS";

TOSGRSRestrictor::TOSGRSRestrictor(uint16_t vid, uint16_t pid)
	: vid_(vid), pid_(pid), port_(nullptr) {
}

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

	// DO NOT re-open — port_ is already open and configured
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
	sp_flush(port_, SP_BUF_INPUT); // Clear stale input

	std::string command = cmd;
	if (command.empty() || command.back() != '\r') {
		command += '\r';
	}

	if (sp_blocking_write(port_, command.c_str(), command.size(), 500) != (int)command.size()) {
		LOG_ERROR(COMPONENT, "Failed to write command to device.");
		return "err";
	}

	// Set up event waiting
	struct sp_event_set* eventSet = nullptr;
	if (sp_new_event_set(&eventSet) != SP_OK) {
		LOG_ERROR(COMPONENT, "Failed to create event set.");
		return "err";
	}

	if (sp_add_port_events(eventSet, port_, SP_EVENT_RX_READY) != SP_OK) {
		LOG_ERROR(COMPONENT, "Failed to add RX_READY event to event set.");
		sp_free_event_set(eventSet);
		return "err";
	}

	// Wait up to 1000ms for the device to respond
	int n = 0;
	char buf[128];

	if (sp_wait(eventSet, 1000) == SP_OK) {
		n = sp_nonblocking_read(port_, buf, sizeof(buf));
	}
	else {
		LOG_ERROR(COMPONENT, "Timeout or error waiting for device response.");
		sp_free_event_set(eventSet);
		return "err";
	}

	sp_free_event_set(eventSet);

	if (n <= 0) {
		LOG_ERROR(COMPONENT, "Read failed or returned no data.");
		return "err";
	}

	std::string response(buf, n);
	while (!response.empty() && (response.back() == '\r' || response.back() == '\n')) {
		response.pop_back();
	}

	LOG_INFO(COMPONENT, "Received ASCII: \"" + response + "\"");
	return response.empty() ? "err" : response;
}

std::string intToHex(int value) {
	std::stringstream ss;
	ss << std::hex << std::showbase << value;
	return ss.str();
}

sp_port* TOSGRSRestrictor::findPort(uint16_t vid, uint16_t pid) {
	sp_port** ports = nullptr;
	if (sp_list_ports(&ports) != SP_OK) return nullptr;

	sp_port* result = nullptr;

	for (int i = 0; ports[i]; ++i) {
		int v = 0, p = 0;
		if (sp_get_port_usb_vid_pid(ports[i], &v, &p) == SP_OK && v == vid && (p == 0x8036 || p == 0x8026)) {
			LOG_INFO(COMPONENT, "Probing port: " + std::string(sp_get_port_name(ports[i])));

			sp_port* testPort = nullptr;
			if (sp_copy_port(ports[i], &testPort) != SP_OK) continue;

			if (sp_open(testPort, SP_MODE_READ_WRITE) != SP_OK) {
				sp_free_port(testPort);
				continue;
			}

			sp_set_baudrate(testPort, 115200);
			sp_set_bits(testPort, 8);
			sp_set_parity(testPort, SP_PARITY_NONE);
			sp_set_stopbits(testPort, 1);
			sp_set_flowcontrol(testPort, SP_FLOWCONTROL_NONE);

			const std::string probe = "getwelcome\r";
			sp_blocking_write(testPort, probe.c_str(), probe.size(), 200);

			char buf[128] = {};
			int n = sp_blocking_read(testPort, buf, sizeof(buf), 500);
			if (n > 0) {
				std::string response(buf, n);

				// Strip trailing CR/LF for log output only
				std::string logResponse = response;
				while (!logResponse.empty() && (logResponse.back() == '\r' || logResponse.back() == '\n')) {
					logResponse.pop_back();
				}

				LOG_INFO(COMPONENT, "Received from probe: \"" + logResponse + "\"");

				std::string lower;
				for (char c : response) lower += std::tolower(c);
				if (lower.find("tos428") != std::string::npos) {
					LOG_INFO(COMPONENT, "Confirmed TOS GRS on port: " + std::string(sp_get_port_name(testPort)));
					result = testPort;
					break;
				}
			}

			else {
				LOG_INFO(COMPONENT, "No response or read failed");
			}

			sp_close(testPort);
			sp_free_port(testPort);
		}
	}

	sp_free_port_list(ports);
	return result;
}

bool TOSGRSRestrictor::isPresent() {
	return TOSGRSRestrictor().findPort(0x2341, 0x8036) != nullptr;
}
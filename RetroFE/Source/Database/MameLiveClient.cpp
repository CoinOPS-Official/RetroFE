#include "MameLiveClient.h"

#include "../Utility/Log.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace {
constexpr std::size_t kMaximumLineBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumSnapshotBytes = 32U * 1024U * 1024U;

int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decodeHex(std::string_view encoded, std::vector<std::uint8_t>& decoded) {
    if ((encoded.size() & 1U) != 0 || encoded.size() / 2U > kMaximumSnapshotBytes) return false;
    decoded.resize(encoded.size() / 2U);
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const int high = hexNibble(encoded[i * 2U]);
        const int low = hexNibble(encoded[i * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        decoded[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::optional<MameLiveSnapshot> parseSnapshot(
    const std::string& line,
    const std::string& expectedGame,
    const std::string& expectedSource,
    const std::vector<MameLiveWatchRequest>& watchRequests,
    std::string& error) {
    using nlohmann::json;

    const json message = json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        error = "invalid JSON";
        return std::nullopt;
    }

    try {
        if (message.value("protocol", std::string()) != "openhi2txt-live" ||
            message.value("protocolVersion", 0) != 1 ||
            message.value("type", std::string()) != "snapshot") {
            error = "unsupported protocol message";
            return std::nullopt;
        }

        MameLiveSnapshot snapshot;
        snapshot.game = message.at("game").get<std::string>();
        snapshot.software = message.value("software", std::string());
        snapshot.source = message.at("source").get<std::string>();
        snapshot.session = message.at("session").get<std::uint64_t>();
        snapshot.sequence = message.at("sequence").get<std::uint64_t>();

        if (snapshot.game != expectedGame) {
            error = "game identity mismatch";
            return std::nullopt;
        }
        const bool sourceExpected = !expectedSource.empty()
            ? snapshot.source == expectedSource
            : std::any_of(watchRequests.begin(), watchRequests.end(), [&snapshot](const auto& request) {
                return request.source == snapshot.source;
            });
        if (!sourceExpected || message.value("encoding", std::string()) != "hex") {
            error = "unsupported snapshot source or encoding";
            return std::nullopt;
        }

        if (message.contains("ranges")) {
            if (watchRequests.empty() || !message.at("ranges").is_array()) {
                error = "unexpected sparse snapshot";
                return std::nullopt;
            }
            snapshot.sourceOffset = message.value("sourceOffset", std::uint64_t{0});
            snapshot.sourceSize = message.at("sourceSize").get<std::uint64_t>();
            const auto watchRequest = std::find_if(
                watchRequests.begin(), watchRequests.end(), [&snapshot](const auto& request) {
                    return request.source == snapshot.source &&
                        request.sourceOffset == snapshot.sourceOffset &&
                        request.sourceSize == snapshot.sourceSize;
                });
            if (watchRequest == watchRequests.end()) {
                error = "sparse snapshot source window mismatch";
                return std::nullopt;
            }
            const auto& encodedRanges = message.at("ranges");
            if (encodedRanges.size() != watchRequest->ranges.size()) {
                error = "sparse snapshot does not contain every requested range";
                return std::nullopt;
            }
            std::size_t transferred = 0;
            snapshot.ranges.reserve(encodedRanges.size());
            for (std::size_t index = 0; index < encodedRanges.size(); ++index) {
                const auto& encodedRange = encodedRanges.at(index);
                MameLiveSnapshotRange range;
                range.offset = encodedRange.at("offset").get<std::uint64_t>();
                const std::uint64_t declaredLength = encodedRange.at("length").get<std::uint64_t>();
                const auto& requested = watchRequest->ranges[index];
                if (range.offset != requested.offset || declaredLength != requested.length ||
                    declaredLength > kMaximumSnapshotBytes - transferred) {
                    error = "sparse snapshot range does not match its watch request";
                    return std::nullopt;
                }
                const std::string& encoded = encodedRange.at("data").get_ref<const std::string&>();
                if (!decodeHex(encoded, range.bytes) || range.bytes.size() != declaredLength) {
                    error = "sparse snapshot range data does not match its declared length";
                    return std::nullopt;
                }
                transferred += range.bytes.size();
                snapshot.ranges.push_back(std::move(range));
            }
        }
        else {
			if (!watchRequests.empty()) {
				error = "expected a sparse snapshot for the selected live source";
				return std::nullopt;
			}
            const std::uint64_t declaredSize = message.at("size").get<std::uint64_t>();
            if (declaredSize > kMaximumSnapshotBytes) {
                error = "snapshot exceeds size limit";
                return std::nullopt;
            }
            const std::string& encoded = message.at("data").get_ref<const std::string&>();
            if (!decodeHex(encoded, snapshot.bytes) || snapshot.bytes.size() != declaredSize) {
                error = "snapshot data does not match its declared size";
                return std::nullopt;
            }
        }
        return snapshot;
    }
    catch (const std::exception&) {
        error = "missing or invalid protocol field";
        return std::nullopt;
    }
}

bool waitForStop(std::stop_token stopToken, std::chrono::milliseconds duration) {
    constexpr auto quantum = std::chrono::milliseconds(25);
    while (duration > std::chrono::milliseconds::zero() && !stopToken.stop_requested()) {
        const auto delay = (std::min)(duration, quantum);
        std::this_thread::sleep_for(delay);
        duration -= delay;
    }
    return stopToken.stop_requested();
}

std::string makeWatchRequest(
    const std::string& game,
    const std::vector<MameLiveWatchRequest>& requests) {
    using nlohmann::json;
	json sources = json::array();
	for (const auto& request : requests) {
		json ranges = json::array();
		for (const auto& range : request.ranges) {
			ranges.push_back({
				{"offset", range.offset},
				{"length", range.length}
			});
		}
		sources.push_back({
			{"source", request.source},
			{"storage", request.storage},
			{"sourceOffset", request.sourceOffset},
			{"sourceSize", request.sourceSize},
			{"ranges", std::move(ranges)}
		});
	}
    return json({
        {"protocol", "openhi2txt-live"},
        {"protocolVersion", 1},
        {"type", "watch"},
        {"game", game},
		{"sources", std::move(sources)}
    }).dump() + "\n";
}

bool sendAll(
    CURL* connection,
    std::string_view data,
    std::stop_token stopToken) {
    std::size_t offset = 0;
    while (offset < data.size() && !stopToken.stop_requested()) {
        std::size_t sent = 0;
        const CURLcode result = curl_easy_send(
            connection, data.data() + offset, data.size() - offset, &sent);
        if (result == CURLE_AGAIN) {
            if (waitForStop(stopToken, std::chrono::milliseconds(10))) return false;
            continue;
        }
        if (result != CURLE_OK || sent == 0) return false;
        offset += sent;
    }
    return offset == data.size();
}
}

MameLiveClient::MameLiveClient(
    std::string expectedGame,
    std::string expectedSource,
    std::uint16_t port,
    std::vector<MameLiveWatchRequest> watchRequests,
    SnapshotHandler handler)
    : expectedGame_(std::move(expectedGame))
    , expectedSource_(std::move(expectedSource))
    , port_(port)
    , watchRequests_(std::move(watchRequests))
    , handler_(std::move(handler)) {
}

MameLiveClient::~MameLiveClient() {
    stop();
}

void MameLiveClient::start() {
    stop();
    worker_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
}

void MameLiveClient::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

void MameLiveClient::run(std::stop_token stopToken) {
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(port_);
    bool waitingLogged = false;
    std::uint64_t lastSession = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t lastSequence = (std::numeric_limits<std::uint64_t>::max)();

    while (!stopToken.stop_requested()) {
        CURL* connection = curl_easy_init();
        if (!connection) {
            LOG_ERROR("LocalHiScores", "Unable to create the MAME live-score localhost connection.");
            if (waitForStop(stopToken, std::chrono::milliseconds(500))) break;
            continue;
        }

        curl_easy_setopt(connection, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(connection, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(connection, CURLOPT_CONNECTTIMEOUT_MS, 250L);
        curl_easy_setopt(connection, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(connection, CURLOPT_TCP_NODELAY, 1L);

        const CURLcode connectResult = curl_easy_perform(connection);
        if (connectResult != CURLE_OK) {
            curl_easy_cleanup(connection);
            if (!waitingLogged) {
                LOG_INFO("LocalHiScores", "Waiting for MAME live scores on 127.0.0.1:" + std::to_string(port_) + ".");
                waitingLogged = true;
            }
            if (waitForStop(stopToken, std::chrono::milliseconds(250))) break;
            continue;
        }

        waitingLogged = false;
        LOG_INFO("LocalHiScores", "Connected to MAME live scores for " + expectedGame_ + ".");
        if (!watchRequests_.empty()) {
            const std::string request = makeWatchRequest(expectedGame_, watchRequests_);
            if (!sendAll(connection, request, stopToken)) {
                curl_easy_cleanup(connection);
                if (!stopToken.stop_requested())
                    waitForStop(stopToken, std::chrono::milliseconds(100));
                continue;
            }
        }
        std::string input;
        bool reconnect = false;

        while (!stopToken.stop_requested() && !reconnect) {
            char buffer[8192];
            std::size_t received = 0;
            const CURLcode receiveResult = curl_easy_recv(connection, buffer, sizeof(buffer), &received);
            if (receiveResult == CURLE_AGAIN) {
                if (waitForStop(stopToken, std::chrono::milliseconds(20))) break;
                continue;
            }
            if (receiveResult != CURLE_OK || received == 0) {
                reconnect = true;
                break;
            }

            input.append(buffer, received);
            if (input.size() > kMaximumLineBytes) {
                LOG_WARNING("LocalHiScores", "Discarding an oversized live-score message.");
                reconnect = true;
                break;
            }

            std::size_t newline = 0;
            while ((newline = input.find('\n')) != std::string::npos) {
                std::string line = input.substr(0, newline);
                input.erase(0, newline + 1U);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                std::string error;
                auto snapshot = parseSnapshot(
                    line, expectedGame_, expectedSource_, watchRequests_, error);
                if (!snapshot) {
                    LOG_WARNING("LocalHiScores", "Ignoring live-score message: " + error + ".");
                    continue;
                }
                if (snapshot->session == lastSession && snapshot->sequence == lastSequence) continue;
                lastSession = snapshot->session;
                lastSequence = snapshot->sequence;

                try {
                    handler_(std::move(*snapshot));
                }
                catch (const std::exception& e) {
                    LOG_ERROR("LocalHiScores", std::string("Live-score handler failed: ") + e.what());
                }
                catch (...) {
                    LOG_ERROR("LocalHiScores", "Live-score handler failed with an unknown exception.");
                }
            }
        }

        curl_easy_cleanup(connection);
        if (!stopToken.stop_requested()) {
            LOG_INFO("LocalHiScores", "MAME live-score connection closed; reconnecting.");
            waitForStop(stopToken, std::chrono::milliseconds(100));
        }
    }
}

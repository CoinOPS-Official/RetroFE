#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct MameLiveSnapshotRange {
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> bytes;
};

struct MameLiveSnapshot {
    std::string game;
    std::string software;
    std::string source;
    std::uint64_t session = 0;
    std::uint64_t sequence = 0;
    std::uint64_t sourceOffset = 0;
    std::uint64_t sourceSize = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<MameLiveSnapshotRange> ranges;
};

struct MameLiveWatchRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct MameLiveWatchRequest {
    std::string source;
    std::string storage;
    std::uint64_t sourceOffset = 0;
    std::uint64_t sourceSize = 0;
    std::vector<MameLiveWatchRange> ranges;
};

class MameLiveClient {
public:
    using SnapshotHandler = std::function<void(MameLiveSnapshot)>;

    MameLiveClient(
        std::string expectedGame,
        std::string expectedSource,
        std::uint16_t port,
        std::vector<MameLiveWatchRequest> watchRequests,
        SnapshotHandler handler);
    ~MameLiveClient();

    MameLiveClient(const MameLiveClient&) = delete;
    MameLiveClient& operator=(const MameLiveClient&) = delete;

    void start();
    void stop();

private:
    void run(std::stop_token stopToken);

    std::string expectedGame_;
    std::string expectedSource_;
    std::uint16_t port_;
    std::vector<MameLiveWatchRequest> watchRequests_;
    SnapshotHandler handler_;
    std::jthread worker_;
};

#include "../Database/GlobalHiScores.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

std::vector<std::string> settingsFromCLI;

namespace {
namespace fs = std::filesystem;

void require(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "Read cache");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write(const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
    out.close();
    require(out.good(), "Write fixture");
}
}

int main() {
    const auto directory = fs::temp_directory_path() /
        ("retrofe-global-score-cache-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto source = directory / "source.json";
    const auto cache = directory / "cache.json";
    auto& scores = GlobalHiScores::getInstance();
    auto load = [&](const std::string& json) {
        write(source, json);
        scores.setGlobalPersistPath(source.string());
        require(scores.loadGlobalCacheFromDisk(), "Load offline score fixture");
        scores.setGlobalPersistPath(cache.string());
    };

    const std::string original = R"({"version":4,"games":[{"gameId":"game","gameName":"Game","contentHash":0,"scores":[{"name":"AAA","score":"100","date":"2026-10-15"}]}]})";
    load(original);
    require(scores.saveGlobalCacheToDisk(), "Create missing cache");
    const auto originalBytes = read(cache);
    auto unchanged = [&]() {
        const auto oldTime = fs::file_time_type::clock::now() - std::chrono::hours(24);
        fs::last_write_time(cache, oldTime);
        const auto stamp = fs::last_write_time(cache);
        const auto bytes = read(cache);
        require(scores.saveGlobalCacheToDisk(), "Unchanged save succeeds");
        require(fs::last_write_time(cache) == stamp, "Unchanged save must preserve modification time");
        require(read(cache) == bytes, "Unchanged save must preserve bytes");
        require(!fs::exists(cache.string() + ".tmp"), "Unchanged save must not leave a temporary file");
    };
    unchanged();
    scores.setGlobalPersistPath(cache.string());
    require(scores.loadGlobalCacheFromDisk(), "Reload existing cache");
    unchanged();

    auto changed = [&](const std::string& json, const char* message) {
        const auto before = read(cache);
        load(json);
        require(scores.saveGlobalCacheToDisk(), "Save changed cache");
        require(read(cache) != before, message);
        unchanged();
    };
    std::string modified = original;
    modified.replace(modified.find("\"Game\""), 6, "\"Renamed\"");
    changed(modified, "A game-name change must be persisted even with an unchanged row hash");
    modified.replace(modified.find("\"100\""), 5, "\"200\"");
    changed(modified, "A score change must be persisted even with an unchanged supplied hash");
    modified.replace(modified.find("2026-10-15"), 10, "2026-10-16");
    changed(modified, "A score-date change must be persisted");
    changed(R"({"version":4,"games":[]})", "Removed games must be persisted");

    load(original);
    require(scores.saveGlobalCacheToDisk(), "Restore populated cache");
    write(cache, std::string(originalBytes.size(), '?'));
    require(scores.saveGlobalCacheToDisk(), "Repair same-size damaged cache");
    require(read(cache) == originalBytes, "Repair must compare contents, not just file size");
    fs::remove(cache);
    require(scores.saveGlobalCacheToDisk(), "Recreate deleted cache after a successful save");

    const auto blocker = directory / "blocked";
    write(blocker, "not a directory");
    scores.setGlobalPersistPath((blocker / "cache.json").string());
    require(!scores.saveGlobalCacheToDisk(), "An unwritable destination must report failure");
    fs::remove(blocker);
    fs::create_directory(blocker);
    require(scores.saveGlobalCacheToDisk(), "A failed save must not suppress the next retry");
    require(read(blocker / "cache.json") == originalBytes, "Changed destinations must receive the cache");

    scores.setGlobalPersistPath(cache.string());
    fs::remove(cache);
    std::atomic<bool> success{true};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([&]() {
        if (!scores.saveGlobalCacheToDisk()) success.store(false);
    });
    for (auto& worker : workers) worker.join();
    require(success.load() && read(cache) == originalBytes, "Concurrent saves must preserve a complete cache");
    unchanged();
    scores.setGlobalPersistPath("");
    fs::remove_all(directory);
    std::cout << "Global score cache regression tests passed\n";
    return EXIT_SUCCESS;
}

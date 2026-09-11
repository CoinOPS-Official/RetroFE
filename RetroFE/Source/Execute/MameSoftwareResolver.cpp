#include "MameSoftwareResolver.h"

#include <rapidxml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
struct RomFingerprint {
    std::uint32_t crc = 0;
    std::uint64_t size = 0;

    auto operator<=>(const RomFingerprint&) const = default;
};

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string attributeValue(const rapidxml::xml_node<>* node, const char* name) {
    if (!node) return {};
    const auto* attribute = node->first_attribute(name);
    return attribute
        ? std::string(attribute->value(), attribute->value_size())
        : std::string();
}

bool startsWithListName(const std::string& canonicalName, const std::string& listName) {
    const std::string prefix = asciiLower(listName) + "_";
    const std::string canonical = asciiLower(canonicalName);
    return canonical.size() > prefix.size() && canonical.compare(0, prefix.size(), prefix) == 0;
}

std::uint16_t little16(const unsigned char* value) {
    return static_cast<std::uint16_t>(value[0]) |
        (static_cast<std::uint16_t>(value[1]) << 8);
}

std::uint32_t little32(const unsigned char* value) {
    return static_cast<std::uint32_t>(value[0]) |
        (static_cast<std::uint32_t>(value[1]) << 8) |
        (static_cast<std::uint32_t>(value[2]) << 16) |
        (static_cast<std::uint32_t>(value[3]) << 24);
}

std::uint32_t updateCrc32(std::uint32_t crc, const unsigned char* data, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}

bool readLooseFingerprint(
    const std::filesystem::path& path,
    std::vector<RomFingerprint>& fingerprints,
    std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "unable to open selected ROM: " + path.string();
        return false;
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    std::uint32_t crc = 0xffffffffU;
    std::uint64_t size = 0;
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = stream.gcount();
        if (count > 0) {
            crc = updateCrc32(crc, buffer.data(), static_cast<std::size_t>(count));
            size += static_cast<std::uint64_t>(count);
        }
    }
    if (!stream.eof()) {
        error = "unable to read selected ROM: " + path.string();
        return false;
    }
    fingerprints.push_back({~crc, size});
    return true;
}

bool readZipFingerprints(
    const std::filesystem::path& path,
    std::vector<RomFingerprint>& fingerprints,
    std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "unable to open selected ROM archive: " + path.string();
        return false;
    }

    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 22) {
        error = "selected ZIP is too small to contain a directory";
        return false;
    }
    const std::uint64_t fileSize = static_cast<std::uint64_t>(end);
    const std::size_t tailSize = static_cast<std::size_t>(
        std::min<std::uint64_t>(fileSize, 22U + 0xffffU));
    std::vector<unsigned char> tail(tailSize);
    stream.seekg(static_cast<std::streamoff>(fileSize - tailSize), std::ios::beg);
    stream.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!stream) {
        error = "unable to read selected ZIP directory";
        return false;
    }

    std::size_t eocd = std::string::npos;
    for (std::size_t position = tail.size() - 22;; --position) {
        if (little32(tail.data() + position) == 0x06054b50U) {
            const std::size_t recordSize = 22U + little16(tail.data() + position + 20);
            if (position + recordSize == tail.size()) {
                eocd = position;
                break;
            }
        }
        if (position == 0) break;
    }
    if (eocd == std::string::npos) {
        error = "selected ZIP has no valid end-of-directory record";
        return false;
    }

    const std::uint16_t disk = little16(tail.data() + eocd + 4);
    const std::uint16_t centralDisk = little16(tail.data() + eocd + 6);
    const std::uint16_t diskEntries = little16(tail.data() + eocd + 8);
    const std::uint16_t totalEntries = little16(tail.data() + eocd + 10);
    const std::uint32_t centralSize = little32(tail.data() + eocd + 12);
    const std::uint32_t centralOffset = little32(tail.data() + eocd + 16);
    if (disk != 0 || centralDisk != 0 || diskEntries != totalEntries) {
        error = "multi-disk ZIP archives are unsupported";
        return false;
    }
    if (totalEntries == 0xffffU || centralSize == 0xffffffffU || centralOffset == 0xffffffffU) {
        error = "ZIP64 ROM archives are unsupported";
        return false;
    }
    if (static_cast<std::uint64_t>(centralOffset) + centralSize > fileSize) {
        error = "selected ZIP has an invalid central-directory range";
        return false;
    }

    stream.seekg(centralOffset, std::ios::beg);
    for (std::uint16_t index = 0; index < totalEntries; ++index) {
        std::array<unsigned char, 46> header{};
        stream.read(reinterpret_cast<char*>(header.data()), header.size());
        if (!stream || little32(header.data()) != 0x02014b50U) {
            error = "selected ZIP has an invalid central-directory entry";
            return false;
        }
        const std::uint16_t nameLength = little16(header.data() + 28);
        const std::uint16_t extraLength = little16(header.data() + 30);
        const std::uint16_t commentLength = little16(header.data() + 32);
        const std::uint32_t uncompressedSize = little32(header.data() + 24);
        if (uncompressedSize == 0xffffffffU) {
            error = "ZIP64 ROM entries are unsupported";
            return false;
        }

        std::string name(nameLength, '\0');
        stream.read(name.data(), name.size());
        if (!stream) {
            error = "unable to read selected ZIP entry name";
            return false;
        }
        stream.seekg(static_cast<std::streamoff>(extraLength) + commentLength, std::ios::cur);
        if (!stream) {
            error = "selected ZIP has a truncated central-directory entry";
            return false;
        }

        const bool directory = !name.empty() && (name.back() == '/' || name.back() == '\\');
        const std::string lowered = asciiLower(name);
        const bool metadata = lowered.starts_with("__macosx/") || lowered.ends_with("/.ds_store") ||
            lowered == ".ds_store";
        if (!directory && !metadata)
            fingerprints.push_back({little32(header.data() + 16), uncompressedSize});
    }
    if (fingerprints.empty()) {
        error = "selected ZIP contains no ROM files";
        return false;
    }
    return true;
}

bool readSelectedFingerprints(
    const std::filesystem::path& path,
    std::vector<RomFingerprint>& fingerprints,
    std::string& error) {
    if (!std::filesystem::is_regular_file(path)) {
        error = "selected ROM does not exist: " + path.string();
        return false;
    }

    std::ifstream probe(path, std::ios::binary);
    std::array<unsigned char, 4> signature{};
    probe.read(reinterpret_cast<char*>(signature.data()), signature.size());
    const bool isZip = probe.gcount() == static_cast<std::streamsize>(signature.size()) &&
        little32(signature.data()) == 0x04034b50U;
    probe.close();

    const bool ok = isZip
        ? readZipFingerprints(path, fingerprints, error)
        : readLooseFingerprint(path, fingerprints, error);
    if (ok) std::sort(fingerprints.begin(), fingerprints.end());
    return ok;
}

std::optional<std::uint64_t> parseUnsigned(const std::string& value, int base) {
    if (value.empty()) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, base);
        if (consumed != value.size()) return std::nullopt;
        return parsed;
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<RomFingerprint> partFingerprints(const rapidxml::xml_node<>* part) {
    std::vector<RomFingerprint> result;
    for (const auto* area = part->first_node("dataarea"); area; area = area->next_sibling("dataarea")) {
        for (const auto* rom = area->first_node("rom"); rom; rom = rom->next_sibling("rom")) {
            if (asciiLower(attributeValue(rom, "status")) == "nodump") continue;
            const auto crc = parseUnsigned(attributeValue(rom, "crc"), 16);
            const auto size = parseUnsigned(attributeValue(rom, "size"), 0);
            if (crc && size && *crc <= 0xffffffffU)
                result.push_back({static_cast<std::uint32_t>(*crc), *size});
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<MameSoftwareStorageArea> loadStorageAreas(
    const std::filesystem::path& hashDirectory,
    const std::string& wantedList,
    const std::string& wantedSoftware) {
    std::vector<MameSoftwareStorageArea> result;
    const auto path = hashDirectory / (wantedList + ".xml");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return result;
    std::vector<char> contents(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    contents.push_back('\0');

    try {
        rapidxml::xml_document<> document;
        document.parse<rapidxml::parse_non_destructive>(contents.data());
        const auto* root = document.first_node("softwarelist");
        if (asciiLower(attributeValue(root, "name")) != asciiLower(wantedList)) return result;
        for (const auto* software = root->first_node("software"); software;
            software = software->next_sibling("software")) {
            if (asciiLower(attributeValue(software, "name")) != asciiLower(wantedSoftware)) continue;
            for (const auto* part = software->first_node("part"); part; part = part->next_sibling("part")) {
                std::string slot;
                for (const auto* feature = part->first_node("feature"); feature;
                    feature = feature->next_sibling("feature")) {
                    if (asciiLower(attributeValue(feature, "name")) == "slot") {
                        slot = attributeValue(feature, "value");
                        break;
                    }
                }
                for (const auto* area = part->first_node("dataarea"); area;
                    area = area->next_sibling("dataarea")) {
                    const std::string name = attributeValue(area, "name");
                    const auto size = parseUnsigned(attributeValue(area, "size"), 0);
                    if (name.empty() || asciiLower(name) == "rom" || !size || *size == 0) continue;
                    result.push_back({
                        attributeValue(part, "name"),
                        attributeValue(part, "interface"),
                        name,
                        *size,
                        slot
                    });
                }
            }
            break;
        }
    }
    catch (const rapidxml::parse_error&) {
        result.clear();
    }
    return result;
}

bool contentsMayMatch(const std::vector<char>& contents, const std::vector<RomFingerprint>& selected) {
    const std::string text = asciiLower(std::string(contents.data(), contents.size()));
    for (const auto& fingerprint : selected) {
        std::ostringstream crc;
        crc << std::hex << std::setfill('0') << std::setw(8) << fingerprint.crc;
        if (text.find(crc.str()) != std::string::npos) return true;
    }
    return false;
}

void collectContentMatches(
    const std::filesystem::path& path,
    const std::vector<RomFingerprint>& selected,
    std::set<std::pair<std::string, std::string>>& matches) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return;
    std::vector<char> contents(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    if (!contentsMayMatch(contents, selected)) return;
    contents.push_back('\0');

    try {
        rapidxml::xml_document<> document;
        document.parse<rapidxml::parse_non_destructive>(contents.data());
        const auto* root = document.first_node("softwarelist");
        const std::string listName = attributeValue(root, "name");
        if (listName.empty()) return;

        for (const auto* software = root->first_node("software"); software;
            software = software->next_sibling("software")) {
            const std::string softwareName = attributeValue(software, "name");
            if (softwareName.empty()) continue;
            for (const auto* part = software->first_node("part"); part; part = part->next_sibling("part")) {
                if (partFingerprints(part) == selected) {
                    matches.emplace(listName, softwareName);
                    break;
                }
            }
        }
    }
    catch (const rapidxml::parse_error&) {
        // Ignore malformed list files and report an unresolved ROM if no
        // valid software-list definition matches.
    }
}

void collectCanonicalMatches(
    const std::filesystem::path& path,
    const std::string& canonicalName,
    std::set<std::pair<std::string, std::string>>& matches) {
    if (!startsWithListName(canonicalName, path.stem().string())) return;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return;
    std::vector<char> contents(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    contents.push_back('\0');

    try {
        rapidxml::xml_document<> document;
        document.parse<rapidxml::parse_non_destructive>(contents.data());
        const auto* root = document.first_node("softwarelist");
        const std::string listName = attributeValue(root, "name");
        if (listName.empty() || !startsWithListName(canonicalName, listName)) return;
        const std::string canonical = asciiLower(canonicalName);
        for (const auto* software = root->first_node("software"); software;
            software = software->next_sibling("software")) {
            const std::string softwareName = attributeValue(software, "name");
            if (!softwareName.empty() && asciiLower(listName + "_" + softwareName) == canonical)
                matches.emplace(listName, softwareName);
        }
    }
    catch (const rapidxml::parse_error&) {
        // Ignore malformed list files.
    }
}

void collectHiscoreMatches(
    const std::filesystem::path& path,
    const std::string& canonicalName,
    std::set<std::pair<std::string, std::string>>& matches) {
    if (path.empty()) return;
    std::ifstream stream(path);
    if (!stream) return;

    const std::string canonical = asciiLower(canonicalName);
    std::string line;
    while (std::getline(stream, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == ';' || line[first] == '@') continue;
        const auto colon = line.find(':', first);
        const auto comma = line.find(',', first);
        if (colon == std::string::npos || comma == std::string::npos || comma > colon) continue;
        if (line.find(',', comma + 1) < colon) continue;

        std::string machine = line.substr(first, comma - first);
        std::string software = line.substr(comma + 1, colon - comma - 1);
        const auto machineEnd = machine.find_last_not_of(" \t");
        const auto softwareFirst = software.find_first_not_of(" \t");
        const auto softwareEnd = software.find_last_not_of(" \t");
        if (machineEnd == std::string::npos || softwareFirst == std::string::npos) continue;
        machine.erase(machineEnd + 1);
        software = software.substr(softwareFirst, softwareEnd - softwareFirst + 1);
        if (asciiLower(machine + "_" + software) == canonical)
            matches.emplace(std::move(machine), std::move(software));
    }
}

std::string cacheKey(
    const std::filesystem::path& hashDirectory,
    const std::filesystem::path& selectedRom,
    const std::filesystem::path& hiscoreDatPath) {
    std::error_code error;
    const auto normalizedHash = std::filesystem::weakly_canonical(hashDirectory, error);
    const std::string directory = error
        ? hashDirectory.lexically_normal().string()
        : normalizedHash.string();
    error.clear();
    const auto normalizedRom = std::filesystem::weakly_canonical(selectedRom, error);
    const std::string rom = error ? selectedRom.lexically_normal().string() : normalizedRom.string();
    error.clear();
    const auto size = std::filesystem::file_size(selectedRom, error);
    const auto modified = std::filesystem::last_write_time(selectedRom, error);
    const auto ticks = error ? 0 : modified.time_since_epoch().count();
    return asciiLower(directory) + "\n" + asciiLower(rom) + "\n" +
        std::to_string(size) + "\n" + std::to_string(ticks) + "\n" +
        asciiLower(hiscoreDatPath.lexically_normal().string());
}
}

MameSoftwareResolution MameSoftwareResolver::describe(
    const std::filesystem::path& hashDirectory,
    const MameSoftwareIdentity& identity) {
    MameSoftwareResolution result;
    result.identity = identity;
    if (identity.softwareList.empty() || identity.software.empty()) {
        result.error = "software-list identity is incomplete";
        return result;
    }
    const auto listPath = hashDirectory / (identity.softwareList + ".xml");
    if (!std::filesystem::is_regular_file(listPath)) {
        result.error = "MAME software list does not exist: " + listPath.string();
        return result;
    }
    result.storageAreas = loadStorageAreas(
        hashDirectory, identity.softwareList, identity.software);
    return result;
}

MameSoftwareResolution MameSoftwareResolver::resolve(
    const std::filesystem::path& hashDirectory,
    const std::filesystem::path& selectedRom,
    const std::string& canonicalName,
    const std::filesystem::path& hiscoreDatPath,
    bool includeStorageAreas) {
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, MameSoftwareResolution> cache;

    const std::string key = cacheKey(hashDirectory, selectedRom, hiscoreDatPath) +
        (includeStorageAreas ? "\nstorage" : "\nidentity-only");
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto cached = cache.find(key);
        if (cached != cache.end()) return cached->second;
    }

    MameSoftwareResolution result;
    if (canonicalName.empty()) {
        result.error = "the canonical item name is empty";
    }
    else if (!std::filesystem::is_directory(hashDirectory)) {
        result.error = "MAME hash directory does not exist: " + hashDirectory.string();
    }
    else {
        std::vector<RomFingerprint> selected;
        if (readSelectedFingerprints(selectedRom, selected, result.error)) {
            std::set<std::pair<std::string, std::string>> matches;
            std::vector<std::filesystem::path> listFiles;
            std::error_code iterationError;
            for (std::filesystem::directory_iterator iterator(hashDirectory, iterationError), end;
                !iterationError && iterator != end;
                iterator.increment(iterationError)) {
                const auto& entry = *iterator;
                if (!entry.is_regular_file()) continue;
                const auto path = entry.path();
                if (asciiLower(path.extension().string()) != ".xml") continue;
                listFiles.push_back(path);
                collectContentMatches(path, selected, matches);
            }

            if (iterationError) {
                result.error = "unable to enumerate MAME hash directory: " + iterationError.message();
            }
            else if (matches.empty()) {
                std::set<std::pair<std::string, std::string>> canonicalMatches;
                for (const auto& path : listFiles)
                    collectCanonicalMatches(path, canonicalName, canonicalMatches);
                if (canonicalMatches.size() == 1) {
                    result.identity.softwareList = canonicalMatches.begin()->first;
                    result.identity.software = canonicalMatches.begin()->second;
                }
                else if (canonicalMatches.size() > 1) {
                    result.error = "canonical item name is ambiguous across MAME software lists";
                }

                std::error_code sizeError;
                const bool placeholder = std::filesystem::file_size(selectedRom, sizeError) == 0 && !sizeError;
                std::set<std::pair<std::string, std::string>> fallbackMatches;
                if (result.identity.software.empty() && result.error.empty() && placeholder)
                    collectHiscoreMatches(hiscoreDatPath, canonicalName, fallbackMatches);
                if (result.identity.software.empty() && result.error.empty() && fallbackMatches.size() == 1) {
                    result.identity.machine = fallbackMatches.begin()->first;
                    result.identity.software = fallbackMatches.begin()->second;
                }
                else if (result.identity.software.empty() && result.error.empty() && fallbackMatches.size() > 1) {
                    result.error = "zero-byte placeholder is ambiguous in hiscore.dat";
                }
                else if (result.identity.software.empty() && result.error.empty() && placeholder) {
                    result.error = "zero-byte placeholder has no matching hiscore.dat entry";
                }
                else if (result.identity.software.empty() && result.error.empty()) {
                    result.error = "selected ROM content does not match a MAME software-list entry";
                }
            }
            else if (matches.size() > 1) {
                std::set<std::pair<std::string, std::string>> canonicalMatches;
                const std::string canonical = asciiLower(canonicalName);
                for (const auto& match : matches) {
                    if (asciiLower(match.first + "_" + match.second) == canonical)
                        canonicalMatches.insert(match);
                }
                if (canonicalMatches.size() == 1) {
                    result.identity.softwareList = canonicalMatches.begin()->first;
                    result.identity.software = canonicalMatches.begin()->second;
                }
                else {
                    std::ostringstream detail;
                    bool first = true;
                    for (const auto& match : matches) {
                        if (!first) detail << ", ";
                        first = false;
                        detail << match.first << ':' << match.second;
                    }
                    result.error = "selected ROM content is ambiguous across MAME software lists: " + detail.str();
                }
            }
            else {
                result.identity.softwareList = matches.begin()->first;
                result.identity.software = matches.begin()->second;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (includeStorageAreas && result.error.empty() && !result.identity.softwareList.empty()) {
            result.storageAreas = loadStorageAreas(
                hashDirectory, result.identity.softwareList, result.identity.software);
        }
        cache.emplace(key, result);
    }
    return result;
}

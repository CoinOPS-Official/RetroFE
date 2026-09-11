#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

struct MameSoftwareIdentity {
    std::string machine;
    std::string softwareList;
    std::string software;
};

struct MameSoftwareStorageArea {
    std::string part;
    std::string interfaceName;
    std::string name;
    std::uint64_t size = 0;
    std::string slot;
};

struct MameSoftwareResolution {
    MameSoftwareIdentity identity;
    std::vector<MameSoftwareStorageArea> storageAreas;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

class MameSoftwareResolver {
public:
    static MameSoftwareResolution describe(
        const std::filesystem::path& hashDirectory,
        const MameSoftwareIdentity& identity);

    static MameSoftwareResolution resolve(
        const std::filesystem::path& hashDirectory,
        const std::filesystem::path& selectedRom,
        const std::string& canonicalName,
        const std::filesystem::path& hiscoreDatPath = {},
        bool includeStorageAreas = true);
};

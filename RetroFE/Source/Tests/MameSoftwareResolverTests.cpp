#include "../Execute/MameSoftwareResolver.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
constexpr std::uint32_t testCrc = 0xcbf43926U;
constexpr const char* testBytes = "123456789";
constexpr std::uint32_t testSize = 9;

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void write16(std::ofstream& output, std::uint16_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8)
    };
    output.write(bytes, sizeof(bytes));
}

void write32(std::ofstream& output, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8),
        static_cast<char>(value >> 16),
        static_cast<char>(value >> 24)
    };
    output.write(bytes, sizeof(bytes));
}

void writeLooseRom(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    output.write(testBytes, testSize);
}

void writeStoredZip(const std::filesystem::path& path) {
    constexpr const char* name = "original-rom-name.bin";
    constexpr std::uint16_t nameLength = 21;
    std::ofstream output(path, std::ios::binary);

    write32(output, 0x04034b50U);
    write16(output, 20);
    write16(output, 0);
    write16(output, 0);
    write16(output, 0);
    write16(output, 0);
    write32(output, testCrc);
    write32(output, testSize);
    write32(output, testSize);
    write16(output, nameLength);
    write16(output, 0);
    output.write(name, nameLength);
    output.write(testBytes, testSize);

    const auto centralOffset = static_cast<std::uint32_t>(output.tellp());
    write32(output, 0x02014b50U);
    write16(output, 20);
    write16(output, 20);
    write16(output, 0);
    write16(output, 0);
    write16(output, 0);
    write16(output, 0);
    write32(output, testCrc);
    write32(output, testSize);
    write32(output, testSize);
    write16(output, nameLength);
    write16(output, 0);
    write16(output, 0);
    write16(output, 0);
    write16(output, 0);
    write32(output, 0);
    write32(output, 0);
    output.write(name, nameLength);
    const auto centralEnd = static_cast<std::uint32_t>(output.tellp());

    write32(output, 0x06054b50U);
    write16(output, 0);
    write16(output, 0);
    write16(output, 1);
    write16(output, 1);
    write32(output, centralEnd - centralOffset);
    write32(output, centralOffset);
    write16(output, 0);
}

void writeList(
    const std::filesystem::path& directory,
    const std::string& list,
    const std::string& software) {
    std::filesystem::create_directories(directory);
    std::ofstream output(directory / (list + ".xml"));
    output << "<?xml version=\"1.0\"?>\n"
        << "<softwarelist name=\"" << list << "\">\n"
        << "  <software name=\"" << software << "\">\n"
        << "    <description>Test</description>\n"
        << "    <part name=\"cart\" interface=\"cart\">\n"
        << "      <feature name=\"slot\" value=\"rom_sram\"/>\n"
        << "      <dataarea name=\"rom\" size=\"9\">\n"
        << "        <rom name=\"original-rom-name.bin\" size=\"9\" crc=\"cbf43926\"/>\n"
        << "      </dataarea>\n"
        << "      <dataarea name=\"sram\" size=\"16384\">\n"
        << "      </dataarea>\n"
        << "    </part>\n"
        << "  </software>\n"
        << "</softwarelist>\n";
}

void writeHiscoreDat(const std::filesystem::path& path) {
    std::ofstream output(path);
    output << "genesis,sor2u:\t; Streets of Rage 2\n"
        << "@:maincpu,program,fffd30,a0,10,00\n";
}
}

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("retrofe-mame-software-resolver-" + std::to_string(unique));
    std::filesystem::create_directories(root);

    const auto looseRom = root / "renamed_console_title.bin";
    writeLooseRom(looseRom);
    const auto snesHash = root / "snes-hash";
    writeList(snesHash, "snes", "game_name");
    auto resolved = MameSoftwareResolver::resolve(snesHash, looseRom, "snes_game_name");
    require(static_cast<bool>(resolved), "loose ROM content must resolve");
    require(resolved.identity.machine.empty(), "ROM metadata must not invent a machine driver");
    require(resolved.identity.softwareList == "snes", "the matched software list must be returned");
    require(resolved.identity.software == "game_name", "underscores in software names must survive");

    const auto zipRom = root / "genesis_sor2u.zip";
    writeStoredZip(zipRom);
    const auto genesisHash = root / "genesis-hash";
    writeList(genesisHash, "megadriv", "sor2u");
    const auto identityOnly = MameSoftwareResolver::resolve(
        genesisHash, zipRom, "genesis_sor2u", {}, false);
    require(static_cast<bool>(identityOnly), "identity-only software resolution must succeed");
    require(identityOnly.storageAreas.empty(),
        "identity-only software resolution must skip storage metadata");
    resolved = MameSoftwareResolver::resolve(genesisHash, zipRom, "genesis_sor2u");
    require(static_cast<bool>(resolved), "renamed ZIP content must resolve independently of its canonical prefix");
    require(resolved.identity.softwareList == "megadriv", "content hashing must find the actual software list");
    require(resolved.identity.software == "sor2u", "content hashing must find the software short name");
    require(resolved.storageAreas.size() == 1, "matched software storage metadata must be retained");
    require(resolved.storageAreas[0].name == "sram", "the storage-area name must be retained");
    require(resolved.storageAreas[0].size == 16384, "the storage-area size must be retained");
    require(resolved.storageAreas[0].part == "cart", "the storage-area part must be retained");
    require(resolved.storageAreas[0].interfaceName == "cart", "the storage-area interface must be retained");
    require(resolved.storageAreas[0].slot == "rom_sram", "the storage-area slot feature must be retained");

    resolved = MameSoftwareResolver::describe(
        genesisHash, {"genesis", "megadriv", "sor2u"});
    require(static_cast<bool>(resolved), "explicit metadata identity must be accepted without hashing the ROM");
    require(resolved.identity.machine == "genesis", "explicit metadata must retain the launch machine");
    require(resolved.storageAreas.size() == 1 && resolved.storageAreas[0].size == 16384,
        "explicit metadata must still obtain storage hints from the software list");

    const auto placeholder = root / "placeholder.zip";
    std::ofstream(placeholder, std::ios::binary);
    const auto hiscoreDat = root / "hiscore.dat";
    writeHiscoreDat(hiscoreDat);
    resolved = MameSoftwareResolver::resolve(genesisHash, placeholder, "genesis_sor2u", hiscoreDat);
    require(static_cast<bool>(resolved), "a zero-byte legacy placeholder may fall back to hiscore.dat");
    require(resolved.identity.machine == "genesis", "placeholder fallback must retain the MAME machine");
    require(resolved.identity.software == "sor2u", "placeholder fallback must resolve the software name");

    const auto ambiguousHash = root / "ambiguous-hash";
    writeList(ambiguousHash, "first", "one");
    writeList(ambiguousHash, "second", "two");
    resolved = MameSoftwareResolver::resolve(ambiguousHash, looseRom, "anything");
    require(!resolved, "identical content in multiple software entries must be rejected as ambiguous");

    const auto unknownRom = root / "unknown.bin";
    {
        std::ofstream output(unknownRom, std::ios::binary);
        output << "different";
    }
    resolved = MameSoftwareResolver::resolve(snesHash, unknownRom, "snes_unknown");
    require(!resolved, "unknown ROM content must not resolve from its filename");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return EXIT_SUCCESS;
}

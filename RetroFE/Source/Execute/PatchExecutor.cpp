#include "PatchExecutor.h"
#include "../Utility/Utils.h"
#include "../Database/Configuration.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

PatchExecutor::PatchExecutor(const std::string& patchFilePath) {
    parsePatchFile(patchFilePath);
}

void PatchExecutor::parsePatchFile(const std::string& patchFilePath) {
    std::ifstream file(patchFilePath);
    std::string line, section;
    while (std::getline(file, line)) {
        line = Utils::filterComments(line);
        line = Utils::trimEnds(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = Utils::toLower(line.substr(1, line.size() - 2));
            continue;
        }
        if (section.compare(0, 8, "settings") == 0) {
            std::string filename = section + ".conf";
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::trimEnds(line.substr(0, eq));
                std::string val = Utils::trimEnds(line.substr(eq + 1));
                settingsFiles_[filename][key] = val;
            }
        } else if (section == "actions") {
            auto eq = line.find('=');
            std::string type = Utils::toLower(Utils::trimEnds(line.substr(0, eq)));
            std::vector<std::string> args;
            if (eq != std::string::npos)
                Utils::listToVector(line.substr(eq + 1), args, ',');
            // Use Utils::trimEnds on each argument; path normalization handled by filesystem::path
            for (auto& a : args) a = Utils::trimEnds(a);
            actions_.push_back({type, args});
        }
    }
}

bool PatchExecutor::writeSettingsFile(const std::string& filePath, const std::map<std::string, std::string>& kv, std::string& errorMsg) {
    std::ofstream out(filePath, std::ios::trunc);
    if (!out) {
        errorMsg = "Cannot open file for write: " + filePath;
        return false;
    }
    for (const auto& pair : kv) {
        out << pair.first << " = " << pair.second << "\n";
    }
    return true;
}

bool PatchExecutor::applyActions(const std::string& rootPath, std::string& errorMsg) {
    namespace fs = std::filesystem;
    for (const auto& action : actions_) {
        try {
            if (action.type == "copy") {
                if (action.args.size() != 2) {
                    errorMsg = "Copy action needs 2 arguments";
                    return false;
                }
                // Paths are relative to rootPath (usually Configuration::absolutePath)
                fs::path src = fs::path(rootPath) / action.args[0];
                fs::path dst = fs::path(rootPath) / action.args[1];
                std::error_code ec;
                if (!fs::exists(src)) {
                    errorMsg = "Source path does not exist: " + src.string();
                    return false;
                }
                if (fs::is_directory(src)) {
                    // dst may be an existing dir or a new dir
                    fs::create_directories(dst);
                    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        errorMsg = "Error copying directory: " + ec.message();
                        return false;
                    }
                } else if (fs::is_regular_file(src)) {
                    // If dst is an existing directory, copy file into it
                    fs::path realDst = dst;
                    if (fs::exists(dst) && fs::is_directory(dst)) {
                        realDst /= src.filename();
                    }
                    fs::create_directories(realDst.parent_path());
                    fs::copy_file(src, realDst, fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        errorMsg = "Error copying file: " + ec.message();
                        return false;
                    }
                } else {
                    errorMsg = "Source is neither file nor directory: " + src.string();
                    return false;
                }
            } else if (action.type == "rename") {
                if (action.args.size() != 2) {
                    errorMsg = "Rename action needs 2 arguments";
                    return false;
                }
                fs::path src = fs::path(rootPath) / action.args[0];
                fs::path dst = fs::path(rootPath) / action.args[1];
                fs::create_directories(dst.parent_path());
                fs::rename(src, dst);
            } else if (action.type == "delete") {
                if (action.args.empty()) {
                    errorMsg = "Delete action needs 1 argument";
                    return false;
                }
                fs::path tgt = fs::path(rootPath) / action.args[0];
                std::error_code ec;
                if (fs::is_directory(tgt)) {
                    fs::remove_all(tgt, ec);
                } else if (fs::exists(tgt)) {
                    fs::remove(tgt, ec);
                }
                if (ec) {
                    errorMsg = "Error deleting: " + ec.message();
                    return false;
                }
            } else {
                errorMsg = "Unknown patch action type: " + action.type;
                return false;
            }
        } catch (const std::exception& e) {
            errorMsg = "Error applying action " + action.type + ": " + e.what();
            return false;
        }
    }
    return true;
}

bool PatchExecutor::execute(const std::string& rootPath, std::string& errorMsg) {
    namespace fs = std::filesystem;
    for (const auto& fileKv : settingsFiles_) {
        std::string filePath = (fs::path(rootPath) / fileKv.first).string();
        if (!writeSettingsFile(filePath, fileKv.second, errorMsg))
            return false;
    }
    if (!applyActions(rootPath, errorMsg))
        return false;
    return true;
}

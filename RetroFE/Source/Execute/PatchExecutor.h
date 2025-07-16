#pragma once
#include <string>
#include <vector>
#include <map>

class PatchExecutor {
public:
    explicit PatchExecutor(const std::string& patchFilePath);

    // Applies the patch; returns true on success, false on error (with errorMsg)
    bool execute(const std::string& rootPath, std::string& errorMsg);

private:
    void parsePatchFile(const std::string& patchFilePath);

    // Each [settingsN] section: filename (e.g. "settings5.conf") -> { key, value }
    std::map<std::string, std::map<std::string, std::string>> settingsFiles_;

    struct Action {
        std::string type;
        std::vector<std::string> args;
    };
    std::vector<Action> actions_;

    bool writeSettingsFile(const std::string& filePath, const std::map<std::string, std::string>& kv, std::string& errorMsg);
    bool applyActions(const std::string& rootPath, std::string& errorMsg);
};

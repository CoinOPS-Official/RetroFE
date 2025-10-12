#pragma once
#include <string>
#include <vector>

class Configuration; // fwd-declare

class PayloadSync {
public:
    struct Entry {
        std::string url;             // REQUIRED
        std::string local;           // REQUIRED (relative to Configuration::absolutePath)
        std::string etagPath;        // optional; auto if empty
        std::string lastModPath;     // optional; auto if empty
        std::string sha256Expected;  // optional (parsed; not enforced here)
        size_t      maxBytes = 0;    // optional; 0 => use Config::maxBytesDefault (or unlimited if that’s 0)
    };

    struct Config {
        bool        enabled = true;
        std::string payloadPath = "payload.txt"; // relative or absolute
        int         intervalSec = 300;           // scheduler uses this (class doesn’t schedule)
        int         initialDelaySec = 5;             //    "
        int         jitterSec = 10;            //    "
        bool        allowGithub = true;          // gate GH domains
        size_t      maxBytesDefault = 0;             // 0 = unlimited

        // Load keys from RetroFE config (defaults above are used if missing)
        // Keys:
        //   payload.enabled
        //   payload.file
        //   payload.interval_seconds
        //   payload.initial_delay_seconds
        //   payload.jitter_seconds
        //   payload.allow_github
        //   payload.max_bytes_default
        static Config LoadFrom(Configuration& cfg);

        // Returns absolute path for payloadPath (if relative, joined with Configuration::absolutePath)
        std::string ResolvePayloadPath() const;
    };

    // Parse payload file into entries (logs on errors, never throws).
    // Accepts '=' or ':' as separator. '#' starts a comment. Blank line ends a stanza.
    static std::vector<Entry> ParseFile(const std::string& payloadPath);

    // Run a single sync pass using the given config (path, allowlist, size defaults).
    // Returns true if all entries either updated successfully or were already 304.
    static bool RunWithConfig(const Config& cfg, bool dryRun = false);

    // Back-compat helper; equivalent to RunWithConfig with defaults + given path.
    static bool RunFromFile(const std::string& payloadPath, bool dryRun = false);

private:
    // ---- internals ----
    static bool AllowUrl(const std::string& url, const Config& cfg);
    static bool SafeLocalRel(const std::string& rel);
    static void FillDefaultSidecars(Entry& e);

    // Conditional GET + atomic replace; uses cfg for default size cap.
    static bool DownloadIfNewer(const Entry& e, const Config& cfg, std::string& outStatus);

    // Small helpers
    static std::string PreprocessLine(const std::string& input); // strip '#' comment + trim
    static void        SplitKeyVal(const std::string& line, std::string& k, std::string& v);
    static std::string SanitizeForSidecar(std::string s);

    // Sidecar I/O
    static std::string ReadWholeText(const std::string& path);
    static bool        WriteWholeText(const std::string& path, const std::string& data);
};

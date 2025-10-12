#include "PayloadSync.h"
#include "../Utility/Utils.h"
#include "../Utility/Log.h"
#include "../Database/Configuration.h"
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <cstdio>
#include <curl/curl.h>

namespace fs = std::filesystem;

// ---------------------- Config ----------------------

PayloadSync::Config PayloadSync::Config::LoadFrom(Configuration& cfg) {
    Config out;
    cfg.getProperty("payload.enabled", out.enabled);
    cfg.getProperty("payload.file", out.payloadPath);
    cfg.getProperty("payload.interval_seconds", out.intervalSec);
    cfg.getProperty("payload.initial_delay_seconds", out.initialDelaySec);
    cfg.getProperty("payload.jitter_seconds", out.jitterSec);
    cfg.getProperty("payload.allow_github", out.allowGithub);
    // size_t through int64 then clamp (getProperty overloads commonly use int)
    int maxDefault = 0;
    if (cfg.getProperty("payload.max_bytes_default", maxDefault)) {
        if (maxDefault > 0) out.maxBytesDefault = static_cast<size_t>(maxDefault);
        else out.maxBytesDefault = 0;
    }
    return out;
}

std::string PayloadSync::Config::ResolvePayloadPath() const {
    return Utils::isAbsolutePath(payloadPath)
        ? payloadPath
        : Utils::combinePath(Configuration::absolutePath, payloadPath);
}

// ---------------------- small helpers ----------------------

std::string PayloadSync::PreprocessLine(const std::string& input) {
    std::string s = Utils::filterComments(input); // strips after '#'
    return Utils::trim(s);
}

void PayloadSync::SplitKeyVal(const std::string& line, std::string& k, std::string& v) {
    size_t peq = line.find('='), pcol = line.find(':');
    size_t p = (peq == std::string::npos) ? pcol :
        (pcol == std::string::npos ? peq : std::min(peq, pcol));
    if (p == std::string::npos) { k.clear(); v.clear(); return; }
    k = Utils::toLower(Utils::trim(line.substr(0, p)));
    v = Utils::trim(line.substr(p + 1));
    if (v.size() >= 2 &&
        ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
        v = v.substr(1, v.size() - 2);
    }
}

std::string PayloadSync::SanitizeForSidecar(std::string s) {
    Utils::replaceSlashesWithUnderscores(s);
    for (char& c : s) {
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    return s;
}

std::string PayloadSync::ReadWholeText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Utils::trim(Utils::removeNullCharacters(s));
}

bool PayloadSync::WriteWholeText(const std::string& path, const std::string& data) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// ---------------------- policy & sidecars ----------------------

bool PayloadSync::AllowUrl(const std::string& url, const Config& cfg) {
    if (!cfg.allowGithub) return false;
    return url.find("raw.githubusercontent.com/") != std::string::npos
        || url.find("objects.githubusercontent.com/") != std::string::npos
        || url.find("github.com/") != std::string::npos;
}

bool PayloadSync::SafeLocalRel(const std::string& rel) {
    return Utils::isSubPath(rel);
}

void PayloadSync::FillDefaultSidecars(Entry& e) {
    if (!e.etagPath.empty() && !e.lastModPath.empty()) return;
    const std::string base = Utils::combinePath(
        Utils::combinePath(Configuration::absolutePath, ".cache"), "payload");
    fs::create_directories(base);
    const std::string tag = SanitizeForSidecar(e.local);
    if (e.etagPath.empty())      e.etagPath = Utils::combinePath(base, tag + ".etag");
    if (e.lastModPath.empty())   e.lastModPath = Utils::combinePath(base, tag + ".lm");
}

// ---------------------- parsing ----------------------

std::vector<PayloadSync::Entry> PayloadSync::ParseFile(const std::string& payloadPath) {
    std::ifstream in(payloadPath, std::ios::binary);
    std::vector<Entry> out;
    if (!in) {
        LOG_ERROR("Payload", "File not found: %s", payloadPath.c_str());
        return out;
    }

    std::unordered_map<std::string, std::string> kv;
    auto flush = [&]() {
        if (kv.empty()) return;
        Entry e;
        auto get = [&](const char* key)->std::string {
            auto it = kv.find(key); return (it == kv.end()) ? std::string() : it->second;
            };
        e.url = get("url");
        e.local = get("local");
        e.etagPath = get("etag");
        e.lastModPath = get("last_modified");
        e.sha256Expected = get("sha256");
        if (auto mb = get("max_bytes"); !mb.empty()) {
            try { e.maxBytes = static_cast<size_t>(std::stoull(mb)); }
            catch (...) {}
        }

        // --- Normalize local path ---
        if (!e.local.empty()) {
            // Strip a leading slash if user specified one (treat as root-relative)
            if ((e.local[0] == '/' || e.local[0] == '\\') && !Utils::isAbsolutePath(e.local)) {
                e.local.erase(0, 1);
                LOG_NOTICE("Payload", "Normalized leading-slash local path to: " << e.local);
            }

            // If still absolute but actually lies under RetroFE root, make it relative
            if (Utils::isAbsolutePath(e.local) && Utils::isSubPath(e.local)) {
                try {
                    namespace fs = std::filesystem;
                    e.local = fs::relative(e.local, Configuration::absolutePath).string();
                    LOG_NOTICE("Payload", "Converted absolute path to relative: " << e.local);
                }
                catch (...) {
                    // ignore conversion errors, we'll catch policy skip later
                }
            }
        }

        if (e.url.empty() || e.local.empty()) {
            LOG_WARNING("Payload", "Skipping stanza missing url/local");
        }
        else {
            FillDefaultSidecars(e);
            out.push_back(std::move(e));
        }
        kv.clear();
        };

    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = PreprocessLine(raw);
        if (line.empty()) { flush(); continue; }
        std::string k, v; SplitKeyVal(line, k, v);
        if (!k.empty()) kv[k] = v;
    }
    flush();
    return out;
}

// ---------------------- curl callbacks ----------------------

struct HeaderState { std::string etag; std::string lastModified; };

static size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    auto line = std::string(buffer, total);
    auto* hs = static_cast<HeaderState*>(userdata);

    auto lower = Utils::toLower(line);
    auto trimCRLF = [](std::string s) {
        s = Utils::trim(s);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        return s;
        };

    if (lower.rfind("etag:", 0) == 0) {
        std::string v = trimCRLF(line.substr(5));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        hs->etag = v;
    }
    else if (lower.rfind("last-modified:", 0) == 0) {
        hs->lastModified = trimCRLF(line.substr(14));
    }
    return total;
}

struct WriteCtx {
    FILE* fp = nullptr;
    size_t maxBytes = 0; // 0 = unlimited
    size_t written = 0;
    bool   aborted = false;
};

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    auto* ctx = static_cast<WriteCtx*>(userdata);
    if (ctx->aborted || !ctx->fp) return 0;

    if (ctx->maxBytes > 0 && ctx->written + n > ctx->maxBytes) {
        ctx->aborted = true;
        return 0; // abort
    }
    size_t w = fwrite(ptr, 1, n, ctx->fp);
    ctx->written += w;
    return w;
}

// ---------------------- downloader ----------------------

bool PayloadSync::DownloadIfNewer(const Entry& e, const Config& cfg, std::string& outStatus) {
    const std::string absLocal = Utils::combinePath(Configuration::absolutePath, e.local);
    const std::string tmpPath = absLocal + ".tmp";
    fs::create_directories(fs::path(absLocal).parent_path());

#ifdef _WIN32
    FILE* fp = nullptr; fopen_s(&fp, tmpPath.c_str(), "wb");
#else
    FILE* fp = std::fopen(tmpPath.c_str(), "wb");
#endif
    if (!fp) { outStatus = "open tmp failed"; return false; }

    const std::string etag = ReadWholeText(e.etagPath);
    const std::string lastMod = ReadWholeText(e.lastModPath);

    HeaderState hs{};
    WriteCtx wctx{ fp, (e.maxBytes ? e.maxBytes : cfg.maxBytesDefault), 0, false };

    CURL* curl = curl_easy_init();
    if (!curl) { fclose(fp); std::error_code ec; fs::remove(tmpPath, ec); outStatus = "curl init failed"; return false; }

    struct curl_slist* headers = nullptr;
    if (!etag.empty()) {
        std::string h = "If-None-Match: " + etag;
        headers = curl_slist_append(headers, h.c_str());
    }
    else if (!lastMod.empty()) {
        std::string h = "If-Modified-Since: " + lastMod;
        headers = curl_slist_append(headers, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, e.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RetroFE-PayloadSync/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (rc != CURLE_OK) { std::error_code ec; fs::remove(tmpPath, ec); outStatus = std::string("curl error: ") + curl_easy_strerror(rc); return false; }
    if (wctx.aborted) { std::error_code ec; fs::remove(tmpPath, ec); outStatus = "aborted: max_bytes exceeded"; return false; }

    if (http == 304) { std::error_code ec; fs::remove(tmpPath, ec); outStatus = "Not modified"; return true; }
    if (http != 200) { std::error_code ec; fs::remove(tmpPath, ec); outStatus = "HTTP " + std::to_string(http); return false; }

#ifdef _WIN32
    { std::error_code ecr; fs::remove(absLocal, ecr); }
#endif
    std::error_code ec;
    fs::rename(tmpPath, absLocal, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(tmpPath, absLocal, fs::copy_options::overwrite_existing, ec);
        std::error_code ec2; fs::remove(tmpPath, ec2);
        if (ec) { outStatus = "atomic replace failed"; return false; }
    }

    if (!hs.etag.empty())        WriteWholeText(e.etagPath, hs.etag);
    if (!hs.lastModified.empty()) WriteWholeText(e.lastModPath, hs.lastModified);

    outStatus = "Updated";
    return true;
}

// ---------------------- runners ----------------------

bool PayloadSync::RunWithConfig(const Config& cfg, bool dryRun) {
    const std::string path = cfg.ResolvePayloadPath();
    auto entries = ParseFile(path);
    if (entries.empty()) {
        LOG_INFO("Payload", "No entries.");
        return true;
    }

    bool okAll = true;
    for (auto& e : entries) {
        if (!AllowUrl(e.url, cfg) || !SafeLocalRel(e.local)) {
            LOG_WARNING("Payload", "Policy skip: " << e.url << " -> " << e.local);
            continue;
        }

        if (dryRun) {
            LOG_INFO("Payload", "[DRY] " << e.url << " -> " << e.local);
            continue;
        }

        std::string msg;
        bool ok = DownloadIfNewer(e, cfg, msg);
        okAll &= ok;

        LOG_INFO("Payload", e.url << " -> " << e.local << " : " << msg);
    }
    return okAll;
}


bool PayloadSync::RunFromFile(const std::string& payloadPath, bool dryRun) {
    Config cfg; cfg.payloadPath = payloadPath; // keep other defaults
    return RunWithConfig(cfg, dryRun);
}

/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */

 /* This file is part of RetroFE.
  * GPLv3+: https://www.gnu.org/licenses/
  */

#include "MetadataDatabase.h"
#include "../Collection/CollectionInfo.h"
#include "../Collection/Item.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "Configuration.h"
#include "DB.h"
#include "GlobalOpts.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <cstring>

#include <curl/curl.h>
#include <rapidxml.hpp>
#include <rapidxml_print.hpp>
#include <sqlite3.h>

namespace fs = std::filesystem;

// ==========================
// Internal helpers (anonymous)
// ==========================
namespace {

    // ---------- HTTP + caching ----------

    struct HttpHeaders {
        std::string etag;
        std::string lastModified;
    };

    // Trim long values in logs
    static std::string preview(const char* s, size_t max = 64) {
        if (!s) return "(null)";
        std::string out(s);
        if (out.size() > max) { out.resize(max); out += "..."; }
        return out;
    }

    static size_t writeFileCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        std::ofstream* out = static_cast<std::ofstream*>(userdata);
        out->write(ptr, static_cast<std::streamsize>(size * nmemb));
        return size * nmemb;
    }

    static size_t captureHeadersCb(char* buffer, size_t size, size_t nitems, void* userdata) {
        const size_t bytes = size * nitems;
        HttpHeaders* h = static_cast<HttpHeaders*>(userdata);

        std::string_view line(buffer, bytes);
        auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string key(line.substr(0, colon));
            std::string val(line.substr(colon + 1));
            auto trim = [](std::string& s) {
                auto issp = [](unsigned char c) { return std::isspace(c); };
                while (!s.empty() && issp((unsigned char)s.front())) s.erase(s.begin());
                while (!s.empty() && issp((unsigned char)s.back()))  s.pop_back();
                };
            for (auto& c : key) c = (char)std::tolower((unsigned char)c);
            trim(val);
            if (key == "etag")            h->etag = val;
            else if (key == "last-modified") h->lastModified = val;
        }
        return bytes;
    }

    static std::unordered_map<std::string, std::string> loadSidecar(const fs::path& p) {
        std::unordered_map<std::string, std::string> kv;
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) {
            auto pos = line.find('=');
            if (pos != std::string::npos)
                kv.emplace(line.substr(0, pos), line.substr(pos + 1));
        }
        return kv;
    }

    static void saveSidecar(const fs::path& p, const HttpHeaders& h) {
        std::ofstream out(p, std::ios::trunc);
        if (!h.etag.empty())         out << "ETag=" << h.etag << "\n";
        if (!h.lastModified.empty()) out << "Last-Modified=" << h.lastModified << "\n";
    }

    // Quick sanity: looks like a HyperList (root <menu>)
    static bool looksLikeHyperlistXml(const fs::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::vector<char> buf((std::istreambuf_iterator<char>(f)), {});
        buf.push_back('\0');
        try {
            rapidxml::xml_document<> doc;
            doc.parse<0>(buf.data());
            return doc.first_node("menu") != nullptr;
        }
        catch (...) {
            return false;
        }
    }

    // Download remote to 'destFile' only if newer (ETag/Last-Modified).
    // Writes to a temp file first; on HTTP 200, atomically replaces destFile.
    // On 304 or error, leaves destFile untouched.
    // Returns {changed?, httpCode}
    static std::pair<bool, long> fetchIfNewer(const std::string& url,
        const fs::path& destFile,
        const fs::path& sidecar,
        std::string* errOut) {

        auto prior = loadSidecar(sidecar);
        LOG_INFO("Metadata", "HTTP check: " + url + " -> " + destFile.string());

        CURL* curl = curl_easy_init();
        if (!curl) { if (errOut) *errOut = "curl_easy_init failed"; return { false, 0 }; }

        HttpHeaders respHdrs;
        struct curl_slist* hdrs = nullptr;
        auto addHdr = [&](const std::string& h) { hdrs = curl_slist_append(hdrs, h.c_str()); };

        if (auto it = prior.find("ETag"); it != prior.end() && !it->second.empty())
            addHdr("If-None-Match: " + it->second);
        if (auto it = prior.find("Last-Modified"); it != prior.end() && !it->second.empty())
            addHdr("If-Modified-Since: " + it->second);

        if (!prior.empty()) {
            LOG_INFO("Metadata", "  sending validators: If-None-Match=" +
                (prior.count("ETag") ? prior["ETag"] : "(none)") +
                " If-Modified-Since=" +
                (prior.count("Last-Modified") ? prior["Last-Modified"] : "(none)"));
        }

        fs::create_directories(destFile.parent_path());

        fs::path tmp = destFile; tmp += ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (errOut) *errOut = "open tmp failed: " + tmp.string();
            curl_easy_cleanup(curl);
            if (hdrs) curl_slist_free_all(hdrs);
            return { false, 0 };
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "RetroFE/1.0 (+libcurl)");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, captureHeadersCb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &respHdrs);

        CURLcode cc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

        out.flush();
        out.close();

        auto rmTmp = [&]() { std::error_code ec; fs::remove(tmp, ec); };

        if (cc != CURLE_OK) {
            rmTmp();
            if (errOut) *errOut = curl_easy_strerror(cc);
            LOG_WARNING("Metadata", "  fetch failed: " + std::string(curl_easy_strerror(cc)));
            curl_easy_cleanup(curl);
            if (hdrs) curl_slist_free_all(hdrs);
            return { false, code };
        }

        if (code == 304) {
            LOG_INFO("Metadata", "  HTTP 304 Not Modified (cache up to date)");
            rmTmp();
            curl_easy_cleanup(curl);
            if (hdrs) curl_slist_free_all(hdrs);
            return { false, code };
        }

        if (code != 200) {
            rmTmp();
            if (errOut) { std::ostringstream os; os << "HTTP " << code; *errOut = os.str(); }
            LOG_WARNING("Metadata", "  unexpected HTTP " + std::to_string(code));
            curl_easy_cleanup(curl);
            if (hdrs) curl_slist_free_all(hdrs);
            return { false, code };
        }

        size_t bytes = 0;
        { std::error_code ec; bytes = fs::file_size(tmp, ec); }

        LOG_INFO("Metadata", "  HTTP 200 OK, received " + std::to_string(bytes) + " bytes");
        if (!respHdrs.etag.empty() || !respHdrs.lastModified.empty()) {
            LOG_INFO("Metadata", "  response validators: ETag=" + (respHdrs.etag.empty() ? "(none)" : respHdrs.etag) +
                " Last-Modified=" + (respHdrs.lastModified.empty() ? "(none)" : respHdrs.lastModified));
        }

        if (!looksLikeHyperlistXml(tmp)) {
            rmTmp();
            if (errOut) *errOut = "remote content failed HyperList validation";
            LOG_WARNING("Metadata", "  validation failed (not a HyperList <menu>)");
            curl_easy_cleanup(curl);
            if (hdrs) curl_slist_free_all(hdrs);
            return { false, code };
        }

        saveSidecar(sidecar, respHdrs);

        // Atomic replace
        std::error_code ec;
        fs::remove(destFile, ec);
        ec = {};
        fs::rename(tmp, destFile, ec);
        if (ec) {
            fs::copy_file(tmp, destFile, fs::copy_options::overwrite_existing, ec);
            rmTmp();
        }

        LOG_INFO("Metadata", "  cached remote -> " + destFile.string());
        curl_easy_cleanup(curl);
        if (hdrs) curl_slist_free_all(hdrs);
        return { true, code };
    }

    // Remote-authoritative tags
    static inline bool isForceOverwriteTag(const char* tag) {
        return std::strcmp(tag, "iscoredid") == 0 || std::strcmp(tag, "iscoredtype") == 0;
    }

    static bool hasNonEmptyText(const rapidxml::xml_node<>* n) {
        if (!n) return false;
        const char* v = n->value();
        if (!v) return false;
        const char* s = v;
        while (*s && std::isspace((unsigned char)*s)) ++s;
        if (*s == '\0') return false;
        const char* e = v + std::strlen(v);
        while (e > s && std::isspace((unsigned char)*(e - 1))) --e;
        return e > s;
    }

    // ---------- Merge (no overwrites of local values) ----------

    static const char* MERGEABLE_TAGS[] = {
        "description","year","players","ctrltype","manufacturer","developer",
        "genre","buttons","joyways","rating","iscoredid","iscoredtype",
        "score","cloneof"
    };

    struct MergeOptions {
        bool treatEmptyAsMissing = true;  // consider "" as missing
        bool appendNewGames = true;  // add remote-only <game>
    };

    static bool isMissingOrEmpty(rapidxml::xml_node<>* n, bool treatEmptyAsMissing) {
        if (!n) return true;
        if (!treatEmptyAsMissing) return false;
        const char* v = n->value();
        if (!v) return true;
        const char* s = v;
        while (*s && std::isspace((unsigned char)*s)) ++s;
        if (*s == '\0') return true;
        const char* e = v + std::strlen(v);
        while (e > s && std::isspace((unsigned char)*(e - 1))) --e;
        return (e == s);
    }

    static rapidxml::xml_node<>* findGameByName(rapidxml::xml_node<>* menu, const std::string& name) {
        for (auto* n = menu->first_node("game"); n; n = n->next_sibling("game")) {
            if (auto* a = n->first_attribute("name")) {
                if (name == a->value()) return n;
            }
        }
        return nullptr;
    }

    static rapidxml::xml_node<>* ensureChildWithText(rapidxml::xml_document<>& doc,
        rapidxml::xml_node<>* parent,
        const char* tag,
        const char* text) {
        if (!parent) return nullptr;

        // Always remove the old node if it exists.
        if (auto* child = parent->first_node(tag)) {
            parent->remove_node(child);
        }

        // Allocate and append a brand new node.
        char* tagA = doc.allocate_string(tag);
        char* valA = doc.allocate_string(text ? text : "");
        auto* node = doc.allocate_node(rapidxml::node_element, tagA, valA);
        parent->append_node(node);
        return node;
    }

    static rapidxml::xml_node<>* deepCloneNode(rapidxml::xml_document<>& toDoc,
        const rapidxml::xml_node<>* src) {
        if (!src) return nullptr;
        char* name = toDoc.allocate_string(src->name());
        char* val = toDoc.allocate_string(src->value());
        auto* dst = toDoc.allocate_node(src->type(), name, val);

        for (auto* a = src->first_attribute(); a; a = a->next_attribute()) {
            char* an = toDoc.allocate_string(a->name());
            char* av = toDoc.allocate_string(a->value());
            dst->append_attribute(toDoc.allocate_attribute(an, av));
        }
        for (auto* c = src->first_node(); c; c = c->next_sibling()) {
            dst->append_node(deepCloneNode(toDoc, c));
        }
        return dst;
    }

    static bool mergeGame(rapidxml::xml_document<>& localDoc,
        rapidxml::xml_node<>* localMenu,
        const rapidxml::xml_node<>* remoteGame,
        const MergeOptions& opt) {
        if (!localMenu || !remoteGame) return false;

        auto* nameAttr = remoteGame->first_attribute("name");
        if (!nameAttr || !nameAttr->value() || !*nameAttr->value()) return false;
        const std::string gname = nameAttr->value();

        auto* localGame = findGameByName(localMenu, gname);
        if (!localGame) {
            if (!opt.appendNewGames) return false;
            localMenu->append_node(deepCloneNode(localDoc, remoteGame));
            LOG_INFO("Metadata", "merge: added missing game '" + gname + "'");
            return true;
        }

        bool changed = false;
        for (const char* tag : MERGEABLE_TAGS) {
            auto* localTag = localGame->first_node(tag);
            auto* remoteTag = remoteGame->first_node(tag);

            if (isForceOverwriteTag(tag)) {
                // Remote is authoritative for these tags
                if (remoteTag) {
                    const char* rv = remoteTag->value() ? remoteTag->value() : "";
                    const char* lv = localTag && localTag->value() ? localTag->value() : "";
                    if (!localTag || std::strcmp(lv, rv) != 0) {
                        LOG_INFO("Metadata", "merge: [" + gname + "] force '" + std::string(tag) +
                            "' '" + preview(lv) + "' -> '" + preview(rv) + "'");
                        ensureChildWithText(localDoc, localGame, tag, rv);
                        changed = true;
                    }
                }
                else {
                    if (localTag) {
                        LOG_INFO("Metadata", "merge: [" + gname + "] remove '" + std::string(tag) +
                            "' (missing in remote)");
                        localGame->remove_node(localTag);
                        changed = true;
                    }
                }
                continue;
            }

            // Fill-only rule
            if (remoteTag && isMissingOrEmpty(localTag, opt.treatEmptyAsMissing) && hasNonEmptyText(remoteTag)) {
                LOG_INFO("Metadata", "merge: [" + gname + "] fill missing '" + std::string(tag) +
                    "' -> '" + preview(remoteTag->value()) + "'");
                ensureChildWithText(localDoc, localGame, tag, remoteTag->value());
                changed = true;
            }
        }
        return changed;
    }


    // Merge remote XML file into local XML; write result to outPath.
    // Returns true if merged content differs from original local.
    static int countGamesWithName(rapidxml::xml_node<>* menu, const std::string& name) {
        int cnt = 0;
        for (auto* n = menu->first_node("game"); n; n = n->next_sibling("game")) {
            if (auto* a = n->first_attribute("name")) {
                if (name == a->value()) ++cnt;
            }
        }
        return cnt;
    }

    static std::string getNodeText(const rapidxml::xml_node<>* p, const char* tag) {
        if (!p) return {};
        if (auto* n = p->first_node(tag)) {
            const char* v = n->value();
            return v ? std::string(v) : std::string();
        }
        return {};
    }

    static bool mergeHyperlistFiles(const fs::path& localPath,
        const fs::path& remotePath,
        const fs::path& outPath,
        const MergeOptions& opt) {
        if (!fs::exists(localPath) || !fs::exists(remotePath)) {
            LOG_WARNING("Metadata", "merge: missing file(s): local=" + localPath.string() +
                " remote=" + remotePath.string());
            return false;
        }

        // load local
        std::ifstream lf(localPath, std::ios::binary);
        std::vector<char> lb((std::istreambuf_iterator<char>(lf)), {});
        lb.push_back('\0');

        // load remote
        std::ifstream rf(remotePath, std::ios::binary);
        std::vector<char> rb((std::istreambuf_iterator<char>(rf)), {});
        rb.push_back('\0');

        rapidxml::xml_document<> ldoc, rdoc;
        try { ldoc.parse<0>(lb.data()); rdoc.parse<0>(rb.data()); }
        catch (const std::exception& e) {
            LOG_ERROR("Metadata", std::string("merge: parse failure: ") + e.what());
            return false;
        }
        catch (...) {
            LOG_ERROR("Metadata", "merge: parse failure (unknown)");
            return false;
        }

        auto* lmenu = ldoc.first_node("menu");
        auto* rmenu = rdoc.first_node("menu");
        if (!lmenu || !rmenu) {
            LOG_ERROR("Metadata", "merge: missing <menu> in local or remote");
            return false;
        }

        // Duplicate detection (optional but useful while debugging)
        // Walk remote names and see if local has dupes
        for (auto* rg = rmenu->first_node("game"); rg; rg = rg->next_sibling("game")) {
            auto* a = rg->first_attribute("name");
            if (!a || !a->value() || !*a->value()) continue;
            std::string gname = a->value();
            int dups = countGamesWithName(lmenu, gname);
            if (dups > 1) {
                LOG_WARNING("Metadata", "merge: DUPLICATE local entries for [" + gname + "]: " + std::to_string(dups));
            }
        }

        bool changed = false;
        int visited = 0, modifiedGames = 0, addedGames = 0;

        // Slightly extended mergeGame to tell us if we added the whole game
        auto mergeGameWithStats = [&](rapidxml::xml_node<>* remoteGame)->bool {
            ++visited;
            auto* nameAttr = remoteGame->first_attribute("name");
            if (!nameAttr || !nameAttr->value() || !*nameAttr->value()) return false;
            const std::string gname = nameAttr->value();

            auto* localGame = findGameByName(lmenu, gname);
            if (!localGame) {
                if (!opt.appendNewGames) return false;
                lmenu->append_node(deepCloneNode(ldoc, remoteGame));
                ++addedGames;
                LOG_INFO("Metadata", "merge: added new game [" + gname + "]");
                return true;
            }

            // Run the regular merge logic + count if anything changed for this game
            bool before = false;
            // We reuse the existing mergeGame but we also detect if it changed this particular game.
            // Implement inline to see game-local delta:
            bool localChanged = false;
            for (const char* tag : MERGEABLE_TAGS) {
                auto* localTag = localGame->first_node(tag);
                auto* remoteTag = remoteGame->first_node(tag);

                if (isForceOverwriteTag(tag)) {
                    if (remoteTag) {
                        const char* rv = remoteTag->value() ? remoteTag->value() : "";
                        const char* lv = (localTag && localTag->value()) ? localTag->value() : "";
                        if (!localTag || std::strcmp(lv, rv) != 0) {
                            LOG_INFO("Metadata", "merge: [" + gname + "] force '" + std::string(tag) +
                                "' '" + (lv ? lv : "") + "' -> '" + rv + "'");
                            ensureChildWithText(ldoc, localGame, tag, rv);
                            localChanged = true;
                        }
                    }
                    else if (localTag) {
                        LOG_INFO("Metadata", "merge: [" + gname + "] remove '" + std::string(tag) + "' (missing on remote)");
                        localGame->remove_node(localTag);
                        localChanged = true;
                    }
                    continue;
                }

                if (remoteTag && isMissingOrEmpty(localTag, opt.treatEmptyAsMissing) && hasNonEmptyText(remoteTag)) {
                    ensureChildWithText(ldoc, localGame, tag, remoteTag->value());
                    LOG_INFO("Metadata", "merge: [" + gname + "] fill '" + std::string(tag) + "' -> '" +
                        (remoteTag->value() ? remoteTag->value() : "") + "'");
                    localChanged = true;
                }
            }
            if (localChanged) ++modifiedGames;
            return localChanged;
            };

        for (auto* rg = rmenu->first_node("game"); rg; rg = rg->next_sibling("game")) {
            changed |= mergeGameWithStats(rg);
        }

        if (!changed) {
            LOG_INFO("Metadata", "merge: no changes (visited=" + std::to_string(visited) + ")");
            return false;
        }

        // --- serialize to outPath ---
        LOG_INFO("Metadata", "merge: writing \"" + outPath.string() + "\" …");
        std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            LOG_ERROR("Metadata", "merge: cannot open for write: " + outPath.string());
            return false;
        }
        rapidxml::print(static_cast<std::ostream&>(ofs), ldoc); // default formatting
        ofs.flush();
        ofs.close();

        // --- verify by reparsing the output and checking a known change if any ---
        // As a simple verification, if we modified any game via "force" we should be able to
        // find at least one such change in the written file. We reparse and check the value.
        {
            std::ifstream vr(outPath, std::ios::binary);
            std::vector<char> vb((std::istreambuf_iterator<char>(vr)), {});
            vb.push_back('\0');
            rapidxml::xml_document<> vdoc;
            try { vdoc.parse<0>(vb.data()); }
            catch (...) {
                LOG_WARNING("Metadata", "merge: verification parse failed for " + outPath.string());
            }

            // (Optional) spot-check: look up some known forced tag from this run if you cache one.
            // For now, just log a summary:
            auto* vmenu = vdoc.first_node("menu");
            int vcount = 0;
            if (vmenu) {
                for (auto* n = vmenu->first_node("game"); n; n = n->next_sibling("game")) ++vcount;
            }
            LOG_INFO("Metadata", "merge: wrote \"" + outPath.string() + "\" (games visited=" +
                std::to_string(visited) + ", changed=" + std::to_string(modifiedGames) +
                ", added=" + std::to_string(addedGames) + ", outGames=" + std::to_string(vcount) + ")");
        }

        return true;
    }



} // anon namespace

// ==========================
// MetadataDatabase methods
// ==========================

MetadataDatabase::MetadataDatabase(DB& db, Configuration& c)
    : config_(c)
    , db_(db) {
}

MetadataDatabase::~MetadataDatabase() = default;

bool MetadataDatabase::resetDatabase() {
    sqlite3* handle = db_.handle;
    char* error = nullptr;

    LOG_INFO("Metadata", "Erasing");

    const char* sql = "DROP TABLE IF EXISTS Meta;";
    if (sqlite3_exec(handle, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::stringstream ss;
        ss << "Unable to drop Metadata table. Error: " << (error ? error : "(null)");
        LOG_ERROR("Metadata", ss.str());
        return false;
    }
    return initialize();
}

bool MetadataDatabase::initialize() {
    // Always ensure schema exists (idempotent)
    {
        sqlite3* handle = db_.handle;
        char* error = nullptr;

        std::string sql;
        sql.append("CREATE TABLE IF NOT EXISTS Meta(");
        sql.append("collectionName TEXT KEY,");
        sql.append("name TEXT NOT NULL DEFAULT '',");
        sql.append("title TEXT NOT NULL DEFAULT '',");
        sql.append("year TEXT NOT NULL DEFAULT '',");
        sql.append("manufacturer TEXT NOT NULL DEFAULT '',");
        sql.append("developer TEXT NOT NULL DEFAULT '',");
        sql.append("genre TEXT NOT NULL DEFAULT '',");
        sql.append("cloneOf TEXT NOT NULL DEFAULT '',");
        sql.append("players TEXT NOT NULL DEFAULT '',");
        sql.append("ctrltype TEXT NOT NULL DEFAULT '',");
        sql.append("buttons TEXT NOT NULL DEFAULT '',");
        sql.append("joyways TEXT NOT NULL DEFAULT '',");
        sql.append("rating TEXT NOT NULL DEFAULT '',");
        sql.append("iscoredId TEXT NOT NULL DEFAULT '',");
        sql.append("iscoredType TEXT NOT NULL DEFAULT '',");
        sql.append("score TEXT NOT NULL DEFAULT '');");
        sql.append("CREATE UNIQUE INDEX IF NOT EXISTS MetaUniqueId ON Meta(collectionName, name);");

        if (sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            std::stringstream ss;
            ss << "Unable to create Metadata table. Error: " << (error ? error : "(null)");
            LOG_ERROR("Metadata", ss.str());
            return false;
        }
    }

    // Always check remotes; merge into local XMLs if remote is newer.
    const bool anyRemoteChanged = syncAllHyperlistRemotes_();

    // Import when DB is stale OR any XML changed on disk
    if (needsRefresh() || anyRemoteChanged) {
        importAllHyperlists_();
    }
    return true;
}

bool MetadataDatabase::updateAndImportHyperlist(const std::string& remoteRawUrl,
    const std::string& localXmlPath,
    const std::string& collectionName) {
    namespace fs = std::filesystem;
    fs::path local(localXmlPath);
    fs::path side = local; side += ".meta";
    fs::path cache = local; cache += ".remote.cache";

    LOG_INFO("Metadata", "update+import: " + localXmlPath + " <- " + remoteRawUrl);

    // 1) Fetch newest into cache
    std::string err;
    auto [downloaded, code] = fetchIfNewer(remoteRawUrl, cache, side, &err);
    if (!err.empty() && code != 304) {
        LOG_WARNING("Metadata", "update+import: fetch error: " + err +
            (code ? (" (HTTP " + std::to_string(code) + ")") : ""));
    }
    if (downloaded) {
        std::error_code ec; size_t sz = fs::file_size(cache, ec);
        LOG_INFO("Metadata", "update+import: cache updated (" + std::to_string(sz) + " bytes)");
    }
    else {
        LOG_INFO("Metadata", "update+import: cache unchanged (HTTP " + std::to_string(code) + ")");
    }

    // Fallback if no cache exists (rare)
    if (!fs::exists(cache)) {
        std::error_code ec; fs::remove(side, ec);
        err.clear();
        std::tie(downloaded, code) = fetchIfNewer(remoteRawUrl, cache, side, &err);
        if (!fs::exists(cache)) {
            LOG_WARNING("Metadata", "update+import: no cache available, importing local only");
            return importHyperlist(localXmlPath, collectionName);
        }
    }

    if (!looksLikeHyperlistXml(cache)) {
        LOG_WARNING("Metadata", "update+import: cache validation failed; importing local only");
        return importHyperlist(localXmlPath, collectionName);
    }

    // 2) Merge cache -> local
    fs::path merged = local; merged += ".merged";
    MergeOptions mopt; mopt.treatEmptyAsMissing = true; mopt.appendNewGames = true;

    bool wrote = mergeHyperlistFiles(local, cache, merged, mopt);
    std::error_code ec;
    if (wrote) {
        // optional backup
        fs::path bak = local; bak += ".bak";
        if (!fs::exists(bak)) {
            std::error_code ecB;
            fs::copy_file(local, bak, fs::copy_options::skip_existing, ecB);
        }
        fs::remove(local, ec);
        ec = {};
        fs::rename(merged, local, ec);
        if (ec) {
            fs::copy_file(merged, local, fs::copy_options::overwrite_existing, ec);
            fs::remove(merged, ec);
        }
        LOG_INFO("Metadata", "update+import: merged remote into " + local.string());
    }
    else {
        fs::remove(merged, ec);
        LOG_INFO("Metadata", "update+import: no merge changes for " + local.string());
    }

    // 3) Import
    bool ok = importHyperlist(localXmlPath, collectionName);
    LOG_INFO("Metadata", std::string("update+import: import ") + (ok ? "OK" : "FAILED") +
        " (" + collectionName + ")");
    return ok;
}


void MetadataDatabase::injectMetadata(CollectionInfo* collection) {
    sqlite3* handle = db_.handle;
    sqlite3_stmt* stmt = nullptr;

    // items -> map for fast lookup
    const std::vector<Item*>* items = &collection->items;
    std::unordered_map<std::string, Item*> itemMap;
    itemMap.reserve(items->size());
    for (auto* item : *items) itemMap.try_emplace(item->name, item);

    if (sqlite3_prepare_v2(handle,
        "SELECT DISTINCT Meta.name, Meta.title, Meta.year, Meta.manufacturer, Meta.developer, "
        "Meta.genre, Meta.players, Meta.ctrltype, Meta.buttons, Meta.joyways, Meta.cloneOf, "
        "Meta.rating, Meta.score, Meta.iscoredId, Meta.iscoredType "
        "FROM Meta WHERE collectionName=? ORDER BY title ASC;",
        -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR("Metadata", "Failed to prepare metadata query for injection.");
        return;
    }

    sqlite3_bind_text(stmt, 1, collection->metadataType.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* nameC = (const char*)sqlite3_column_text(stmt, 0);
        const char* fullTitleC = (const char*)sqlite3_column_text(stmt, 1);
        const char* yearC = (const char*)sqlite3_column_text(stmt, 2);
        const char* manufacturerC = (const char*)sqlite3_column_text(stmt, 3);
        const char* developerC = (const char*)sqlite3_column_text(stmt, 4);
        const char* genreC = (const char*)sqlite3_column_text(stmt, 5);
        const char* playersC = (const char*)sqlite3_column_text(stmt, 6);
        const char* ctrlTypeC = (const char*)sqlite3_column_text(stmt, 7);
        const char* buttonsC = (const char*)sqlite3_column_text(stmt, 8);
        const char* joyWaysC = (const char*)sqlite3_column_text(stmt, 9);
        const char* cloneOfC = (const char*)sqlite3_column_text(stmt, 10);
        const char* ratingC = (const char*)sqlite3_column_text(stmt, 11);
        const char* scoreC = (const char*)sqlite3_column_text(stmt, 12);
        const char* iscoredIdC = (const char*)sqlite3_column_text(stmt, 13);
        const char* iscoredTypeC = (const char*)sqlite3_column_text(stmt, 14);

        if (!nameC) continue;
        auto it = itemMap.find(nameC);
        if (it == itemMap.end()) continue;

        Item* item = it->second;
        item->title = fullTitleC ? fullTitleC : "";
        item->fullTitle = item->title;
        item->year = yearC ? yearC : "";
        item->manufacturer = manufacturerC ? manufacturerC : "";
        item->developer = developerC ? developerC : "";
        item->genre = genreC ? genreC : "";
        item->numberPlayers = playersC ? playersC : "";
        item->numberButtons = buttonsC ? buttonsC : "";
        item->ctrlType = ctrlTypeC ? ctrlTypeC : "";
        item->joyWays = joyWaysC ? joyWaysC : "";
        item->cloneof = cloneOfC ? cloneOfC : "";
        item->rating = ratingC ? ratingC : "";
        item->score = scoreC ? scoreC : "";
        item->iscoredId = iscoredIdC ? iscoredIdC : "";
        item->iscoredType = iscoredTypeC ? iscoredTypeC : "";
    }

    sqlite3_finalize(stmt);
}

bool MetadataDatabase::needsRefresh() {
    bool metaLock = false;
    config_.getProperty(OPTION_METALOCK, metaLock);
    if (metaLock) return false;

    sqlite3* handle = db_.handle;
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(handle, "SELECT COUNT(*) FROM Meta;", -1, &stmt, nullptr) != SQLITE_OK) {
        return true; // table missing
    }

    bool result = true;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);

        fs::path metaDbPath = Utils::combinePath(Configuration::absolutePath, "meta.db");
        fs::path exePath;
#ifdef WIN32
        exePath = Utils::combinePath(Configuration::absolutePath, "retrofe", "RetroFE.exe");
#else
        exePath = Utils::combinePath(Configuration::absolutePath, "RetroFE");
        if (!fs::exists(exePath)) {
            exePath = Utils::combinePath(Configuration::absolutePath, "retrofe");
        }
#endif
        auto metaDbTime = fs::exists(metaDbPath) ? fs::last_write_time(metaDbPath) : fs::file_time_type::min();
        auto exeTime = fs::exists(exePath) ? fs::last_write_time(exePath) : fs::file_time_type::min();
        auto metadirTime = timeDir(Utils::combinePath(Configuration::absolutePath, "meta"));

        result = (count == 0 || metaDbTime < metadirTime || exeTime < metadirTime);
    }

    sqlite3_finalize(stmt);
    return result;
}

bool MetadataDatabase::importHyperlist(const std::string& hyperlistFile, const std::string& collectionName) {
    config_.setProperty("status", "Scraping data from \"" + hyperlistFile + "\"");

    std::ifstream file(hyperlistFile.c_str());
    if (!file) {
        LOG_ERROR("Metadata", "Could not open file: " + hyperlistFile);
        return false;
    }

    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    buffer.push_back('\0');

    rapidxml::xml_document<> doc;
    try {
        doc.parse<0>(buffer.data());
        rapidxml::xml_node<> const* root = doc.first_node("menu");
        if (!root) {
            LOG_ERROR("Metadata", "Does not appear to be a HyperList file (missing <menu> tag)");
            return false;
        }

        sqlite3* handle = db_.handle;
        char* error = nullptr;
        if (sqlite3_exec(handle, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &error) != SQLITE_OK) {
            LOG_ERROR("Metadata", std::string("SQL begin failed: ") + (error ? error : "(null)"));
            return false;
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT OR REPLACE INTO Meta "
            "(name, title, year, manufacturer, developer, genre, players, ctrltype, buttons, joyways, "
            " cloneOf, collectionName, rating, score, iscoredId, iscoredType) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

        if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            LOG_ERROR("Metadata", "SQL Error preparing insert statement");
            sqlite3_exec(handle, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }

        auto getV = [](rapidxml::xml_node<> const* parent, const char* tag) -> const char* {
            auto* n = parent->first_node(tag);
            return n ? n->value() : "";
            };

        for (auto const* game = root->first_node("game"); game; game = game->next_sibling("game")) {
            const char* name = (game->first_attribute("name") ? game->first_attribute("name")->value() : "");
            if (!name || name[0] == '\0') continue;

            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, getV(game, "description"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, getV(game, "year"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, getV(game, "manufacturer"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, getV(game, "developer"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, getV(game, "genre"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, getV(game, "players"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, getV(game, "ctrltype"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 9, getV(game, "buttons"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 10, getV(game, "joyways"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 11, getV(game, "cloneof"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 12, collectionName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 13, getV(game, "rating"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 14, getV(game, "score"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 15, getV(game, "iscoredid"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 16, getV(game, "iscoredtype"), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                LOG_ERROR("Metadata", "SQL Error executing insert");
                sqlite3_finalize(stmt);
                sqlite3_exec(handle, "ROLLBACK;", nullptr, nullptr, nullptr);
                return false;
            }
            sqlite3_reset(stmt);
        }

        sqlite3_finalize(stmt);
        sqlite3_exec(handle, "COMMIT;", nullptr, nullptr, nullptr);
        config_.setProperty("status", "Saved data from \"" + hyperlistFile + "\"");
        return true;
    }
    catch (rapidxml::parse_error& e) {
        auto line = static_cast<long>(std::count(buffer.begin(), buffer.begin() + (e.where<char>() - &buffer.front()), '\n') + 1);
        std::stringstream ss;
        ss << "Could not parse hyperlist file. [Line: " << line << "] Reason: " << e.what();
        LOG_ERROR("Metadata", ss.str());
    }
    catch (std::exception& e) {
        LOG_ERROR("Metadata", std::string("Could not parse hyperlist file. Reason: ") + e.what());
    }
    return false;
}

fs::file_time_type MetadataDatabase::timeDir(const std::string& path) {
    fs::file_time_type lastTime = fs::file_time_type::min();
    if (!fs::exists(path)) return lastTime;

    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (!fs::is_regular_file(entry) && !fs::is_directory(entry)) continue;
        auto t = fs::last_write_time(entry);
        if (t > lastTime) lastTime = t;
    }
    return lastTime;
}

bool MetadataDatabase::importAllHyperlists_() {
    const std::string hyperListPath = Utils::combinePath(Configuration::absolutePath, "meta", "hyperlist");

    if (!fs::exists(hyperListPath) || !fs::is_directory(hyperListPath)) {
        LOG_WARNING("MetadataDatabase", "Could not read directory \"" + hyperListPath + "\"");
        return true; // not fatal
    }

    size_t imported = 0;
    for (const auto& entry : fs::directory_iterator(hyperListPath)) {
        if (!fs::is_regular_file(entry) || entry.path().extension() != ".xml") continue;

        const std::string importFile = entry.path().string();
        const std::string basename = entry.path().stem().string();
        const std::string collection = basename.substr(0, basename.find_first_of('.'));

        LOG_INFO("Metadata", "Importing hyperlist: " + importFile);
        if (importHyperlist(importFile, collection)) {
            ++imported;
        }
    }

    LOG_INFO("Metadata", "HyperList import complete (" + std::to_string(imported) + " file(s)).");
    return true;
}

// Fetch+merge pass that runs every startup.
// Looks for *.xml with sidecar ".remote" next to "MAME.xml" or "MAME".
bool MetadataDatabase::syncAllHyperlistRemotes_() {
    namespace fs = std::filesystem;
    const std::string hyperListPath = Utils::combinePath(Configuration::absolutePath, "meta", "hyperlist");
    if (!fs::exists(hyperListPath) || !fs::is_directory(hyperListPath)) return false;

    bool anyChanged = false;

    for (const auto& entry : fs::directory_iterator(hyperListPath)) {
        if (!fs::is_regular_file(entry) || entry.path().extension() != ".xml") continue;

        const fs::path xmlPath = entry.path();

        // Accept "MAME.xml.remote" and "MAME.remote"
        fs::path remote1 = xmlPath; remote1 += ".remote";
        fs::path remote2 = xmlPath.parent_path() / (xmlPath.stem().string() + ".remote");
        fs::path remoteSidecar;
        if (fs::exists(remote1)) remoteSidecar = remote1;
        else if (fs::exists(remote2)) remoteSidecar = remote2;
        else continue;

        std::ifstream r(remoteSidecar);
        std::string url; std::getline(r, url);
        if (url.empty()) continue;

        // Persistent remote cache + validators
        fs::path cache = xmlPath; cache += ".remote.cache";
        fs::path side = xmlPath; side += ".meta";

        // Pull newest into cache (304 keeps existing cache intact)
        std::string err;
        auto [downloaded, code] = fetchIfNewer(url, cache, side, &err);
        if (!err.empty() && code != 304) {
            LOG_WARNING("Metadata", "Fetch " + url + " : " + err + (code ? (" (HTTP " + std::to_string(code) + ")") : ""));
        }

        // If we somehow have no cache (e.g., first run but server replied 304),
        // force a fresh download once by deleting the sidecar and retrying.
        if (!fs::exists(cache)) {
            std::error_code ec;
            fs::remove(side, ec);
            err.clear();
            std::tie(downloaded, code) = fetchIfNewer(url, cache, side, &err);
            if (!fs::exists(cache)) continue; // give up for now
        }

        if (!looksLikeHyperlistXml(cache)) continue;

        // Merge cache -> local (add missing games/tags, never overwrite values)
        fs::path merged = xmlPath; merged += ".merged";
        MergeOptions mopt; mopt.treatEmptyAsMissing = true; mopt.appendNewGames = true;

        bool wrote = mergeHyperlistFiles(xmlPath, cache, merged, mopt);
        std::error_code ec;
        if (wrote) {
            // optional one-time backup
            fs::path bak = xmlPath; bak += ".bak";
            if (!fs::exists(bak)) {
                std::error_code ecB;
                fs::copy_file(xmlPath, bak, fs::copy_options::skip_existing, ecB);
            }
            fs::remove(xmlPath, ec);
            ec = {};
            fs::rename(merged, xmlPath, ec);
            if (ec) {
                fs::copy_file(merged, xmlPath, fs::copy_options::overwrite_existing, ec);
                fs::remove(merged, ec);
            }
            anyChanged = true;
            LOG_INFO("Metadata", "Merged remote into " + xmlPath.string());
        }
        else {
            fs::remove(merged, ec);
        }
    }
    return anyChanged;
}

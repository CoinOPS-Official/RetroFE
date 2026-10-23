#include "../Collection/CollectionInfo.h"
#include "../Collection/CollectionInfoBuilder.h"
#include "../Collection/Item.h"
#include "../Collection/ItemSort.h"
#include "../Collection/TitleCleanup.h"
#include "../Collection/MenuParser.h"
#include "../Collection/PlaylistRules.h"
#include "../Database/Configuration.h"
#include "../Database/DB.h"
#include "../Database/MetadataDatabase.h"
#include "../Utility/Utils.h"
#include "../Utility/PayloadSync.h"
#include "../Utility/DirectoryContents.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>

std::vector<std::string> settingsFromCLI;

namespace {
void require(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.good(), "Read saved playlist");
    std::vector<std::string> result;
    for (std::string line; std::getline(input, line);) result.push_back(line);
    return result;
}

void selectionChecks() {
    const std::vector<std::string> cycle{"all", "favorites", "lastplayed"};
    auto choose = [&](const std::string& current, const std::vector<std::string>& usable) {
        return PlaylistRules::findAttractPlaylist(cycle, current, [&](const std::string& candidate) {
            return std::find(usable.begin(), usable.end(), candidate) != usable.end();
        });
    };
    require(choose("favorites", {"favorites", "lastplayed"}) == cycle.begin() + 1,
        "Keep the already-advanced playlist when usable");
    require(choose("favorites", {"lastplayed"}) == cycle.begin() + 2,
        "Skip consecutive excluded or missing playlists");
    require(choose("lastplayed", {"all"}) == cycle.begin(), "Wrap the playlist cycle");
    require(choose("outside-cycle", {"favorites"}) == cycle.begin() + 1,
        "Search from the beginning when current is absent");
    int visited = 0;
    require(PlaylistRules::findAttractPlaylist(cycle, "all", [&](const std::string&) {
        ++visited;
        return false;
    }) == cycle.end() && visited == 3, "Exhaust an unusable cycle exactly once");
    const std::vector<std::string> empty;
    require(PlaylistRules::findAttractPlaylist(empty, "all", [](const std::string&) {
        require(false, "Never dereference an empty cycle");
        return true;
    }) == empty.end(), "Handle an empty cycle");
    const std::vector<std::string> single{"all"};
    require(PlaylistRules::findAttractPlaylist(single, "all", [](const std::string&) {
        return false;
    }) == single.end(), "Handle a single skipped playlist");

    require(PlaylistRules::collectionIsSkipped("Arcade,Console,Handheld", "Console"),
        "Match individual CSV collection names");
    require(PlaylistRules::collectionIsSkipped("Arcade,Console,Handheld", "Handheld"),
        "Match the final CSV collection name");
    require(!PlaylistRules::collectionIsSkipped("Arcade,Console", "Arc"), "Do not match prefixes");
    require(!PlaylistRules::collectionIsSkipped("", "Arcade"), "Empty skip list permits history");
    require(PlaylistRules::collectionIsSkipped("Arcade", "Arcade"), "Preserve single-name matching");
}

void menuChecks(Configuration& config, const std::filesystem::path& root) {
    CollectionInfo menu(config, "Menu", "", "", "", "");
    MenuParser parser;
    std::vector<std::string> names(64, "Same");
    names.insert(names.end(), {"zeta", "Alpha", "same", "beta"});
    parser.buildMenuFromCollectionLaunchers(&menu, names);
    require(menu.items.size() == names.size(), "Retain duplicate menu titles");
    require(std::is_sorted(menu.items.begin(), menu.items.end(), [](const Item* a, const Item* b) {
        return Utils::toLower(a->fullTitle) < Utils::toLower(b->fullTitle);
    }), "Sort launcher titles case-insensitively");
    for (auto* item : menu.items) delete item;
    menu.items.clear();

    const auto directory = root / "collections" / "Menu" / "menu";
    std::filesystem::create_directories(directory);
    for (const auto* name : {"zeta.txt", "Alpha.txt", "beta.txt"}) std::ofstream(directory / name);
    require(parser.buildMenuItems(&menu, true), "Build directory menu");
    require(menu.items.size() == 3 && menu.items[0]->name == "Alpha" &&
        menu.items[1]->name == "beta" && menu.items[2]->name == "zeta", "Sort directory menu titles");
    for (auto* item : menu.items) delete item;
    menu.items.clear();
    std::ofstream(root / "collections" / "Menu" / "menu.txt") << "zeta\nAlpha\nbeta\n";
    require(parser.buildMenuItems(&menu, false), "Build explicit unsorted menu");
    require(menu.items[0]->name == "zeta" && menu.items[1]->name == "Alpha",
        "Preserve explicit menu.txt order");
    for (auto* item : menu.items) delete item;
    menu.items.clear();
}

void historyChecks(Configuration& config, const std::filesystem::path& root) {
    DB db(":memory:");
    MetadataDatabase metadata(db, config);
    CollectionInfoBuilder builder(config, metadata);
    CollectionInfo arcade(config, "Arcade", "", "", "", "");
    CollectionInfo console(config, "Console", "", "", "", "");
    std::vector<std::unique_ptr<Item>> owned;
    auto makeItem = [&](CollectionInfo& collection, const std::string& name) {
        auto entry = std::make_unique<Item>();
        entry->name = entry->title = entry->fullTitle = name;
        entry->collectionInfo = &collection;
        entry->leaf = true;
        auto* result = entry.get();
        owned.push_back(std::move(entry));
        return result;
    };
    auto* launched = makeItem(arcade, "new");
    auto* local = makeItem(arcade, "same");
    auto* foreign = makeItem(console, "same");
    auto* underscore = makeItem(arcade, "_literal");
    arcade.items = {launched, local, foreign, underscore};
    const auto path = root / "collections" / "Arcade" / "playlists" / "lastplayed.txt";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "_Console:same\nsame\n_Arcade:new\n_literal\n";
    config.setProperty("globalFavLast", false);
    config.setProperty("lastPlayedSortType", std::string("alpha"));
    builder.updateLastPlayedPlaylist(&arcade, launched, 10);
    require(readLines(path) == std::vector<std::string>{"new", "_Console:same", "same", "_literal"},
        "Preserve qualified and local history while moving the launched game to the front");
    const auto& list = *arcade.playlists.at("lastplayed");
    require(list.size() == 4 && std::find(list.begin(), list.end(), foreign) != list.end() &&
        std::find(list.begin(), list.end(), local) != list.end(), "Resolve same-named games by collection");
    require(list.front() == underscore && launched->playCount == 1,
        "Preserve configured alpha display sorting and increment play count once");
    const auto before = readLines(path);
    builder.updateLastPlayedPlaylist(&arcade, launched, 0);
    require(readLines(path) == before && launched->playCount == 1, "Size zero disables history updates");

    config.setProperty("globalFavLast", true);
    const auto global = root / "collections" / "Favorites" / "playlists" / "lastplayed.txt";
    std::filesystem::create_directories(global.parent_path());
    arcade.items = {launched, local};
    std::ofstream(global) << "_Console:same\n_Arcade:same\n_Arcade:new\n_Handheld:older\n";
    builder.updateLastPlayedPlaylist(&arcade, launched, 3);
    require(readLines(global) == std::vector<std::string>{"_Arcade:new", "_Console:same", "_Arcade:same"},
        "Retain unloaded collections in global history and enforce the total size limit");
    require(arcade.playlists.at("lastplayed")->size() == 2,
        "Do not add unresolved foreign games to the active playlist");
    std::ofstream(global) << "_Console:same\n_Arcade:same\n";
    builder.updateLastPlayedPlaylist(&arcade, launched, 1);
    require(readLines(global) == std::vector<std::string>{"_Arcade:new"}, "Size one retains only the latest game");
}

void loadingChecks(Configuration& config, const std::filesystem::path& root) {
    config.setProperty("globalFavLast", false);
    DB db(":memory:");
    MetadataDatabase metadata(db, config);
    CollectionInfoBuilder builder(config, metadata);
    const auto collection = root / "collections" / "Load";
    std::filesystem::create_directories(collection / "roms" / "nested");
    std::ofstream(collection / "include.txt") << "beta\nalpha\nmissing\nbeta\n";
    std::ofstream(collection / "exclude.txt") << "missing\n";
    for (const auto* name : {"alpha.zip", "alpha.7z", "ignored.zip"})
        std::ofstream(collection / "roms" / name);
    for (const auto* name : {"alpha.zip", "beta.zip"})
        std::ofstream(collection / "roms" / "nested" / name);
    config.setProperty("collections.Load.list.extensions", std::string("zip,7z"));
    config.setProperty("collections.Load.list.romHierarchy", true);
    auto destroyItems = [](CollectionInfo& info) {
        for (auto* item : info.items) delete item;
        info.items.clear();
    };
    std::unique_ptr<CollectionInfo> scanned(builder.buildCollection("Load"));
    require(scanned->items.size() == 2, "Deduplicate ROM names across extensions and recursive directories");
    std::set<std::string> names;
    for (auto* item : scanned->items) names.insert(item->name);
    require(names == std::set<std::string>{"alpha", "beta"}, "Respect include and exclude filters");
    destroyItems(*scanned);
    config.setProperty("collections.Load.list.includeMissingItems", true);
    std::unique_ptr<CollectionInfo> missing(builder.buildCollection("Load"));
    require(missing->items.size() == 2 && missing->items[0]->name == "beta" && missing->items[1]->name == "alpha",
        "Keep first include-file order when showing missing items");
    destroyItems(*missing);
    std::filesystem::create_directories(root / "collections" / "Master");
    const auto sub = root / "collections" / "Master" / "Load.sub";
    std::ofstream(sub) << "gamma\ngamma\n";
    std::unique_ptr<CollectionInfo> merged(builder.buildCollection("Load", "Master"));
    require(merged->items.size() == 1 && merged->items[0]->name == "gamma",
        "A populated sub-file takes precedence over the collection include file");
    destroyItems(*merged);
    std::ofstream(sub) << "";
    std::unique_ptr<CollectionInfo> fallback(builder.buildCollection("Load", "Master"));
    require(fallback->items.size() == 2 && fallback->items[0]->name == "beta",
        "An empty sub-file falls back to the collection include file");
    destroyItems(*fallback);

    CollectionInfo arcade(config, "Scope", "", "", "", "");
    CollectionInfo console(config, "Console", "", "", "", "");
    Item local, foreign, tail, duplicate;
    local.name = foreign.name = duplicate.name = "same";
    tail.name = "tail";
    local.collectionInfo = tail.collectionInfo = duplicate.collectionInfo = &arcade;
    foreign.collectionInfo = &console;
    arcade.items = {&local, &foreign, &tail, &duplicate};
    const auto playlists = root / "playlist-index";
    std::filesystem::create_directories(playlists);
    std::ofstream(playlists / "favorites.txt") << "_Console:same\nsame\n*\n_Console:*\nsame\n";
    std::ofstream(playlists / "x") << "ignored";
    std::map<std::string, Item*> playlistItems;
    builder.loadPlaylistItems(&arcade, &playlistItems, playlists.string());
    require(*arcade.playlists.at("favorites") == std::vector<Item*>{&foreign, &local, &local, &tail, &duplicate, &foreign},
        "Indexing preserves first matches, collection-qualified names, wildcard order and overlaps");
    require(local.isFavorite && foreign.isFavorite && duplicate.isFavorite, "Wildcard favorites retain all matching items");
    for (auto& [name, item] : playlistItems) delete item;
}

void playlistTitleChecks(const std::filesystem::path& root) {
    Configuration config;
    config.setProperty("firstCollection", std::string("TitleNames"));
    config.setProperty("cyclePlaylist", std::string("max,lastplayed,favorites,retroPC,old school,plain,equals"));
    DB db(":memory:");
    MetadataDatabase metadata(db, config);
    CollectionInfoBuilder builder(config, metadata);
    const auto path = root / "playlist-titles";
    std::filesystem::create_directories(path);
    for (const auto* id : {"max", "lastplayed", "favorites", "retroPC", "old school", "plain", "equals"})
        std::ofstream(path / (std::string(id) + ".txt")) << "game\n";
    auto verify = [&](bool mapped) {
        CollectionInfo info(config, "TitleNames", "", "", "", "");
        Item game; game.name = "game"; game.collectionInfo = &info; info.items = {&game};
        std::map<std::string, Item*> entries;
        builder.loadPlaylistItems(&info, &entries, path.string());
        require(entries.size() == 7, "titles.conf is metadata, not an extra playlist");
        for (const auto& [id, entry] : entries) {
            require(entry->name == id, "Display titles retain stable playlist identifiers");
            require(*info.playlists.at(id) == std::vector<Item*>{&game},
                "Display titles do not change playlist membership");
            require(entry->title == info.getPlaylistTitle(id) && entry->fullTitle == entry->title,
                "Playlist menus and display-title lookup agree");
        }
        require(entries.at("plain")->title == "plain", "Unmapped playlist falls back to its identifier");
        require(info.getPlaylistTitle("unknown") == "unknown", "Unknown title lookup falls back safely");
        if (mapped) {
            require(entries.at("max")->title == "All", "Filename keys and last valid duplicate take effect");
            require(entries.at("lastplayed")->title == "Last Played", "Extensionless keys are accepted");
            require(entries.at("favorites")->title == "Favorites", "Generated playlists support display titles");
            require(entries.at("retroPC")->title == "Retro PC", "Filename case is preserved");
            require(entries.at("old school")->title == "Caf\xc3\xa9 Classics", "UTF-8 display titles and spaced keys survive");
            require(entries.at("equals")->title == "Top = Picks #1", "Title punctuation is retained");
            require(info.getPlaylistTitle("all") == "Every Game", "Synthetic playlists can have display titles");
        } else {
            require(entries.at("max")->title == "max", "Missing titles.conf retains existing behavior");
        }
        for (const auto& [id, entry] : entries) delete entry;
    };
    verify(false);
    std::ofstream(path / "titles.conf", std::ios::binary)
        << "\xef\xbb\xbf# Display titles\r\n"
           "  ; another comment\r\n"
           "max = Earlier title\r\n"
           " max.txt = All \r\n"
           "max.txt =   \r\n"
           "lastplayed = Last Played\r\n"
           "favorites.txt = Favorites\r\n"
           "retroPC.txt = Retro PC\r\n"
           "old school.txt = Caf\xc3\xa9 Classics\r\n"
           "equals.txt = Top = Picks #1\r\n"
           "all.txt = Every Game\r\n"
           "../plain.txt = wrong\r\n"
           "= ignored\r\n"
           "invalid line\r\n";
    verify(true);
    std::filesystem::remove(path / "titles.conf");
    verify(false);
}

void metadataAndSortChecks(Configuration& config, const std::filesystem::path& root) {
    const auto directory = root / "item-info";
    std::filesystem::create_directories(directory / "nested");
    std::ofstream(directory / "default.conf") << "shared=default\nexisting=default\n";
    std::ofstream(directory / "one.conf") << "shared=item\nunique=one\n";
    std::ofstream(directory / "nested" / "two.conf") << "unique=two\n";
    Item one, two, absent, referenceOne, referenceTwo, referenceAbsent;
    one.name = referenceOne.name = "one";
    two.name = referenceTwo.name = "nested/two";
    absent.name = referenceAbsent.name = "absent";
    one.setInfo("existing", "original");
    referenceOne.setInfo("existing", "original");
    const std::vector<Item*> optimized{&one, &two, &absent};
    const std::vector<Item*> reference{&referenceOne, &referenceTwo, &referenceAbsent};
    for (auto* item : reference) {
        item->loadInfo((directory / "default.conf").string());
        item->loadInfo((directory / (item->name + ".conf")).string());
    }
    Item::loadCollectionInfo(optimized, directory.string());
    for (std::size_t i = 0; i < optimized.size(); ++i)
        require(optimized[i]->info_ == reference[i]->info_, "Shared parsing preserves existing metadata precedence");
    Item::loadCollectionInfo(optimized, (root / "absent-info").string());
    require(one.info_ == referenceOne.info_, "Missing info directory leaves existing values intact");
#ifdef _WIN32
    std::ofstream(directory / "MIXED.CONF") << "unique=mixed\n";
    Item mixed;
    mixed.name = "mixed";
    Item::loadCollectionInfo({&mixed}, directory.string());
    require(mixed.info_.at("unique") == "mixed", "Preserve case-insensitive Windows filename matching");
#endif

    CollectionInfo info(config, "Sort", "", "", "", "");
    one.collectionInfo = two.collectionInfo = absent.collectionInfo = &info;
    one.fullTitle = "Zeta";
    two.fullTitle = "alpha";
    absent.fullTitle = "Beta";
    info.items = {&one, &two, &absent};
    info.sortItems();
    require(info.items == std::vector<Item*>{&two, &absent, &one}, "Keep case-insensitive title sorting");
    one.fullTitle = "aardvark";
    info.sortItems();
    require(info.items.front() == &one, "Title edits are visible on the next sort");
    one.year = "2000";
    two.year = "1980";
    absent.year = "1990";
    sortItemsByKeys(info.items, "year", true);
    require(info.items == std::vector<Item*>{&two, &absent, &one}, "Preserve ascending metadata sorting");
    one.lastPlayed = "100";
    two.lastPlayed = "300";
    absent.lastPlayed = "200";
    sortItemsByKeys(info.items, "lastplayed", true);
    require(info.items == std::vector<Item*>{&two, &absent, &one}, "Preserve descending history sorting");
    one.lastPlayed = "400";
    sortItemsByKeys(info.items, "lastplayed", true);
    require(info.items.front() == &one, "Updated history values are visible on the next sort");
}

void startupAndTitleChecks(Configuration& config, const std::filesystem::path& root) {
    const auto directory = root / "directory-snapshot";
    std::filesystem::create_directories(directory / "launchers.local");
    std::ofstream(directory / "settings.conf") << "";
    std::ofstream(directory / "launchers.linux.local") << "not a directory";
    const DirectoryContents contents(directory);
    require(contents.contains("settings.conf") && !contents.contains("settings15.conf"),
        "Directory snapshot finds existing files and missing overrides");
    require(contents.isDirectory("launchers.local") && !contents.isDirectory("launchers.linux.local") &&
        !contents.isDirectory("absent"), "Directory snapshot distinguishes directories from files");
#ifdef _WIN32
    require(contents.contains("SETTINGS.CONF"), "Directory snapshot preserves Windows case matching");
#endif
    const DirectoryContents missing(root / "absent-directory");
    require(!missing.contains("settings.conf"), "Missing directory snapshot handles absent files");

    CollectionInfo collection(config, "Titles", "", "", "", "");
    Item complete, unmatched, nested;
    complete.title = complete.fullTitle = "Game (USA) [Rev A] (Set 2)";
    unmatched.title = "Game (USA) (unfinished";
    nested.title = "Game (outer(inner) tail)";
    collection.items = {&complete, &unmatched, &nested};
    collection.playlists["all"] = &collection.items;
    collection.playlists["favorites"] = new std::vector<Item*>{&complete, &unmatched};
    cleanPlaylistTitles(collection.playlists, true, true);
    require(complete.title == complete.fullTitle, "Enabled title groups remain visible");
    cleanPlaylistTitles(collection.playlists, false, true);
    require(complete.title == "Game  [Rev A] " && unmatched.title == "Game  (unfinished",
        "Hide complete parenthesis groups while preserving whitespace, brackets and unmatched trailing text");
    require(nested.title == "Game  tail)", "Preserve first-closing-delimiter semantics for nested groups");
    cleanPlaylistTitles(collection.playlists, true, false);
    require(complete.title == "Game   " && complete.fullTitle == "Game (USA) [Rev A] (Set 2)",
        "Square bracket cleanup affects display title only");
    const auto previous = complete.title;
    cleanPlaylistTitles(collection.playlists, false, false);
    require(complete.title == previous, "Shared playlist items remain correct after repeated cleanup");
}
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("retrofe-collection-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto previous = Configuration::absolutePath;
    Configuration::absolutePath = root.string();
    Configuration config;
    selectionChecks();
    menuChecks(config, root);
    historyChecks(config, root);
    loadingChecks(config, root);
    playlistTitleChecks(root);
    metadataAndSortChecks(config, root);
    startupAndTitleChecks(config, root);
    PayloadSync::Config payload;
    payload.payloadPath = (root / "missing-payload.txt").string();
    std::atomic<bool> cancelled{true};
    require(!PayloadSync::RunWithConfig(payload, false, &cancelled), "Cancelled sync stops before doing file or network work");
    Configuration::absolutePath = previous;
    std::filesystem::remove_all(root);
    std::cout << "Collection regression checks passed\n";
}

# Playlist display titles

Place an optional titles.conf in a collection's playlists directory:

    # Playlist filename = displayed title
    max.txt = All
    lastplayed.txt = Last Played
    retroPC.txt = Retro PC
    old school.txt = Old School

Keys match playlist filenames exactly, including case; .txt can be omitted.
Spaces around keys and values are ignored. Values may contain UTF-8, = and #.
Blank lines and full-line # or ; comments are ignored.
The last valid duplicate wins; blank titles and malformed entries do not hide a playlist.
An unmapped playlist keeps its original identifier as its displayed title.

Only display titles change. Playlist files, cyclePlaylist, artwork lookup,
sorting rules, saved selection identifiers and game membership retain the original names.
Layout textFormat is applied after the title is resolved.
Both scrolling text menus and ordinary playlist text components use the mapping.
The mapping is per collection and loaded when playlists are loaded; restart RetroFE after editing.
Favorites/history regeneration does not rewrite titles.conf.
Older builds ignore this .conf file.

The current Pi's primary mapping is:
/home/user/AriseMicro/collections/Arcade/playlists/titles.conf

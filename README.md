# sqltorrent

Sqltorrent is a custom VFS for sqlite which allows applications to query an sqlite database contained within a torrent.
Queries can be processed immediately after the database has been opened, even though the database file is still being downloaded.
Pieces of the file which are required to complete a query are prioritized so that queries complete reasonably quickly even if only a small fraction of the whole database has been downloaded.

# Building

The Visual Studio projects expect Boost and libtorrent to be available in the "boost" and "libtorrent-rasterbar" directories adjacent to the sqltorrent directory.
The Boost Build jam file can be configured to build against the installed versions of Boost and libtorrent (the default) or to look for them in the directories indicated by the environment variables BOOST_ROOT and TORRENT_ROOT by setting the use-boost and use-torrent build parameters to source.

# Using

WARNING: sqltorrent is currently NOT thread safe. Only one query may be outstanding to any database using sqltorrent.

If your application is using sqlite directly via the C/C++ interface you can call sqltorrent_init(0) to register the VFS. You can then use the VFS by calling sqlite3_open_v2 and passing the path to the .torrent file as the filename and "torrent" as the zVfs name.

If your application is using sqlite via bindings which expose the sqlite3_load_extension function you can use that to load sqltorrent built as a shared library.
Because loading an extension at runtime requires an open database, you will need to open an empty in-memory database and use that to load sqltorrent.
Once loaded sqltorrent will become the default VFS so any non-torrented databases you want to use must be opened before you load the extension.
The in-memory database you use to load sqltorrent must be kept open for as long as you have open databases using sqltorrent.

# Creating torrents

Sqltorrent currently only supports torrents containing a single sqlite database file.
For efficiency the piece size of the torrent should be kept fairly small, around 32KB.
It is also recommended to set the page size equal to the piece size when creating the sqlite database.

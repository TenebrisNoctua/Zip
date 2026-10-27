# Zip
A zip archive library for Luwow-Project: listing an archive's entries, hashing them and extracting them.

This repository is not stand-alone. You must build it externally from within another
repository like https://github.com/Luwow-Project/Release

## Project Overview

The archive is memory-mapped, and stored and deflated entries are read with the library's own inflate, zip64 archives included. Paths are UTF-8 and relative paths resolve against the working directory, whose path handling and digests it shares. Every function raises an error on failure.

## Library Documentation

### `zip.read(path: string) -> { { name: string, size: number, packedSize: number, isDirectory: boolean, isSymlink: boolean, mode: number } }`

A zip archive's entries, in the order its directory lists them. `mode` holds the Unix permission bits and `isSymlink` marks Unix symlinks, for archives made on Unix-like systems such as macOS; both are 0 and false otherwise. Stored and deflated entries are supported, zip64 archives included, with the library's own inflate. The archive is memory-mapped instead of read, so only the pages being used are loaded.

### `zip.hashEntries(path: string, names: { string }?, algorithm: string?) -> { [string]: string }`

The lowercase hex digest of each entry's contents, every file's when `names` is nil. `algorithm` can be `"md5"`, `"blake2b-128"` or `"blake2b-256"`. Each entry is checked against its CRC.

### `zip.extract(path: string, targets: { [string]: string | { string } } | string) -> { written: number, failed: { string } }`

Writes each named entry to its path or paths, making their folders and replacing existing files. Given a folder instead of targets, writes every entry into it as the archive lays them out; an archive with an entry that would end up outside the folder (such as `../x`) raises before anything is written. An entry is inflated once, however many paths it goes to. Paths that can't be written, such as files in use, are returned in `failed`; a damaged archive or a missing entry raises.
-- This is the type system for the Zip API as it exists in C++ code.

--[=[
	An entry of a zip archive, returned by `zip.read`.
]=]
export type Entry = {
	-- As the archive names it, with the separators it uses.
	name: string,
	size: number,
	packedSize: number,
	isDirectory: boolean,
	-- A Unix symlink, from an archive made on a Unix-like system
	isSymlink: boolean,
	-- The Unix permission bits
	mode: number
}

--[=[
	Extraction result from `zip.extract`.
]=]
export type Extraction = {
	written: number,
	failed: { string }
}

export type HashAlgorithm = "md5" | "blake2b-128" | "blake2b-256"

--[=[
	Options for `zip.pack`.
]=]
export type PackOptions = {
	-- Whether entries are deflated, each one only when that makes it smaller. True when left out.
	compress: boolean?
}

--[=[
	What `zip.pack` wrote, how many entries, and the archive's size in bytes.
]=]
export type Packing = {
	entries: number,
	size: number
}

local zip = {}

--[=[
	Lists a zip archive's entries, in the order its directory has them. Stored and deflated entries can
	be read, zip64 archives included. Yields the calling thread.

	@param path The archive.
]=]
function zip.read(path: string): { Entry }
    return nil :: any
end

--[=[
	Hashes the contents of a zip archive's entries, each checked against its CRC.

	@param path The archive.
	@param names The entries, every file's when nil.
	@param algorithm "md5" by default.
	@return Each entry's name and its lowercase hex digest.
]=]
function zip.hashEntries(path: string, names: { string }?, algorithm: HashAlgorithm?): { [string]: string }
    return nil :: any
end

--[=[
	Writes entries of a zip archive to files, making their folders and replacing what's there. The archive
	is mapped rather than read, and each entry is inflated once however many paths it goes to. Off the
	state. Raises when the archive is damaged or doesn't have an entry.

	@param path The archive.
	@param targets Each entry's name, and the path or paths it's written to. Or a folder: every entry is
	written into it as the archive lays them out, and an archive with an entry that would end up outside the
	folder, such as "../x", raises before anything is written.
]=]
function zip.extract(path: string, targets: { [string]: string | { string } } | string): Extraction
    return nil :: any
end

--[=[
	Writes a zip archive, replacing a file already there. Folders are entries of their own, so empty ones
	come back with zip.extract, and their contents are in name order. Names are UTF-8.

	@param path The archive to write.
	@param sources A folder: everything in it, named from the folder. Or each entry name, and the file or
	folder that goes under it. Names that would end up outside the folder zip.extract writes into, such as
	"../x", raise.
	@param options See `PackOptions`.
]=]
function zip.pack(path: string, sources: string | { [string]: string }, options: PackOptions?): Packing
    return nil :: any
end

return table.freeze(zip)

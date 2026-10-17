#pragma once

// File access, path helpers, and the directories a game uses for its data.
//
// Operations that call the operating system can block. Submit them through
// jobs::kick_io() so a waiting job can park its fiber instead of holding up a
// worker. Direct calls suit boot code and tools. Path inspection and normalize()
// work on the spelling alone and do not access the filesystem.
//
// Paths use UTF-8 and '/' separators. Backslashes are rejected so a path keeps
// the same meaning across platforms. Absolute paths start at '/' or a drive
// root such as "C:/".
//
// Input paths are borrowed StringViews. Output Strings use the caller's
// allocator and may grow; read_file() takes a separate allocator for its bytes.
// Fallible operations return Result values with a portable FileError.

#include <ember/containers/span.h>
#include <ember/containers/string.h>
#include <ember/core/enum.h>
#include <ember/core/result.h>
#include <ember/core/types.h>
#include <ember/memory/alignment.h>
#include <ember/memory/allocator.h>

#include <cstddef>

namespace ember::jobs
{
	struct JobDef;
}

namespace ember::fs
{
	// Engine path limit in bytes, including the terminating NUL. A platform may
	// impose a smaller limit; paths that exceed either report NameTooLong.
	inline constexpr size_t MAX_PATH_BYTES = 4096;

	// Portable failure codes keep callers independent of OS error values.
	// None means success; Unknown covers errors without a specific mapping.
	enum class FileError : u8
	{
		None = 0,
		Unknown,  // A system error without a more specific mapping.
		NotFound, // The path or a parent directory does not exist.
		AlreadyExists,
		AccessDenied, // Permissions, a read-only volume, or a sharing restriction.
		InvalidPath,  // Empty, a backslash, NUL, network share, or escape above the root.
		NameTooLong,  // Exceeds the engine or platform path limit.
		NotDirectory, // A path component was expected to be a directory.
		IsDirectory,  // A regular file was expected.
		DirectoryNotEmpty,
		NoSpace,
		TooLarge,    // Exceeds the caller's limit or the platform's supported size.
		EndOfFile,   // An exact read reached the end before filling its destination.
		Busy,        // A resource limit prevents opening a file or creating a temporary.
		Unsupported, // The platform cannot perform the operation, such as a cross-volume rename.
		Io,          // An I/O operation failed.
		Count
	};

	EMBER_ENUM_NAMES(FileError, "None", "Unknown", "NotFound", "AlreadyExists", "AccessDenied",
	    "InvalidPath", "NameTooLong", "NotDirectory", "IsDirectory", "DirectoryNotEmpty", "NoSpace",
	    "TooLarge", "EndOfFile", "Busy", "Unsupported", "Io");

	// Checks a path against the rules above without touching the disk. Empty, a NUL, a
	// backslash, a network share, a drive without its slash, and a length past MAX_PATH_BYTES
	// fail. Every call that takes a path checks it; data-driven callers can check first.
	Result<void, FileError> validate(StringView path);

	// An open handle with explicit ownership. A value-initialized File is closed.
	// Copying this record aliases the handle; it does not open another file or
	// create another owner. The caller that opened it must close it once.
	struct File
	{
		u64 handle = 0;

		explicit operator bool() const
		{
			return handle != 0;
		}
	};

	enum class OpenMode : u8
	{
		Read,      // Opens an existing file for reading.
		Write,     // Creates or truncates a file for writing.
		ReadWrite, // Creates if missing; otherwise preserves the contents.
		CreateNew, // Creates a file that must not exist yet, for writing; AlreadyExists otherwise.
	};

	// Opens a regular file with sharing that allows tools to replace assets during
	// hot reload. Directories report IsDirectory; devices and pipes report Unsupported.
	Result<File, FileError> open(StringView path, OpenMode mode);

	// Closes the handle and clears this File, making another close on it harmless.
	// Copies still alias the old handle. Deferred write errors can surface here,
	// so callers must check the result even after successful writes.
	Result<void, FileError> close(File& file);

	// Reports the current file length in bytes. Another writer can change it
	// after the call, so this is not a guarantee about a later read.
	Result<u64, FileError> size(File file);

	// Reads up to into.size() bytes at offset and returns the count. There is no
	// shared cursor, so threads can read independent ranges of the same file.
	// Use read_exact_at() when the destination must be filled; a single read
	// may return fewer bytes than requested.
	Result<size_t, FileError> read_at(File file, u64 offset, Span<u8> into);

	// Fills into from offset, continuing across short reads. Reports EndOfFile
	// if the file ends first. Do not treat the buffer as complete after a failure.
	Result<void, FileError> read_exact_at(File file, u64 offset, Span<u8> into);

	// Writes from at offset, extending the file when needed, and returns the count.
	// write_exact_at() continues across short writes until all bytes are written
	// or an error occurs.
	Result<size_t, FileError> write_at(File file, u64 offset, Span<const u8> from);
	Result<void, FileError> write_exact_at(File file, u64 offset, Span<const u8> from);

	// Requests that buffered writes reach persistent storage. This can be slow,
	// so flush at save boundaries. Persistence still depends on the filesystem
	// and storage device honoring the request.
	Result<void, FileError> flush(File file);

	// A whole-file buffer and the allocator and alignment used for its storage.
	// Copying the record aliases the bytes. It does not release them automatically;
	// keep the allocator alive until the buffer is released or its arena lifetime ends.
	struct FileData
	{
		u8* data = nullptr;
		size_t size = 0;
		size_t alignment = 0;
		memory::Allocator* allocator = nullptr;

		Span<u8> bytes() const
		{
			return {data, size};
		}

		// Borrows the bytes as text without appending a NUL. Parsers must use the
		// view's length and finish before the buffer is reclaimed.
		StringView text() const
		{
			return size == 0 ? StringView() : StringView((const char*)data, size);
		}
	};

	struct ReadFileDef
	{
		// Upper limit for a whole-file read. Larger files report TooLarge so callers
		// can stream them in bounded ranges instead of allocating one large buffer.
		u64 max_bytes = 256_mb;

		// Required alignment of the returned bytes. Must be a power of two so binary
		// data can be loaded into storage with the alignment its consumer needs.
		size_t alignment = DEFAULT_ALIGNMENT;
	};

	// Reads the file through allocator under tag, subject to def's size limit.
	// No text terminator is appended. The allocator must outlive the returned buffer.
	Result<FileData, FileError> read_file(StringView path, memory::Allocator* allocator,
	    memory::Tag tag = memory::Tag::Unknown, const ReadFileDef& def = {});

	// Returns the bytes to the allocator they came from and clears the record. An empty record
	// is harmless, and bytes from an arena need no call: the arena's lifetime takes them.
	void free_file(FileData& data);

	// Creates or truncates the target and writes its contents. Failure can leave
	// a partial file; use write_file_atomic() when readers need a complete version.
	Result<void, FileError> write_file(StringView path, Span<const u8> bytes);

	// Writes a sibling file, then renames it over the target so readers do not see
	// a partially written replacement. durable requests a flush before the rename
	// where supported. Persistence after a crash also depends on the platform
	// and filesystem.
	Result<void, FileError> write_file_atomic(StringView path, Span<const u8> bytes,
	    bool durable = true);

	enum class FileType : u8
	{
		Other = 0, // A device, pipe, socket, or another non-regular entry.
		Regular,
		Directory,
		Symlink, // Reported by enumerate(); status() follows the link.
		Count
	};

	EMBER_ENUM_NAMES(FileType, "Other", "Regular", "Directory", "Symlink");

	struct FileInfo
	{
		FileType type = FileType::Other;
		u64 size = 0;

		// Last content modification time, expressed as nanoseconds since the epoch.
		// Timestamp resolution depends on the platform and filesystem.
		i64 modified_ns = 0;
	};

	// Inspects the path, following symlinks to their targets. Use enumerate()
	// when the distinction between a link and its target matters.
	Result<FileInfo, FileError> status(StringView path);

	// Returns false for missing paths and for errors such as denied access.
	// Use status() when the caller needs to distinguish those cases.
	bool exists(StringView path);

	// One directory child. name has no parent path and is borrowed only for the
	// callback; copy it into owned storage if it must survive the call.
	struct DirectoryEntry
	{
		StringView name;
		FileType type = FileType::Other;
	};

	// Receives each child and the caller's user pointer. Return true to continue
	// or false to stop without reporting an error.
	using EnumerateFn = bool (*)(const DirectoryEntry& entry, void* user);

	// Visits children other than "." and ".." without allocating their names.
	// Order comes from the operating system and is not sorted. Stopping through
	// the callback is a successful enumeration.
	Result<void, FileError> enumerate(StringView directory, EnumerateFn fn, void* user);

	// Creates one directory. An existing directory is fine; a file in the way reports
	// AlreadyExists and a missing parent NotFound. create_directories() walks the path.
	Result<void, FileError> create_directory(StringView path);

	// Creates missing directories along the path. Existing directories are fine;
	// a file blocking the path reports AlreadyExists.
	Result<void, FileError> create_directories(StringView path);

	Result<void, FileError> remove_file(StringView path);

	// Removes an empty directory. A directory with children reports DirectoryNotEmpty.
	Result<void, FileError> remove_directory(StringView path);

	// Moves a file or directory, replacing an existing destination file.
	// Replacement is atomic within one volume; cross-volume moves report Unsupported.
	Result<void, FileError> rename(StringView from, StringView to);

	// Flushes a directory's entries to persistent storage, so that a rename a save depends on
	// survives a crash. Does nothing on a platform without such a call, where the file
	// system's journal covers the rename.
	Result<void, FileError> sync_directory(StringView path);

	bool is_absolute(StringView path);

	// Borrows the last path component. Roots and paths ending in a separator
	// have no file name and return an empty view.
	StringView file_name(StringView path);

	// Borrows the final dot and the suffix after it, or an empty view when absent.
	// A leading dot alone does not form an extension, so ".gitignore" has none.
	StringView extension(StringView path);

	// Borrows the file name with its extension removed.
	StringView stem(StringView path);

	// Returns the parent spelling: "anim" for "anim/walk.anim", "/" for "/walk.anim",
	// and an empty view for a bare name. A root is its own parent. A trailing
	// separator is removed rather than removing another component.
	StringView parent(StringView path);

	// Cleans the spelling without accessing the filesystem. Removes repeated
	// separators, ".", and trailing separators; ".." removes the preceding component.
	// Relative paths keep leading ".." components, but climbing above an absolute
	// root reports InvalidPath. An empty result becomes ".".
	// out may own the input view and is unchanged on failure.
	Result<void, FileError> normalize(StringView path, String& out);

	// Joins a relative right path beneath left and normalizes the result.
	// An absolute right path reports InvalidPath so it cannot silently replace
	// the caller's base path. out may own either input view.
	Result<void, FileError> join(StringView left, StringView right, String& out);

	// Resolves relative paths against the working directory and normalizes the
	// spelling. Does not resolve symlinks or require the target to exist.
	Result<void, FileError> absolute(StringView path, String& out);

	// Returns the working directory or the directory containing the executable
	// as an absolute, normalized path. out keeps the caller's allocator and may grow.
	Result<void, FileError> working_directory(String& out);
	Result<void, FileError> executable_directory(String& out);

	// Builds the per-user path for saves and settings, or for data the game can
	// regenerate. app must be one plain path component. These calls return a
	// location; the caller creates the directory when it is needed.
	Result<void, FileError> user_data_directory(StringView app, String& out);
	Result<void, FileError> user_cache_directory(StringView app, String& out);

	// Returns the platform's temporary directory. Callers manage the lifetime
	// and cleanup of the files they create there.
	Result<void, FileError> temporary_directory(String& out);

	// Requests adapt file operations to the I/O threads. The task writes its
	// result into the request, and a normal job counter reports completion.
	//
	//     fs::ReadFile read{.path = path, .allocator = memory::heap(),
	//                       .tag = memory::Tag::Assets};
	//     jobs::Counter done;
	//
	//     if (jobs::kick_io(fs::job(read), done) == jobs::IoKickError::None)
	//     {
	//         jobs::wait(done);
	//         if (read.result)
	//         {
	//             use(*read.result);
	//         }
	//     }
	//
	// Requests, borrowed paths and buffers, file handles, allocators, and counters
	// must stay valid until completion. Keep each request at a stable address and
	// do not inspect or modify it while its task runs. A refused submission leaves
	// the request unchanged, so check kick_io() before reading the result.
	//
	// Several requests can share a counter for one batch wait. Streams can keep a
	// file open and submit bounded ReadAt requests, giving higher-priority queued
	// work a chance between chunks. Set the JobDef priority before submission;
	// it does not preempt an operation already running.

	struct OpenFile
	{
		StringView path; // Keep the path storage alive until completion.
		OpenMode mode = OpenMode::Read;

		Result<File, FileError> result = {};
	};

	struct ReadFile
	{
		StringView path;                        // Keep the path storage alive until completion.
		memory::Allocator* allocator = nullptr; // Required; outlives the task and returned storage.
		memory::Tag tag = memory::Tag::Unknown;
		ReadFileDef def = {};

		Result<FileData, FileError> result = {}; // Caller owns the returned buffer.
	};

	// Reads up to into.size() bytes at offset and reports how many arrived.
	// Keep the file open and the destination alive until the task completes.
	struct ReadAt
	{
		File file = {};
		u64 offset = 0;
		Span<u8> into = {}; // Keep the destination alive until completion.

		Result<size_t, FileError> result = {};
	};

	struct WriteFile
	{
		StringView path;           // Keep the path storage alive until completion.
		Span<const u8> bytes = {}; // Keep the source bytes alive and unchanged until completion.
		bool atomic = true;
		bool durable = true;

		Result<void, FileError> result = {};
	};

	// Returns a job that performs the request and stores its result. The job
	// borrows the request rather than copying it. Submit through jobs::kick_io()
	// so blocking file operations run outside the worker scheduler.
	jobs::JobDef job(OpenFile& request);
	jobs::JobDef job(ReadFile& request);
	jobs::JobDef job(ReadAt& request);
	jobs::JobDef job(WriteFile& request);
}

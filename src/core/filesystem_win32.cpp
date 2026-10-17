// The Windows half of the file system: every public function that is one system call deep.
// Paths are checked against the engine's contract here, spelled the system's way in UTF-16 on
// the stack, behind the prefix that lifts the old 260 character limit, and never allocated.

#if defined(_WIN32)

	#include <ember/core/filesystem.h>
	#include <ember/core/log.h>
	#include <ember/core/macros.h>

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#include <windows.h>

	#include <shlobj.h>

	#include <cstring>
	#include <cwchar>
	#include <limits>
	#include <utility>

namespace ember::fs
{
	namespace
	{
		// The prefix that lifts the 260 character limit and makes the system take a path as it
		// is spelled, which wants an absolute path behind it.
		constexpr wchar_t LONG_PATH_PREFIX[] = L"\\\\?\\";
		constexpr size_t PREFIX_LENGTH = 4;

		// Every open shares everything, so a tool may replace what the engine is reading.
		constexpr DWORD SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

		// 100-nanosecond ticks from the system's epoch, 1601, to Unix's.
		constexpr u64 UNIX_EPOCH_TICKS = 116'444'736'000'000'000ull;

		// The system's error, sorted into the codes callers branch on.
		FileError classify(DWORD code)
		{
			switch (code)
			{
				case ERROR_FILE_NOT_FOUND:
				case ERROR_PATH_NOT_FOUND:
				case ERROR_INVALID_DRIVE:
					return FileError::NotFound;
				case ERROR_FILE_EXISTS:
				case ERROR_ALREADY_EXISTS:
					return FileError::AlreadyExists;
				case ERROR_ACCESS_DENIED:
				case ERROR_SHARING_VIOLATION:
				case ERROR_LOCK_VIOLATION:
				case ERROR_WRITE_PROTECT:
					return FileError::AccessDenied;
				case ERROR_INVALID_NAME:
				case ERROR_BAD_PATHNAME:
				case ERROR_NO_UNICODE_TRANSLATION:
					return FileError::InvalidPath;
				case ERROR_FILENAME_EXCED_RANGE:
				case ERROR_INSUFFICIENT_BUFFER:
				case ERROR_BUFFER_OVERFLOW:
					return FileError::NameTooLong;
				case ERROR_DIRECTORY:
					return FileError::NotDirectory;
				case ERROR_DIR_NOT_EMPTY:
					return FileError::DirectoryNotEmpty;
				case ERROR_DISK_FULL:
				case ERROR_HANDLE_DISK_FULL:
					return FileError::NoSpace;
				case ERROR_BUSY:
				case ERROR_TOO_MANY_OPEN_FILES:
				case ERROR_DELETE_PENDING:
				case ERROR_USER_MAPPED_FILE:
					return FileError::Busy;
				case ERROR_FILE_TOO_LARGE:
					return FileError::TooLarge;
				case ERROR_HANDLE_EOF:
					return FileError::EndOfFile;
				case ERROR_NOT_SAME_DEVICE:
				case ERROR_NOT_SUPPORTED:
				case ERROR_CALL_NOT_IMPLEMENTED:
				case ERROR_INVALID_FUNCTION:
					return FileError::Unsupported;
				case ERROR_CRC:
				case ERROR_IO_DEVICE:
				case ERROR_READ_FAULT:
				case ERROR_WRITE_FAULT:
				case ERROR_GEN_FAILURE:
					return FileError::Io;
				default:
					return FileError::Unknown;
			}
		}

		// The portable code for a system error. One the table has no name for is logged with its
		// number, which is all a reader of the log could use.
		FileError win32_error(DWORD code)
		{
			const FileError error = classify(code);
			if (error == FileError::Unknown)
			{
				EMBER_WARN("fs: system error {}", (u32)code);
			}

			return error;
		}

		FileError last_error()
		{
			return win32_error(GetLastError());
		}

		// The system's spelling of a path: checked against the engine's contract, UTF-16,
		// backslashes, absolute against the working directory, and the prefix in front.
		struct WidePath
		{
			wchar_t text[MAX_PATH_BYTES];

			Result<void, FileError> set(StringView path)
			{
				if (auto valid = validate(path); !valid)
				{
					return valid;
				}

				wchar_t relative[MAX_PATH_BYTES];
				const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
				    (int)path.size(), relative, (int)(MAX_PATH_BYTES - 1));
				if (count == 0)
				{
					return last_error();
				}

				relative[count] = L'\0';
				for (int index = 0; index < count; ++index)
				{
					if (relative[index] == L'/')
					{
						relative[index] = L'\\';
					}
				}

				const DWORD room = (DWORD)(MAX_PATH_BYTES - PREFIX_LENGTH);
				const DWORD written =
				    GetFullPathNameW(relative, room, text + PREFIX_LENGTH, nullptr);
				if (written == 0)
				{
					return last_error();
				}

				if (written >= room)
				{
					return FileError::NameTooLong;
				}

				std::memcpy(text, LONG_PATH_PREFIX, PREFIX_LENGTH * sizeof(wchar_t));
				return {};
			}
		};

		// Gives a system path to the caller's String in the engine's spelling: UTF-8, slashes,
		// no prefix, and no trailing separator but a root's.
		Result<void, FileError> give(String& out, const wchar_t* text, size_t count)
		{
			char buffer[MAX_PATH_BYTES];
			size_t length = 0;
			if (count != 0)
			{
				const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
				    (int)count, buffer, (int)(MAX_PATH_BYTES - 1), nullptr, nullptr);
				if (written == 0)
				{
					return last_error();
				}

				length = (size_t)written;
			}

			for (size_t index = 0; index < length; ++index)
			{
				if (buffer[index] == '\\')
				{
					buffer[index] = '/';
				}
			}

			const char* start = buffer;
			if (StringView(buffer, length).starts_with("//?/"))
			{
				start += PREFIX_LENGTH;
				length -= PREFIX_LENGTH;
			}

			while (length > 1 && start[length - 1] == '/' && !(length == 3 && start[1] == ':'))
			{
				--length;
			}

			out.assign(start, length);
			return {};
		}

		HANDLE handle_of(File file)
		{
			return (HANDLE)(uintptr_t)file.handle;
		}

		// A file's handle is never null, so the handle itself is the File's word.
		File file_of(HANDLE handle)
		{
			return File{(u64)(uintptr_t)handle};
		}

		i64 unix_ns(u64 ticks)
		{
			return ticks >= UNIX_EPOCH_TICKS ? (i64)(ticks - UNIX_EPOCH_TICKS) * 100
			                                 : -(i64)(UNIX_EPOCH_TICKS - ticks) * 100;
		}

		// The offset of a positional read or write. On a handle opened without
		// FILE_FLAG_OVERLAPPED the system takes the offset from here, runs the call to completion
		// and returns, which is the documented way to read at an offset without a shared cursor.
		OVERLAPPED at(u64 offset)
		{
			OVERLAPPED position = {};
			position.Offset = (DWORD)offset;
			position.OffsetHigh = (DWORD)(offset >> 32);
			return position;
		}

		// One call moves at most what a DWORD can say; the caller's loop takes the rest.
		DWORD chunk(size_t count)
		{
			constexpr size_t MOST = 0x7FFF'FFFF;
			return (DWORD)(count < MOST ? count : MOST);
		}

		// One of the directories the shell keeps for the user, in the engine's spelling.
		Result<void, FileError> known_folder(REFKNOWNFOLDERID folder, String& out)
		{
			PWSTR path = nullptr;
			const HRESULT result = SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path);
			if (FAILED(result))
			{
				EMBER_WARN("fs: known folder failed with {:#x}", (u32)result);
				return FileError::Unknown;
			}

			const Result<void, FileError> given = give(out, path, std::wcslen(path));
			CoTaskMemFree(path);
			return given;
		}

		// An app name is one plain component: no separator, neither dot name, none of the
		// characters this platform refuses in a name, and no trailing dot or space, which it
		// would drop.
		bool plain_component(StringView app)
		{
			if (app.empty() || app == "." || app == ".." || app.back() == '.' || app.back() == ' ')
			{
				return false;
			}

			for (const char character : app)
			{
				if ((unsigned char)character < 32 ||
				    StringView("/\\<>:\"|?*").find(character) != StringView::npos)
				{
					return false;
				}
			}

			return true;
		}
	}

	Result<File, FileError> open(StringView path, OpenMode mode)
	{
		WidePath wide;
		if (auto set = wide.set(path); !set)
		{
			return set.error;
		}

		DWORD access = 0;
		DWORD disposition = 0;
		switch (mode)
		{
			case OpenMode::Read:
				access = GENERIC_READ;
				disposition = OPEN_EXISTING;
				break;
			case OpenMode::Write:
				access = GENERIC_WRITE;
				disposition = CREATE_ALWAYS;
				break;
			case OpenMode::ReadWrite:
				access = GENERIC_READ | GENERIC_WRITE;
				disposition = OPEN_ALWAYS;
				break;
			case OpenMode::CreateNew:
				access = GENERIC_WRITE;
				disposition = CREATE_NEW;
				break;
		}

		const HANDLE handle = CreateFileW(wide.text, access, SHARE_ALL, nullptr, disposition,
		    FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			// A directory refuses with the same code a locked file does; tell them apart.
			const DWORD code = GetLastError();
			if (code == ERROR_ACCESS_DENIED)
			{
				const DWORD attributes = GetFileAttributesW(wide.text);
				if (attributes != INVALID_FILE_ATTRIBUTES &&
				    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					return FileError::IsDirectory;
				}
			}

			return win32_error(code);
		}

		if (GetFileType(handle) != FILE_TYPE_DISK)
		{
			(void)CloseHandle(handle);
			return FileError::Unsupported;
		}

		return file_of(handle);
	}

	Result<void, FileError> close(File& file)
	{
		if (!file)
		{
			return {};
		}

		// Cleared first, so a failed close is not tried again.
		const HANDLE handle = handle_of(std::exchange(file, File{}));
		if (!CloseHandle(handle))
		{
			return last_error();
		}

		return {};
	}

	Result<u64, FileError> size(File file)
	{
		EMBER_ASSERT(file);

		LARGE_INTEGER found = {};
		if (!GetFileSizeEx(handle_of(file), &found))
		{
			return last_error();
		}

		return (u64)found.QuadPart;
	}

	Result<size_t, FileError> read_at(File file, u64 offset, Span<u8> into)
	{
		EMBER_ASSERT(file);

		// Offsets come from pack tables, which are data: a range that wraps is an error, not a
		// bug.
		if (into.size() > std::numeric_limits<u64>::max() - offset ||
		    offset > (u64)std::numeric_limits<LONGLONG>::max())
		{
			return FileError::TooLarge;
		}

		if (into.empty())
		{
			return size_t{0};
		}

		OVERLAPPED position = at(offset);
		DWORD transferred = 0;
		if (!ReadFile(handle_of(file), into.data(), chunk(into.size()), &transferred, &position))
		{
			// Past the end is not a failure: there was nothing there to read.
			const DWORD code = GetLastError();
			if (code == ERROR_HANDLE_EOF)
			{
				return size_t{0};
			}

			return win32_error(code);
		}

		return (size_t)transferred;
	}

	Result<size_t, FileError> write_at(File file, u64 offset, Span<const u8> from)
	{
		EMBER_ASSERT(file);

		if (from.size() > std::numeric_limits<u64>::max() - offset ||
		    offset > (u64)std::numeric_limits<LONGLONG>::max())
		{
			return FileError::TooLarge;
		}

		if (from.empty())
		{
			return size_t{0};
		}

		OVERLAPPED position = at(offset);
		DWORD transferred = 0;
		if (!WriteFile(handle_of(file), from.data(), chunk(from.size()), &transferred, &position))
		{
			return last_error();
		}

		return (size_t)transferred;
	}

	Result<void, FileError> flush(File file)
	{
		EMBER_ASSERT(file);

		if (!FlushFileBuffers(handle_of(file)))
		{
			return last_error();
		}

		return {};
	}

	Result<FileInfo, FileError> status(StringView path)
	{
		WidePath wide;
		if (auto set = wide.set(path); !set)
		{
			return set.error;
		}

		// Through a handle rather than the attributes by name, so that a link is followed to
		// what it names, as stat() does. Backup semantics is what lets a directory open.
		const HANDLE handle = CreateFileW(wide.text, FILE_READ_ATTRIBUTES, SHARE_ALL, nullptr,
		    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			return last_error();
		}

		FILE_BASIC_INFO basic = {};
		FILE_STANDARD_INFO standard = {};
		const bool queried =
		    GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) &&
		    GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard));
		const FileError failure = queried ? FileError::None : last_error();
		const DWORD kind = GetFileType(handle);
		(void)CloseHandle(handle);

		if (!queried)
		{
			return failure;
		}

		FileInfo info;
		if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			info.type = FileType::Directory;
		}
		else if (kind == FILE_TYPE_DISK)
		{
			info.type = FileType::Regular;
			info.size = (u64)standard.EndOfFile.QuadPart;
		}

		info.modified_ns = unix_ns((u64)basic.LastWriteTime.QuadPart);
		return info;
	}

	Result<void, FileError> enumerate(StringView directory, EnumerateFn fn, void* user)
	{
		EMBER_ASSERT(fn != nullptr);

		WidePath wide;
		if (auto set = wide.set(directory); !set)
		{
			return set;
		}

		// The search is the directory and a star.
		const size_t length = std::wcslen(wide.text);
		if (length + 2 >= MAX_PATH_BYTES)
		{
			return FileError::NameTooLong;
		}

		wide.text[length] = L'\\';
		wide.text[length + 1] = L'*';
		wide.text[length + 2] = L'\0';

		WIN32_FIND_DATAW item = {};
		const HANDLE find = FindFirstFileExW(wide.text, FindExInfoBasic, &item,
		    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
		if (find == INVALID_HANDLE_VALUE)
		{
			return last_error();
		}

		Result<void, FileError> result;
		char name[MAX_PATH_BYTES];
		bool more = true;
		while (more)
		{
			const size_t count = std::wcslen(item.cFileName);
			const bool dots =
			    (count == 1 && item.cFileName[0] == L'.') ||
			    (count == 2 && item.cFileName[0] == L'.' && item.cFileName[1] == L'.');
			if (!dots)
			{
				const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
				    item.cFileName, (int)count, name, (int)(sizeof(name) - 1), nullptr, nullptr);
				if (written == 0)
				{
					result = last_error();
					break;
				}

				FileType type = FileType::Regular;
				if ((item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				{
					type = FileType::Symlink;
				}
				else if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					type = FileType::Directory;
				}
				else if ((item.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0)
				{
					type = FileType::Other;
				}

				const DirectoryEntry entry{.name = StringView(name, (size_t)written), .type = type};
				if (!fn(entry, user))
				{
					break;
				}
			}

			more = FindNextFileW(find, &item) != 0;
			if (!more && GetLastError() != ERROR_NO_MORE_FILES)
			{
				result = last_error();
			}
		}

		(void)FindClose(find);
		return result;
	}

	Result<void, FileError> create_directory(StringView path)
	{
		WidePath wide;
		if (auto set = wide.set(path); !set)
		{
			return set;
		}

		if (CreateDirectoryW(wide.text, nullptr))
		{
			return {};
		}

		// Already there is fine when it is a directory.
		const DWORD code = GetLastError();
		if (code == ERROR_ALREADY_EXISTS)
		{
			const DWORD attributes = GetFileAttributesW(wide.text);
			if (attributes != INVALID_FILE_ATTRIBUTES &&
			    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				return {};
			}
		}

		return win32_error(code);
	}

	Result<void, FileError> remove_file(StringView path)
	{
		WidePath wide;
		if (auto set = wide.set(path); !set)
		{
			return set;
		}

		if (!DeleteFileW(wide.text))
		{
			return last_error();
		}

		return {};
	}

	Result<void, FileError> remove_directory(StringView path)
	{
		WidePath wide;
		if (auto set = wide.set(path); !set)
		{
			return set;
		}

		if (!RemoveDirectoryW(wide.text))
		{
			return last_error();
		}

		return {};
	}

	Result<void, FileError> rename(StringView from, StringView to)
	{
		WidePath source;
		WidePath target;
		if (auto set = source.set(from); !set)
		{
			return set;
		}

		if (auto set = target.set(to); !set)
		{
			return set;
		}

		if (!MoveFileExW(source.text, target.text, MOVEFILE_REPLACE_EXISTING))
		{
			return last_error();
		}

		return {};
	}

	Result<void, FileError> sync_directory(StringView path)
	{
		// There is no call for a directory's entries here; NTFS journals the rename itself. The
		// path is still checked, so a bad one fails the same way on every platform.
		return validate(path);
	}

	Result<void, FileError> working_directory(String& out)
	{
		wchar_t wide[MAX_PATH_BYTES];
		const DWORD count = GetCurrentDirectoryW((DWORD)MAX_PATH_BYTES, wide);
		if (count == 0)
		{
			return last_error();
		}

		// A count past the buffer is the size the buffer should have been.
		if (count >= MAX_PATH_BYTES)
		{
			return FileError::NameTooLong;
		}

		return give(out, wide, count);
	}

	Result<void, FileError> executable_directory(String& out)
	{
		wchar_t wide[MAX_PATH_BYTES];
		const DWORD count = GetModuleFileNameW(nullptr, wide, (DWORD)MAX_PATH_BYTES);
		if (count == 0)
		{
			return last_error();
		}

		// A full buffer means the path was cut to fit.
		if (count >= MAX_PATH_BYTES)
		{
			return FileError::NameTooLong;
		}

		if (auto given = give(out, wide, count); !given)
		{
			return given;
		}

		const StringView directory = parent(out);
		out.resize(directory.size());
		return {};
	}

	Result<void, FileError> user_data_directory(StringView app, String& out)
	{
		EMBER_ASSERT_MSG(plain_component(app), "an app name is one plain directory component");

		if (auto base = known_folder(FOLDERID_RoamingAppData, out); !base)
		{
			return base;
		}

		return join(out, app, out);
	}

	Result<void, FileError> user_cache_directory(StringView app, String& out)
	{
		EMBER_ASSERT_MSG(plain_component(app), "an app name is one plain directory component");

		if (auto base = known_folder(FOLDERID_LocalAppData, out); !base)
		{
			return base;
		}

		return join(out, app, out);
	}

	Result<void, FileError> temporary_directory(String& out)
	{
		wchar_t wide[MAX_PATH_BYTES];
		const DWORD count = GetTempPathW((DWORD)MAX_PATH_BYTES, wide);
		if (count == 0)
		{
			return last_error();
		}

		if (count >= MAX_PATH_BYTES)
		{
			return FileError::NameTooLong;
		}

		return give(out, wide, count);
	}
}

#endif

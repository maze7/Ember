// The POSIX half of the file system: every public function that is one system call deep, for
// Linux and macOS. Paths are checked against the engine's contract here, copied onto the stack
// with a NUL for the system, and never allocated.

#if !defined(_WIN32)

	#include <ember/core/filesystem.h>
	#include <ember/core/log.h>
	#include <ember/core/macros.h>

	#include <cerrno>
	#include <climits>
	#include <cstdlib>
	#include <cstring>
	#include <limits>
	#include <utility>

	#include <dirent.h>
	#include <fcntl.h>
	#include <pwd.h>
	#include <sys/stat.h>
	#include <unistd.h>

	#if defined(__APPLE__)
		#include <mach-o/dyld.h>
	#endif

namespace ember::fs
{
	namespace
	{
		// errno, sorted into the codes callers branch on.
		FileError classify(int value)
		{
			switch (value)
			{
				case ENOENT:
					return FileError::NotFound;
				case EEXIST:
					return FileError::AlreadyExists;
				case EACCES:
				case EPERM:
				case EROFS:
					return FileError::AccessDenied;
				case ELOOP:
				case EINVAL:
					return FileError::InvalidPath;
				case ENAMETOOLONG:
					return FileError::NameTooLong;
				case ENOTDIR:
					return FileError::NotDirectory;
				case EISDIR:
					return FileError::IsDirectory;
				case ENOTEMPTY:
					return FileError::DirectoryNotEmpty;
				case ENOSPC:
	#if defined(EDQUOT)
				case EDQUOT:
	#endif
					return FileError::NoSpace;
				case EFBIG:
				case EOVERFLOW:
					return FileError::TooLarge;
				case EBUSY:
				case EMFILE:
				case ENFILE:
				case EAGAIN:
					return FileError::Busy;
				case EXDEV:
				case ENOSYS:
				case ENOTSUP:
	#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
				case EOPNOTSUPP:
	#endif
					return FileError::Unsupported;
				case EIO:
					return FileError::Io;
				default:
					return FileError::Unknown;
			}
		}

		// The portable code for a system error. One the table has no name for is logged with its
		// number, which is all a reader of the log could use.
		FileError posix_error(int value)
		{
			const FileError error = classify(value);
			if (error == FileError::Unknown)
			{
				EMBER_WARN("fs: system error {}", value);
			}

			return error;
		}

		FileError last_error()
		{
			return posix_error(errno);
		}

		// The system wants a C string and a StringView has none: the engine's path, checked,
		// copied onto the stack with its NUL. A drive root means nothing here and is refused,
		// rather than made a directory named "C:".
		struct CPath
		{
			char text[MAX_PATH_BYTES];

			Result<void, FileError> set(StringView path)
			{
				if (auto valid = validate(path); !valid)
				{
					return valid;
				}

				if (path.size() >= 3 && path[1] == ':' && path[2] == '/')
				{
					return FileError::InvalidPath;
				}

				std::memcpy(text, path.data(), path.size());
				text[path.size()] = '\0';
				return {};
			}
		};

		// A descriptor in a File, one up, so that zero stays "no file" while stdin is zero.
		int fd_of(File file)
		{
			return (int)file.handle - 1;
		}

		File file_of(int fd)
		{
			return File{(u64)fd + 1};
		}

		FileType type_of(mode_t mode)
		{
			if (S_ISREG(mode))
			{
				return FileType::Regular;
			}

			if (S_ISDIR(mode))
			{
				return FileType::Directory;
			}

			return S_ISLNK(mode) ? FileType::Symlink : FileType::Other;
		}

		i64 modified_ns(const struct stat& info)
		{
	#if defined(__APPLE__)
			const timespec& time = info.st_mtimespec;
	#else
			const timespec& time = info.st_mtim;
	#endif
			return (i64)time.tv_sec * 1'000'000'000 + (i64)time.tv_nsec;
		}

		// fsync, or on macOS the fuller flush that reaches the drive and not only its cache.
		Result<void, FileError> sync(int fd)
		{
			int result;
			do
			{
	#if defined(__APPLE__)
				result = ::fcntl(fd, F_FULLFSYNC);
	#else
				result = ::fsync(fd);
	#endif
			} while (result < 0 && errno == EINTR);

			if (result < 0)
			{
				return last_error();
			}

			return {};
		}

		// Gives a system path to the caller's String in the engine's spelling: no trailing
		// separator but the root's.
		void give(String& out, const char* text, size_t length)
		{
			while (length > 1 && text[length - 1] == '/')
			{
				--length;
			}

			out.assign(text, length);
		}

		Result<void, FileError> home_directory(String& out)
		{
			const char* home = std::getenv("HOME");
			if (home != nullptr && home[0] == '/')
			{
				give(out, home, std::strlen(home));
				return {};
			}

			// A service or a test may run without HOME; the account still has one.
			struct passwd record;
			struct passwd* found = nullptr;
			char buffer[16384];
			const int result = ::getpwuid_r(::getuid(), &record, buffer, sizeof(buffer), &found);
			if (result != 0)
			{
				return posix_error(result);
			}

			if (found == nullptr || found->pw_dir == nullptr || found->pw_dir[0] != '/')
			{
				return FileError::NotFound;
			}

			give(out, found->pw_dir, std::strlen(found->pw_dir));
			return {};
		}

		// The home directory with [child] under it.
		Result<void, FileError> home_child(StringView child, String& out)
		{
			if (auto home = home_directory(out); !home)
			{
				return home;
			}

			out.append(child.data(), child.size());
			return {};
		}

		// A user directory the XDG way: the variable when it is set and absolute, else home
		// plus the child the specification names.
		Result<void, FileError> xdg_directory(const char* variable, StringView child, String& out)
		{
			const char* configured = std::getenv(variable);
			if (configured != nullptr && configured[0] == '/')
			{
				give(out, configured, std::strlen(configured));
				return {};
			}

			return home_child(child, out);
		}

		// An app name is one plain component: no separator, and neither of the dot names.
		bool plain_component(StringView app)
		{
			return !app.empty() && app != "." && app != ".." && app.find('/') == StringView::npos;
		}
	}

	Result<File, FileError> open(StringView path, OpenMode mode)
	{
		CPath native;
		if (auto set = native.set(path); !set)
		{
			return set.error;
		}

		// O_NONBLOCK so that opening a pipe does not wait for its other end; a regular file, the
		// only kind kept, ignores it.
		int flags = O_CLOEXEC | O_NOCTTY | O_NONBLOCK;
		switch (mode)
		{
			case OpenMode::Read:
				flags |= O_RDONLY;
				break;
			case OpenMode::Write:
				flags |= O_WRONLY | O_CREAT | O_TRUNC;
				break;
			case OpenMode::ReadWrite:
				flags |= O_RDWR | O_CREAT;
				break;
			case OpenMode::CreateNew:
				flags |= O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
				break;
		}

		int fd;
		do
		{
			fd = ::open(native.text, flags, 0666);
		} while (fd < 0 && errno == EINTR);

		if (fd < 0)
		{
			return last_error();
		}

		struct stat info;
		if (::fstat(fd, &info) != 0)
		{
			const FileError error = last_error();
			(void)::close(fd);
			return error;
		}

		if (!S_ISREG(info.st_mode))
		{
			(void)::close(fd);
			return S_ISDIR(info.st_mode) ? FileError::IsDirectory : FileError::Unsupported;
		}

		return file_of(fd);
	}

	Result<void, FileError> close(File& file)
	{
		if (!file)
		{
			return {};
		}

		// Cleared first, so a failed close is not tried again, and not retried on EINTR: the
		// descriptor is gone either way, and a retry could close one another thread has since
		// been given.
		const int fd = fd_of(std::exchange(file, File{}));
		if (::close(fd) != 0)
		{
			return last_error();
		}

		return {};
	}

	Result<u64, FileError> size(File file)
	{
		EMBER_ASSERT(file);

		struct stat info;
		if (::fstat(fd_of(file), &info) != 0)
		{
			return last_error();
		}

		return (u64)info.st_size;
	}

	Result<size_t, FileError> read_at(File file, u64 offset, Span<u8> into)
	{
		EMBER_ASSERT(file);

		// Offsets come from pack tables, which are data: a range that wraps is an error, not a
		// bug.
		if (into.size() > std::numeric_limits<u64>::max() - offset ||
		    offset > (u64)std::numeric_limits<off_t>::max())
		{
			return FileError::TooLarge;
		}

		if (into.empty())
		{
			return size_t{0};
		}

		// One call moves at most what a ssize_t can say; the caller's loop takes the rest.
		constexpr size_t MOST = (size_t)std::numeric_limits<ssize_t>::max();
		const size_t count = into.size() < MOST ? into.size() : MOST;

		ssize_t received;
		do
		{
			received = ::pread(fd_of(file), into.data(), count, (off_t)offset);
		} while (received < 0 && errno == EINTR);

		if (received < 0)
		{
			return last_error();
		}

		return (size_t)received;
	}

	Result<size_t, FileError> write_at(File file, u64 offset, Span<const u8> from)
	{
		EMBER_ASSERT(file);

		if (from.size() > std::numeric_limits<u64>::max() - offset ||
		    offset > (u64)std::numeric_limits<off_t>::max())
		{
			return FileError::TooLarge;
		}

		if (from.empty())
		{
			return size_t{0};
		}

		constexpr size_t MOST = (size_t)std::numeric_limits<ssize_t>::max();
		const size_t count = from.size() < MOST ? from.size() : MOST;

		ssize_t sent;
		do
		{
			sent = ::pwrite(fd_of(file), from.data(), count, (off_t)offset);
		} while (sent < 0 && errno == EINTR);

		if (sent < 0)
		{
			return last_error();
		}

		return (size_t)sent;
	}

	Result<void, FileError> flush(File file)
	{
		EMBER_ASSERT(file);
		return sync(fd_of(file));
	}

	Result<FileInfo, FileError> status(StringView path)
	{
		CPath native;
		if (auto set = native.set(path); !set)
		{
			return set.error;
		}

		struct stat found;
		if (::stat(native.text, &found) != 0)
		{
			return last_error();
		}

		return FileInfo{
		    .type = type_of(found.st_mode),
		    .size = (u64)found.st_size,
		    .modified_ns = modified_ns(found),
		};
	}

	Result<void, FileError> enumerate(StringView directory, EnumerateFn fn, void* user)
	{
		EMBER_ASSERT(fn != nullptr);

		CPath native;
		if (auto set = native.set(directory); !set)
		{
			return set;
		}

		DIR* stream = ::opendir(native.text);
		if (stream == nullptr)
		{
			return last_error();
		}

		Result<void, FileError> result;
		for (;;)
		{
			errno = 0;
			const dirent* item = ::readdir(stream);
			if (item == nullptr)
			{
				if (errno != 0)
				{
					result = last_error();
				}

				break;
			}

			const StringView name = item->d_name;
			if (name == "." || name == "..")
			{
				continue;
			}

			// The entry's type comes with it on most file systems; where it does not, ask.
			FileType type = FileType::Other;
			switch (item->d_type)
			{
				case DT_REG:
					type = FileType::Regular;
					break;
				case DT_DIR:
					type = FileType::Directory;
					break;
				case DT_LNK:
					type = FileType::Symlink;
					break;
				case DT_UNKNOWN:
				{
					struct stat info;
					if (::fstatat(::dirfd(stream), item->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0)
					{
						type = type_of(info.st_mode);
					}

					break;
				}
				default:
					break;
			}

			if (!fn(DirectoryEntry{.name = name, .type = type}, user))
			{
				break;
			}
		}

		if (::closedir(stream) != 0 && result)
		{
			result = last_error();
		}

		return result;
	}

	Result<void, FileError> create_directory(StringView path)
	{
		CPath native;
		if (auto set = native.set(path); !set)
		{
			return set;
		}

		if (::mkdir(native.text, 0777) == 0)
		{
			return {};
		}

		// Already there is fine when it is a directory.
		const int code = errno;
		struct stat info;
		if (code == EEXIST && ::stat(native.text, &info) == 0 && S_ISDIR(info.st_mode))
		{
			return {};
		}

		return posix_error(code);
	}

	Result<void, FileError> remove_file(StringView path)
	{
		CPath native;
		if (auto set = native.set(path); !set)
		{
			return set;
		}

		if (::unlink(native.text) != 0)
		{
			return last_error();
		}

		return {};
	}

	Result<void, FileError> remove_directory(StringView path)
	{
		CPath native;
		if (auto set = native.set(path); !set)
		{
			return set;
		}

		if (::rmdir(native.text) != 0)
		{
			return last_error();
		}

		return {};
	}

	Result<void, FileError> rename(StringView from, StringView to)
	{
		CPath source;
		CPath target;
		if (auto set = source.set(from); !set)
		{
			return set;
		}

		if (auto set = target.set(to); !set)
		{
			return set;
		}

		if (::rename(source.text, target.text) != 0)
		{
			return last_error();
		}

		return {};
	}

	Result<void, FileError> sync_directory(StringView path)
	{
		CPath native;
		if (auto set = native.set(path); !set)
		{
			return set;
		}

		int fd;
		do
		{
			fd = ::open(native.text, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		} while (fd < 0 && errno == EINTR);

		if (fd < 0)
		{
			return last_error();
		}

		Result<void, FileError> result = sync(fd);
		if (::close(fd) != 0 && result)
		{
			result = last_error();
		}

		return result;
	}

	Result<void, FileError> working_directory(String& out)
	{
		char buffer[MAX_PATH_BYTES];
		if (::getcwd(buffer, sizeof(buffer)) == nullptr)
		{
			return errno == ERANGE ? FileError::NameTooLong : last_error();
		}

		give(out, buffer, std::strlen(buffer));
		return {};
	}

	Result<void, FileError> executable_directory(String& out)
	{
	#if defined(__APPLE__)
		// The path the loader has may be relative or go through a link; realpath settles it,
		// and wants PATH_MAX, which the buffer covers.
		static_assert(MAX_PATH_BYTES >= PATH_MAX);

		char executable[MAX_PATH_BYTES];
		u32 capacity = sizeof(executable);
		if (::_NSGetExecutablePath(executable, &capacity) != 0)
		{
			return FileError::NameTooLong;
		}

		char buffer[MAX_PATH_BYTES];
		if (::realpath(executable, buffer) == nullptr)
		{
			return last_error();
		}

		const size_t count = std::strlen(buffer);
	#else
		char buffer[MAX_PATH_BYTES];
		const ssize_t read = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
		if (read < 0)
		{
			return last_error();
		}

		// A full buffer means the link was cut to fit, and the path is not the whole path.
		if ((size_t)read >= sizeof(buffer) - 1)
		{
			return FileError::NameTooLong;
		}

		const size_t count = (size_t)read;
	#endif

		const StringView directory = parent(StringView(buffer, count));
		out.assign(directory.data(), directory.size());
		return {};
	}

	Result<void, FileError> user_data_directory(StringView app, String& out)
	{
		EMBER_ASSERT_MSG(plain_component(app), "an app name is one plain directory component");

	#if defined(__APPLE__)
		if (auto base = home_child("/Library/Application Support", out); !base)
	#else
		if (auto base = xdg_directory("XDG_DATA_HOME", "/.local/share", out); !base)
	#endif
		{
			return base;
		}

		return join(out, app, out);
	}

	Result<void, FileError> user_cache_directory(StringView app, String& out)
	{
		EMBER_ASSERT_MSG(plain_component(app), "an app name is one plain directory component");

	#if defined(__APPLE__)
		if (auto base = home_child("/Library/Caches", out); !base)
	#else
		if (auto base = xdg_directory("XDG_CACHE_HOME", "/.cache", out); !base)
	#endif
		{
			return base;
		}

		return join(out, app, out);
	}

	Result<void, FileError> temporary_directory(String& out)
	{
		const char* configured = std::getenv("TMPDIR");
		const char* directory = configured != nullptr && configured[0] == '/' ? configured : "/tmp";
		give(out, directory, std::strlen(directory));
		return {};
	}
}

#endif

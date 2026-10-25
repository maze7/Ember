// Portable path rules and file operations built on the OS primitives in filesystem_posix.cpp
// and filesystem_win32.cpp. Temporary paths use bounded stack storage.

#include <ember/core/filesystem.h>

#include <ember/core/jobs.h>
#include <ember/core/macros.h>
#include <ember/core/time.h>

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ember::fs
{
	namespace
	{
		constexpr u32 TEMPORARY_ATTEMPTS = 64;

		// A timestamp and sequence reduce temporary-name collisions; exclusive creation decides
		// ownership if another process chooses the same name.
		constinit std::atomic<u32> s_temporary_sequence{0};

		// NUL-terminated engine paths in bounded stack storage. OS backends convert separately.
		struct PathBuffer
		{
			char text[MAX_PATH_BYTES];
			size_t length = 0;

			PathBuffer()
			{
				text[0] = '\0';
			}

			StringView view() const
			{
				return {text, length};
			}

			// Clears first, so an oversized assignment returns false and leaves the buffer empty.
			bool assign(StringView part)
			{
				length = 0;
				text[0] = '\0';
				return append(part);
			}

			// An oversized append returns false without changing the buffer.
			bool append(StringView part)
			{
				if (part.size() > MAX_PATH_BYTES - 1 - length)
				{
					return false;
				}

				std::memcpy(text + length, part.data(), part.size());
				length += part.size();
				text[length] = '\0';
				return true;
			}

			bool push(char character)
			{
				return append(StringView(&character, 1));
			}

			void truncate(size_t count)
			{
				length = count;
				text[length] = '\0';
			}
		};

		bool ascii_alpha(char character)
		{
			return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
		}

		// Bytes of a lexical root: one for "/", three for "C:/", none for a relative path.
		size_t root_length(StringView path)
		{
			if (path.size() >= 3 && ascii_alpha(path[0]) && path[1] == ':' && path[2] == '/')
			{
				return 3;
			}

			return !path.empty() && path[0] == '/' ? 1 : 0;
		}

		StringView last_component(const PathBuffer& buffer, size_t root)
		{
			const StringView held = buffer.view().substr(root);
			const size_t slash = held.rfind('/');
			return slash == StringView::npos ? held : held.substr(slash + 1);
		}

		// Shared normalization keeps internal path operations off the heap.
		Result<void, FileError> normalize_into(StringView path, PathBuffer& out)
		{
			if (auto valid = validate(path); !valid)
			{
				return valid;
			}

			const size_t root = root_length(path);
			out.assign(path.substr(0, root));

			size_t cursor = root;
			while (cursor < path.size())
			{
				while (cursor < path.size() && path[cursor] == '/')
				{
					++cursor;
				}

				const size_t begin = cursor;
				while (cursor < path.size() && path[cursor] != '/')
				{
					++cursor;
				}

				const StringView component = path.substr(begin, cursor - begin);
				if (component.empty() || component == ".")
				{
					continue;
				}

				if (component == "..")
				{
					// Relative paths may keep leading ".."; absolute paths may not.
					const StringView last = last_component(out, root);
					if (!last.empty() && last != "..")
					{
						out.truncate(out.length - last.size());
						if (out.length > root)
						{
							out.truncate(out.length - 1);
						}

						continue;
					}

					if (root != 0)
					{
						return FileError::InvalidPath;
					}
				}

				if ((out.length > root && !out.push('/')) || !out.append(component))
				{
					return FileError::NameTooLong;
				}
			}

			if (out.length == 0)
			{
				out.push('.');
			}

			return {};
		}

		void assign(String& out, const PathBuffer& buffer)
		{
			out.assign(buffer.text, buffer.length);
		}

		// Read until the buffer is full or EOF; a short read at EOF is successful.
		Result<size_t, FileError> read_up_to(File file, u64 offset, Span<u8> into)
		{
			size_t read = 0;
			while (read < into.size())
			{
				auto count = read_at(file, offset + read, into.subspan(read));
				if (!count)
				{
					return count.error;
				}

				if (*count == 0)
				{
					break;
				}

				read += *count;
			}

			return read;
		}
	}

	Result<void, FileError> validate(StringView path)
	{
		if (path.size() >= MAX_PATH_BYTES)
		{
			return FileError::NameTooLong;
		}

		// Two leading slashes name a network share, which POSIX leaves to the implementation
		// and Windows reads as UNC; three or more are one slash, as POSIX says.
		const bool share = path.size() >= 2 && path[0] == '/' && path[1] == '/' &&
		                   (path.size() == 2 || path[2] != '/');
		if (path.empty() || share)
		{
			return FileError::InvalidPath;
		}

		// A drive letter without its slash is relative to a working directory that only Windows
		// keeps, one per drive.
		if (path.size() >= 2 && ascii_alpha(path[0]) && path[1] == ':' &&
		    (path.size() == 2 || path[2] != '/'))
		{
			return FileError::InvalidPath;
		}

		for (const char character : path)
		{
			if (character == '\0' || character == '\\')
			{
				return FileError::InvalidPath;
			}
		}

		return {};
	}

	Result<void, FileError> read_exact_at(File file, u64 offset, Span<u8> into)
	{
		size_t done = 0;
		while (done < into.size())
		{
			auto count = read_at(file, offset + done, into.subspan(done));
			if (!count)
			{
				return count.error;
			}

			if (*count == 0)
			{
				return FileError::EndOfFile;
			}

			done += *count;
		}

		return {};
	}

	Result<void, FileError> write_exact_at(File file, u64 offset, Span<const u8> from)
	{
		size_t done = 0;
		while (done < from.size())
		{
			auto count = write_at(file, offset + done, from.subspan(done));
			if (!count)
			{
				return count.error;
			}

			// No progress on a non-empty range would loop forever: the device is not taking
			// bytes.
			if (*count == 0)
			{
				return FileError::Io;
			}

			done += *count;
		}

		return {};
	}

	Result<FileData, FileError> read_file(StringView path, memory::Allocator* allocator,
	    memory::Tag tag, const ReadFileDef& def)
	{
		EMBER_ASSERT(allocator != nullptr);
		EMBER_ASSERT(std::has_single_bit(def.alignment));

		auto opened = open(path, OpenMode::Read);
		if (!opened)
		{
			return opened.error;
		}

		File file = *opened;
		Result<void, FileError> result;
		size_t count = 0;
		if (auto length = size(file); !length)
		{
			result = length.error;
		}
		else if (*length > def.max_bytes || *length > std::numeric_limits<size_t>::max())
		{
			result = FileError::TooLarge;
		}
		else
		{
			count = (size_t)*length;
		}

		// Use the stat size: shrinking during the read returns EndOfFile; later growth is ignored.
		u8* bytes = nullptr;
		if (result && count != 0)
		{
			bytes = (u8*)allocator->allocate(tag, count, def.alignment, std::source_location{});
			result = read_exact_at(file, 0, Span<u8>(bytes, count));
		}

		if (auto closed = close(file); result && !closed)
		{
			result = closed;
		}

		if (!result)
		{
			if (bytes != nullptr)
			{
				allocator->deallocate(bytes, count, def.alignment);
			}

			return result.error;
		}

		return FileData{.data = bytes,
		    .size = count,
		    .alignment = def.alignment,
		    .allocator = allocator};
	}

	void free_file(FileData& data)
	{
		if (data.data != nullptr)
		{
			EMBER_ASSERT(data.allocator != nullptr);
			data.allocator->deallocate(data.data, data.size, data.alignment);
		}

		data = {};
	}

	Result<void, FileError> write_file(StringView path, Span<const u8> bytes)
	{
		auto opened = open(path, OpenMode::Write);
		if (!opened)
		{
			return opened.error;
		}

		File file = *opened;
		Result<void, FileError> result = write_exact_at(file, 0, bytes);
		if (auto closed = close(file); result && !closed)
		{
			result = closed;
		}

		return result;
	}

	Result<void, FileError> write_file_atomic(StringView path, Span<const u8> bytes, bool durable)
	{
		if (auto valid = validate(path); !valid)
		{
			return valid;
		}

		const StringView name = file_name(path);
		if (name.empty() || name == "." || name == "..")
		{
			return FileError::InvalidPath;
		}

		// Keep temporary basenames independent of the target so long target names leave room.
		// Retry exclusive creation with a new sequence if a name is already taken.
		const StringView directory = parent(path);
		const char* separator = directory.empty() || directory.back() == '/' ? "" : "/";

		PathBuffer temporary;
		File file;
		for (u32 attempt = 0; attempt < TEMPORARY_ATTEMPTS && !file; ++attempt)
		{
			const u32 sequence = s_temporary_sequence.fetch_add(1, std::memory_order_relaxed);
			const int length =
			    std::snprintf(temporary.text, sizeof(temporary.text), "%.*s%s.ember-%llx-%u.tmp",
			        (int)directory.size(), directory.empty() ? "" : directory.data(), separator,
			        (unsigned long long)now_ns(), sequence);
			if (length < 0 || (size_t)length >= sizeof(temporary.text))
			{
				return FileError::NameTooLong;
			}

			temporary.length = (size_t)length;

			auto created = open(temporary.view(), OpenMode::CreateNew);
			if (created)
			{
				file = *created;
			}
			else if (created.error != FileError::AlreadyExists)
			{
				return created.error;
			}
		}

		if (!file)
		{
			return FileError::Busy;
		}

		// Until rename succeeds, every failure path must remove the temporary sibling.
		Result<void, FileError> result = write_exact_at(file, 0, bytes);
		if (result && durable)
		{
			result = flush(file);
		}

		if (auto closed = close(file); result && !closed)
		{
			result = closed;
		}

		if (result)
		{
			result = rename(temporary.view(), path);
		}

		if (!result)
		{
			(void)remove_file(temporary.view());
			return result;
		}

		// Rename already succeeded. A sync failure reports uncertain durability without undoing it.
		if (durable)
		{
			return sync_directory(directory.empty() ? StringView(".") : directory);
		}

		return {};
	}

	bool exists(StringView path)
	{
		return (bool)status(path);
	}

	Result<void, FileError> create_directories(StringView path)
	{
		PathBuffer clean;
		if (auto normalized = normalize_into(path, clean); !normalized)
		{
			return normalized;
		}

		const size_t root = root_length(clean.view());
		if (clean.length == root || clean.view() == ".")
		{
			return {};
		}

		// Each prefix in turn, so a parent exists before its child.
		for (size_t end = root + 1; end <= clean.length; ++end)
		{
			if (end == clean.length || clean.text[end] == '/')
			{
				if (auto created = create_directory(clean.view().substr(0, end)); !created)
				{
					return created;
				}
			}
		}

		return {};
	}

	bool is_absolute(StringView path)
	{
		return root_length(path) != 0;
	}

	StringView file_name(StringView path)
	{
		if (path.empty() || path.back() == '/' || path.size() <= root_length(path))
		{
			return {};
		}

		const size_t slash = path.rfind('/');
		return slash == StringView::npos ? path : path.substr(slash + 1);
	}

	StringView extension(StringView path)
	{
		const StringView name = file_name(path);
		const size_t dot = name.rfind('.');
		return dot == StringView::npos || dot == 0 ? StringView() : name.substr(dot);
	}

	StringView stem(StringView path)
	{
		const StringView name = file_name(path);
		const size_t dot = name.rfind('.');
		return dot == StringView::npos || dot == 0 ? name : name.substr(0, dot);
	}

	StringView parent(StringView path)
	{
		const size_t root = root_length(path);

		size_t end = path.size();
		while (end > root && path[end - 1] == '/')
		{
			--end;
		}

		if (end <= root)
		{
			return path.substr(0, root);
		}

		if (end < path.size())
		{
			return path.substr(0, end);
		}

		const size_t slash = path.rfind('/', end - 1);
		if (slash == StringView::npos || slash < root)
		{
			return path.substr(0, root);
		}

		return path.substr(0, slash);
	}

	Result<void, FileError> normalize(StringView path, String& out)
	{
		PathBuffer clean;
		if (auto normalized = normalize_into(path, clean); !normalized)
		{
			return normalized;
		}

		assign(out, clean);
		return {};
	}

	Result<void, FileError> join(StringView left, StringView right, String& out)
	{
		if (auto valid = validate(right); !valid)
		{
			return valid;
		}

		if (is_absolute(right))
		{
			return FileError::InvalidPath;
		}

		if (left.empty())
		{
			return normalize(right, out);
		}

		if (auto valid = validate(left); !valid)
		{
			return valid;
		}

		PathBuffer combined;
		if (!combined.append(left) || (left.back() != '/' && !combined.push('/')) ||
		    !combined.append(right))
		{
			return FileError::NameTooLong;
		}

		return normalize(combined.view(), out);
	}

	Result<void, FileError> absolute(StringView path, String& out)
	{
		if (auto valid = validate(path); !valid)
		{
			return valid;
		}

		if (is_absolute(path))
		{
			return normalize(path, out);
		}

		// Preserve path before writing out; the view may refer to out's storage.
		PathBuffer relative;
		relative.assign(path);

		if (auto found = working_directory(out); !found)
		{
			return found;
		}

		return join(out, relative.view(), out);
	}

	jobs::JobDef job(OpenFile& request)
	{
		return {
		    .fn =
		        [](void* data)
		    {
			    OpenFile& request = *(OpenFile*)data;
			    request.result = open(request.path, request.mode);
		    },
		    .data = &request,
		    .name = "open file",
		};
	}

	jobs::JobDef job(ReadFile& request)
	{
		EMBER_ASSERT(request.allocator != nullptr);

		return {
		    .fn =
		        [](void* data)
		    {
			    ReadFile& request = *(ReadFile*)data;
			    request.result =
			        read_file(request.path, request.allocator, request.tag, request.def);
		    },
		    .data = &request,
		    .name = "read file",
		};
	}

	jobs::JobDef job(ReadAt& request)
	{
		EMBER_ASSERT(request.file);

		return {
		    .fn =
		        [](void* data)
		    {
			    ReadAt& request = *(ReadAt*)data;
			    request.result = read_up_to(request.file, request.offset, request.into);
		    },
		    .data = &request,
		    .name = "read range",
		};
	}

	jobs::JobDef job(WriteFile& request)
	{
		return {
		    .fn =
		        [](void* data)
		    {
			    WriteFile& request = *(WriteFile*)data;
			    request.result =
			        request.atomic ? write_file_atomic(request.path, request.bytes, request.durable)
					               : write_file(request.path, request.bytes);
		    },
		    .data = &request,
		    .name = "write file",
		};
	}
}

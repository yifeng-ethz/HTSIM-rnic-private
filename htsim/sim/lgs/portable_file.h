#ifndef HTSIM_LGS_PORTABLE_FILE_H
#define HTSIM_LGS_PORTABLE_FILE_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef B
#pragma push_macro("B")
#undef B
#define HTSIM_RESTORE_B_MACRO
#endif
#include <windows.h>
#ifdef HTSIM_RESTORE_B_MACRO
#pragma pop_macro("B")
#undef HTSIM_RESTORE_B_MACRO
#endif

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

inline int htsim_open_rw_truncate(const char* filename)
{
    return _open(filename,
                 _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY,
                 _S_IWRITE | _S_IREAD);
}

inline int htsim_close_file(int fd)
{
    const int commit_result = _commit(fd);
    const int close_result = _close(fd);
    return commit_result == 0 ? close_result : commit_result;
}

inline int htsim_resize_file(int fd, std::uint64_t size)
{
    return _chsize_s(fd, size) == 0 ? 0 : -1;
}

inline int htsim_fileno(FILE* file)
{
    return _fileno(file);
}

inline std::uint64_t htsim_file_size(FILE* file)
{
    const __int64 original = _ftelli64(file);
    assert(original >= 0);
    assert(_fseeki64(file, 0, SEEK_END) == 0);
    const __int64 size = _ftelli64(file);
    assert(size >= 0);
    assert(_fseeki64(file, original, SEEK_SET) == 0);
    return static_cast<std::uint64_t>(size);
}

inline void* htsim_map_file(int fd, std::size_t length, bool shared)
{
    const intptr_t native_handle = _get_osfhandle(fd);
    if (native_handle == -1) {
        return nullptr;
    }

    const DWORD protection = shared ? PAGE_READWRITE : PAGE_WRITECOPY;
    HANDLE mapping = CreateFileMappingW(reinterpret_cast<HANDLE>(native_handle),
                                        nullptr,
                                        protection,
                                        0,
                                        0,
                                        nullptr);
    if (mapping == nullptr) {
        return nullptr;
    }

    const DWORD access = shared ? FILE_MAP_WRITE : FILE_MAP_COPY;
    void* view = MapViewOfFile(mapping, access, 0, 0, length);
    CloseHandle(mapping);
    return view;
}

inline int htsim_unmap_file(void* address, std::size_t length)
{
    // FlushViewOfFile is harmless for copy-on-write views and ensures shared
    // serializer views reach the file before the descriptor is committed.
    FlushViewOfFile(address, length);
    return UnmapViewOfFile(address) != 0 ? 0 : -1;
}

#else

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

inline int htsim_open_rw_truncate(const char* filename)
{
    return ::open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR);
}

inline int htsim_close_file(int fd)
{
    return ::close(fd);
}

inline int htsim_resize_file(int fd, std::uint64_t size)
{
    return ::ftruncate(fd, static_cast<off_t>(size));
}

inline int htsim_fileno(FILE* file)
{
    return ::fileno(file);
}

inline std::uint64_t htsim_file_size(FILE* file)
{
    struct stat info;
    const int result = ::fstat(htsim_fileno(file), &info);
    assert(result == 0);
    return static_cast<std::uint64_t>(info.st_size);
}

inline void* htsim_map_file(int fd, std::size_t length, bool shared)
{
    void* mapping = ::mmap(nullptr,
                           length,
                           PROT_READ | PROT_WRITE,
                           shared ? MAP_SHARED : MAP_PRIVATE,
                           fd,
                           0);
    return mapping == MAP_FAILED ? nullptr : mapping;
}

inline int htsim_unmap_file(void* address, std::size_t length)
{
    return ::munmap(address, length);
}

#endif

#endif

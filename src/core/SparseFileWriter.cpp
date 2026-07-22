#include "core/SparseFileWriter.h"

#include <QFileInfo>
#include <QDir>
#include <QMutexLocker>
#include <QtGlobal>
#include <cerrno>
#include <cstring>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

namespace qtidm {

namespace {
constexpr std::int64_t mapWindowSize = 64LL * 1024LL * 1024LL;

#ifndef Q_OS_WIN
int sysPageSize()
{
    static const int size = static_cast<int>(::sysconf(_SC_PAGESIZE));
    return size;
}
#else
int sysPageSize()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<int>(si.dwPageSize);
}

int win32Open(const QString& path, bool write)
{
    const auto native = path.toStdWString();
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    DWORD share = FILE_SHARE_READ | (write ? 0 : FILE_SHARE_WRITE);
    DWORD disposition = write ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileW(native.c_str(), access, share, nullptr,
                           disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    return h != INVALID_HANDLE_VALUE
        ? _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_BINARY)
        : -1;
}
#endif
}

SparseFileWriter::~SparseFileWriter()
{
    close();
}

bool SparseFileWriter::open(const QString& path, std::int64_t size, bool preserveUnknownLength)
{
    QMutexLocker lock(&mutex_);
    QDir().mkpath(QFileInfo(path).absolutePath());

#ifndef Q_OS_WIN
    fd_ = ::open(path.toLocal8Bit().constData(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
#else
    fd_ = win32Open(path, true);
#endif

    if (fd_ < 0) {
        lastError_ = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    fileSize_ = size;
    const auto shouldResize = size >= 0 || !preserveUnknownLength;
    const auto initialSize = size > 0 ? size : 0;
    if (shouldResize) {
#ifndef Q_OS_WIN
        if (::ftruncate(fd_, initialSize) != 0) {
#else
        if (::_chsize_s(fd_, initialSize) != 0) {
#endif
            lastError_ = QString::fromLocal8Bit(std::strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }
    return true;
}

bool SparseFileWriter::setExpectedSize(std::int64_t size)
{
    QMutexLocker lock(&mutex_);
    if (fd_ < 0 || size < 0) {
        lastError_ = QStringLiteral("writer is not open");
        return false;
    }
    unmap();
#ifndef Q_OS_WIN
    if (::ftruncate(fd_, size) != 0) {
#else
    if (::_chsize_s(fd_, size) != 0) {
#endif
        lastError_ = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    fileSize_ = size;
    return true;
}

bool SparseFileWriter::writeAt(std::int64_t offset, const char* data, std::int64_t size)
{
    QMutexLocker lock(&mutex_);
    if (fd_ < 0 || offset < 0 || size < 0) {
        lastError_ = QStringLiteral("writer is not open");
        return false;
    }
    if (fileSize_ > 0 && offset + size > fileSize_) {
        lastError_ = QStringLiteral("write exceeds target size");
        return false;
    }
    if (fileSize_ > 0 && size <= mapWindowSize && map(offset, size)) {
        std::memcpy(static_cast<char*>(mapped_) + (offset - mappedOffset_), data, static_cast<std::size_t>(size));
        return true;
    }

    std::int64_t written = 0;
    while (written < size) {
#ifndef Q_OS_WIN
        const auto rc = ::pwrite(fd_, data + written, static_cast<std::size_t>(size - written), offset + written);
#else
        const auto rc = ::_lseeki64(fd_, offset + written, SEEK_SET) >= 0
            ? ::_write(fd_, data + written, static_cast<unsigned int>(size - written))
            : -1;
#endif
        if (rc <= 0) {
            lastError_ = QString::fromLocal8Bit(std::strerror(errno));
            return false;
        }
        written += rc;
    }
    return true;
}

void SparseFileWriter::close()
{
    QMutexLocker lock(&mutex_);
    unmap();
    if (fd_ >= 0) {
#ifndef Q_OS_WIN
        ::fsync(fd_);
#endif
        ::close(fd_);
        fd_ = -1;
    }
}

QString SparseFileWriter::lastError() const
{
    QMutexLocker lock(&mutex_);
    return lastError_;
}

bool SparseFileWriter::map(std::int64_t offset, std::int64_t size)
{
    const auto pageSize = static_cast<std::int64_t>(sysPageSize());
    const auto base = (offset / pageSize) * pageSize;
    const auto end = qMin(fileSize_, base + mapWindowSize);
    const auto length = end - base;
    if (mapped_ && offset >= mappedOffset_ && offset + size <= mappedOffset_ + mappedSize_) {
        return true;
    }
    unmap();

#ifndef Q_OS_WIN
    mapped_ = ::mmap(nullptr, static_cast<std::size_t>(length), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, base);
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        mappedOffset_ = 0;
        mappedSize_ = 0;
        lastError_ = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
#else
    HANDLE hFile = reinterpret_cast<HANDLE>(::_get_osfhandle(fd_));
    if (hFile == INVALID_HANDLE_VALUE) {
        lastError_ = QStringLiteral("failed to get file handle for mapping");
        return false;
    }
    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!hMap) {
        lastError_ = QStringLiteral("CreateFileMapping failed");
        return false;
    }
    LARGE_INTEGER liBase;
    liBase.QuadPart = base;
    mapped_ = MapViewOfFile(hMap, FILE_MAP_WRITE, liBase.HighPart, liBase.LowPart,
                            static_cast<SIZE_T>(length));
    CloseHandle(hMap);
    if (!mapped_) {
        mappedOffset_ = 0;
        mappedSize_ = 0;
        lastError_ = QStringLiteral("MapViewOfFile failed");
        return false;
    }
#endif

    mappedOffset_ = base;
    mappedSize_ = length;
    return true;
}

void SparseFileWriter::unmap()
{
    if (mapped_) {
#ifndef Q_OS_WIN
        ::msync(mapped_, static_cast<std::size_t>(mappedSize_), MS_ASYNC);
        ::munmap(mapped_, static_cast<std::size_t>(mappedSize_));
#else
        ::FlushViewOfFile(mapped_, static_cast<SIZE_T>(mappedSize_));
        ::UnmapViewOfFile(mapped_);
#endif
        mapped_ = nullptr;
        mappedOffset_ = 0;
        mappedSize_ = 0;
    }
}

}
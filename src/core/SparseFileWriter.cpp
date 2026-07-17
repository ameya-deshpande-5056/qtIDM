#include "core/SparseFileWriter.h"

#include <QFileInfo>
#include <QDir>
#include <QMutexLocker>
#include <QtGlobal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace qtidm {

namespace {
constexpr std::int64_t mapWindowSize = 64LL * 1024LL * 1024LL;
}

SparseFileWriter::~SparseFileWriter()
{
    close();
}

bool SparseFileWriter::open(const QString& path, std::int64_t size)
{
    QMutexLocker lock(&mutex_);
    QDir().mkpath(QFileInfo(path).absolutePath());
    fd_ = ::open(path.toLocal8Bit().constData(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        lastError_ = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    fileSize_ = size;
    if (size > 0 && ::ftruncate(fd_, size) != 0) {
        lastError_ = QString::fromLocal8Bit(std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
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
        const auto rc = ::pwrite(fd_, data + written, static_cast<std::size_t>(size - written), offset + written);
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
        ::fsync(fd_);
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
    const auto pageSize = static_cast<std::int64_t>(::sysconf(_SC_PAGESIZE));
    const auto base = (offset / pageSize) * pageSize;
    const auto end = qMin(fileSize_, base + mapWindowSize);
    const auto length = end - base;
    if (mapped_ && offset >= mappedOffset_ && offset + size <= mappedOffset_ + mappedSize_) {
        return true;
    }
    unmap();
    mapped_ = ::mmap(nullptr, static_cast<std::size_t>(length), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, base);
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        mappedOffset_ = 0;
        mappedSize_ = 0;
        lastError_ = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    mappedOffset_ = base;
    mappedSize_ = length;
    return true;
}

void SparseFileWriter::unmap()
{
    if (mapped_) {
        ::msync(mapped_, static_cast<std::size_t>(mappedSize_), MS_ASYNC);
        ::munmap(mapped_, static_cast<std::size_t>(mappedSize_));
        mapped_ = nullptr;
        mappedOffset_ = 0;
        mappedSize_ = 0;
    }
}

}

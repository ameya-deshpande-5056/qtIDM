#pragma once

#include <QString>
#include <QMutex>
#include <cstdint>

namespace qtidm {

class SparseFileWriter {
public:
    ~SparseFileWriter();

    bool open(const QString& path, std::int64_t size, bool preserveUnknownLength = false);
    bool setExpectedSize(std::int64_t size);
    bool writeAt(std::int64_t offset, const char* data, std::int64_t size);
    void close();
    QString lastError() const;

private:
    bool map(std::int64_t offset, std::int64_t size);
    void unmap();

    mutable QMutex mutex_;
    int fd_ = -1;
    std::int64_t fileSize_ = 0;
    void* mapped_ = nullptr;
    std::int64_t mappedOffset_ = 0;
    std::int64_t mappedSize_ = 0;
    QString lastError_;
};

}
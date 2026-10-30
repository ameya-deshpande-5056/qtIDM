#pragma once

#include <QMutex>
#include <QString>
#include <cstdint>

namespace qtidm {

class SparseFileWriter final {
public:
    SparseFileWriter() = default;
    ~SparseFileWriter();

    bool open(const QString& path, std::int64_t size, bool preserveUnknownLength = false);
    bool setExpectedSize(std::int64_t size);
    bool writeAt(std::int64_t offset, const char* data, std::int64_t size);
    bool sync();
    void close();
    QString lastError() const;
    bool isMmapDisabled() const;

private:
    bool map(std::int64_t offset, std::int64_t size);
    void unmap();

    int fd_ = -1;
    void* mapped_ = nullptr;
    std::int64_t mappedOffset_ = 0;
    std::int64_t mappedSize_ = 0;
    std::int64_t fileSize_ = 0;
    bool mmapDisabled_ = false;
    mutable QMutex mutex_;
    QString lastError_;
};

}

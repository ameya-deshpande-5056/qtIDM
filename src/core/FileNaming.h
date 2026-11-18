#pragma once

#include <QByteArray>
#include <QString>

namespace qtidm {

class FileNaming final {
public:
    static constexpr int kMagicHeaderSize = 64;

    // Inspects leading file bytes (typically first 32-64 bytes) and returns a
    // lowercase extension without leading dot (e.g. "mp4", "mkv", "webm", "zip",
    // "7z", "rar", "pdf"), or empty string if unrecognized.
    static QString sniffExtensionFromMagicBytes(const QByteArray& headerBytes);

    // Returns true if the filename carries no meaningful user-facing name
    // (e.g. "videoplayback", "download.bin", "download.php", UUIDs, hex hashes, numeric IDs).
    static bool isMeaninglessFilename(const QString& filename);

    // Returns true if the file has a script/generic extension or no extension at all.
    static bool hasGenericOrMissingExtension(const QString& filename);

    // Combines current filename, forwarded browser page title, Content-Disposition name,
    // MIME type, and sniffed magic-byte extension into a clean, semantic filename.
    static QString resolveSemanticFilename(const QString& currentFilename,
                                           const QString& pageTitle,
                                           const QByteArray& magicHeader,
                                           const QString& contentDispositionName = {},
                                           const QString& mimeType = {});
};

} // namespace qtidm

#include "core/FileNaming.h"

#include "core/DownloadTypes.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <cstring>

namespace qtidm {

namespace {
bool startsWithBytes(const QByteArray& data, const char* sig, int len)
{
    return data.size() >= len && std::memcmp(data.constData(), sig, static_cast<size_t>(len)) == 0;
}

bool isSpecializedZipExtension(const QString& ext)
{
    static const QSet<QString> specialized {
        QStringLiteral("docx"), QStringLiteral("xlsx"), QStringLiteral("pptx"),
        QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp"),
        QStringLiteral("epub"), QStringLiteral("jar"), QStringLiteral("apk"),
        QStringLiteral("xpi"), QStringLiteral("whl"), QStringLiteral("vsix"),
        QStringLiteral("crx"), QStringLiteral("cbz")
    };
    return specialized.contains(ext.toLower());
}

bool isScriptOrGenericExtension(const QString& ext)
{
    static const QSet<QString> generic {
        QStringLiteral("bin"), QStringLiteral("dat"), QStringLiteral("tmp"),
        QStringLiteral("part"), QStringLiteral("download"), QStringLiteral("php"),
        QStringLiteral("asp"), QStringLiteral("aspx"), QStringLiteral("cgi"),
        QStringLiteral("jsp"), QStringLiteral("do"), QStringLiteral("action"),
        QStringLiteral("cfm"), QStringLiteral("pl")
    };
    return ext.isEmpty() || generic.contains(ext.toLower());
}

QString sanitizePageTitle(const QString& rawTitle)
{
    QString title = rawTitle;
    title.replace(QLatin1Char('\0'), QString());
    static const QRegularExpression siteSuffix(QStringLiteral(R"(\s*(?:::|\||—|–|-)\s*(?:YouTube|Prime Video|Netflix|Twitch|Vimeo|Dailymotion|MediaFire|Google Drive|GoFile|GitHub)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    title.remove(siteSuffix);
    return safeSuggestedFilename(title);
}

QString stripExtension(const QString& filename, QString* outExt = nullptr)
{
    const int dot = filename.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && dot < filename.size() - 1) {
        const QString ext = filename.mid(dot + 1).toLower();
        static const QRegularExpression extRe(QStringLiteral("^[a-z0-9]{1,8}$"));
        if (extRe.match(ext).hasMatch()) {
            if (outExt) {
                *outExt = ext;
            }
            return filename.left(dot);
        }
    }
    if (outExt) {
        outExt->clear();
    }
    return filename;
}
} // namespace

QString FileNaming::sniffExtensionFromMagicBytes(const QByteArray& headerBytes)
{
    if (headerBytes.size() < 4) {
        return {};
    }

    // EBML: Matroska (.mkv) / WebM (.webm) — \x1a\x45\xdf\xa3
    static constexpr char kEbml[] = { '\x1a', '\x45', '\xdf', '\xa3' };
    if (startsWithBytes(headerBytes, kEbml, 4)) {
        const auto lower = headerBytes.toLower();
        if (lower.contains("webm")) {
            return QStringLiteral("webm");
        }
        return QStringLiteral("mkv");
    }

    // ISO Base Media File Format: "ftyp" at offset 4 -> mp4 / m4v / m4a / mov / 3gp / avif / heic
    if (headerBytes.size() >= 12 && headerBytes.mid(4, 4) == "ftyp") {
        const QByteArray brand = headerBytes.mid(8, 4);
        if (brand == "qt  ") {
            return QStringLiteral("mov");
        }
        if (brand == "M4A " || brand == "F4A ") {
            return QStringLiteral("m4a");
        }
        if (brand == "M4V " || brand == "F4V ") {
            return QStringLiteral("m4v");
        }
        if (brand.startsWith("3gp") || brand.startsWith("3g2")) {
            return QStringLiteral("3gp");
        }
        if (brand == "avif" || brand == "avis") {
            return QStringLiteral("avif");
        }
        if (brand == "heic" || brand == "heix" || brand == "mif1") {
            return QStringLiteral("heic");
        }
        return QStringLiteral("mp4");
    }

    // ZIP archive: PK\x03\x04 / PK\x05\x06 / PK\x07\x08
    static constexpr char kZip1[] = { 'P', 'K', '\x03', '\x04' };
    static constexpr char kZip2[] = { 'P', 'K', '\x05', '\x06' };
    static constexpr char kZip3[] = { 'P', 'K', '\x07', '\x08' };
    if (startsWithBytes(headerBytes, kZip1, 4)
        || startsWithBytes(headerBytes, kZip2, 4)
        || startsWithBytes(headerBytes, kZip3, 4)) {
        return QStringLiteral("zip");
    }

    // 7-Zip: 7z\xbc\xaf\x27\x1c
    static constexpr char k7z[] = { '7', 'z', '\xbc', '\xaf', '\x27', '\x1c' };
    if (startsWithBytes(headerBytes, k7z, 6)) {
        return QStringLiteral("7z");
    }

    // RAR4 / RAR5: Rar!\x1a\x07
    static constexpr char kRar[] = { 'R', 'a', 'r', '!', '\x1a', '\x07' };
    if (startsWithBytes(headerBytes, kRar, 6)) {
        return QStringLiteral("rar");
    }

    // PDF: %PDF-
    if (startsWithBytes(headerBytes, "%PDF-", 5)) {
        return QStringLiteral("pdf");
    }

    // GZIP: \x1f\x8b
    static constexpr char kGzip[] = { '\x1f', '\x8b' };
    if (startsWithBytes(headerBytes, kGzip, 2)) {
        return QStringLiteral("gz");
    }

    // XZ: \xfd7zXZ\x00
    static constexpr char kXz[] = { '\xfd', '7', 'z', 'X', 'Z', '\x00' };
    if (startsWithBytes(headerBytes, kXz, 6)) {
        return QStringLiteral("xz");
    }

    // Zstandard: \x28\xb5\x2f\xfd
    static constexpr char kZstd[] = { '\x28', '\xb5', '\x2f', '\xfd' };
    if (startsWithBytes(headerBytes, kZstd, 4)) {
        return QStringLiteral("zst");
    }

    // BZip2: BZh
    if (startsWithBytes(headerBytes, "BZh", 3)) {
        return QStringLiteral("bz2");
    }

    // PNG: \x89PNG\r\n\x1a\n
    static constexpr char kPng[] = { '\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n' };
    if (startsWithBytes(headerBytes, kPng, 8)) {
        return QStringLiteral("png");
    }

    // JPEG: \xff\xd8\xff
    static constexpr char kJpeg[] = { '\xff', '\xd8', '\xff' };
    if (startsWithBytes(headerBytes, kJpeg, 3)) {
        return QStringLiteral("jpg");
    }

    // GIF: GIF87a / GIF89a
    if (startsWithBytes(headerBytes, "GIF87a", 6) || startsWithBytes(headerBytes, "GIF89a", 6)) {
        return QStringLiteral("gif");
    }

    // RIFF containers: WEBP / AVI / WAVE
    if (headerBytes.size() >= 12 && startsWithBytes(headerBytes, "RIFF", 4)) {
        const QByteArray fourcc = headerBytes.mid(8, 4);
        if (fourcc == "WEBP") return QStringLiteral("webp");
        if (fourcc == "AVI ") return QStringLiteral("avi");
        if (fourcc == "WAVE") return QStringLiteral("wav");
    }

    // FLAC / OGG / FLV / MP3
    if (startsWithBytes(headerBytes, "fLaC", 4)) {
        return QStringLiteral("flac");
    }
    if (startsWithBytes(headerBytes, "OggS", 4)) {
        return QStringLiteral("ogg");
    }
    if (startsWithBytes(headerBytes, "FLV\x01", 4)) {
        return QStringLiteral("flv");
    }
    if (startsWithBytes(headerBytes, "ID3", 3)
        || (static_cast<unsigned char>(headerBytes[0]) == 0xFF
            && (static_cast<unsigned char>(headerBytes[1]) == 0xFB
                || static_cast<unsigned char>(headerBytes[1]) == 0xF3
                || static_cast<unsigned char>(headerBytes[1]) == 0xF2))) {
        return QStringLiteral("mp3");
    }

    // SQLite
    if (startsWithBytes(headerBytes, "SQLite format 3\0", 16)) {
        return QStringLiteral("sqlite");
    }

    // POSIX tar ("ustar" at offset 257 if supplied)
    if (headerBytes.size() >= 262 && headerBytes.mid(257, 5) == "ustar") {
        return QStringLiteral("tar");
    }

    // HTML document (useful for detecting web-page error responses)
    const QByteArray trimmed = headerBytes.trimmed().toLower();
    if (trimmed.startsWith("<!doctype html") || trimmed.startsWith("<html")) {
        return QStringLiteral("html");
    }

    return {};
}

bool FileNaming::hasGenericOrMissingExtension(const QString& filename)
{
    QString ext;
    stripExtension(QFileInfo(filename).fileName().trimmed(), &ext);
    return isScriptOrGenericExtension(ext);
}

bool FileNaming::isMeaninglessFilename(const QString& filename)
{
    const QString leaf = QFileInfo(filename).fileName().trimmed();
    if (leaf.isEmpty()) {
        return true;
    }
    QString ext;
    const QString stem = stripExtension(leaf, &ext).trimmed();
    const QString lowerStem = stem.toLower();
    if (lowerStem.isEmpty()) {
        return true;
    }

    static const QSet<QString> genericStems {
        QStringLiteral("download"), QStringLiteral("file"), QStringLiteral("videoplayback"),
        QStringLiteral("playback"), QStringLiteral("stream"), QStringLiteral("media"),
        QStringLiteral("index"), QStringLiteral("default"), QStringLiteral("get"),
        QStringLiteral("fetch"), QStringLiteral("serve"), QStringLiteral("content"),
        QStringLiteral("attachment"), QStringLiteral("blob"), QStringLiteral("blob_download"),
        QStringLiteral("sw_download"), QStringLiteral("unnamed"), QStringLiteral("untitled"),
        QStringLiteral("output"), QStringLiteral("video"), QStringLiteral("audio"),
        QStringLiteral("uc"), QStringLiteral("dl"), QStringLiteral("getfile")
    };
    if (genericStems.contains(lowerStem)) {
        return true;
    }

    // Script extension always denotes a server endpoint rather than a real filename
    if (ext == QStringLiteral("php") || ext == QStringLiteral("aspx")
        || ext == QStringLiteral("asp") || ext == QStringLiteral("cgi")
        || ext == QStringLiteral("jsp") || ext == QStringLiteral("do")) {
        return true;
    }

    // Pure numeric IDs (e.g. "123456789")
    static const QRegularExpression numericRe(QStringLiteral("^\\d+$"));
    if (numericRe.match(lowerStem).hasMatch()) {
        return true;
    }

    // UUIDs (e.g. "550e8400-e29b-41d4-a716-446655440000")
    static const QRegularExpression uuidRe(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    if (uuidRe.match(lowerStem).hasMatch()) {
        return true;
    }

    // Hex hashes (>= 16 hex characters: MD5/SHA1/SHA256/CDN object hash)
    static const QRegularExpression hexHashRe(QStringLiteral("^[0-9a-f]{16,}$"));
    if (hexHashRe.match(lowerStem).hasMatch()) {
        return true;
    }

    return false;
}

QString FileNaming::resolveSemanticFilename(const QString& currentFilename,
                                            const QString& pageTitle,
                                            const QByteArray& magicHeader,
                                            const QString& contentDispositionName,
                                            const QString& mimeType)
{
    const QString leaf = safeSuggestedFilename(QFileInfo(currentFilename).fileName());
    const QString cdName = safeSuggestedFilename(QFileInfo(contentDispositionName).fileName());
    const QString cleanTitle = sanitizePageTitle(pageTitle);

    QString currentExt;
    const QString currentStem = stripExtension(leaf, &currentExt);
    QString cdExt;
    const QString cdStem = stripExtension(cdName, &cdExt);

    QString sniffedExt = sniffExtensionFromMagicBytes(magicHeader);
    // Never replace a real binary filename with ".html"
    if (sniffedExt == QStringLiteral("html")) {
        sniffedExt.clear();
    }

    // 1. Choose best human-readable stem
    QString chosenStem;
    if (!cdStem.isEmpty() && !isMeaninglessFilename(cdName)) {
        chosenStem = cdStem;
    } else if (!currentStem.isEmpty() && !isMeaninglessFilename(leaf)) {
        chosenStem = currentStem;
    } else if (!cleanTitle.isEmpty()) {
        QString titleExt;
        chosenStem = stripExtension(cleanTitle, &titleExt);
        if (chosenStem.isEmpty()) {
            chosenStem = cleanTitle;
        }
    } else if (!cdStem.isEmpty()) {
        chosenStem = cdStem;
    } else if (!currentStem.isEmpty()) {
        chosenStem = currentStem;
    } else {
        chosenStem = QStringLiteral("download");
    }

    // 2. Choose best extension (magic bytes authoritative unless specialized ZIP container)
    QString chosenExt;
    const QString existingExt = !isScriptOrGenericExtension(cdExt) ? cdExt
        : (!isScriptOrGenericExtension(currentExt) ? currentExt : QString {});

    if (!sniffedExt.isEmpty()) {
        if (sniffedExt == QStringLiteral("zip") && isSpecializedZipExtension(existingExt)) {
            chosenExt = existingExt;
        } else {
            chosenExt = sniffedExt;
        }
    } else if (!existingExt.isEmpty()) {
        chosenExt = existingExt;
    } else {
        chosenExt = extensionFromMime(mimeType);
    }

    if (chosenExt.isEmpty()) {
        return chosenStem;
    }
    return chosenStem + QLatin1Char('.') + chosenExt;
}

} // namespace qtidm

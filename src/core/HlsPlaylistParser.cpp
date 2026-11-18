#include "core/HlsPlaylistParser.h"

#include <openssl/evp.h>
#include <QtEndian>
#include <QRegularExpression>
#include <algorithm>

namespace qtidm {

bool HlsPlaylistParser::isM3u8(const QByteArray& content)
{
    QByteArray trimmed = content.trimmed();
    if (trimmed.startsWith("\xef\xbb\xbf")) {
        trimmed = trimmed.mid(3).trimmed();
    }
    return trimmed.startsWith("#EXTM3U");
}

QMap<QString, QString> HlsPlaylistParser::parseAttributes(const QString& attributeLine)
{
    QMap<QString, QString> attributes;
    int i = 0;
    const int len = attributeLine.length();

    while (i < len) {
        while (i < len && (attributeLine[i] == QLatin1Char(' ') || attributeLine[i] == QLatin1Char(','))) {
            ++i;
        }
        if (i >= len) break;

        const int eqPos = attributeLine.indexOf(QLatin1Char('='), i);
        if (eqPos < 0) break;

        const QString key = attributeLine.mid(i, eqPos - i).trimmed().toUpper();
        i = eqPos + 1;

        QString value;
        if (i < len && attributeLine[i] == QLatin1Char('"')) {
            ++i; // skip open quote
            const int closeQuote = attributeLine.indexOf(QLatin1Char('"'), i);
            if (closeQuote >= 0) {
                value = attributeLine.mid(i, closeQuote - i);
                i = closeQuote + 1;
            } else {
                value = attributeLine.mid(i);
                i = len;
            }
        } else {
            const int commaPos = attributeLine.indexOf(QLatin1Char(','), i);
            if (commaPos >= 0) {
                value = attributeLine.mid(i, commaPos - i).trimmed();
                i = commaPos + 1;
            } else {
                value = attributeLine.mid(i).trimmed();
                i = len;
            }
        }
        attributes.insert(key, value);
    }
    return attributes;
}

HlsPlaylist HlsPlaylistParser::parse(const QString& playlistText, const QUrl& baseUrl)
{
    HlsPlaylist playlist;
    const QStringList lines = playlistText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);

    bool pendingStreamInf = false;
    HlsVariant pendingVariant;

    HlsKey currentKey;
    currentKey.method = QStringLiteral("NONE");

    HlsInitSegment currentInit;
    double pendingDuration = 0.0;
    qint64 pendingByteRangeOffset = -1;
    qint64 pendingByteRangeLength = -1;
    qint64 lastByteRangeOffset = 0;
    QUrl lastByteRangeUri;

    int mediaSequence = 0;
    int currentSegmentIndex = 0;
    bool hasEndList = false;

    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        if (line.startsWith(QLatin1Char('#'))) {
            if (line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"))) {
                playlist.isMaster = true;
                pendingStreamInf = true;
                pendingVariant = HlsVariant {};

                const QString attrStr = line.mid(18);
                const auto attrs = parseAttributes(attrStr);

                pendingVariant.bandwidth = attrs.value(QStringLiteral("BANDWIDTH")).toLongLong();
                pendingVariant.averageBandwidth = attrs.value(QStringLiteral("AVERAGE-BANDWIDTH")).toLongLong();
                pendingVariant.codecs = attrs.value(QStringLiteral("CODECS"));
                pendingVariant.audioGroup = attrs.value(QStringLiteral("AUDIO"));
                pendingVariant.subtitleGroup = attrs.value(QStringLiteral("SUBTITLES"));
                pendingVariant.frameRate = attrs.value(QStringLiteral("FRAME-RATE")).toDouble();

                const QString resStr = attrs.value(QStringLiteral("RESOLUTION"));
                if (!resStr.isEmpty()) {
                    const int xPos = resStr.indexOf(QLatin1Char('x'));
                    if (xPos > 0) {
                        pendingVariant.width = resStr.left(xPos).toInt();
                        pendingVariant.height = resStr.mid(xPos + 1).toInt();
                    }
                }
            } else if (line.startsWith(QStringLiteral("#EXT-X-MEDIA:"))) {
                playlist.isMaster = true;
                const auto attrs = parseAttributes(line.mid(13));
                if (attrs.value(QStringLiteral("TYPE")).compare(QStringLiteral("AUDIO"), Qt::CaseInsensitive) == 0) {
                    HlsAudioRendition rendition;
                    rendition.groupId = attrs.value(QStringLiteral("GROUP-ID"));
                    rendition.name = attrs.value(QStringLiteral("NAME"));
                    rendition.language = attrs.value(QStringLiteral("LANGUAGE"));
                    rendition.isDefault = (attrs.value(QStringLiteral("DEFAULT")).compare(QStringLiteral("YES"), Qt::CaseInsensitive) == 0);
                    rendition.autoSelect = (attrs.value(QStringLiteral("AUTOSELECT")).compare(QStringLiteral("YES"), Qt::CaseInsensitive) == 0);
                    const QString uriStr = attrs.value(QStringLiteral("URI"));
                    if (!uriStr.isEmpty()) {
                        rendition.uri = baseUrl.resolved(QUrl(uriStr));
                    }
                    playlist.audioRenditions.append(rendition);
                }
            } else if (line.startsWith(QStringLiteral("#EXT-X-TARGETDURATION:"))) {
                playlist.targetDuration = line.mid(22).trimmed().toInt();
            } else if (line.startsWith(QStringLiteral("#EXT-X-MEDIA-SEQUENCE:"))) {
                mediaSequence = line.mid(22).trimmed().toInt();
                playlist.mediaSequence = mediaSequence;
                currentSegmentIndex = mediaSequence;
            } else if (line.startsWith(QStringLiteral("#EXT-X-KEY:"))) {
                const auto attrs = parseAttributes(line.mid(11));
                currentKey.method = attrs.value(QStringLiteral("METHOD")).toUpper();
                const QString keyUriStr = attrs.value(QStringLiteral("URI"));
                if (!keyUriStr.isEmpty()) {
                    currentKey.uri = baseUrl.resolved(QUrl(keyUriStr));
                } else {
                    currentKey.uri = QUrl();
                }
                const QString ivStr = attrs.value(QStringLiteral("IV"));
                if (ivStr.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
                    currentKey.iv = QByteArray::fromHex(ivStr.mid(2).toLatin1());
                    while (currentKey.iv.size() < 16) {
                        currentKey.iv.prepend('\0');
                    }
                } else {
                    currentKey.iv.clear(); // Will be generated per-segment from sequence number
                }
            } else if (line.startsWith(QStringLiteral("#EXT-X-MAP:"))) {
                const auto attrs = parseAttributes(line.mid(11));
                const QString mapUriStr = attrs.value(QStringLiteral("URI"));
                if (!mapUriStr.isEmpty()) {
                    currentInit.uri = baseUrl.resolved(QUrl(mapUriStr));
                }
                const QString brStr = attrs.value(QStringLiteral("BYTERANGE"));
                if (!brStr.isEmpty()) {
                    const int atPos = brStr.indexOf(QLatin1Char('@'));
                    if (atPos >= 0) {
                        currentInit.byteRangeLength = brStr.left(atPos).toLongLong();
                        currentInit.byteRangeOffset = brStr.mid(atPos + 1).toLongLong();
                    } else {
                        currentInit.byteRangeLength = brStr.toLongLong();
                        currentInit.byteRangeOffset = 0;
                    }
                }
            } else if (line.startsWith(QStringLiteral("#EXTINF:"))) {
                QString val = line.mid(8);
                const int comma = val.indexOf(QLatin1Char(','));
                if (comma >= 0) {
                    val = val.left(comma);
                }
                pendingDuration = val.trimmed().toDouble();
            } else if (line.startsWith(QStringLiteral("#EXT-X-BYTERANGE:"))) {
                const QString brStr = line.mid(17).trimmed();
                const int atPos = brStr.indexOf(QLatin1Char('@'));
                if (atPos >= 0) {
                    pendingByteRangeLength = brStr.left(atPos).toLongLong();
                    pendingByteRangeOffset = brStr.mid(atPos + 1).toLongLong();
                } else {
                    pendingByteRangeLength = brStr.toLongLong();
                    pendingByteRangeOffset = -1; // Will use contiguous offset
                }
            } else if (line.startsWith(QStringLiteral("#EXT-X-ENDLIST"))) {
                hasEndList = true;
            }
        } else {
            // Non-comment line is a URI
            if (pendingStreamInf) {
                pendingVariant.uri = baseUrl.resolved(QUrl(line));
                playlist.variants.append(pendingVariant);
                pendingStreamInf = false;
            } else if (pendingDuration > 0.0 || !playlist.isMaster) {
                HlsSegment seg;
                seg.index = currentSegmentIndex;
                seg.uri = baseUrl.resolved(QUrl(line));
                seg.duration = pendingDuration;
                seg.initSegment = currentInit;
                seg.key = currentKey;

                // Handle sequence-number derived IV if unspecified
                if (seg.key.method == QStringLiteral("AES-128") && seg.key.iv.isEmpty()) {
                    seg.key.iv.resize(16);
                    seg.key.iv.fill(0);
                    const quint32 seqBigEndian = qToBigEndian(static_cast<quint32>(currentSegmentIndex));
                    std::memcpy(seg.key.iv.data() + 12, &seqBigEndian, 4);
                }

                // Handle byte range offset continuity
                if (pendingByteRangeLength > 0) {
                    seg.byteRangeLength = pendingByteRangeLength;
                    if (pendingByteRangeOffset >= 0) {
                        seg.byteRangeOffset = pendingByteRangeOffset;
                        lastByteRangeOffset = pendingByteRangeOffset + pendingByteRangeLength;
                    } else {
                        seg.byteRangeOffset = (seg.uri == lastByteRangeUri) ? lastByteRangeOffset : 0;
                        lastByteRangeOffset = seg.byteRangeOffset + pendingByteRangeLength;
                    }
                    lastByteRangeUri = seg.uri;
                }

                playlist.segments.append(seg);
                playlist.totalDuration += pendingDuration;

                pendingDuration = 0.0;
                pendingByteRangeLength = -1;
                pendingByteRangeOffset = -1;
                currentSegmentIndex++;
            }
        }
    }

    playlist.isLive = !playlist.isMaster && !hasEndList;
    return playlist;
}

int HlsPlaylistParser::selectBestVariant(const QVector<HlsVariant>& variants, int preferredMaxHeight)
{
    if (variants.isEmpty()) return -1;

    int bestIndex = 0;
    qint64 bestScore = -1;

    for (int i = 0; i < variants.size(); ++i) {
        const auto& v = variants[i];
        if (preferredMaxHeight > 0 && v.height > preferredMaxHeight) {
            continue;
        }

        // Score primarily by resolution (pixels), secondarily by bandwidth
        const qint64 pixels = static_cast<qint64>(v.width) * static_cast<qint64>(v.height);
        const qint64 score = (pixels > 0) ? (pixels * 10000000LL + v.bandwidth) : v.bandwidth;

        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }
    return bestIndex;
}

QUrl HlsPlaylistParser::selectAudioRenditionUrl(const HlsPlaylist& playlist, const QString& audioGroupId)
{
    if (audioGroupId.isEmpty()) {
        return QUrl();
    }
    const HlsAudioRendition* candidate = nullptr;
    for (const auto& r : playlist.audioRenditions) {
        if (r.groupId == audioGroupId && !r.uri.isEmpty()) {
            if (r.isDefault) {
                return r.uri;
            }
            if (!candidate || (!candidate->autoSelect && r.autoSelect)) {
                candidate = &r;
            }
        }
    }
    return candidate ? candidate->uri : QUrl();
}

QByteArray HlsPlaylistParser::decryptAes128Cbc(const QByteArray& encryptedData, const QByteArray& key, const QByteArray& iv)
{
    if (encryptedData.isEmpty() || key.size() != 16 || iv.size() != 16) {
        return encryptedData;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return QByteArray();
    }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.constData()),
                           reinterpret_cast<const unsigned char*>(iv.constData())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }

    QByteArray decrypted;
    decrypted.resize(encryptedData.size() + EVP_MAX_BLOCK_LENGTH);

    int outLen1 = 0;
    if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(decrypted.data()), &outLen1,
                          reinterpret_cast<const unsigned char*>(encryptedData.constData()),
                          encryptedData.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }

    int outLen2 = 0;
    if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(decrypted.data()) + outLen1, &outLen2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }

    decrypted.resize(outLen1 + outLen2);
    EVP_CIPHER_CTX_free(ctx);
    return decrypted;
}

} // namespace qtidm

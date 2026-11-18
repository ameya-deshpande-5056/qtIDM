#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>
#include <QVector>

namespace qtidm {

struct HlsKey {
    QString method; // "NONE", "AES-128", "SAMPLE-AES"
    QUrl uri;
    QByteArray iv; // 16 bytes
    QByteArray keyData; // 16 bytes fetched key
};

struct HlsInitSegment {
    QUrl uri;
    qint64 byteRangeOffset = -1;
    qint64 byteRangeLength = -1;
};

struct HlsSegment {
    int index = 0;
    QUrl uri;
    double duration = 0.0;
    qint64 byteRangeOffset = -1;
    qint64 byteRangeLength = -1;
    HlsInitSegment initSegment;
    HlsKey key;
};

struct HlsVariant {
    QUrl uri;
    qint64 bandwidth = 0;
    qint64 averageBandwidth = 0;
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    QString codecs;
    QString audioGroup;
    QString subtitleGroup;
};

struct HlsAudioRendition {
    QString groupId;
    QString name;
    QString language;
    bool isDefault = false;
    bool autoSelect = false;
    QUrl uri;
};

struct HlsPlaylist {
    bool isMaster = false;
    bool isLive = false; // true if #EXT-X-ENDLIST is missing
    int targetDuration = 0;
    int mediaSequence = 0;
    double totalDuration = 0.0;
    QVector<HlsVariant> variants;
    QVector<HlsAudioRendition> audioRenditions;
    QVector<HlsSegment> segments;
};

class HlsPlaylistParser {
public:
    static bool isM3u8(const QByteArray& content);
    static HlsPlaylist parse(const QString& playlistText, const QUrl& baseUrl);
    static int selectBestVariant(const QVector<HlsVariant>& variants, int preferredMaxHeight = 0);
    static QUrl selectAudioRenditionUrl(const HlsPlaylist& playlist, const QString& audioGroupId);
    static QMap<QString, QString> parseAttributes(const QString& attributeLine);
    static QByteArray decryptAes128Cbc(const QByteArray& encryptedData, const QByteArray& key, const QByteArray& iv);
};

} // namespace qtidm
